/*
 * R7_HermesTalk — R6 (status, greeting, head movement, camera, microphone, head taps, QUIET)
 * plus a sprite-based screen, a camera view on the robot's own screen (R7a) and
 * hold-to-talk with Hermes (R7b).
 *
 * v0.17.0, changes from v0.16.0 (settings on the robot, for prebuilt release firmware):
 *   - Wi-Fi, token, hook host and hook secret can live in the robot's settings storage (NVS
 *     namespace "hermes"), written over the USB cable by tools/stackchan-setup.py on the
 *     Mac. Saved settings win; secrets.h is now optional and used only when nothing is saved.
 *     The servo calibration ("servo" namespace) is never touched.
 *   - USB serial setup, one command per line, replies start with "@R7 ":
 *       HELLO, STATUS, SET <key> <value>, SAVE, CLEAR, REBOOT
 *     keys: ssid, pass, token, hook_host, hook_secret. SET only stages a value; SAVE checks
 *     all five and stores them together; they apply after REBOOT. Nothing ever prints the
 *     password, token or secret (STATUS gives "set"/"missing" and lengths only).
 *   - Screen: "SETUP NEEDED" when nothing is set; /status: "settings" (saved/secrets.h/none)
 *
 * v0.16.0, changes from v0.15.0 (the Mac transcribes before the model starts):
 *   - The talk webhook is sent from its own task, so the robot keeps answering requests while
 *     it waits: the gateway's route script now fetches the clip (POST /voice/take) and has it
 *     transcribed before it answers the webhook. Before, the robot could not serve the clip
 *     until the webhook reply came, so the two would have waited on each other.
 *   - HOOK_TIMEOUT_MS 10 s (was 3 s): the reply now comes after the transcript (~2 s)
 *   - THINKING shows from the moment the webhook is sent; a failed webhook ends it with the
 *     usual notice. A clip already taken by the Mac is not reported as "sent" afterwards.
 *
 * v0.15.0, changes from v0.14.0 (talk in a noisy room):
 *   - The speech line adapts to the room: a 100 ms piece is speech at rms >= the larger of
 *     VOICE_SPEECH_RMS (200) and VOICE_FLOOR_X (2.5) times the quietest piece so far (the
 *     room's noise floor). Air conditioning at rms ~160-230 kept every clip going to 8 s.
 *     Pieces are re-judged as the floor drops, so words said before the first pause still
 *     count. In a quiet room (floor ~17) the line stays 200, as in v0.14.0.
 *   - /status: voice_floor_rms (quietest piece of the last talk recording)
 *
 * v0.14.0, changes from v0.13.0 (idle life; robot-only, no camera, mic, speech or network):
 *   - After FACE_AFTER_MS (1 min) with nothing happening, the status screen gives way to a
 *     simple face that blinks every few seconds; a short tap on the screen (or any request,
 *     talk or banner) brings the status screen back for another minute
 *   - After GLANCE_AFTER_MS (2 min) idle, every 3-6 min one small glance: 8° left or right
 *     or 5° up from where the head is, held GLANCE_HOLD_MS, then back. Same limits, stall
 *     check and servo power-off as any move (run_move); the eyes look the same way.
 *   - Never while QUIET, camera view, talk, THINKING, speaking or a banner; no glances at
 *     night (NIGHT_FROM_H-NIGHT_TO_H local time, ROBOT_TZ; none without NTP time), when the
 *     face sleeps (eyes closed)
 *   - POST /idle?on=0|1 (token) turns idle life off or on (on after each restart);
 *     /status: idle, face, idle_glances
 *   - Local time: configTzTime(ROBOT_TZ) instead of configTime(0, 0); unix time (webhook
 *     timestamps) is unchanged
 *
 * v0.13.0, changes from v0.12.1:
 *   - POST /camera/snap?describe=1 (sent by r34-look.sh, which has the photo described by
 *     gemma3 on the Mac): after the photo is sent, a blue "ANALYZING IMAGE..." banner shows
 *     until the robot speaks, moves or shows the camera view, or PHOTO_THINK_MAX_MS (90 s).
 *     A plain /camera/snap (r4-snap.sh, save only) shows no banner, as before.
 *
 * v0.12.1, changes from v0.12.0:
 *   - A clip ends after 1.2 s of quiet (VOICE_END_QUIET_CHUNKS 12, was 8): a 0.8 s thinking
 *     pause mid-sentence cut a question short in the v0.12.0 robot test
 *
 * v0.12.0, changes from v0.11.0:
 *   - Talk stops recording when the person stops speaking: the clip is recorded in 100 ms
 *     pieces (VOICE_CHUNK) and ends after VOICE_END_QUIET_CHUNKS quiet pieces (0.8 s) that
 *     follow speech, at the earliest after VOICE_MIN_CHUNKS (1 s). No speech within
 *     VOICE_NO_SPEECH_CHUNKS (3 s) = "heard nothing", nothing sent. At most VOICE_MAX_SECS
 *     (8 s, was a fixed 5 s). A piece counts as speech at rms >= VOICE_SPEECH_RMS.
 *   - Clips are variable length; /voice/take returns just the recorded part
 *   - /status: voice_len_ms, voice_end (quiet / max / no speech), voice_loud_rms (loudest
 *     piece; signal level only, for tuning VOICE_SPEECH_RMS)
 *   - /mic/record is unchanged (fixed length)
 *
 * v0.11.0, changes from v0.10.0:
 *   - A tap on the head now starts talk exactly like a hold (beep, record, webhook); the
 *     look-up tap gesture, the Mac tap flag and GET /events + POST /events/ack are removed,
 *     so the R7a listener r7-talk.py no longer works with this sketch (use v0.10.0 for it)
 *   - /status drops tap_seq, tap_pending, gestures_done, gestures_skipped
 *
 * v0.10.0 (R7b), changes from v0.9.0:
 *   - Hold the head touch sensor for HOLD_MS (1 s): the robot beeps, records VOICE_SECS (5 s)
 *     with the MIC REC banner, keeps the clip in PSRAM and sends ONE signed webhook
 *     {"clip":N} to the Hermes gateway on the Mac (HERMES_HOOK_HOST, port 8644). Hermes then
 *     fetches the clip with POST /voice/take (token), which returns it once and wipes it.
 *     A clip nobody takes is wiped after VOICE_KEEP_MS (60 s); turning QUIET on wipes it too.
 *     A near-silent clip (rms < VOICE_MIN_RMS) is wiped at once and not sent.
 *   - Shorter touches are taps exactly as before (Mac listener flag + look-up gesture)
 *   - Webhook signing: generic HMAC V2 as the gateway expects, X-Webhook-Timestamp (unix s,
 *     from NTP) and X-Webhook-Signature-V2 = hex HMAC-SHA256(secret, "<ts>.<body>"). No clock,
 *     no talk. The webhook carries only the clip number, never audio or text.
 *   - Blue THINKING banner from the webhook until the robot speaks, moves or shows the
 *     camera, or THINK_MAX_MS (45 s); short maroon notices when talk is refused or fails
 *   - The robot now starts contact with the Mac, but only this one webhook, and only after
 *     someone holds its head
 *   - /status: talk counters, clock_ok, hook_ok; the mDNS name stackchan.local is advertised
 *
 * v0.9.0 (R7a-1), changes from R6_HermesTouch v0.8.1:
 *   - Screen is composed from sprites (PSRAM canvases): a status layer or the camera layer,
 *     then banners (CAMERA, MIC REC, LIVE) on top, pushed in one go (no flicker)
 *   - POST /camera/view?on=1|0 shows the camera on the robot's screen only (red LIVE frame
 *     with a countdown); switched off after 2 minutes, by QUIET, or by a bus fault
 *   - Head-touch chip health (touch_ok) in /status, and a baseline recalibration at start-up
 *     and after each camera-view start/stop (R6c saw a whole boot with no taps)
 *
 * Builds on the tested R6_HermesTouch v0.8.1 (see ../SETUP-STATUS.md).
 *
 * Endpoints (all require header "Authorization: Bearer <token>"):
 *   GET  /status                       — status, counters, bus health, quiet/tap state
 *   POST /say                          — one short WAV, played once, quietly (as R2/R3)
 *   POST /head/read                    — power servos briefly and report angles; no movement
 *   POST /head/move?yaw=<n>&pitch=<n>  — one move, angles in tenths of a degree (as R3 v0.4.0)
 *   POST /camera/snap                  — capture one 320x240 frame, returned as JPEG (as R4)
 *   POST /camera/view?on=<1|0>         — camera view on the robot's screen on/off (R7a)
 *   POST /mic/record?secs=<1..8>       — record once, returned as 16 kHz mono 16-bit WAV (as R5)
 *   POST /voice/take?clip=<n>          — the held-to-talk clip n as WAV, once; then wiped (R7b)
 *
 * Touch:
 *   - A tap or hold on the head touch sensor starts talk (see v0.11.0 / v0.10.0 above); the
 *     10 s recording cooldown spaces talk turns
 *   - QUIET mode: press and hold the screen to toggle. While quiet, head touches are ignored and
 *     /say, /head/move, /camera/snap and /mic/record are refused (HTTP 423); a grey QUIET
 *     banner is shown. QUIET is off after a restart.
 *
 * Microphone use:
 *   - Records only during a /mic/record request, at most 8 s; nothing is stored on the
 *     robot, streamed, or sent anywhere except the HTTP reply
 *   - Before recording: red "MIC REC" banner and a short quiet beep; the banner counts
 *     down while recording; 10 s cooldown between recordings
 *   - Speaker and microphone share clock pins (BCK 34 / WS 33), so the speaker is stopped
 *     for the recording and restarted afterwards (as in M5Unified's Microphone example)
 *   - "Off": after recording, Mic.end() resets the ES7210 codec and powers down its inputs
 *   - The reply carries X-Peak and X-Rms headers (signal level only), so a recording can
 *     be checked for "not silent" without anyone listening to it
 *
 * Motion (as R3 v0.4.0): each move is ONE position command per servo with a travel time;
 * the library still handles power, torque and the current sensor, its motion task is kept
 * off the servo bus, and this sketch shares the library's servo UART.
 *
 * Movement safety (see move_rules.h):
 *   - yaw -30°..+30°, pitch 10°..70° (M5Stack's documented safe pitch range is ~5°–85°)
 *   - at most 30° per axis per move, travel time MOVE_MS_PER_DEG per degree, 3 s cooldown
 *   - servo power is ON only during a read or move, then torque off and power OFF
 *   - refuses to move if the servos report motion before any command, if a position
 *     read fails, or if the servo calibration (zero positions) is missing
 *   - aborts on a 4 s timeout or a sustained current spike by cutting servo power
 *     (best effort: on USB power the current sensor may not see servo load)
 *   - never calls goHome() (pitch 0°) or continuous rotation
 *
 * Camera (as R4): no frames except during a /camera/snap request or while the camera view
 * is on; nothing stored on the robot or streamed off it; red CAMERA banner during a
 * capture; 10 s cooldown between captures.
 *
 * Camera view (R7a): frames go only to the robot's screen, at most CAM_VIEW_FPS a second,
 * inside a red LIVE frame with a countdown. Off after CAM_VIEW_MAX_MS, when QUIET is turned
 * on, or after a bus fault; off after a restart. Two modes (CAM_VIEW_STREAM):
 *   stream: the camera is set up once (control bus borrowed as below), then its control
 *           bus driver is removed with SCCB_Deinit() and the internal I2C taken back while
 *           frames keep arriving on the separate parallel data pins. The sensor runs its
 *           own exposure, so nothing else is sent to it. Taps, mic and moves keep working.
 *   slow:   fallback, one full hand-over (as for a snapshot) per frame, about 1 a second;
 *           taps are noticed between frames only.
 * A /camera/snap while streaming uses the current frame, without a hand-over.
 *
 * Shared bus: the camera's control bus and M5Unified's internal I2C bus (servo POWER
 * switch, battery/current monitor, touch, speaker amplifier, power management) are the
 * same pins and I2C port, so they take turns:
 *   servo power off -> release internal I2C -> camera init -> one frame ->
 *   camera deinit -> re-take internal I2C -> health check (battery reading)
 * Requests are handled one at a time, so a capture never overlaps a move or head read.
 * A capture is also refused while a clip is playing. If the health check fails, BOTH the
 * camera and movement are disabled until restart, since servo power could no longer be
 * switched reliably.
 *
 * Not used: LEDs, IR, NFC.
 *
 * Setup:
 *   1. Settings: EITHER run tools/stackchan-setup.py after uploading (stores them on the
 *      robot), OR copy secrets.h.example to secrets.h in this folder and fill it in.
 *   2. Board: Espressif "esp32" package -> "M5CoreS3", PSRAM enabled, USB CDC On Boot
 *      enabled (all the defaults).
 *   3. Upload with the Arduino IDE, or flash a release build with esptool (RELEASE.md).
 *
 * Mac scripts (unchanged, same IP/port): ../R3_HermesMove/r3-head.sh,
 * ../R2_HermesGreeting/r2-say.sh, ../R4_HermesCamera/r4-snap.sh and r4-describe.sh.
 *
 * Camera pins and settings follow M5Stack's official M5CoreS3 library
 * (src/utility/GC0308.cpp, MIT licence).
 *
 * Libraries: M5StackChan 1.1.1, M5Unified, M5GFX; esp32-camera ships with the esp32 core.
 * Recovery: re-upload R6_HermesTouch or any sketch from your sketchbook.
 */
#include <Arduino.h>
#include <M5StackChan.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <mbedtls/md.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <drivers/FTServo_Arduino/src/SCSCL.h>
#include <utils/settings/settings.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <nvs.h>
#include "move_rules.h"
#include "move_outcome.h"
#include "robot_config.h"

/* v0.17.0: secrets.h is optional; settings saved on the robot win over it */
#if __has_include("secrets.h")
#include "secrets.h"
#if !defined(HERMES_HOOK_HOST) || !defined(HERMES_HOOK_SECRET)
#error "secrets.h needs HERMES_HOOK_HOST and HERMES_HOOK_SECRET for R7b hold-to-talk (see the header comment)"
#endif
#define HAVE_SECRETS_H 1
#else
#define HAVE_SECRETS_H 0
#endif

/* Exported by the esp32-camera component (sccb.h is not a public header). Called a second
 * time by esp_camera_deinit(), which is then a no-op because the driver no longer owns
 * the I2C port (checked by disassembling the library in esp32s3-libs 3.3.12). */
extern "C" int SCCB_Deinit(void);

static const char* SKETCH_NAME      = "R7_HermesTalk";
static const char* SKETCH_VERSION   = "0.17.0";
static const uint16_t HTTP_PORT     = 8080;
static const size_t MIN_TOKEN_LEN   = 16;
static const size_t MAX_WAV_BYTES   = 192 * 1024;
static const uint32_t MAX_WAV_MS    = 6000;
static const uint32_t COOLDOWN_MS   = 5000;
static const uint8_t SPEAKER_VOLUME = 128;  // of 255 (v0.8.1: 64 lost soft word endings)

static const uint32_t MOVE_MS_PER_DEG    = 40;    // travel time per degree of the larger axis change
static const uint32_t MOVE_MIN_MS        = 300;   // so 10° takes 400 ms and 30° takes 1200 ms
static const uint32_t SERVO_SETTLE_MS    = 300;   // after power on, before the first check
static const uint32_t SERVO_READY_MS     = 2000;  // give up if not quiet by then
static const uint32_t SERVO_POLL_MS      = 100;
static const int SERVO_QUIET_CHECKS      = 3;     // consecutive "not moving" answers needed
static const uint32_t MOVE_TIMEOUT_MS    = 4000;
static const uint32_t MOVE_HOLD_MS       = 300;   // after arrival, before torque off
static const uint32_t MOVE_COOLDOWN_MS   = 3000;

/* Servo bus details, as set up by M5StackChan 1.1.1 (M5StackChan.cpp servo_init) */
static const uart_port_t SERVO_UART = UART_NUM_1;
static const uint8_t YAW_ID         = 1;
static const uint8_t PITCH_ID       = 2;
static const int RAW_MIN            = 0;
static const int RAW_MAX            = 1000;
static const float STALL_DELTA_MA        = 1500.0f;
static const uint32_t STALL_HOLD_MS      = 200;

WebServer server(HTTP_PORT);

static bool token_ok              = false;
static uint32_t requests_served   = 0;
static uint32_t requests_rejected = 0;
static uint32_t clips_spoken      = 0;
static uint32_t moves_done        = 0;
static uint32_t last_draw_ms      = 0;
static uint32_t last_play_ms      = 0;
static bool has_played            = false;
static uint32_t last_move_end_ms  = 0;
static bool has_moved             = false;

static const uint32_t SNAP_COOLDOWN_MS = 10000;
static const int WARMUP_FRAMES         = 3;     // let auto exposure settle; these are discarded
static const uint8_t JPEG_QUALITY      = 80;    // 0..100
static const float BATTERY_V_MIN       = 2.5f;  // health check: a sane reading means the bus works
static const float BATTERY_V_MAX       = 4.6f;

static uint32_t photos_taken  = 0;
static uint32_t camera_errors = 0;
static uint32_t last_snap_ms  = 0;
static bool has_snapped       = false;
static bool bus_failed        = false;  // internal bus did not come back after a capture

static const bool CAM_VIEW_STREAM      = true;    // false: slow fallback, one hand-over per frame
static const uint32_t CAM_VIEW_MAX_MS  = 120000;  // camera view switches itself off after 2 minutes
static const uint32_t CAM_VIEW_FPS     = 10;      // stream mode; drawing a frame takes ~40 ms
static const uint32_t CAM_SLOW_GAP_MS  = 1000;    // slow mode: pause between hand-overs
static const int CAM_VIEW_MAX_MISSES   = 3;       // consecutive missing frames before giving up
                                                  // (each miss can block ~4 s in the driver)
static const int LIVE_BORDER           = 4;

static bool cam_view           = false;  // camera view on screen
static bool cam_streaming      = false;  // camera driver running (stream mode only)
static uint32_t cam_view_start_ms = 0;
static uint32_t last_frame_ms  = 0;
static int frame_misses        = 0;
static uint32_t views_started  = 0;
static uint32_t view_frames    = 0;
static const char* view_end_reason = "none";

/* Screen layers (R7a): status or camera below, banners on top, composed in screen_spr */
static M5Canvas status_spr;
static M5Canvas cam_spr;
static M5Canvas screen_spr;
static bool screen_ok        = false;  // all three canvases allocated
static bool banner_on        = false;
static uint16_t banner_bg    = TFT_RED;
static char banner_text[32]  = "";

static bool touch_ok          = false;  // head-touch chip answered on the internal bus
static uint32_t touch_recals  = 0;

static const uint32_t MIC_RATE        = 16000;
static const int MIC_MAX_SECS         = 8;
static const size_t MIC_WARMUP        = 2400;  // 150 ms recorded first and discarded (codec settling)
static const uint32_t REC_COOLDOWN_MS = 10000;
static const float CUE_HZ             = 880.0f;
static const uint32_t CUE_MS          = 200;
static const size_t WAV_HEADER_BYTES  = 44;

static int16_t* rec_buf          = nullptr;  // PSRAM: warm-up + up to MIC_MAX_SECS of samples
static uint32_t recordings_made  = 0;
static uint32_t mic_errors       = 0;
static uint32_t last_rec_ms      = 0;
static bool has_recorded         = false;

static bool quiet               = false;
static uint32_t taps_ignored    = 0;  // head touches ignored while QUIET

/* Hold-to-talk (R7b) */
static const uint32_t HOLD_MS          = 1000;   // a hold reports once at 1 s, a tap on release; both talk
static const int VOICE_MAX_SECS        = MIC_MAX_SECS;  // 8 s
static const size_t VOICE_WAV_BYTES    = WAV_HEADER_BYTES + (size_t)VOICE_MAX_SECS * MIC_RATE * 2;
static const size_t VOICE_CHUNK        = MIC_RATE / 10;  // 100 ms pieces, checked as they arrive
static const int VOICE_SPEECH_RMS      = 200;    // a piece this loud is speech (room ~17, speech ~1300 average)
static const int VOICE_FLOOR_X         = 25;     // ...and at least 2.5x (tenths) the quietest piece (v0.15.0)
static const size_t VOICE_END_QUIET_CHUNKS = 12; // 1.2 s quiet after speech ends the clip
static const size_t VOICE_MIN_CHUNKS   = 10;     // never shorter than 1 s
static const size_t VOICE_NO_SPEECH_CHUNKS = 30; // no speech in the first 3 s = nothing said
static const uint32_t VOICE_KEEP_MS    = 60000;  // a clip nobody took is wiped after this
static const uint32_t THINK_MAX_MS     = 45000;
static const uint32_t PHOTO_THINK_MAX_MS = 90000; // gemma3 on the Mac takes ~40 s per photo
static const uint32_t NOTICE_MS        = 2500;
static const uint16_t HOOK_PORT        = 8644;
static const char* HOOK_PATH           = "/p/stackchan/webhooks/robot-voice";
static const uint32_t HOOK_TIMEOUT_MS  = 10000;  // reply comes after the Mac's transcript (v0.16.0)
static const uint32_t HOOK_TASK_STACK  = 8192;
static const size_t MIN_HOOK_SECRET    = 32;
static const time_t CLOCK_VALID_AFTER  = 1700000000;  // NTP has set the clock (Nov 2023)

static uint8_t* voice_buf      = nullptr;  // PSRAM: WAV header + up to VOICE_MAX_SECS of samples
static size_t voice_bytes      = 0;        // WAV length of the waiting clip
static bool voice_ready        = false;    // a clip is waiting for /voice/take
static uint32_t voice_seq      = 0;
static uint32_t voice_ms       = 0;
static int32_t voice_peak      = 0;
static int voice_rms           = 0;
static uint32_t voices_sent    = 0;
static uint32_t voices_failed  = 0;
static uint32_t voices_taken   = 0;
static uint32_t voices_expired = 0;
static uint32_t voices_silent  = 0;
static const char* voice_last  = "none";
static uint32_t voice_len_ms   = 0;        // length of the last talk recording
static const char* voice_end   = "none";   // why it ended: quiet / max / no speech
static int voice_loud_rms      = 0;        // loudest 100 ms piece of the last talk recording
static int voice_floor_rms     = 0;        // quietest 100 ms piece of the last talk recording
static int last_hook_code      = 0;
static bool hook_ok            = false;    // secret and host set (saved settings or secrets.h)
static IPAddress hook_ip;                  // resolved hook host; 0.0.0.0 = not yet
static uint32_t boot_id        = 0;        // random per boot, part of each webhook's X-Request-ID
static bool thinking           = false;
static volatile bool hook_pending = false;  // the webhook task is running (v0.16.0)
static volatile bool hook_done    = false;  // ...and has finished; hook_code holds its result
static volatile int hook_code     = 0;
static uint32_t hook_seq          = 0;      // clip number the task is announcing
static uint32_t think_start_ms = 0;
static uint32_t think_mark     = 0;        // activity count when thinking began
static uint32_t think_max_ms   = THINK_MAX_MS; // PHOTO_THINK_MAX_MS while an image is analyzed
static uint32_t notice_until_ms = 0;       // 0 = no notice banner showing

/* Idle life (v0.14.0): face and small glances while nothing else is happening */
static const uint32_t FACE_AFTER_MS     = 60000;
static const uint32_t GLANCE_AFTER_MS   = 120000;
static const uint32_t GLANCE_GAP_MIN_MS = 180000;  // 3..6 min between glances
static const uint32_t GLANCE_GAP_MAX_MS = 360000;
static const uint32_t GLANCE_HOLD_MS    = 2500;    // >= MOVE_COOLDOWN_MS
static const int GLANCE_YAW             = 80;      // tenths of a degree
static const int GLANCE_PITCH           = 50;
static const int NIGHT_FROM_H           = 22;      // local hours with no glances, face asleep
static const int NIGHT_TO_H             = 8;
static const char* ROBOT_TZ             = "EST5EDT,M3.2.0,M11.1.0";  // POSIX TZ: US Eastern
static const uint32_t BLINK_MS          = 150;

static bool idle_on            = true;
static uint32_t idle_sig       = 0;        // activity counters when idle began
static uint32_t idle_since_ms  = 0;
static uint32_t next_glance_ms = 0;        // 0 = none planned yet
static uint32_t idle_glances   = 0;
static bool glance_out         = false;    // head is away from glance_home
static uint32_t glance_back_ms = 0;
static int glance_home_yaw     = 0;
static int glance_home_pitch   = 0;
static bool face_shown         = false;
static bool face_asleep        = false;
static bool eyes_closed        = false;
static uint32_t blink_ms       = 0;        // next blink start or end
static int eye_dx              = 0;        // -1 left, 1 right (as the robot sees it)
static int eye_dy              = 0;        // -1 up

static int last_yaw     = 0;
static int last_pitch   = 0;
static bool angles_known = false;

/*
 * Shares the UART the library already installed (a second begin() would fail),
 * so this object only needs to know the port number.
 */
class SharedScsBus : public SCSCL {
public:
    void attach(uart_port_t port) { uart_num = port; }
};
static SharedScsBus bus;

/* Calibrated raw position of 0° for each servo, read from the library's NVS settings */
static int yaw_zero   = -1;
static int pitch_zero = -1;
static bool zero_ok   = false;

static uint8_t* wav_buf = nullptr;
static size_t wav_len   = 0;

/* State of the current /say upload, decided when the upload starts */
enum class SayState { None, Accepting, Unauthorized, Busy, Cooldown, NoBuffer, TooLarge, Aborted };
static SayState say_state = SayState::None;

/* ---------- auth ---------- */

/* Constant-time comparison so response timing does not leak the token */
static bool tokens_equal(const String& given, const char* expected)
{
    size_t len = strlen(expected);
    if (given.length() != len) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= (uint8_t)given[i] ^ (uint8_t)expected[i];
    }
    return diff == 0;
}

/* ---- v0.17.0: settings (saved on the robot, or secrets.h) and the USB setup protocol ---- */

static const char* CFG_NS = "hermes";  // NVS namespace; the servo calibration lives in "servo"
static RobotConfig cfg;                  // in use since boot
static RobotConfig cfg_staged;           // SET values waiting for SAVE
static bool cfg_staged_set[CFG_COUNT] = {};      // which CFG_FIELDS have been SET since the last SAVE/CLEAR
static const char* cfg_source = "none";  // "saved", "secrets.h" or "none"
static char serial_line[300];
static size_t serial_len      = 0;
static bool serial_overflow   = false;

static char* cfg_field(RobotConfig& c, const CfgField& f)
{
    return (char*)&c + f.offset;
}

static bool printable_no_space(const char* v)
{
    for (; *v; v++) {
        if (*v <= ' ' || *v > '~') {
            return false;
        }
    }
    return true;
}

/* nullptr if the value may be stored under this key, else why not */
static const char* cfg_value_problem(const char* key, const char* v)
{
    size_t n = strlen(v);
    if (!strcmp(key, "ssid")) {
        if (n < 1 || n > 32) return "ssid must be 1-32 bytes";
        for (const char* p = v; *p; p++) {
            if ((unsigned char)*p < ' ' || *p == 0x7f) return "ssid has a control character";
        }
        return nullptr;
    }
    if (!strcmp(key, "pass")) {
        if (n != 0 && (n < 8 || n > 64)) return "pass must be empty (open network) or 8-64 characters";
        for (const char* p = v; *p; p++) {
            if (*p < ' ' || *p > '~') return "pass must be printable ASCII";
        }
        return nullptr;
    }
    if (!strcmp(key, "token")) {
        if (n < MIN_TOKEN_LEN || n > 128) return "token must be 16-128 characters";
        return printable_no_space(v) ? nullptr : "token must be printable ASCII without spaces";
    }
    if (!strcmp(key, "hook_host")) {
        if (n < 1 || n > 63) return "hook_host must be 1-63 characters";
        for (const char* p = v; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return "hook_host: letters, digits, '.', '-' only";
        }
        return nullptr;
    }
    if (!strcmp(key, "hook_secret")) {
        if (n < MIN_HOOK_SECRET || n > 128) return "hook_secret must be 32-128 characters";
        return printable_no_space(v) ? nullptr : "hook_secret must be printable ASCII without spaces";
    }
    return "unknown key";
}

/* Reads the "hermes" namespace; false if it is missing or any value is absent or bad */
static bool cfg_load_saved(RobotConfig& c)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = true;
    for (const CfgField& f : CFG_FIELDS) {
        size_t len = f.size;
        char* dst  = cfg_field(c, f);
        if (nvs_get_str(h, f.key, dst, &len) != ESP_OK || cfg_value_problem(f.key, dst)) {
            ok = false;
        }
    }
    nvs_close(h);
    if (!ok) {
        memset(&c, 0, sizeof(c));
    }
    return ok;
}

static void cfg_load()
{
    memset(&cfg, 0, sizeof(cfg));
    if (cfg_load_saved(cfg)) {
        cfg_source = "saved";
    } else {
#if HAVE_SECRETS_H
        strlcpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid));
        strlcpy(cfg.pass, WIFI_PASSWORD, sizeof(cfg.pass));
        strlcpy(cfg.token, HERMES_STATUS_TOKEN, sizeof(cfg.token));
        strlcpy(cfg.hook_host, HERMES_HOOK_HOST, sizeof(cfg.hook_host));
        strlcpy(cfg.hook_secret, HERMES_HOOK_SECRET, sizeof(cfg.hook_secret));
        cfg_source = "secrets.h";
        if (!strcmp(cfg.token, "REPLACE_WITH_A_LONG_RANDOM_TOKEN")) cfg.token[0] = '\0';
        if (!strcmp(cfg.hook_secret, "REPLACE_WITH_THE_GATEWAY_ROUTE_SECRET")) cfg.hook_secret[0] = '\0';
        if (!strcmp(cfg.ssid, "your-home-wifi-name")) cfg.ssid[0] = '\0';
#else
        cfg_source = "none";
#endif
    }
    token_ok = !cfg_value_problem("token", cfg.token);
    hook_ok  = !cfg_value_problem("hook_secret", cfg.hook_secret) && !cfg_value_problem("hook_host", cfg.hook_host);
}

static bool setup_needed()
{
    return !token_ok || cfg.ssid[0] == '\0';
}

static void cfg_unstage()
{
    memset(&cfg_staged, 0, sizeof(cfg_staged));
    memset(cfg_staged_set, 0, sizeof(cfg_staged_set));
}

/* Writes all five staged values in one NVS commit; nullptr on success */
static const char* cfg_save_staged()
{
    for (size_t i = 0; i < CFG_COUNT; i++) {
        if (!cfg_staged_set[i]) {
            return "SET all five keys first (ssid, pass, token, hook_host, hook_secret)";
        }
        const char* why = cfg_value_problem(CFG_FIELDS[i].key, cfg_field(cfg_staged, CFG_FIELDS[i]));
        if (why) {
            return why;
        }
    }
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return "settings storage cannot be opened";
    }
    esp_err_t err = ESP_OK;
    for (const CfgField& f : CFG_FIELDS) {
        if (err == ESP_OK) {
            err = nvs_set_str(h, f.key, cfg_field(cfg_staged, f));
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? nullptr : "settings storage write failed";
}

static const char* cfg_clear_saved()
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return nullptr;  // nothing saved
    }
    esp_err_t err = nvs_erase_all(h);  // this namespace only
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? nullptr : "settings storage erase failed";
}

static void serial_status()
{
    Serial.printf("@R7 OK STATUS sketch=%s version=%s settings=%s", SKETCH_NAME, SKETCH_VERSION, cfg_source);
    for (const CfgField& f : CFG_FIELDS) {
        const char* v = cfg_field(cfg, f);
        if (!f.secret && v[0]) {
            Serial.printf(" %s=\"%s\"", f.key, v);
        } else {
            Serial.printf(" %s=%s", f.key, v[0] ? "set" : "missing");
        }
        if (f.secret && v[0]) {
            Serial.printf("(%u)", (unsigned)strlen(v));
        }
    }
    Serial.printf(" token_ok=%d hook_ok=%d wifi=%s ip=%s staged=",
                  token_ok, hook_ok, WiFi.status() == WL_CONNECTED ? "connected" : "not-connected",
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "none");
    bool any = false;
    for (size_t i = 0; i < CFG_COUNT; i++) {
        if (cfg_staged_set[i]) {
            Serial.printf("%s%s", any ? "," : "", CFG_FIELDS[i].key);
            any = true;
        }
    }
    Serial.printf("%s\n", any ? "" : "none");
}

static void serial_command(char* line)
{
    if (!strcmp(line, "HELLO")) {
        Serial.printf("@R7 OK HELLO %s %s setup=1\n", SKETCH_NAME, SKETCH_VERSION);
    } else if (!strcmp(line, "STATUS")) {
        serial_status();
    } else if (!strncmp(line, "SET ", 4)) {
        char* key = line + 4;
        char* val = strchr(key, ' ');
        if (val) {
            *val++ = '\0';  // the value is the rest of the line, spaces included (Wi-Fi names)
        } else {
            val = key + strlen(key);  // "SET pass" with nothing after = empty (open network)
        }
        const CfgField* field = nullptr;
        size_t index          = 0;
        for (size_t i = 0; i < CFG_COUNT; i++) {
            if (!strcmp(CFG_FIELDS[i].key, key)) {
                field = &CFG_FIELDS[i];
                index = i;
            }
        }
        const char* why = field ? cfg_value_problem(key, val) : "unknown key";
        if (why) {
            Serial.printf("@R7 ERR SET %s: %s\n", field ? field->key : "?", why);
        } else {
            strlcpy(cfg_field(cfg_staged, *field), val, field->size);
            cfg_staged_set[index] = true;
            Serial.printf("@R7 OK SET %s\n", field->key);
        }
    } else if (!strcmp(line, "SAVE")) {
        const char* why = cfg_save_staged();
        if (why) {
            Serial.printf("@R7 ERR SAVE %s\n", why);
        } else {
            cfg_unstage();
            Serial.printf("@R7 OK SAVE stored; REBOOT to use\n");
        }
    } else if (!strcmp(line, "CLEAR")) {
        const char* why = cfg_clear_saved();
        cfg_unstage();
        if (why) {
            Serial.printf("@R7 ERR CLEAR %s\n", why);
        } else {
            Serial.printf("@R7 OK CLEAR saved settings erased (servo calibration kept); REBOOT to use\n");
        }
    } else if (!strcmp(line, "REBOOT")) {
        Serial.printf("@R7 OK REBOOT\n");
        Serial.flush();
        delay(200);
        ESP.restart();
    } else if (line[0]) {
        Serial.printf("@R7 ERR unknown command (HELLO, STATUS, SET, SAVE, CLEAR, REBOOT)\n");
    }
}

/* Called from loop(): collects one line at a time from the USB serial port */
static void serial_tick()
{
    while (Serial.available() > 0) {
        int ch = Serial.read();
        if (ch < 0) {
            break;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            serial_line[serial_len] = '\0';
            if (serial_overflow) {
                Serial.printf("@R7 ERR line too long\n");
            } else {
                serial_command(serial_line);
            }
            memset(serial_line, 0, sizeof(serial_line));  // do not leave a secret in RAM longer than needed
            serial_len      = 0;
            serial_overflow = false;
        } else if (serial_len < sizeof(serial_line) - 1) {
            serial_line[serial_len++] = (char)ch;
        } else {
            serial_overflow = true;
        }
    }
}

static bool is_authorized()
{
    if (!token_ok || !server.hasHeader("Authorization")) {
        return false;
    }
    String header = server.header("Authorization");
    if (!header.startsWith("Bearer ")) {
        return false;
    }
    return tokens_equal(header.substring(7), cfg.token);
}

static void send_json_error(int code, const char* msg)
{
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    server.send(code, "application/json", body);
}

/* ---------- WAV validation (unchanged from R2) ---------- */

static uint16_t rd16(const uint8_t* p)
{
    return p[0] | (p[1] << 8);
}

static uint32_t rd32(const uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t* p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/*
 * Validate the whole file against the received length. M5Unified's playWav walks
 * chunks using the sizes inside the file, so a malformed file must never reach it.
 * The walk below mirrors playWav's (no pad bytes) so both find the same data chunk.
 */
static const char* validate_wav(uint8_t* buf, size_t len)
{
    if (len < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVEfmt ", 8)) {
        return "not a RIFF/WAVE file with fmt first";
    }
    uint32_t fmt_size = rd32(buf + 16);
    if (fmt_size < 16 || 20 + (size_t)fmt_size > len) {
        return "bad fmt chunk";
    }
    uint16_t audio_fmt = rd16(buf + 20);
    uint16_t channels  = rd16(buf + 22);
    uint32_t rate      = rd32(buf + 24);
    uint16_t bits      = rd16(buf + 34);
    if (audio_fmt != 1 || channels != 1 || bits != 16 || rate < 8000 || rate > 24000) {
        return "must be PCM mono 16-bit 8000-24000 Hz";
    }

    size_t off = 20 + fmt_size;
    while (true) {
        if (off + 8 > len) {
            return "no data chunk";
        }
        uint32_t size = rd32(buf + off + 4);
        if ((size_t)size > len - off - 8) {
            return "chunk exceeds file";
        }
        if (!memcmp(buf + off, "data", 4)) {
            if (size == 0) {
                return "empty data chunk";
            }
            if ((uint64_t)size * 1000 / ((uint64_t)rate * 2) > MAX_WAV_MS) {
                return "clip too long";
            }
            break;
        }
        off += 8 + size;
    }

    /* Make playWav's own bound match what we actually received */
    wr32(buf + 4, len - 8);
    return nullptr;
}

/* ---------- servo power and head helpers ---------- */

static void servo_power_on()
{
    M5StackChan.setServoPowerEnabled(true);
    delay(SERVO_SETTLE_MS);
}

static void servo_power_off()
{
    M5StackChan.Motion.setTorqueEnabled(false);
    M5StackChan.setServoPowerEnabled(false);
}

/*
 * With nothing commanded, a healthy head reports "not moving". A failed bus read
 * (ReadMove returns -1) shows up as "moving", so this doubles as a link check.
 */
static bool servos_quiet()
{
    return !M5StackChan.Motion.isMoving();
}

/*
 * Servos need a moment to boot after power on and fail reads until then.
 * Poll until they answer "not moving" several times in a row, or give up.
 */
static uint32_t ready_waited_ms = 0;

static bool servos_ready()
{
    uint32_t t0 = millis();
    int quiet = 0;
    while (millis() - t0 < SERVO_READY_MS) {
        quiet = servos_quiet() ? quiet + 1 : 0;
        if (quiet >= SERVO_QUIET_CHECKS) {
            ready_waited_ms = SERVO_SETTLE_MS + (millis() - t0);
            return true;
        }
        delay(SERVO_POLL_MS);
    }
    ready_waited_ms = SERVO_SETTLE_MS + (millis() - t0);
    return false;
}

static void send_not_ready()
{
    char body[160];
    snprintf(body, sizeof(body),
             "{\"error\":\"servos not responding or already moving; power off\",\"waited_ms\":%lu}",
             (unsigned long)ready_waited_ms);
    server.send(503, "application/json", body);
}

/* Same conversions as the library: one raw step is 0.3125°, angles in tenths of a degree */
static int angle_to_raw(int zero, int angle)
{
    return zero + angle * 16 / 5 / 10;
}

static int raw_to_angle(int zero, int raw)
{
    return (raw - zero) * 5 * 10 / 16;
}

static void load_servo_zeros()
{
    Settings settings("servo", false);  // read-only
    yaw_zero   = settings.GetInt("zero_pos_1", -1);
    pitch_zero = settings.GetInt("zero_pos_2", -1);
    zero_ok    = yaw_zero >= RAW_MIN && yaw_zero <= RAW_MAX && pitch_zero >= RAW_MIN && pitch_zero <= RAW_MAX;
}

/*
 * Reads both positions straight from the servos. Unlike the library's
 * getCurrentAngle(), a failed read is reported instead of replaced by a guess.
 */
static bool read_angles(int& yaw, int& pitch)
{
    int yaw_raw   = bus.ReadPos(YAW_ID);
    int pitch_raw = bus.ReadPos(PITCH_ID);
    if (yaw_raw < RAW_MIN || yaw_raw > RAW_MAX || pitch_raw < RAW_MIN || pitch_raw > RAW_MAX) {
        return false;
    }
    yaw          = raw_to_angle(yaw_zero, yaw_raw);
    pitch        = raw_to_angle(pitch_zero, pitch_raw);
    last_yaw     = yaw;
    last_pitch   = pitch;
    angles_known = true;
    return true;
}

/* A failed read (-1) counts as still moving, so the timeout catches a lost link */
static bool servos_arrived()
{
    return bus.ReadMove(YAW_ID) == 0 && bus.ReadMove(PITCH_ID) == 0;
}

/* ---------- camera (as R4_HermesCamera v0.5.0) ---------- */

/* Same pins as M5Stack's official GC0308 driver for CoreS3; no XCLK, reset or power-down pin */
static camera_config_t make_camera_config(bool stream)
{
    camera_config_t c = {};
    c.pin_pwdn     = -1;
    c.pin_reset    = -1;
    c.pin_xclk     = -1;
    c.pin_sccb_sda = 12;
    c.pin_sccb_scl = 11;
    c.pin_d7       = 47;
    c.pin_d6       = 48;
    c.pin_d5       = 16;
    c.pin_d4       = 15;
    c.pin_d3       = 42;
    c.pin_d2       = 41;
    c.pin_d1       = 40;
    c.pin_d0       = 39;
    c.pin_vsync    = 46;
    c.pin_href     = 38;
    c.pin_pclk     = 45;

    c.xclk_freq_hz  = 20000000;
    c.ledc_timer    = LEDC_TIMER_0;
    c.ledc_channel  = LEDC_CHANNEL_0;
    c.pixel_format  = PIXFORMAT_RGB565;  // the GC0308 has no JPEG mode; converted below
    c.frame_size    = FRAMESIZE_QVGA;    // 320x240
    c.jpeg_quality  = 0;
    c.fb_count      = stream ? 2 : 1;  // stream: one frame filling while one is drawn
    c.fb_location   = CAMERA_FB_IN_PSRAM;
    c.grab_mode     = stream ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
    c.sccb_i2c_port = -1;
    return c;
}

static bool internal_bus_ok()
{
    float v = M5StackChan.getBatteryVoltage();
    return v >= BATTERY_V_MIN && v <= BATTERY_V_MAX;
}

static const char* BUS_LOST = "internal bus did not recover; camera, mic and movement disabled until restart";

/*
 * One full hand-over (as R4): servo power off -> release internal I2C -> camera init ->
 * warm-up frames -> use(frame) -> camera deinit -> re-take internal I2C -> health check.
 * Returns nullptr on success, otherwise why. use() returns false if it could not use the frame.
 */
static const char* with_one_frame(bool (*use)(camera_fb_t*, void*), void* ctx)
{
    const char* why = nullptr;

    M5StackChan.setServoPowerEnabled(false);  // already off between moves; make sure before giving up the bus
    M5.In_I2C.release();

    camera_config_t cfg = make_camera_config(false);
    if (esp_camera_init(&cfg) != ESP_OK) {
        why = "camera init failed";
    } else {
        for (int i = 0; i < WARMUP_FRAMES; i++) {
            camera_fb_t* warm = esp_camera_fb_get();
            if (warm) {
                esp_camera_fb_return(warm);
            }
        }
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            why = "no frame";
        } else {
            if (!use(fb, ctx)) {
                why = "frame could not be used";
            }
            esp_camera_fb_return(fb);
        }
        esp_camera_deinit();
    }

    M5.In_I2C.begin();
    if (!internal_bus_ok()) {
        bus_failed = true;
        why        = BUS_LOST;
    }
    return why;
}

struct JpegOut {
    uint8_t* jpg = nullptr;
    size_t len   = 0;
};

static bool frame_to_jpeg(camera_fb_t* fb, void* ctx)
{
    JpegOut* out = (JpegOut*)ctx;
    if (!frame2jpg(fb, JPEG_QUALITY, &out->jpg, &out->len)) {
        out->jpg = nullptr;
        return false;
    }
    return true;
}

/*
 * Capture one frame and convert it to JPEG. Returns a malloc'd buffer the caller
 * must free(), or nullptr with *why set. While the camera view streams, the current
 * frame is used; otherwise one full hand-over, which always hands the bus back.
 */
static uint8_t* capture_jpeg(size_t* out_len, const char** why)
{
    JpegOut out;
    *why = nullptr;

    if (cam_streaming) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            *why = "no frame";
        } else {
            if (!frame_to_jpeg(fb, &out)) {
                *why = "jpeg conversion failed";
            }
            esp_camera_fb_return(fb);
        }
    } else {
        *why = with_one_frame(frame_to_jpeg, &out);
        if (*why && out.jpg) {
            free(out.jpg);  // e.g. converted, then the bus check failed
            out.jpg = nullptr;
        }
    }
    *out_len = out.jpg ? out.len : 0;
    return out.jpg;
}

/* ---------- camera view on the robot's screen (R7a) ---------- */

static bool frame_to_screen(camera_fb_t* fb, void* ctx)
{
    if (fb->format != PIXFORMAT_RGB565 || (int)fb->width != cam_spr.width() ||
        (int)fb->height != cam_spr.height()) {
        return false;
    }
    /* The camera sends RGB565 high byte first, which is M5GFX's default for uint16_t data */
    cam_spr.pushImage(0, 0, fb->width, fb->height, (const uint16_t*)fb->buf);
    return true;
}

/* Re-take the head-touch baseline after bus or power disturbances (library recommendation) */
static void touch_recalibrate()
{
    M5StackChan.TouchSensor.recalibrate();
    touch_recals++;
}

static bool touch_chip_ok()
{
    touch_ok = M5.In_I2C.scanID(SI12T_GND_ADDRESS);
    return touch_ok;
}

/* Returns nullptr on success, otherwise why (and the view stays off) */
static const char* view_start()
{
    if (CAM_VIEW_STREAM) {
        M5StackChan.setServoPowerEnabled(false);
        M5.In_I2C.release();

        camera_config_t cfg = make_camera_config(true);
        bool init_ok        = esp_camera_init(&cfg) == ESP_OK;
        /* The sensor is configured now; drop the camera's control-bus driver so the internal
         * I2C can have the pins back while frames keep arriving on the data pins */
        SCCB_Deinit();
        M5.In_I2C.begin();

        if (!internal_bus_ok()) {
            if (init_ok) {
                esp_camera_deinit();
            }
            bus_failed = true;
            return BUS_LOST;
        }
        if (!init_ok) {
            return "camera init failed";
        }
        cam_streaming = true;
    }

    cam_view          = true;
    cam_view_start_ms = millis();
    last_frame_ms     = 0;
    frame_misses      = 0;
    views_started++;
    cam_spr.fillScreen(TFT_BLACK);
    touch_recalibrate();
    return nullptr;
}

static void view_stop(const char* reason)
{
    if (!cam_view) {
        return;
    }
    if (cam_streaming) {
        esp_camera_deinit();  // stops the data pins; no control-bus traffic
        cam_streaming = false;
    }
    cam_view        = false;
    view_end_reason = reason;
    if (!internal_bus_ok()) {
        bus_failed = true;
    } else {
        touch_recalibrate();
    }
    last_draw_ms = millis();
    draw_screen();
}

/* Called from loop(): next frame when due, and the automatic switch-off */
static void view_tick()
{
    if (!cam_view) {
        return;
    }
    if (millis() - cam_view_start_ms >= CAM_VIEW_MAX_MS) {
        view_stop("timeout");
        return;
    }
    uint32_t gap = CAM_VIEW_STREAM ? 1000 / CAM_VIEW_FPS : CAM_SLOW_GAP_MS;
    if (last_frame_ms && millis() - last_frame_ms < gap) {
        return;
    }
    last_frame_ms = millis();

    bool got = false;
    if (cam_streaming) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            got = frame_to_screen(fb, nullptr);
            esp_camera_fb_return(fb);
        }
    } else {
        const char* why = with_one_frame(frame_to_screen, nullptr);
        got             = why == nullptr;
        if (why == BUS_LOST) {
            camera_errors++;
            view_stop("bus fault");
            return;
        }
        last_frame_ms = millis();  // the gap counts from the end of a slow hand-over
    }

    if (got) {
        frame_misses = 0;
        view_frames++;
    } else if (++frame_misses >= CAM_VIEW_MAX_MISSES) {
        camera_errors++;
        view_stop("no frames");
        return;
    }
    compose_screen();
}

static void handle_camera_view()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }
    String arg = server.hasArg("on") ? server.arg("on") : String("");
    if (arg != "0" && arg != "1") {
        send_json_error(400, "need on=1 or on=0");
        return;
    }

    if (arg == "0") {
        view_stop("request");
    } else {
        if (bus_failed) {
            send_json_error(503, "camera disabled until restart (internal bus check failed)");
            return;
        }
        if (!screen_ok) {
            send_json_error(503, "no screen buffers (PSRAM?)");
            return;
        }
        if (refused_quiet()) {
            return;
        }
        if (M5.Speaker.isPlaying()) {
            send_json_error(409, "busy: audio playing");
            return;
        }
        if (cam_view) {
            cam_view_start_ms = millis();  // already on: restart the 2 minutes
        } else {
            const char* why = view_start();
            if (why) {
                camera_errors++;
                last_draw_ms = millis();
                draw_screen();
                send_json_error(500, why);
                return;
            }
        }
    }

    requests_served++;
    char body[160];
    snprintf(body, sizeof(body), "{\"camera_view\":%s,\"mode\":\"%s\",\"auto_off_s\":%lu,\"views_started\":%lu}",
             cam_view ? "true" : "false", CAM_VIEW_STREAM ? "stream" : "slow",
             cam_view ? (unsigned long)((CAM_VIEW_MAX_MS - (millis() - cam_view_start_ms)) / 1000) : 0UL,
             (unsigned long)views_started);
    server.send(200, "application/json", body);
}

/* ---------- handlers ---------- */

static void handle_status()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }

    float voltage    = M5StackChan.getBatteryVoltage();
    float current_ma = M5StackChan.getBatteryCurrent() * 1000.0f;  // positive = discharging

    char body[1800];
    snprintf(body, sizeof(body),
             "{\"sketch\":\"%s\",\"version\":\"%s\",\"uptime_s\":%lu,"
             "\"battery_v\":%.2f,\"battery_ma\":%.1f,\"wifi_rssi_dbm\":%d,"
             "\"free_heap\":%u,\"servo_power\":false,\"speaker_volume\":%u,"
             "\"clips_spoken\":%lu,\"moves_done\":%lu,\"angles_known\":%s,"
             "\"last_yaw\":%d,\"last_pitch\":%d,\"servo_zero_ok\":%s,\"free_psram\":%u,"
             "\"photos_taken\":%lu,\"camera_errors\":%lu,\"bus_failed\":%s,\"internal_bus_ok\":%s,"
             "\"recordings_made\":%lu,\"mic_errors\":%lu,\"quiet\":%s,\"taps_ignored\":%lu,"
             "\"camera_view\":%s,\"camera_view_mode\":\"%s\",\"camera_view_left_s\":%lu,"
             "\"views_started\":%lu,\"view_frames\":%lu,\"view_end\":\"%s\",\"screen_ok\":%s,"
             "\"touch_ok\":%s,\"touch_recals\":%lu,"
             "\"voice_seq\":%lu,\"voice_ready\":%s,\"voices_sent\":%lu,\"voices_failed\":%lu,"
             "\"voices_taken\":%lu,\"voices_expired\":%lu,\"voices_silent\":%lu,\"voice_last\":\"%s\","
             "\"voice_len_ms\":%lu,\"voice_end\":\"%s\",\"voice_loud_rms\":%d,\"voice_floor_rms\":%d,"
             "\"last_hook_code\":%d,\"hook_ok\":%s,\"hook_ip_known\":%s,\"clock_ok\":%s,"
             "\"idle\":%s,\"face\":%s,\"idle_glances\":%lu,"
             "\"settings\":\"%s\",\"requests_served\":%lu}",
             SKETCH_NAME, SKETCH_VERSION, (unsigned long)(millis() / 1000), voltage, current_ma,
             (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), SPEAKER_VOLUME,
             (unsigned long)clips_spoken, (unsigned long)moves_done, angles_known ? "true" : "false",
             last_yaw, last_pitch, zero_ok ? "true" : "false", (unsigned)ESP.getFreePsram(),
             (unsigned long)photos_taken, (unsigned long)camera_errors, bus_failed ? "true" : "false",
             internal_bus_ok() ? "true" : "false", (unsigned long)recordings_made, (unsigned long)mic_errors,
             quiet ? "true" : "false", (unsigned long)taps_ignored,
             cam_view ? "true" : "false", CAM_VIEW_STREAM ? "stream" : "slow", view_left_s(),
             (unsigned long)views_started, (unsigned long)view_frames, view_end_reason,
             screen_ok ? "true" : "false", touch_chip_ok() ? "true" : "false", (unsigned long)touch_recals,
             (unsigned long)voice_seq, voice_ready ? "true" : "false", (unsigned long)voices_sent,
             (unsigned long)voices_failed, (unsigned long)voices_taken, (unsigned long)voices_expired,
             (unsigned long)voices_silent, voice_last, (unsigned long)voice_len_ms, voice_end, voice_loud_rms,
             voice_floor_rms,
             last_hook_code, hook_ok ? "true" : "false",
             hook_ip_known() ? "true" : "false", clock_ok() ? "true" : "false",
             idle_on ? "true" : "false", face_shown ? "true" : "false", (unsigned long)idle_glances,
             cfg_source, (unsigned long)(requests_served + 1));

    requests_served++;
    server.send(200, "application/json", body);
}

/* Upload callback: runs per chunk before handle_say_done */
static void handle_say_upload()
{
    HTTPUpload& up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
        wav_len = 0;
        if (!is_authorized()) {
            say_state = SayState::Unauthorized;
        } else if (!wav_buf) {
            say_state = SayState::NoBuffer;
        } else if (M5.Speaker.isPlaying()) {
            say_state = SayState::Busy;
        } else if (has_played && millis() - last_play_ms < COOLDOWN_MS) {
            say_state = SayState::Cooldown;
        } else {
            say_state = SayState::Accepting;
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (say_state != SayState::Accepting) {
            return;
        }
        if (wav_len + up.currentSize > MAX_WAV_BYTES) {
            say_state = SayState::TooLarge;
            return;
        }
        memcpy(wav_buf + wav_len, up.buf, up.currentSize);
        wav_len += up.currentSize;
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        say_state = SayState::Aborted;
    }
}

static void handle_say_done()
{
    SayState state = say_state;
    say_state      = SayState::None;

    switch (state) {
        case SayState::None:
            /* No file part arrived: still check auth first so probes learn nothing */
            if (!is_authorized()) {
                requests_rejected++;
                send_json_error(401, "unauthorized");
            } else {
                send_json_error(400, "expected one multipart file");
            }
            return;
        case SayState::Unauthorized:
            requests_rejected++;
            send_json_error(401, "unauthorized");
            return;
        case SayState::NoBuffer:
            send_json_error(503, "no audio buffer (PSRAM?)");
            return;
        case SayState::Busy:
            send_json_error(409, "already playing");
            return;
        case SayState::Cooldown:
            send_json_error(429, "cooldown");
            return;
        case SayState::TooLarge:
            send_json_error(413, "file too large");
            return;
        case SayState::Aborted:
            send_json_error(400, "upload aborted");
            return;
        case SayState::Accepting:
            break;
    }
    if (refused_quiet()) {
        return;
    }

    const char* problem = validate_wav(wav_buf, wav_len);
    if (problem) {
        send_json_error(400, problem);
        return;
    }

    if (!M5.Speaker.playWav(wav_buf, wav_len, 1, -1, true)) {
        send_json_error(500, "playWav refused the file");
        return;
    }

    last_play_ms = millis();
    has_played   = true;
    clips_spoken++;
    requests_served++;

    char body[128];
    snprintf(body, sizeof(body), "{\"played\":true,\"bytes\":%u,\"clips_spoken\":%lu}", (unsigned)wav_len,
             (unsigned long)clips_spoken);
    server.send(200, "application/json", body);
}

/* Power the servos just long enough to read where the head is; commands no movement */
static void handle_head_read()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }

    if (bus_failed) {
        send_json_error(503, "movement disabled until restart (internal bus check failed)");
        return;
    }
    if (!zero_ok) {
        send_json_error(503, "servo calibration (zero positions) missing");
        return;
    }

    servo_power_on();
    if (!servos_ready()) {
        servo_power_off();
        send_not_ready();
        return;
    }
    int yaw, pitch;
    bool read_ok = read_angles(yaw, pitch);
    servo_power_off();
    if (!read_ok) {
        send_json_error(503, "servo position read failed; power off");
        return;
    }

    requests_served++;
    char body[160];
    snprintf(body, sizeof(body), "{\"yaw\":%d,\"pitch\":%d,\"units\":\"0.1deg\",\"moved\":false,\"ready_ms\":%lu}", yaw, pitch,
             (unsigned long)ready_waited_ms);
    server.send(200, "application/json", body);
}

/* ---------- one move, used by /head/move ---------- */

/* MoveFail and MoveOutcome live in move_outcome.h (Arduino generates prototypes above sketch types) */

/*
 * Powers the servos, reads the start angles, checks the move against move_rules, makes
 * ONE move and powers off. With relative=true, yaw/pitch are offsets from the start angles
 * (no caller since v0.11.0). Callers check cooldown, range, bus and calibration first.
 */
static void run_move(int yaw, int pitch, bool relative, MoveOutcome& o)
{
    float baseline_ma = M5StackChan.getBatteryCurrent() * 1000.0f;

    servo_power_on();
    if (!servos_ready()) {
        servo_power_off();
        o.fail = MoveFail::NotReady;
        return;
    }

    if (!read_angles(o.start_yaw, o.start_pitch)) {
        servo_power_off();
        o.fail = MoveFail::ReadFailed;
        return;
    }
    if (relative) {
        yaw   += o.start_yaw;
        pitch += o.start_pitch;
    }
    o.target_yaw   = yaw;
    o.target_pitch = pitch;

    o.problem = move_rules::check_move(o.start_yaw, o.start_pitch, yaw, pitch);
    if (o.problem) {
        servo_power_off();
        o.fail = MoveFail::Rejected;
        return;
    }

    /* Travel time from the larger axis change, in whole degrees */
    int step_deg = max(abs(yaw - o.start_yaw), abs(pitch - o.start_pitch)) / 10;
    o.move_ms    = max(MOVE_MIN_MS, (uint32_t)step_deg * MOVE_MS_PER_DEG);

    uint32_t t0         = millis();
    uint32_t over_since = 0;

    /* One command per servo: target position plus travel time; speed 0 lets time govern */
    if (!bus.WritePos(YAW_ID, angle_to_raw(yaw_zero, yaw), o.move_ms, 0) ||
        !bus.WritePos(PITCH_ID, angle_to_raw(pitch_zero, pitch), o.move_ms, 0)) {
        o.cmd_failed = true;
    }
    delay(40);  // let the servos raise their "moving" flag

    while (!o.cmd_failed && !servos_arrived()) {
        float delta = M5StackChan.getBatteryCurrent() * 1000.0f - baseline_ma;
        if (delta > o.peak_delta_ma) {
            o.peak_delta_ma = delta;
        }
        if (delta > STALL_DELTA_MA) {
            if (over_since == 0) {
                over_since = millis();
            } else if (millis() - over_since > STALL_HOLD_MS) {
                o.stall_abort = true;
                break;
            }
        } else {
            over_since = 0;
        }
        if (millis() - t0 > MOVE_TIMEOUT_MS) {
            o.timed_out = true;
            break;
        }
        delay(20);
    }

    o.ok       = !o.timed_out && !o.stall_abort && !o.cmd_failed;
    o.duration = millis() - t0;
    if (o.ok) {
        delay(MOVE_HOLD_MS);
        o.end_known = read_angles(o.end_yaw, o.end_pitch);
    } else {
        /* Abort: cut power right away rather than command anything else */
        angles_known = false;
    }
    servo_power_off();

    last_move_end_ms = millis();
    has_moved        = true;
    if (o.ok) {
        moves_done++;
    }
}

static void handle_head_move()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }

    int yaw, pitch;
    if (!server.hasArg("yaw") || !server.hasArg("pitch") ||
        !move_rules::parse_angle(server.arg("yaw").c_str(), yaw) ||
        !move_rules::parse_angle(server.arg("pitch").c_str(), pitch)) {
        send_json_error(400, "need integer yaw and pitch (tenths of a degree)");
        return;
    }
    if (refused_quiet()) {
        return;
    }
    if (M5.Speaker.isPlaying()) {
        send_json_error(409, "busy: audio playing");
        return;
    }
    if (has_moved && millis() - last_move_end_ms < MOVE_COOLDOWN_MS) {
        send_json_error(429, "cooldown");
        return;
    }

    /* Range check before powering anything; step check needs the real start angles */
    const char* problem = move_rules::check_move(yaw, pitch, yaw, pitch);
    if (problem) {
        send_json_error(400, problem);
        return;
    }

    if (bus_failed) {
        send_json_error(503, "movement disabled until restart (internal bus check failed)");
        return;
    }
    if (!zero_ok) {
        send_json_error(503, "servo calibration (zero positions) missing");
        return;
    }

    MoveOutcome o;
    run_move(yaw, pitch, false, o);

    switch (o.fail) {
        case MoveFail::NotReady:
            send_not_ready();
            return;
        case MoveFail::ReadFailed:
            send_json_error(503, "servo position read failed; power off");
            return;
        case MoveFail::Rejected: {
            char body[200];
            snprintf(body, sizeof(body), "{\"error\":\"%s\",\"start_yaw\":%d,\"start_pitch\":%d}", o.problem,
                     o.start_yaw, o.start_pitch);
            server.send(400, "application/json", body);
            return;
        }
        case MoveFail::None:
            break;
    }
    if (o.ok) {
        requests_served++;
    }

    char end_buf[48];
    if (o.end_known) {
        snprintf(end_buf, sizeof(end_buf), "\"end_yaw\":%d,\"end_pitch\":%d", o.end_yaw, o.end_pitch);
    } else {
        snprintf(end_buf, sizeof(end_buf), "\"end_yaw\":null,\"end_pitch\":null");
    }

    char body[420];
    snprintf(body, sizeof(body),
             "{\"moved\":%s,\"target_yaw\":%d,\"target_pitch\":%d,\"start_yaw\":%d,\"start_pitch\":%d,"
             "%s,\"move_ms\":%lu,\"duration_ms\":%lu,\"peak_current_delta_ma\":%.0f,"
             "\"timed_out\":%s,\"stall_abort\":%s,\"cmd_failed\":%s,\"servo_power\":false,\"moves_done\":%lu}",
             o.ok ? "true" : "false", yaw, pitch, o.start_yaw, o.start_pitch, end_buf, (unsigned long)o.move_ms,
             (unsigned long)o.duration, o.peak_delta_ma, o.timed_out ? "true" : "false",
             o.stall_abort ? "true" : "false", o.cmd_failed ? "true" : "false", (unsigned long)moves_done);
    server.send(o.ok ? 200 : 500, "application/json", body);
}

static void handle_snap()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }
    if (bus_failed) {
        send_json_error(503, "camera disabled until restart (internal bus check failed)");
        return;
    }
    if (refused_quiet()) {
        return;
    }
    if (has_snapped && millis() - last_snap_ms < SNAP_COOLDOWN_MS) {
        send_json_error(429, "cooldown");
        return;
    }
    if (M5.Speaker.isPlaying()) {
        send_json_error(409, "busy: audio playing");
        return;
    }

    show_banner(TFT_RED, "CAMERA");

    uint32_t t0     = millis();
    size_t len      = 0;
    const char* why = nullptr;
    uint8_t* jpg    = capture_jpeg(&len, &why);
    uint32_t took   = millis() - t0;

    clear_banner();
    last_snap_ms = millis();
    has_snapped  = true;
    if (bus_failed) {
        view_stop("bus fault");
    }
    last_draw_ms = millis();
    draw_screen();

    if (!jpg) {
        camera_errors++;
        send_json_error(500, why ? why : "capture failed");
        return;
    }

    photos_taken++;
    requests_served++;
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Capture-Ms", String(took));
    /* On ESP32 "PROGMEM" is ordinary memory; this overload sends binary data with a length */
    server.send_P(200, "image/jpeg", (const char*)jpg, len);
    free(jpg);

    if (server.arg("describe") == "1") {
        /* The Mac is now describing the photo: show that, ended like THINKING */
        show_banner(TFT_BLUE, "ANALYZING IMAGE...");
        thinking       = true;
        think_start_ms = millis();
        think_max_ms   = PHOTO_THINK_MAX_MS;
        think_mark     = clips_spoken + moves_done + views_started;
    }
}

/* ---------- microphone ---------- */

static void wr16le(uint8_t* p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static void wr32le(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (v >> (8 * i)) & 0xFF;
    }
}

static void write_wav_header(uint8_t* h, uint32_t data_bytes)
{
    memcpy(h, "RIFF", 4);
    wr32le(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    wr32le(h + 16, 16);           // fmt chunk size
    wr16le(h + 20, 1);            // PCM
    wr16le(h + 22, 1);            // mono
    wr32le(h + 24, MIC_RATE);
    wr32le(h + 28, MIC_RATE * 2); // byte rate
    wr16le(h + 32, 2);            // block align
    wr16le(h + 34, 16);           // bits per sample
    memcpy(h + 36, "data", 4);
    wr32le(h + 40, data_bytes);
}

static void draw_mic_banner(int secs_left)
{
    show_banner(TFT_RED, "MIC REC  %d s", secs_left);
}

static void handle_mic_record()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }
    if (bus_failed) {
        send_json_error(503, "microphone disabled until restart (internal bus check failed)");
        return;
    }
    if (!rec_buf) {
        send_json_error(503, "no recording buffer (PSRAM?)");
        return;
    }

    /* secs: one or two digits, 1..MIC_MAX_SECS */
    String arg = server.hasArg("secs") ? server.arg("secs") : String("5");
    int secs   = 0;
    if (arg.length() < 1 || arg.length() > 2) {
        secs = -1;
    }
    for (size_t i = 0; secs >= 0 && i < arg.length(); i++) {
        secs = isdigit((unsigned char)arg[i]) ? secs * 10 + (arg[i] - '0') : -1;
    }
    if (secs < 1 || secs > MIC_MAX_SECS) {
        send_json_error(400, "secs must be 1..8");
        return;
    }
    if (refused_quiet()) {
        return;
    }
    if (has_recorded && millis() - last_rec_ms < REC_COOLDOWN_MS) {
        send_json_error(429, "cooldown");
        return;
    }
    if (M5.Speaker.isPlaying()) {
        send_json_error(409, "busy: audio playing");
        return;
    }

    uint32_t took   = 0;
    size_t got      = 0;
    const char* why = record_samples(secs, took, got, false);
    if (why) {
        mic_errors++;
        send_json_error(500, why);
        return;
    }

    int16_t* pcm    = rec_buf + MIC_WARMUP;
    size_t samples  = (size_t)secs * MIC_RATE;
    int32_t peak    = 0;
    int rms         = 0;
    measure_level(pcm, samples, peak, rms);

    /* The warm-up region is discarded; the 44-byte header goes in its last bytes */
    uint8_t* wav    = (uint8_t*)pcm - WAV_HEADER_BYTES;
    size_t data_len = samples * 2;
    write_wav_header(wav, data_len);

    recordings_made++;
    requests_served++;
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Record-Ms", String(took));
    server.sendHeader("X-Peak", String(peak));
    server.sendHeader("X-Rms", String(rms));
    /* On ESP32 "PROGMEM" is ordinary memory; this overload sends binary data with a length */
    server.send_P(200, "audio/wav", (const char*)wav, WAV_HEADER_BYTES + data_len);
}

/* Signal level only, so silence can be detected without listening */
static void measure_level(const int16_t* pcm, size_t samples, int32_t& peak, int& rms)
{
    peak          = 0;
    double sum_sq = 0.0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        if (v > peak) {
            peak = v;
        }
        sum_sq += (double)pcm[i] * pcm[i];
    }
    rms = (int)sqrt(sum_sq / samples);
}

/*
 * Talk recording: after the warm-up request, 100 ms pieces are queued back to back (the
 * library records queued requests without gaps) and each finished piece is checked. Stops
 * VOICE_END_QUIET_CHUNKS quiet pieces after speech (not before VOICE_MIN_CHUNKS), after
 * VOICE_NO_SPEECH_CHUNKS without speech, or at secs. got = samples recorded after the warm-up.
 * Speech line: the larger of VOICE_SPEECH_RMS and VOICE_FLOOR_X/10 x the quietest piece so far;
 * as the floor drops, earlier pieces are judged again against the new line.
 */
static const char* capture_until_quiet(int secs, size_t& got)
{
    static int level[VOICE_MAX_SECS * MIC_RATE / VOICE_CHUNK];  // rms of each piece
    size_t chunks    = std::min<size_t>((size_t)secs * MIC_RATE / VOICE_CHUNK, sizeof(level) / sizeof(level[0]));
    size_t queued    = 0;  // pieces handed to the library
    size_t done      = 0;  // pieces finished and checked
    voice_loud_rms   = 0;
    voice_floor_rms  = 0;
    voice_end        = "max";
    got              = 0;
    if (!M5.Mic.record(rec_buf, MIC_WARMUP, MIC_RATE)) {
        return "mic record refused";
    }
    int shown      = secs;
    uint32_t t0    = millis();
    uint32_t limit = (uint32_t)secs * 1000 + 2000;
    while (done < chunks) {
        while (queued < chunks && M5.Mic.isRecording() < 2) {
            if (!M5.Mic.record(rec_buf + MIC_WARMUP + queued * VOICE_CHUNK, VOICE_CHUNK, MIC_RATE)) {
                got = done * VOICE_CHUNK;
                return "mic record refused";
            }
            queued++;
        }
        /* Requests finish in order; the first one is the warm-up */
        size_t finished = 1 + queued - M5.Mic.isRecording();
        while (done + 1 < finished && done < chunks) {
            int32_t peak = 0;
            int rms      = 0;
            measure_level(rec_buf + MIC_WARMUP + done * VOICE_CHUNK, VOICE_CHUNK, peak, rms);
            level[done] = rms;
            if (rms > voice_loud_rms) {
                voice_loud_rms = rms;
            }
            if (done == 0 || rms < voice_floor_rms) {
                voice_floor_rms = rms;
            }
            done++;
            int line         = max(VOICE_SPEECH_RMS, voice_floor_rms * VOICE_FLOOR_X / 10);
            bool speech      = voice_loud_rms >= line;
            size_t quiet_run = 0;
            while (quiet_run < done && level[done - 1 - quiet_run] < line) {
                quiet_run++;
            }
            if (speech && quiet_run >= VOICE_END_QUIET_CHUNKS && done >= VOICE_MIN_CHUNKS) {
                voice_end = "quiet";
                got       = done * VOICE_CHUNK;
                return nullptr;
            }
            if (!speech && done >= VOICE_NO_SPEECH_CHUNKS) {
                voice_end = "no speech";
                got       = done * VOICE_CHUNK;
                return nullptr;
            }
        }
        int left = secs - (int)((millis() - t0) / 1000);
        if (left != shown && left >= 0) {
            shown = left;
            draw_mic_banner(left);
        }
        if (millis() - t0 > limit) {
            got = done * VOICE_CHUNK;
            return "recording timed out";
        }
        delay(10);
    }
    got = done * VOICE_CHUNK;
    return nullptr;
}

/* One recording of up to secs seconds into rec_buf (after MIC_WARMUP), with banner, beep,
 * speaker hand-over and bus check; starts the recording cooldown. until_quiet = talk rules
 * (capture_until_quiet); otherwise exactly secs. Returns nullptr or the reason. */
static const char* record_samples(int secs, uint32_t& took, size_t& got, bool until_quiet)
{
    draw_mic_banner(secs);  // stays up (over the status or the last camera frame) until cleared

    /* Audible cue while the speaker still owns the shared pins */
    M5.Speaker.tone(CUE_HZ, CUE_MS);
    uint32_t c0 = millis();
    while (M5.Speaker.isPlaying() && millis() - c0 < CUE_MS + 300) {
        delay(5);
    }
    M5.Speaker.end();

    const char* why = nullptr;
    size_t samples  = (size_t)secs * MIC_RATE;
    uint32_t t0     = millis();
    got             = 0;
    if (!M5.Mic.begin()) {
        why = "mic begin failed";
    } else if (until_quiet) {
        why = capture_until_quiet(secs, got);  // end() below drops any pieces still queued
    } else if (!M5.Mic.record(rec_buf, MIC_WARMUP + samples, MIC_RATE)) {
        why = "mic record refused";
    } else {
        int shown      = secs;
        uint32_t limit = (uint32_t)secs * 1000 + 2000;
        while (M5.Mic.isRecording() && millis() - t0 < limit) {
            int left = secs - (int)((millis() - t0) / 1000);
            if (left != shown && left >= 0) {
                shown = left;
                draw_mic_banner(left);
            }
            delay(10);
        }
        if (M5.Mic.isRecording()) {
            why = "recording timed out";
        } else {
            got = samples;
        }
    }
    M5.Mic.end();  // resets the codec and powers its inputs down
    took = millis() - t0;

    M5.Speaker.begin();
    M5.Speaker.setVolume(SPEAKER_VOLUME);

    if (!internal_bus_ok()) {
        bus_failed = true;
        why        = BUS_LOST;
        view_stop("bus fault");
    }

    clear_banner();
    last_rec_ms  = millis();
    has_recorded = true;
    last_draw_ms = millis();
    draw_screen();
    return why;
}

/* ---------- touch-to-talk signal (R6a) ---------- */

/* True (and a 423 already sent) when QUIET mode refuses the request */
static bool refused_quiet()
{
    if (!quiet) {
        return false;
    }
    send_json_error(423, "quiet mode (hold the robot's screen to toggle)");
    return true;
}

/* ---------- hold-to-talk (R7b) ---------- */

static bool clock_ok()
{
    return time(nullptr) > CLOCK_VALID_AFTER;
}

static bool hook_ip_known()
{
    return hook_ip != IPAddress((uint32_t)0);
}

/* Zero the clip so nothing of it stays in memory */
static void wipe_voice(const char* outcome)
{
    if (voice_buf) {
        memset(voice_buf, 0, VOICE_WAV_BYTES);
    }
    voice_bytes = 0;
    voice_ready = false;
    voice_last  = outcome;
}

/* Short maroon notice, cleared by voice_tick() */
static void show_notice(const char* reason)
{
    show_banner(TFT_MAROON, "TALK: %s", reason);
    notice_until_ms = millis() + NOTICE_MS;
    if (notice_until_ms == 0) {
        notice_until_ms = 1;
    }
}

/* "name.local" through mDNS, anything else through DNS */
static bool resolve_hook_host()
{
    String host = cfg.hook_host;
    IPAddress ip((uint32_t)0);
    if (host.endsWith(".local")) {
        ip = MDNS.queryHost(host.substring(0, host.length() - 6).c_str(), 2000);
    } else if (!WiFi.hostByName(host.c_str(), ip)) {
        ip = IPAddress((uint32_t)0);
    }
    hook_ip = ip;
    return hook_ip_known();
}

static bool sign_hook(const char* ts, const char* body, char hex[65])
{
    String msg = String(ts) + "." + body;
    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, (const uint8_t*)cfg.hook_secret, strlen(cfg.hook_secret),
                               (const uint8_t*)msg.c_str(), msg.length(), mac) != 0) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    }
    return true;
}

/* The one request this robot ever starts: {"clip":N} to the gateway. Returns the HTTP code,
 * or a negative number when nothing was sent (-100 name not found, -101 signing failed). */
static int send_hook(uint32_t seq)
{
    if (!hook_ip_known() && !resolve_hook_host()) {
        return -100;
    }
    char body[32];
    snprintf(body, sizeof(body), "{\"clip\":%lu}", (unsigned long)seq);
    char ts[24];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(nullptr));
    char sig[65];
    if (!sign_hook(ts, body, sig)) {
        return -101;
    }
    char rid[40];
    snprintf(rid, sizeof(rid), "stackchan-%08lx-%lu", (unsigned long)boot_id, (unsigned long)seq);

    HTTPClient http;
    http.setConnectTimeout(HOOK_TIMEOUT_MS);
    http.setTimeout(HOOK_TIMEOUT_MS);
    http.begin(hook_ip.toString(), HOOK_PORT, HOOK_PATH);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Webhook-Timestamp", ts);
    http.addHeader("X-Webhook-Signature-V2", sig);
    http.addHeader("X-Request-ID", rid);
    int code = http.POST((uint8_t*)body, strlen(body));
    http.end();
    if (code < 0) {
        hook_ip = IPAddress((uint32_t)0);  // look the name up again next time (the Mac may have moved)
    }
    return code;
}

static void hook_task(void*);

/* Head tapped or held: record until the person stops speaking, keep the clip, tell Hermes */
static void start_voice()
{
    if (quiet) {
        taps_ignored++;
        return;
    }
    const char* no = nullptr;
    if (!hook_ok) {
        no = "no hook secret";
    } else if (bus_failed || !rec_buf || !voice_buf) {
        no = "mic unavailable";
    } else if (!clock_ok()) {
        no = "no clock yet";
    } else if (voice_ready || thinking || hook_pending || M5.Speaker.isPlaying()) {
        no = "busy";
    } else if (has_recorded && millis() - last_rec_ms < REC_COOLDOWN_MS) {
        no = "wait a moment";
    }
    if (no) {
        show_notice(no);
        return;
    }
    clear_banner();
    notice_until_ms = 0;

    uint32_t took      = 0;
    size_t samples     = 0;
    const char* why    = record_samples(VOICE_MAX_SECS, took, samples, true);
    size_t rec_samples = MIC_WARMUP + (size_t)VOICE_MAX_SECS * MIC_RATE;  // all of it may hold sound
    voice_len_ms       = samples * 1000 / MIC_RATE;
    if (why) {
        memset(rec_buf, 0, rec_samples * sizeof(int16_t));
        mic_errors++;
        show_notice(why);
        return;
    }
    recordings_made++;
    const int16_t* pcm = rec_buf + MIC_WARMUP;
    voice_seq++;
    if (strcmp(voice_end, "no speech") == 0 || samples == 0) {
        memset(rec_buf, 0, rec_samples * sizeof(int16_t));
        voices_silent++;
        voice_last = "silent";
        show_notice("heard nothing");
        return;
    }
    measure_level(pcm, samples, voice_peak, voice_rms);
    write_wav_header(voice_buf, samples * 2);
    memcpy(voice_buf + WAV_HEADER_BYTES, pcm, samples * 2);
    voice_bytes = WAV_HEADER_BYTES + samples * 2;
    memset(rec_buf, 0, rec_samples * sizeof(int16_t));
    voice_ready = true;
    voice_ms    = millis();

    show_banner(TFT_BLUE, "THINKING...");
    thinking       = true;
    think_start_ms = millis();
    think_max_ms   = THINK_MAX_MS;
    think_mark     = clips_spoken + moves_done + views_started;
    hook_seq       = voice_seq;
    hook_done      = false;
    hook_pending   = true;
    if (xTaskCreate(hook_task, "hook", HOOK_TASK_STACK, nullptr, 1, nullptr) != pdPASS) {
        hook_code = send_hook(hook_seq);  // no task: send in place (the Mac then falls back to hear_clip)
        hook_done = true;
    }
}

/* The webhook, off the main loop so /voice/take can be served while the Mac transcribes */
static void hook_task(void*)
{
    hook_code = send_hook(hook_seq);
    hook_done = true;
    vTaskDelete(nullptr);
}

/* From voice_tick(): the webhook task's result */
static void finish_hook(int code)
{
    last_hook_code = code;
    if (code == 202 || code == 200) {
        voices_sent++;
        if (voice_ready) {
            voice_last = "sent";  // not yet taken; a taken clip keeps "taken"
        }
        return;
    }
    voices_failed++;
    thinking = false;
    const char* why = code == -100 ? "mac not found" : "hook failed";
    if (voice_ready) {
        wipe_voice(why);
    } else {
        voice_last = why;
    }
    show_notice(why);
}

/* Called from loop(): clip expiry, end of THINKING, notices */
static void voice_tick()
{
    if (hook_pending && hook_done) {
        hook_pending = false;
        finish_hook(hook_code);
    }
    if (voice_ready && millis() - voice_ms > VOICE_KEEP_MS) {
        voices_expired++;
        wipe_voice("expired");
    }
    if (thinking && (clips_spoken + moves_done + views_started != think_mark ||
                     millis() - think_start_ms > think_max_ms)) {
        thinking = false;
        if (banner_on && banner_bg == TFT_BLUE) {
            clear_banner();
            last_draw_ms = millis();
            draw_screen();
        }
    }
    if (notice_until_ms && (int32_t)(millis() - notice_until_ms) >= 0) {
        notice_until_ms = 0;
        if (banner_on && banner_bg == TFT_MAROON) {
            clear_banner();
            last_draw_ms = millis();
            draw_screen();
        }
    }
}

static void handle_voice_take()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }
    String arg  = server.hasArg("clip") ? server.arg("clip") : String("");
    bool digits = arg.length() >= 1 && arg.length() <= 9;
    for (size_t i = 0; digits && i < arg.length(); i++) {
        digits = isdigit((unsigned char)arg[i]);
    }
    if (!digits) {
        send_json_error(400, "need clip");
        return;
    }
    if (!voice_ready || (uint32_t)arg.toInt() != voice_seq) {
        send_json_error(404, "no such clip (taken, expired, or never made)");
        return;
    }
    voices_taken++;
    requests_served++;
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Peak", String(voice_peak));
    server.sendHeader("X-Rms", String(voice_rms));
    server.send_P(200, "audio/wav", (const char*)voice_buf, voice_bytes);
    wipe_voice("taken");
}

/* Called from loop(): head taps and holds (talk) and the QUIET toggle */
static void check_touch()
{
    auto& screen_touch = M5.Touch.getDetail();
    if (screen_touch.wasClicked()) {
        idle_since_ms = millis();  // a short tap on the screen shows the status again
    }
    if (screen_touch.wasHold()) {
        quiet = !quiet;
        if (quiet) {
            view_stop("quiet");
            if (voice_ready) {
                wipe_voice("quiet");
            }
            thinking = false;
        }
        last_draw_ms = millis();
        draw_screen();
    }

    if (M5StackChan.TouchSensor.wasHold() || M5StackChan.TouchSensor.wasClicked()) {
        start_voice();
    }
}

/* ---------- idle life (v0.14.0) ---------- */

/* Counters that change whenever someone or the Mac does something (idle glances do not) */
static uint32_t activity_signature()
{
    return requests_served + requests_rejected + voices_sent + voices_failed + voices_silent +
           clips_spoken + photos_taken + recordings_made + views_started + taps_ignored;
}

/* Anything that must never be interrupted by idle life */
static bool idle_blocked()
{
    return !idle_on || quiet || cam_view || banner_on || voice_ready || thinking ||
           M5.Speaker.isPlaying();
}

/* Local night hours; without NTP time it counts as night (no glances) */
static bool is_night()
{
    if (!clock_ok()) {
        return true;
    }
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    return lt.tm_hour >= NIGHT_FROM_H || lt.tm_hour < NIGHT_TO_H;
}

static bool can_move_now()
{
    return zero_ok && !bus_failed && !M5.Speaker.isPlaying() &&
           (!has_moved || millis() - last_move_end_ms >= MOVE_COOLDOWN_MS);
}

/* One small glance away from where the head is; the eyes look the same way */
static void start_glance()
{
    static const int dirs[3][2] = {{-GLANCE_YAW, 0}, {GLANCE_YAW, 0}, {0, GLANCE_PITCH}};
    int pick = esp_random() % 3;
    MoveOutcome o;
    eye_dx = dirs[pick][0] > 0 ? 1 : (dirs[pick][0] < 0 ? -1 : 0);
    eye_dy = dirs[pick][1] > 0 ? -1 : 0;
    draw_screen();
    run_move(dirs[pick][0], dirs[pick][1], true, o);
    if (o.ok) {
        idle_glances++;
        glance_out        = true;
        glance_home_yaw   = o.start_yaw;
        glance_home_pitch = o.start_pitch;
        glance_back_ms    = millis() + GLANCE_HOLD_MS;
    } else {
        eye_dx = eye_dy = 0;  // refused (e.g. near a limit) or aborted: stay put
        draw_screen();
    }
}

static void end_glance()
{
    MoveOutcome o;
    glance_out = false;
    run_move(glance_home_yaw, glance_home_pitch, false, o);
    eye_dx = eye_dy = 0;
    draw_screen();
}

/* Called from loop(): face on/off, blinking, glances */
static void idle_tick()
{
    uint32_t now = millis();
    uint32_t sig = activity_signature();
    if (sig != idle_sig || idle_blocked()) {
        idle_sig       = sig;
        idle_since_ms  = now;
        next_glance_ms = 0;
    }
    uint32_t idle_ms = now - idle_since_ms;

    /* A glance is brought back, unless something else started meanwhile: then the head stays
       (a move would end THINKING early, and Hermes or the person decides where it looks) */
    if (glance_out && (int32_t)(now - glance_back_ms) >= 0 && can_move_now()) {
        if (quiet || bus_failed || cam_view || voice_ready || thinking) {
            glance_out = false;
            eye_dx = eye_dy = 0;
        } else {
            end_glance();
        }
        return;
    }

    bool want_face = !idle_blocked() && idle_ms >= FACE_AFTER_MS;
    bool asleep    = is_night();
    if (want_face != face_shown || (want_face && asleep != face_asleep)) {
        face_shown  = want_face;
        face_asleep = asleep;
        eyes_closed = false;
        blink_ms    = now + 3000 + esp_random() % 4000;
        draw_screen();
    }
    if (!face_shown) {
        return;
    }

    if (!face_asleep && (int32_t)(now - blink_ms) >= 0) {
        eyes_closed = !eyes_closed;
        blink_ms    = now + (eyes_closed ? BLINK_MS : 3000 + esp_random() % 4000);
        draw_screen();
    }

    if (face_asleep || glance_out || idle_ms < GLANCE_AFTER_MS) {
        return;
    }
    if (next_glance_ms == 0) {
        next_glance_ms = now + esp_random() % 60000;  // first glance within a minute
    }
    if ((int32_t)(now - next_glance_ms) >= 0 && can_move_now()) {
        next_glance_ms = now + GLANCE_GAP_MIN_MS + esp_random() % (GLANCE_GAP_MAX_MS - GLANCE_GAP_MIN_MS);
        start_glance();
    }
}

/* The face layer: two eyes (closed while blinking or asleep) and a small smile */
static void render_face(LovyanGFX& d)
{
    d.fillScreen(TFT_BLACK);
    const int ex[2] = {d.width() / 2 - 60, d.width() / 2 + 60};
    const int ey    = d.height() / 2 - 25;
    for (int i = 0; i < 2; i++) {
        /* The robot's left is the viewer's right */
        int x = ex[i] - eye_dx * 12;
        int y = ey + eye_dy * 10;
        if (eyes_closed || face_asleep) {
            d.fillRect(x - 20, y - 2, 40, 5, TFT_WHITE);
        } else {
            d.fillCircle(x, y, 20, TFT_WHITE);
        }
    }
    d.fillArc(d.width() / 2, ey + 45, 34, 30, 30, 150, TFT_WHITE);
    if (face_asleep) {
        d.setTextColor(TFT_DARKGREY, TFT_BLACK);
        d.setCursor(d.width() / 2 + 95, ey - 50);
        d.print("z z");
    }
}

static void handle_idle()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }
    String on = server.arg("on");
    if (on != "0" && on != "1") {
        send_json_error(400, "need on=0 or on=1");
        return;
    }
    idle_on = on == "1";
    requests_served++;
    char body[80];
    snprintf(body, sizeof(body), "{\"idle\":%s,\"idle_glances\":%lu}", idle_on ? "true" : "false",
             (unsigned long)idle_glances);
    server.send(200, "application/json", body);
}

static void handle_not_found()
{
    send_json_error(404, "not found");
}

/* ---------- screen (R7a): layers composed in screen_spr, then one push ---------- */

static unsigned long view_left_s()
{
    if (!cam_view) {
        return 0;
    }
    uint32_t on_ms = millis() - cam_view_start_ms;
    return on_ms >= CAM_VIEW_MAX_MS ? 0 : (CAM_VIEW_MAX_MS - on_ms + 999) / 1000;
}

/* Banner on top of whatever layer is showing; stays until clear_banner() */
static void show_banner(uint16_t bg, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(banner_text, sizeof(banner_text), fmt, ap);
    va_end(ap);
    banner_bg = bg;
    banner_on = true;
    compose_screen();
}

static void clear_banner()
{
    banner_on = false;
}

static void draw_banner(LovyanGFX& g)
{
    g.fillRect(0, 0, g.width(), 40, banner_bg);
    g.setTextColor(TFT_WHITE, banner_bg);
    g.setCursor(8, 12);
    g.print(banner_text);
}

/* Red frame and countdown, so a live camera is obvious to anyone looking at the robot */
static void draw_live_frame(LovyanGFX& g)
{
    for (int i = 0; i < LIVE_BORDER; i++) {
        g.drawRect(i, i, g.width() - 2 * i, g.height() - 2 * i, TFT_RED);
    }
    unsigned long left = view_left_s();
    int y              = g.height() - LIVE_BORDER - 24;
    g.fillRect(LIVE_BORDER, y, 116, 24, TFT_RED);
    g.setTextColor(TFT_WHITE, TFT_RED);
    g.setCursor(LIVE_BORDER + 6, y + 4);
    g.printf("LIVE %lu:%02lu", left / 60, left % 60);
}

static void compose_screen()
{
    auto& d = M5StackChan.Display();
    if (!screen_ok) {
        /* No canvases: draw straight to the display as R6 did (camera view is refused) */
        render_status(d);
        if (banner_on) {
            draw_banner(d);
        }
        return;
    }
    if (cam_view) {
        cam_spr.pushSprite(&screen_spr, 0, 0);
        draw_live_frame(screen_spr);
    } else {
        status_spr.pushSprite(&screen_spr, 0, 0);
    }
    if (banner_on) {
        draw_banner(screen_spr);
    }
    screen_spr.pushSprite(&d, 0, 0);
}

/* Redraw the status layer and show the screen */
static void draw_screen()
{
    if (screen_ok) {
        render_status(status_spr);
    }
    compose_screen();
}

static void render_status(LovyanGFX& d)
{
    if (face_shown) {
        render_face(d);
        return;
    }
    d.fillScreen(TFT_BLACK);
    d.setCursor(0, 0);
    if (quiet) {
        d.fillRect(0, 0, d.width(), 32, TFT_DARKGREY);
        d.setTextColor(TFT_WHITE, TFT_DARKGREY);
        d.setCursor(8, 8);
        d.printf("QUIET (hold to resume)");
        d.setCursor(0, 36);
    }
    d.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    d.printf("%s v%s\n", SKETCH_NAME, SKETCH_VERSION);
    d.setTextColor(TFT_WHITE, TFT_BLACK);

    if (WiFi.status() == WL_CONNECTED) {
        d.printf("IP %s:%u\n", WiFi.localIP().toString().c_str(), HTTP_PORT);
    } else {
        d.printf("Wi-Fi connecting...\n");
    }

    d.printf("Bat %.2fV %.0fmA\n", M5StackChan.getBatteryVoltage(), M5StackChan.getBatteryCurrent() * 1000.0f);
    d.printf("served   %lu\nrejected %lu\nspoken   %lu\nmoves    %lu\nphotos   %lu\nheard    %lu\n",
             (unsigned long)requests_served, (unsigned long)requests_rejected, (unsigned long)clips_spoken,
             (unsigned long)moves_done, (unsigned long)photos_taken, (unsigned long)recordings_made);
    if (angles_known) {
        d.printf("head %d,%d\n", last_yaw, last_pitch);
    }
    d.printf("views %lu  touch %s\n", (unsigned long)views_started, touch_chip_ok() ? "ok" : "NO");
    d.printf("talk %lu  clock %s\n", (unsigned long)voices_sent, clock_ok() ? "ok" : "NO");

    if (setup_needed()) {
        d.setTextColor(TFT_ORANGE);
        d.printf("SETUP NEEDED: connect USB,\nrun stackchan-setup.py\n");
    }
    if (!wav_buf) {
        d.setTextColor(TFT_RED);
        d.printf("NO AUDIO BUFFER\n");
    }
    if (!rec_buf) {
        d.setTextColor(TFT_RED);
        d.printf("NO MIC BUFFER\n");
    }
    if (!zero_ok) {
        d.setTextColor(TFT_RED);
        d.printf("NO SERVO CALIBRATION\n");
    }
    if (bus_failed) {
        d.setTextColor(TFT_RED);
        d.printf("BUS FAULT - restart\n");
    }
    if (!screen_ok) {
        d.setTextColor(TFT_RED);
        d.printf("NO SCREEN BUFFERS\n");
    }
    if (!hook_ok && !setup_needed()) {
        d.setTextColor(TFT_RED);
        d.printf("TALK OFF (hook settings)\n");
    }
}

void setup()
{
    Serial.begin(115200);

    /* Init StackChan (reads servo positions, torque off), then cut servo power */
    M5StackChan.begin();
    M5StackChan.setServoPowerEnabled(false);

    /* Keep the library's motion task off the servo bus; this sketch drives the servos */
    M5StackChan.Motion.setAutoTorqueReleaseEnabled(false);
    bus.attach(SERVO_UART);
    load_servo_zeros();

    M5.Speaker.begin();
    M5.Speaker.setVolume(SPEAKER_VOLUME);

    /* Microphone: 16 kHz mono; begun only while recording (shares pins with the speaker) */
    auto mic_cfg        = M5.Mic.config();
    mic_cfg.sample_rate = MIC_RATE;
    mic_cfg.stereo      = false;
    M5.Mic.config(mic_cfg);
    rec_buf = (int16_t*)heap_caps_malloc((MIC_WARMUP + (size_t)MIC_MAX_SECS * MIC_RATE) * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    voice_buf = (uint8_t*)heap_caps_calloc(1, VOICE_WAV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    /* Head touch: a touch held HOLD_MS reports one hold and no click; shorter ones are clicks */
    M5StackChan.TouchSensor.setHoldThresh(HOLD_MS);

    /* Three full-screen 16-bit layers in PSRAM (about 150 KB each) */
    auto& d = M5StackChan.Display();
    d.setTextSize(2);
    screen_ok = true;
    for (M5Canvas* c : {&status_spr, &cam_spr, &screen_spr}) {
        c->setPsram(true);
        c->setColorDepth(16);
        screen_ok = screen_ok && c->createSprite(d.width(), d.height()) != nullptr;
        c->setTextSize(2);
    }

    cfg_load();  // v0.17.0: saved settings, else secrets.h; sets token_ok and hook_ok
    boot_id = esp_random();

    wav_buf = (uint8_t*)heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    if (cfg.ssid[0]) {
        WiFi.begin(cfg.ssid, cfg.pass);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
            serial_tick();  // setup works while Wi-Fi is still trying
            delay(250);
        }
    }

    /* R7b: UTC from NTP for webhook timestamps (set in the background); mDNS to find the Mac */
    configTzTime(ROBOT_TZ, "pool.ntp.org", "time.apple.com");  // local time for night (v0.14.0)
    MDNS.begin("stackchan");
    if (hook_ok && WiFi.status() == WL_CONNECTED) {
        resolve_hook_host();
    }

    /* Only the Authorization header is needed; WebServer discards others */
    const char* header_keys[] = {"Authorization"};
    server.collectHeaders(header_keys, 1);
    server.on("/status", HTTP_GET, handle_status);
    server.on("/say", HTTP_POST, handle_say_done, handle_say_upload);
    server.on("/head/read", HTTP_POST, handle_head_read);
    server.on("/head/move", HTTP_POST, handle_head_move);
    server.on("/camera/snap", HTTP_POST, handle_snap);
    server.on("/camera/view", HTTP_POST, handle_camera_view);
    server.on("/mic/record", HTTP_POST, handle_mic_record);
    server.on("/voice/take", HTTP_POST, handle_voice_take);
    server.on("/idle", HTTP_POST, handle_idle);
    server.onNotFound(handle_not_found);
    server.begin();

    /* Never print the token or Wi-Fi password */
    /* Head-touch baseline after start-up power switching (R6c saw one boot with no taps) */
    if (touch_chip_ok()) {
        touch_recalibrate();
    }

    Serial.printf("[R7] %s ready, ip=%s port=%u settings=%s token_ok=%d wav_buf=%d screen_ok=%d touch_ok=%d hook_ok=%d mac=%s\n",
                  SKETCH_NAME, WiFi.localIP().toString().c_str(), HTTP_PORT, cfg_source, token_ok, wav_buf != nullptr,
                  screen_ok, touch_ok, hook_ok, hook_ip.toString().c_str());
    draw_screen();
}

void loop()
{
    M5StackChan.update();
    check_touch();
    serial_tick();
    server.handleClient();
    view_tick();
    voice_tick();
    idle_tick();

    if (millis() - last_draw_ms > 2000) {
        last_draw_ms = millis();
        draw_screen();
    }
    delay(2);
}
