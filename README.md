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
2. **Hermes gateway** receives the webhook, checks the signature and timestamp, and starts a
   run on a dedicated `stackchan` profile whose *only* tools are the robot tools.
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
- Python 3 (standard library only)
- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) (`whisper-server`, kept running on 127.0.0.1 by the
  tool server, and `whisper-cli` as the fallback) and the `ggml-base.en.bin` model
  in `R5_HermesListen/models/` (not in the repository: about 141 MB)
- [Ollama](https://ollama.com/) with `gemma3`, only for "what do you see?"
- A model provider for the Hermes profile. The robot now runs on `minimax-m3.1-flash:cloud`
  through Ollama (`provider: custom`, `base_url: http://localhost:11434/v1`), which answered
  about twice as fast as Claude Opus in our tests. Development used Claude Opus. Nothing in the
  robot or tools depends on the model, as long as it supports tool calls.

## Setup outline

This is an outline, not a step-by-step tutorial. Read `SETUP-STATUS.md` for the full record.

1. **Firmware secrets.** In `R7_HermesTalk/`, copy `secrets.h.example` to `secrets.h` and fill in
   Wi-Fi, a long random token (`openssl rand -hex 24`), the Mac's `.local` name and the webhook
   secret. `secrets.h` is git-ignored. Never commit it.
2. **Keychain.** Store the token as generic password `stackchan-r1-token` and the webhook secret as
   `stackchan-webhook-secret`. Scripts read them at run time and never print them.
3. **Robot address.** Set `ROBOT_IP` in each folder's `*.conf` (e.g. `R7_HermesTalk/r7.conf`).
4. **Upload** `R7_HermesTalk/R7_HermesTalk.ino` with the Arduino IDE.
5. **Hermes profile.** Create a `stackchan` profile whose only tool server is
   `stackchan_tools.py` (memory off):

   ```yaml
   mcp_servers:
     stackchan:
       command: /path/to/python3
       args: [-I, /path/to/R7_HermesTalk/stackchan_tools.py]
   tools:
     tool_search:
       enabled: "off"   # list the 6 robot tools up front: saves a model round trip per turn
   ```

6. **Webhook routes.** Add the routes to the gateway config (`~/.hermes/config.yaml`), with the
   secret itself kept in `~/.hermes/.env` as `STACKCHAN_WEBHOOK_SECRET`:

   ```yaml
   platforms:
     webhook:
       enabled: true
       extra:
         port: 8644
         rate_limit: 6
         max_body_bytes: 4096
         routes:
           robot-voice:
             profile: stackchan
             secret: ${STACKCHAN_WEBHOOK_SECRET}
             toolsets: [stackchan]
             deliver: log
             prompt: |
               Someone just touched the StackChan robot's head and spoke. Call hear_clip with
               clip {clip} to get their words. If hear_clip also shows earlier exchanges, use
               them to understand follow-ups. Then respond through the robot tools only ...
   ```

   Then `hermes gateway restart`. A `robot-text` route with a `{text}` prompt is handy for typed
   tests with `r7-hook.py`.
7. **Test** with `python3 -I R7_HermesTalk/r7-hook.py "look left"`, then tap the robot's head.

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

- Faster replies (now about 3.5 s from the webhook to the first robot action, plus the recording, which ends 1.2 s after you stop speaking)
- Running the `stackchan` profile on a local model (Ollama) so no text leaves the Mac

## Acknowledgements

- `STACKCHAN-AI-HELPER-START-HERE.md`, the community starter guide by Eric and Tycho, which this
  project followed
- Developed with help from Claude Code
- StackChan was originally created by Shinya Ishikawa. M5Stack produces the CoreS3 version.

## License

[MIT](LICENSE)
