# Installing a release (no Arduino IDE)

Each [GitHub Release](https://github.com/jjs357/stackchan-hermes/releases) has the robot
firmware (`R7_HermesTalk`) already built. You flash it with `esptool` and give the robot its
settings with `tools/stackchan-setup.py`. You don't need the Arduino IDE, any libraries or a
`secrets.h`.

This replaces **steps 4 and 5** of the [README setup](README.md#setup-on-a-new-mac-step-by-step).
Do README steps 1–3 first (Mac tools, Claude Code, Hermes), then this page, then continue at
README step 6.

> **Read this before flashing.** Your robot keeps its **servo calibration** in a settings
> area of its flash memory (0x9000–0xDFFF). The same area also holds the Wi-Fi name and password
> and the secrets this project stores there. The commands below never write to that area.
> **Do not** run `esptool erase-flash`, and **do not** flash a single "merged" or "factory"
> image at address 0x0. Either one erases the calibration, and the head would then move to the
> wrong positions.

## What you need

- A StackChan with the M5Stack CoreS3 controller, and a USB-C **data** cable (some cables
  only charge)
- The Mac that will run Hermes, with Python 3.11 or newer (README step 1) and this repository
  cloned
- A **2.4 GHz** Wi-Fi network. The robot cannot join a network that is 5 GHz only.

## 1. Tools

Make a small Python environment for `esptool` and `pyserial`:

```bash
python3 -m venv ~/stackchan-venv
~/stackchan-venv/bin/pip install esptool pyserial
```

The commands below use `~/stackchan-venv/bin/...`. If you already have esptool 4.x, write
`read_flash` and `write_flash` with underscores instead of the hyphens shown below.

## 2. Download and check

From the release page, download `stackchan-r7-vX.Y.Z.zip` (the version is in the name), then:

```bash
cd ~/Downloads
unzip stackchan-r7-v*.zip
cd stackchan-r7-v*/
shasum -a 256 -c SHA256SUMS          # every line must say OK
```

It contains `bootloader.bin`, `partitions.bin`, `boot_app0.bin` and
`R7_HermesTalk-vX.Y.Z.bin`. `BUILD-INFO.txt` lists the exact board package and library
versions that went into the build.

## 3. Find the robot's port

Plug the robot into the Mac. Close the Arduino IDE's Serial Monitor if it's open.

```bash
ls /dev/cu.usbmodem*
PORT=/dev/cu.usbmodem1101            # use the name shown above
```

If nothing appears, try another cable. If esptool later says it cannot connect, put the robot
in download mode: hold the reset button on the side for about 2 seconds, until the small
green light comes on. Then run the command again.

## 4. Back up the settings area

Do this once, before the first flash. It saves the servo calibration (and anything else in the
settings area) to a file:

```bash
~/stackchan-venv/bin/esptool --chip esp32s3 --port $PORT read-flash 0x9000 0x5000 ~/stackchan-nvs-backup.bin
```

Keep that file private. Once the robot is set up, the same area also holds your Wi-Fi password
and the robot's secrets. To put it back later:
`esptool --chip esp32s3 --port $PORT write-flash 0x9000 ~/stackchan-nvs-backup.bin`

## 5. Flash

From the unzipped release folder:

```bash
~/stackchan-venv/bin/esptool --chip esp32s3 --port $PORT write-flash \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin \
  0x10000 R7_HermesTalk-v*.bin
```

The robot restarts when esptool finishes. On a robot that has never been set up, the screen shows
**SETUP NEEDED**.

## 6. Settings

From the top folder of your clone, in Terminal **on the Mac's own screen** (over SSH, macOS
usually cannot reach the login Keychain):

```bash
cd stackchan-hermes
~/stackchan-venv/bin/python tools/stackchan-setup.py
```

The script:

1. Makes the robot token and the webhook secret, or reuses the ones already in your Keychain
   (`stackchan-r1-token`, `stackchan-webhook-secret`). It stores the webhook secret in
   `~/.hermes/.env` for Hermes.
2. Asks for your Wi-Fi name and password (the password is not shown), and for the Mac address
   the robot should send talk requests to. The default is this Mac's current address. Reserve
   that address in your router so it doesn't change.
3. Sends all of this to the robot over the cable. The robot stores it and restarts.
4. Waits for the robot to join Wi-Fi, writes its address as `ROBOT_IP` into every `rN.conf`
   file in the clone, and checks the robot with the token.

If the script says the webhook secret changed, run `hermes gateway restart`.

Then continue with **README step 6** (Hermes profiles). README step 8 has the checks.

## Later changes

| You want to | Do this |
|---|---|
| Change Wi-Fi, or switch the robot to another Mac | Run `tools/stackchan-setup.py` again. Use `--host ADDRESS` for another Mac. It reuses the Keychain secrets, and the other Mac needs the same webhook secret in its `~/.hermes/.env`. |
| See what the robot has | `tools/stackchan-setup.py --status` (shows "set" and lengths, never the secrets) |
| Make new secrets | `tools/stackchan-setup.py --new-secrets`, then `hermes gateway restart` |
| Install a newer release | Steps 2, 3 and 5. The settings stay on the robot. |
| Go back to an Arduino build with `secrets.h` | `tools/stackchan-setup.py --clear`. Saved settings always win over `secrets.h`. |

## Safety notes

- The setup protocol works only over the USB cable, so anyone who can plug into the robot can
  change its settings. (They could also reflash it.)
- On the robot, the settings are stored unencrypted, just as a `secrets.h` build stores them
  inside the firmware.
- A release build never contains a `secrets.h`. `tools/build-release.sh` builds without one and
  stops if any of the builder's Keychain values appears in the files.

## Known limits

- These steps were tested on a StackChan that had already been flashed from the Arduino IDE (the
  board's default 16 MB layout). A robot still running **M5Stack's factory firmware** may use a
  different flash layout. It has not been tested, and its servo calibration might sit somewhere
  else. Make the backup in step 4 first. If you are unsure, build and upload once with the
  Arduino IDE (README step 5), then use releases after that.

## For maintainers: building a release

```bash
tools/build-release.sh --libraries ~/Documents/Arduino/libraries   # your sketchbook's libraries
```

The script builds `R7_HermesTalk` with `arduino-cli`, without `secrets.h`, using the board
defaults. It checks the flash layout (four parts, settings area at 0x9000) and writes
`dist/stackchan-r7-vX.Y.Z/` and a `.zip` with `SHA256SUMS` and `BUILD-INFO.txt`. Attach the
zip to a GitHub Release. Never attach `R7_HermesTalk.ino.merged.bin`.
