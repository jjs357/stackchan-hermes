#!/usr/bin/env python3
"""
stackchan-setup.py: give a StackChan running R7_HermesTalk v0.17.0 or later its settings over
the USB cable, so a prebuilt release firmware needs no secrets.h and no compiling.

What it does, in order:
  1. Finds the robot on USB (Espressif USB ID 303a:...) and checks that it answers the setup
     protocol (HELLO).
  2. Robot token and webhook secret: reuses the ones in the macOS login Keychain
     (stackchan-r1-token, stackchan-webhook-secret), or makes new random ones and stores them
     there. --new-secrets always makes new ones.
  3. Writes the webhook secret to ~/.hermes/.env as STACKCHAN_WEBHOOK_SECRET (mode 600).
  4. Asks for the Wi-Fi name and password (the password is not shown) and the Mac's address
     for talk webhooks (default: this Mac's current address).
  5. Sends all five settings to the robot, which stores them in its own settings storage
     (NVS namespace "hermes"; the servo calibration is never touched), then restarts it.
  6. Waits for the robot to join Wi-Fi, writes its address as ROBOT_IP into every rN.conf in
     this repository, and checks GET /status with the token (read-only).

Nothing here prints the token, the secret or the Wi-Fi password. Run it in Terminal on the
Mac's own screen: over SSH macOS usually cannot reach the login Keychain.

Other uses:
  --status   show what the robot has (no changes)
  --clear    erase the robot's saved settings (servo calibration kept) and restart it

Needs Python 3.9+ and pyserial (pip install pyserial; esptool installs it too).
"""
import argparse
import getpass
import json
import os
import re
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is missing: python3 -m pip install pyserial (or use the venv from RELEASE.md)")

VERSION = "1.0.0"
REPO = Path(__file__).resolve().parent.parent
TOKEN_ITEM = "stackchan-r1-token"
SECRET_ITEM = "stackchan-webhook-secret"
ENV_FILE = Path.home() / ".hermes" / ".env"
ENV_KEY = "STACKCHAN_WEBHOOK_SECRET"
ESPRESSIF_VID = 0x303A
ROBOT_PORT = 8080
REPLY = "@R7 "


class SetupError(Exception):
    pass


# ---------------------------------------------------------------- Keychain and .env

def keychain_get(item):
    r = subprocess.run(["security", "find-generic-password", "-s", item, "-w"],
                       capture_output=True, text=True)
    if r.returncode == 0:
        return r.stdout.rstrip("\n")
    if "could not be found" in r.stderr:
        return None
    raise SetupError(f"Keychain read of {item} failed: {r.stderr.strip()}. "
                     "Run this on the Mac's own screen, not over SSH.")


def keychain_set(item, value):
    # Through `security -i` on stdin, so the value never appears in a process list
    user = os.environ.get("USER") or getpass.getuser()
    if not re.fullmatch(r"[0-9a-f]+", value):
        raise SetupError("internal: only hex values are stored")
    cmd = f'add-generic-password -U -s "{item}" -a "{user}" -w {value}\n'
    r = subprocess.run(["security", "-i"], input=cmd, capture_output=True, text=True)
    if r.returncode != 0 or "error" in (r.stderr + r.stdout).lower():
        raise SetupError(f"Keychain write of {item} failed. Run this on the Mac's own screen, not over SSH.")
    if keychain_get(item) != value:
        raise SetupError(f"Keychain write of {item} did not stick")


def env_set_secret(value):
    """Sets STACKCHAN_WEBHOOK_SECRET in ~/.hermes/.env; True if the file changed."""
    if not ENV_FILE.parent.is_dir():
        print(f"  ~/.hermes not found: install Hermes (README step 3), then run this again "
              f"or add {ENV_KEY} as in README step 4.3")
        return False
    lines = ENV_FILE.read_text().splitlines() if ENV_FILE.exists() else []
    new_line = f"{ENV_KEY}={value}"
    kept = [ln for ln in lines if not ln.startswith(ENV_KEY + "=")]
    changed = [ln for ln in lines if ln.startswith(ENV_KEY + "=")] != [new_line]
    if changed:
        fd = os.open(ENV_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write("\n".join(kept + [new_line]) + "\n")
    os.chmod(ENV_FILE, 0o600)
    return changed


def get_or_make(item, label, renew):
    value = None if renew else keychain_get(item)
    if value:
        print(f"  {label}: reusing the one in the Keychain ({item})")
        return value, False
    value = secrets.token_hex(24)
    keychain_set(item, value)
    print(f"  {label}: new random value stored in the Keychain ({item})")
    return value, True


# ---------------------------------------------------------------- serial protocol

class Robot:
    def __init__(self, port):
        self.port = port
        self.ser = None

    def open(self, wait_s=30):
        deadline = time.monotonic() + wait_s
        while True:
            try:
                s = serial.Serial()
                s.port = self.port
                s.baudrate = 115200
                s.timeout = 0.2
                s.dtr = False  # do not reset the robot when the port opens
                s.rts = False
                s.open()
                self.ser = s
                return
            except (serial.SerialException, OSError):
                if time.monotonic() > deadline:
                    raise SetupError(f"cannot open {self.port}: is the robot plugged in and is "
                                     "the Arduino IDE's Serial Monitor closed?")
                time.sleep(0.5)

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except (serial.SerialException, OSError):
                pass
            self.ser = None

    def command(self, line, timeout=10):
        """Sends one command line, returns the reply text after '@R7 OK '; raises on ERR."""
        word = line.split(" ", 2)[0] if not line.startswith("SET ") else "SET"
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        deadline = time.monotonic() + timeout
        buf = b""
        while time.monotonic() < deadline:
            buf += self.ser.read(256)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip()
                if not text.startswith(REPLY):
                    continue  # the sketch's own log lines
                rest = text[len(REPLY):]
                if rest.startswith("ERR"):
                    raise SetupError("robot says: " + rest[4:])
                if rest.startswith("OK " + word):
                    return rest[3:]
        raise SetupError(f"no reply to {word} within {timeout} s")

    def hello(self, wait_s=40):
        """Waits until the sketch answers (it may be restarting)."""
        deadline = time.monotonic() + wait_s
        while True:
            try:
                reply = self.command("HELLO", timeout=2)
                return reply
            except SetupError:
                pass  # no answer yet: still starting
            except (serial.SerialException, OSError):
                self.close()  # the USB port went away during a restart
            if time.monotonic() > deadline:
                raise SetupError("the robot does not answer the setup protocol. It needs "
                                 "R7_HermesTalk v0.17.0 or later (see RELEASE.md).")
            time.sleep(0.5)
            if self.ser is None:
                try:
                    self.open(wait_s=2)
                except SetupError:
                    pass

    def status(self):
        reply = self.command("STATUS")
        info = {}
        for m in re.finditer(r'(\w+)=("([^"]*)"|\S+)', reply):
            info[m.group(1)] = m.group(3) if m.group(3) is not None else m.group(2)
        return reply, info


def find_port(given):
    if given:
        return given
    found = [p.device for p in list_ports.comports()
             if p.vid == ESPRESSIF_VID and p.device.startswith("/dev/cu.")]
    if len(found) == 1:
        return found[0]
    if not found:
        raise SetupError("no StackChan found on USB. Plug it in with a data cable "
                         "(or pass --port /dev/cu.usbmodem...).")
    print("More than one Espressif device is connected:")
    for i, d in enumerate(found, 1):
        print(f"  {i}. {d}")
    pick = input("Which one is the robot? [number] ").strip()
    if not pick.isdigit() or not 1 <= int(pick) <= len(found):
        raise SetupError("no port chosen")
    return found[int(pick) - 1]


# ---------------------------------------------------------------- Mac side

def mac_address():
    for ifc in ("en0", "en1"):
        r = subprocess.run(["ipconfig", "getifaddr", ifc], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    return ""


def write_conf_files(robot_ip):
    written = []
    for example in sorted(REPO.glob("*/r*.conf.example")):
        conf = example.with_name(example.name[: -len(".example")])
        source = conf if conf.exists() else example
        lines = source.read_text().splitlines()
        if any(ln.startswith("ROBOT_IP=") for ln in lines):
            lines = [f"ROBOT_IP={robot_ip}" if ln.startswith("ROBOT_IP=") else ln for ln in lines]
        else:
            lines.append(f"ROBOT_IP={robot_ip}")
        conf.write_text("\n".join(lines) + "\n")
        written.append(str(conf.relative_to(REPO)))
    return written


def check_status(robot_ip, token):
    req = urllib.request.Request(f"http://{robot_ip}:{ROBOT_PORT}/status",
                                 headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, {}
    except (urllib.error.URLError, OSError, ValueError):
        return 0, {}


def ask(prompt, default=""):
    shown = f" [{default}]" if default else ""
    value = input(f"{prompt}{shown}: ").strip()
    return value or default


# ---------------------------------------------------------------- main

def run_setup(robot, args):
    print("\n1. Secrets (Keychain)")
    token, _ = get_or_make(TOKEN_ITEM, "robot token", args.new_secrets)
    secret, secret_new = get_or_make(SECRET_ITEM, "webhook secret", args.new_secrets)

    print("\n2. Hermes")
    env_changed = env_set_secret(secret)
    if env_changed:
        print(f"  {ENV_KEY} written to ~/.hermes/.env (mode 600)")
    elif ENV_FILE.exists():
        print(f"  ~/.hermes/.env already has this {ENV_KEY}")

    print("\n3. Wi-Fi and the Mac's address")
    print("  The robot needs a 2.4 GHz network (it cannot join 5 GHz-only networks).")
    ssid = args.ssid or ask("  Wi-Fi name")
    if not ssid:
        raise SetupError("no Wi-Fi name given")
    password = getpass.getpass("  Wi-Fi password (not shown; empty for an open network): ")
    host = args.host or ask("  Mac address for talk webhooks (the Mac running the Hermes gateway)",
                            mac_address())
    if not host:
        raise SetupError("no Mac address given")
    if host.endswith(".local"):
        print("  Note: a fixed IP (reserved in your router) is more reliable than a .local name.")

    print("\n4. Sending settings to the robot")
    robot.command("SET ssid " + ssid)
    robot.command("SET pass " + password)
    robot.command("SET token " + token)
    robot.command("SET hook_host " + host)
    robot.command("SET hook_secret " + secret)
    robot.command("SAVE")
    print("  saved on the robot; restarting it")
    try:
        robot.command("REBOOT", timeout=3)
    except SetupError:
        pass  # the USB port may drop before the reply arrives
    robot.close()
    time.sleep(3)

    print("\n5. Waiting for the robot to join Wi-Fi")
    robot.open(wait_s=30)
    robot.hello()
    deadline = time.monotonic() + 45
    info = {}
    while time.monotonic() < deadline:
        _, info = robot.status()
        if info.get("wifi") == "connected":
            break
        time.sleep(2)
    if info.get("settings") != "saved":
        raise SetupError("the robot did not load the saved settings (STATUS settings="
                         f"{info.get('settings')})")
    if info.get("wifi") != "connected":
        raise SetupError("the robot has its settings but did not join Wi-Fi in 45 s. Check the "
                         "Wi-Fi name and password (2.4 GHz) and run this again.")
    robot_ip = info.get("ip", "")
    print(f"  robot address: {robot_ip}  (reserve it in your router so it does not change)")

    print("\n6. Mac-side settings files")
    for name in write_conf_files(robot_ip):
        print(f"  {name}: ROBOT_IP={robot_ip}")

    print("\n7. Check: GET /status with the token")
    code, st = check_status(robot_ip, token)
    if code == 200:
        print(f"  HTTP 200, {st.get('sketch')} v{st.get('version')}, settings={st.get('settings')}, "
              f"hook_ok={st.get('hook_ok')}, servo_zero_ok={st.get('servo_zero_ok')}")
    else:
        print(f"  HTTP {code or 'no answer'}: the Mac cannot reach the robot yet (same network? firewall?)")

    print("\nDone.")
    if secret_new or env_changed:
        print("The webhook secret changed: run  hermes gateway restart  so Hermes uses it.")
    print("Next: README step 6 (Hermes profiles), if not done yet.")


def main():
    ap = argparse.ArgumentParser(description="Give a StackChan (R7_HermesTalk v0.17.0+) its settings over USB.")
    ap.add_argument("--port", help="serial port (default: the one Espressif device found)")
    ap.add_argument("--ssid", help="Wi-Fi name (default: ask)")
    ap.add_argument("--host", help="Mac address for talk webhooks (default: ask, offering this Mac's address)")
    ap.add_argument("--new-secrets", action="store_true",
                    help="make a new token and webhook secret even if the Keychain has them")
    ap.add_argument("--status", action="store_true", help="only show what the robot has")
    ap.add_argument("--clear", action="store_true",
                    help="erase the robot's saved settings (servo calibration kept) and restart it")
    ap.add_argument("--version", action="version", version=VERSION)
    args = ap.parse_args()

    robot = Robot(find_port(args.port))
    try:
        robot.open()
        print("Robot: " + robot.hello() + f"  ({robot.port})")
        if args.status:
            reply, _ = robot.status()
            print(reply)
        elif args.clear:
            if ask("Erase the robot's Wi-Fi, token and secret? Type yes").lower() != "yes":
                print("Nothing changed.")
                return 0
            print(robot.command("CLEAR"))
            try:
                robot.command("REBOOT", timeout=3)
            except SetupError:
                pass
            print("Restarting the robot; it will show SETUP NEEDED (or use secrets.h if it was built with one).")
        else:
            run_setup(robot, args)
        return 0
    except SetupError as e:
        print(f"\nStopped: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nStopped. Nothing is saved on the robot until step 4's SAVE.", file=sys.stderr)
        return 130
    finally:
        robot.close()


if __name__ == "__main__":
    sys.exit(main())
