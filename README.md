# StackChan + Hermes

Connecting an [M5Stack StackChan](https://docs.m5stack.com/) desk robot (CoreS3-based) to the
[Hermes Agent](https://github.com/NousResearch/hermes-agent) framework, so an AI agent running on
a Mac can see, hear, move and speak through the robot. You tap the robot's head and ask
something, and it answers aloud.

The project is built in small milestones (R1–R7). Each one adds a single capability, is tested
on the robot, and is kept in its own folder, so you can follow along or stop at any stage.
Safety and privacy limits are built into the firmware rather than left to the AI.

> **Status:** experimental, personal project. Tested on one robot with one Mac (macOS 15).
> Not a packaged product, and not affiliated with M5Stack, Nous Research or Anthropic.

## What it does today

- **Tap to talk.** A tap (or a 1 s hold) on the head starts a recording that ends 1.2 s after
  you stop speaking (at most 8 s). The robot beeps,
  shows a red **MIC REC** banner, then sends a signed notice to Hermes on the Mac. Hermes fetches
  the clip, transcribes it locally with whisper.cpp, and replies through the robot.
- **Spoken commands and questions.** "Look left", "nod", "turn the camera on", "say good
  morning", or any question. Commands are carried out and confirmed in a few words; questions
  get a one- or two-sentence spoken answer.
- **Follow-up questions.** For 2 minutes after the robot last spoke, the next question can
  refer to the previous ones ("How many people live there?"). The tool server keeps these few
  exchanges in memory only.
- **Live camera view** on the robot's own screen (about 10 fps), switched off automatically after 2 minutes.
- **"What do you see?"** A photo is described by a local vision model (gemma3 via Ollama), and
  only the text description reaches the agent. The photo is deleted.
- **QUIET mode.** Hold the robot's *screen* to toggle. While quiet, the robot ignores touches
  and refuses to speak, move, record or take photos.

## How it works

```
 tap on head                                Mac
┌────────────┐  signed webhook {"clip": N}  ┌──────────────────────────────┐
│  StackChan │ ───────────────────────────▶ │ Hermes gateway (:8644)        │
│  firmware  │                              │   └─ "stackchan" profile      │
│ (Arduino)  │ ◀─── token-protected HTTP ── │        └─ MCP tool server     │
└────────────┘   /voice/take /say           │           stackchan_tools.py  │
                 /head/move /camera/...     │           (runs the scripts)  │
                                            │ whisper.cpp · Ollama · say    │
                                            └──────────────────────────────┘
```

1. **Firmware** (`R7_HermesTalk/R7_HermesTalk.ino`) runs a small HTTP server on the robot. Every
   endpoint needs a bearer token. The only thing the robot ever starts is one HMAC-signed
   webhook after someone touches its head, and that webhook carries only a clip number.
2. **Hermes gateway** receives the webhook, checks the signature and timestamp, and runs the
   route script `stackchan-heard.py`, which has the tool server fetch and transcribe the clip
   (over a private local socket). It then starts a run on a dedicated `stackchan` profile whose
   *only* tools are the robot tools, with the words already in the prompt.
3. **`stackchan_tools.py`** is a minimal, standard-library MCP server. Each tool runs one of the
   project's shell scripts with checked arguments (never a shell string):
   `say`, `gesture`, `look`, `hear`, `hear_clip`, `camera_view`.
4. The scripts talk to the robot with the token from the macOS Keychain. Speech is generated
   with macOS `say`, transcription runs offline in whisper.cpp, and image description runs
   in local Ollama.

## Repository layout

Each milestone folder is a complete Arduino sketch plus its Mac-side scripts. Later folders
build on earlier ones, and the robot runs one sketch at a time.

| Folder | Milestone | Adds |
|---|---|---|
| `R1_HermesStatus/` | R1 | Wi-Fi, token-protected `GET /status` |
| `R2_HermesGreeting/` | R2 | `POST /say`: speak one short WAV (`r2-say.sh`, macOS TTS) |
| `R3_HermesMove/` | R3 | Head movement with hard limits (`r3-head.sh`, `move_rules.h`) |
| `R4_HermesCamera/` | R4 | One camera frame on request (`r4-snap.sh`) |
| `R34_HermesMoveCamera/` | R3+R4 | Combined. `r34-look.sh` = photo → local gemma3 → text |
| `R5_HermesListen/` | R5 | Microphone on request (`r5-hear.sh` → local whisper.cpp) |
| `R6_HermesTouch/` | R6 | Head taps, QUIET mode, first `stackchan` MCP tool server |
| `R7_HermesTalk/` | R7 | **Current.** Sprite screen, live camera view, tap-to-talk via webhook |
| `SimpleServoBehavior/` | — | Early standalone servo test |

Other Mac-side scripts in `R7_HermesTalk/`:

- `r7-hook.py`: sends a signed test webhook (`"look left"` or `--clip N`).
- `r7-clip.sh`: fetches and transcribes one clip.
- `r7-view.sh`: switches the camera view on or off.
- `r7-talk.py`: an older, fully local command listener. It needs firmware v0.10.0 or earlier.

`SETUP-STATUS.md` is the detailed build log (every decision, test batch and undo step).

## Requirements

**Hardware:** M5Stack StackChan (CoreS3 controller, head servos, camera, microphone, touch sensor)
and a Mac on the same Wi-Fi network.

**Robot side:** Arduino IDE 2.x or `arduino-cli` with

| Component | Version used |
|---|---|
| Board package | Espressif `esp32` 3.3.12, board `esp32:esp32:m5stack_cores3` |
| M5StackChan | 1.1.1 |
| M5Unified / M5GFX | 0.2.28 / 0.2.35 |

**Mac side** (scripts assume macOS: Keychain, `say`, Bonjour):

- [Hermes Agent](https://github.com/NousResearch/hermes-agent) with the gateway running
- Python 3.11 or newer, standard library only (Homebrew `python@3.13`; macOS's own 3.9 is too old)
- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) (`whisper-server`, kept running on 127.0.0.1 by the
  tool server, and `whisper-cli` as the fallback) and the `ggml-base.en.bin` model
  in `R5_HermesListen/models/` (not in the repository: about 141 MB)
- [Ollama](https://ollama.com/) with `gemma3` (or `gemma3:27b` on a large Apple Silicon Mac), only for "what do you see?"
- A model provider for the Hermes profile. The robot runs on Claude Opus. We also tried
  `minimax-m3.1-flash:cloud` through Ollama (`provider: custom`, `base_url: http://localhost:11434/v1`):
  about twice as fast, but on a request it could not fulfil it looped through the robot tools,
  including taking photos nobody asked for. The tool server's per-turn limits (since 0.7.0)
  hold whatever the model does, but compare a new model with the dry-run eval first.

## Setup on a new Mac, step by step

This takes about an hour. It assumes you already use the Arduino IDE (board package and libraries
from the table above) and are comfortable in Terminal. Every command runs in Terminal on the Mac
that will run Hermes, from the top folder of your clone unless it says otherwise.

```bash
git clone https://github.com/jjs357/stackchan-hermes.git
cd stackchan-hermes
```

### 1. Mac tools

1. Install [Homebrew](https://brew.sh), then Python and whisper.cpp:
   ```bash
   brew install python@3.13 whisper-cpp
   ```
2. Install the [Ollama](https://ollama.com/) app and a local vision model for "what do you see?":
   ```bash
   ollama pull gemma3          # about 3 GB; slow on an Intel Mac (~35 s per photo)
   ollama pull gemma3:27b      # optional, Apple Silicon with 32 GB or more (~2 s per photo)
   ```
3. Download the speech model and check it before use (the tool server checks it again):
   ```bash
   mkdir -p R5_HermesListen/models
   curl -fL -o R5_HermesListen/models/ggml-base.en.bin \
     https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin
   shasum -a 256 R5_HermesListen/models/ggml-base.en.bin
   # must print a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002
   ```

### 2. Claude Code (the robot's model)

The robot's Hermes profile runs Claude Opus on a Claude Pro or Max subscription through the
official `claude` command. Install it and log in once:

```bash
curl -fsSL https://claude.ai/install.sh | bash
claude            # then type /login, finish in the browser, and /exit
```

Every robot turn counts against that subscription's usage. To use another model provider
instead, see "Changing the model" below.

### 3. Hermes Agent

```bash
curl -fsSL https://hermes-agent.nousresearch.com/install.sh | bash
hermes gateway status
```

The installer may offer a Nous Portal sign-in. The robot does not use it; skip it or pick the
free plan. `hermes gateway status` should say the gateway is supervised by launchd. If it does
not, run `hermes gateway install`.

### 4. Secrets (once per robot)

Make two random values. Keep this Terminal window private and close it afterwards.

```bash
openssl rand -hex 24      # 1: the robot token
openssl rand -hex 24      # 2: the webhook secret
```

1. **Robot:** in `R7_HermesTalk/`, copy `secrets.h.example` to `secrets.h` and fill in your
   Wi-Fi, value 1 as `HERMES_STATUS_TOKEN`, value 2 as `HERMES_HOOK_SECRET`, and the Mac's
   address as `HERMES_HOOK_HOST`. `ipconfig getifaddr en0` shows the Mac's address; reserve it
   in your router so it does not change. `secrets.h` is git-ignored: never commit or share it.
2. **Mac Keychain** (each command asks for the value twice and hides what you type):
   ```bash
   security add-generic-password -s stackchan-r1-token -a "$USER" -w          # value 1
   security add-generic-password -s stackchan-webhook-secret -a "$USER" -w    # value 2
   ```
3. **Hermes** reads the webhook secret from `~/.hermes/.env`. This copies it from the Keychain
   without showing it:
   ```bash
   printf 'STACKCHAN_WEBHOOK_SECRET=%s\n' \
     "$(security find-generic-password -s stackchan-webhook-secret -w)" >> ~/.hermes/.env
   chmod 600 ~/.hermes/.env
   ```

Run these in Terminal on the Mac's own screen. Over SSH, macOS usually cannot reach the login
Keychain ("User interaction is not allowed" or "not found").

### 5. Robot firmware and address

1. Upload `R7_HermesTalk/R7_HermesTalk.ino` with the Arduino IDE. The robot shows its IP
   address on its screen after it joins Wi-Fi.
2. Give every milestone folder its settings file, with your robot's address:
   ```bash
   ROBOT=192.168.1.50        # your robot's address
   for f in */r*.conf.example; do
     sed "s/^ROBOT_IP=.*/ROBOT_IP=$ROBOT/" "$f" > "${f%.example}"
   done
   ```
   With `gemma3:27b`, also add `VISION_MODEL=gemma3:27b` to `R34_HermesMoveCamera/r34.conf`.
   The `.conf` files are git-ignored.
3. Check that the Mac can reach the robot with the token (read-only):
   ```bash
   bash R1_HermesStatus/r1-status.sh | tail -1        # HTTP 200
   ```

### 6. Hermes profiles

Two profiles, each with only the robot tools: `stackchan` drives the real robot, and
`stackchaneval` runs the same tool server in dry-run mode for testing (no robot).

```bash
PY="$(brew --prefix)/bin/python3.13"
for p in stackchan stackchaneval; do
  hermes profile create "$p" --no-alias --no-skills
  hermes -p "$p" plugins install claude-subscription-directsdk
  sed -e "s#@PYTHON@#$PY#" -e "s#@REPO@#$PWD#" \
    "R7_HermesTalk/hermes-setup/$p-profile.yaml" > ~/.hermes/profiles/$p/config.yaml
done
```

The templates in `R7_HermesTalk/hermes-setup/` explain each setting. The memory is off, the
approval deny list keeps photos and recordings away from the model, and the dry-run server is
named `stackchandry`. It must not share the live server's name, or the shared gateway hands it
the live robot connection.

### 7. Webhook routes and the route script

```bash
mkdir -p ~/.hermes/profiles/stackchan/scripts
install -m 700 R7_HermesTalk/hermes-scripts/stackchan-heard.py ~/.hermes/profiles/stackchan/scripts/
cp ~/.hermes/config.yaml ~/.hermes/config.yaml.bak
grep -n '^platforms:' ~/.hermes/config.yaml || cat R7_HermesTalk/hermes-setup/webhook-routes.yaml >> ~/.hermes/config.yaml
hermes gateway restart
```

If `grep` prints a line, your config already has a `platforms:` section. Then copy the
`webhook:` block from `webhook-routes.yaml` under it by hand instead.

`robot-voice` is the robot's tap-to-talk route. Its route script has the tool server fetch and
transcribe the clip before the model starts, so the prompt already holds the words.
`robot-text` takes typed test sentences.

### 8. Check, in this order

```bash
hermes logs gateway -n 30 | grep webhook     # "Listening on ... :8644 — routes: robot-voice, robot-text"
ls -l ~/.hermes/stackchan/heard.sock         # srw------- : the tool server is up
python3.13 -I R7_HermesTalk/eval/run-eval.py profile --only left,see,weather   # dry run, no robot
python3.13 -I R7_HermesTalk/r7-hook.py "look left"   # typed: the robot turns and says so
```

Then tap (or hold) the robot's head, wait for the beep, and say "look left".

## Looking after Hermes

| Task | What to do |
|---|---|
| Is it running? | `hermes gateway status` |
| After editing `stackchan_tools.py`, a profile, a route or a prompt | `hermes gateway restart`, then a dry run (step 8) before talking to the robot |
| What did the robot do? | `R7_HermesTalk/logs/tools.log`: one line per tool call with time and duration, never the words |
| Hermes logs | `hermes logs gateway`, `hermes logs errors`, `hermes logs agent` (add `-n 50` or `-f`) |
| Update Hermes | `hermes update`, then `hermes -p stackchan plugins update` and `hermes -p stackchaneval plugins update`, `hermes gateway restart`, and a dry run |
| Update Claude Code | `claude update` (Opus 5.5 needs Claude Code 2.1.280 or newer) |
| Robot got a new address | Change `ROBOT_IP` in the `r*.conf` files. No restart needed |
| Mac got a new address | Change `HERMES_HOOK_HOST` in `secrets.h` and re-upload the sketch |
| Change what the robot is told | Edit the `robot-voice` prompt in `~/.hermes/config.yaml`, restart, dry run. Keep the `<<<`/`>>>` lines that mark the words as a person's request, not instructions |
| Changing the model | Edit `model:` in `~/.hermes/profiles/stackchan/config.yaml`. First try the new model in `stackchaneval` with `run-eval.py`: faster models have taken photos nobody asked for |
| Stop everything now | Hold the robot's screen (QUIET mode), or `hermes gateway stop` (`hermes gateway start` to resume) |

### When something goes wrong

| You see | Likely cause | Fix |
|---|---|---|
| Robot shows `TALK: hook failed` or `mac not found` | Gateway not running, or `HERMES_HOOK_HOST` wrong | `hermes gateway status`; check the Mac's address |
| Robot shows THINKING but nothing happens | Model or tool error | `hermes logs errors -n 20` and the end of `tools.log` |
| `tools.log`: `hear_clip-early busy` | A long tool call (a photo) was running | Nothing: the model fetches the words itself |
| `tools.log`: `hear_clip error` | The Mac cannot reach the robot | Check `ROBOT_IP`, run `r1-status.sh` |
| `Keychain entry ... not found` over SSH | macOS keeps the login Keychain from SSH sessions | Run the command in Terminal on the Mac itself |
| `does not support this model` | Claude Code too old | `claude update`, then `hermes gateway restart` |
| `model is cloud-backed; refusing to send the image` | `VISION_MODEL` names a cloud model | Use a local Ollama model |
| Talk always runs to 8 s | Very loud room | Firmware v0.15.0 or later adapts; `/status` shows `voice_floor_rms` |

## Safety and privacy design

- **The firmware enforces the limits, not the AI:**
  - head yaw −30°…+30° and pitch 10°…70°
  - at most 30° per move, with a 3 s cooldown
  - servo power only during a move
  - stall and timeout aborts
  - recordings at most 8 s with a beep and banner, and a 10 s cooldown
- **Visible and audible cues:** every recording or capture is shown on the screen (MIC REC, CAMERA, LIVE).
- **Authentication:** every robot endpoint needs the token. Webhooks need an HMAC-SHA256 signature
  with a timestamp (±5 min replay window), and the gateway rate-limits them.
- **Audio and photos stay local.** A voice clip is handed out once and then wiped on the robot,
  and an unclaimed clip is wiped after 60 s. Transcription and image description run on the Mac,
  and temporary files are deleted.
- **What leaves the LAN:** only text (transcripts and image descriptions) goes to the model provider,
  and only if that provider is in the cloud.
- **Tool server limits:** it allows 30 calls per 10 minutes and logs only time, tool name
  and result, never the words.
- **Consent:** the robot has a microphone and camera. Tell people nearby, and use QUIET mode.

## Roadmap

- Faster replies (with Opus now about 6.5–8 s from the webhook to the first robot action, most of it one model call, plus the recording, which ends 1.2 s after you stop speaking)
- Running the `stackchan` profile on a local model (Ollama) so no text leaves the Mac

## Acknowledgements

- `STACKCHAN-AI-HELPER-START-HERE.md`, the community starter guide by Eric and Tycho, which this
  project followed
- Developed with help from Claude Code
- StackChan was originally created by Shinya Ishikawa. M5Stack produces the CoreS3 version.

## License

[MIT](LICENSE)
