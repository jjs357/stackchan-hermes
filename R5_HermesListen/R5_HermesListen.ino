/*
 * R5_HermesListen — R34 (status, greeting, bounded head movement, one camera frame)
 * plus ONE short microphone recording per request
 *
 * Builds on the tested R34_HermesMoveCamera v0.6.0 (see ../SETUP-STATUS.md).
 *
 * Endpoints (all require header "Authorization: Bearer <token>"):
 *   GET  /status                       — status, counters, bus health
 *   POST /say                          — one short WAV, played once, quietly (as R2/R3)
 *   POST /head/read                    — power servos briefly and report angles; no movement
 *   POST /head/move?yaw=<n>&pitch=<n>  — one move, angles in tenths of a degree (as R3 v0.4.0)
 *   POST /camera/snap                  — capture one 320x240 frame, returned as JPEG (as R4)
 *   POST /mic/record?secs=<1..8>       — record once, returned as 16 kHz mono 16-bit WAV
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
 * Camera (as R4): no frames except during a /camera/snap request; nothing stored on the
 * robot or streamed; red CAMERA banner during capture; 10 s cooldown.
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
 *   1. Copy your secrets.h (same token) into this folder.
 *   2. Board: Espressif "esp32" package -> "M5CoreS3", PSRAM enabled (the default).
 *   3. Upload with the Arduino IDE.
 *
 * Mac scripts (unchanged, same IP/port): ../R3_HermesMove/r3-head.sh,
 * ../R2_HermesGreeting/r2-say.sh, ../R4_HermesCamera/r4-snap.sh and r4-describe.sh.
 *
 * Camera pins and settings follow M5Stack's official M5CoreS3 library
 * (src/utility/GC0308.cpp, MIT licence).
 *
 * Libraries: M5StackChan 1.1.1, M5Unified, M5GFX; esp32-camera ships with the esp32 core.
 * Recovery: re-upload R34_HermesMoveCamera or any sketch from your sketchbook.
 */
#include <Arduino.h>
#include <M5StackChan.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <drivers/FTServo_Arduino/src/SCSCL.h>
#include <utils/settings/settings.h>
#include <esp_camera.h>
#include <img_converters.h>
#include "secrets.h"
#include "move_rules.h"

static const char* SKETCH_NAME      = "R5_HermesListen";
static const char* SKETCH_VERSION   = "0.7.0";
static const uint16_t HTTP_PORT     = 8080;
static const size_t MIN_TOKEN_LEN   = 16;
static const size_t MAX_WAV_BYTES   = 192 * 1024;
static const uint32_t MAX_WAV_MS    = 6000;
static const uint32_t COOLDOWN_MS   = 5000;
static const uint8_t SPEAKER_VOLUME = 64;  // of 255: deliberately quiet

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
static bool capturing         = false;

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

static bool is_authorized()
{
    if (!token_ok || !server.hasHeader("Authorization")) {
        return false;
    }
    String header = server.header("Authorization");
    if (!header.startsWith("Bearer ")) {
        return false;
    }
    return tokens_equal(header.substring(7), HERMES_STATUS_TOKEN);
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
static camera_config_t make_camera_config()
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
    c.fb_count      = 1;
    c.fb_location   = CAMERA_FB_IN_PSRAM;
    c.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
    c.sccb_i2c_port = -1;
    return c;
}

static bool internal_bus_ok()
{
    float v = M5StackChan.getBatteryVoltage();
    return v >= BATTERY_V_MIN && v <= BATTERY_V_MAX;
}

static void draw_camera_banner()
{
    auto& d = M5StackChan.Display();
    d.fillRect(0, 0, d.width(), 40, TFT_RED);
    d.setTextColor(TFT_WHITE, TFT_RED);
    d.setCursor(8, 12);
    d.printf("CAMERA");
}

/*
 * Capture one frame and convert it to JPEG. Returns a malloc'd buffer the caller
 * must free(), or nullptr with *why set. Always hands the bus back before returning.
 */
static uint8_t* capture_jpeg(size_t* out_len, const char** why)
{
    uint8_t* jpg = nullptr;
    *out_len     = 0;
    *why         = nullptr;

    M5StackChan.setServoPowerEnabled(false);  // already off between moves; make sure before giving up the bus
    M5.In_I2C.release();

    camera_config_t cfg = make_camera_config();
    if (esp_camera_init(&cfg) != ESP_OK) {
        *why = "camera init failed";
    } else {
        for (int i = 0; i < WARMUP_FRAMES; i++) {
            camera_fb_t* warm = esp_camera_fb_get();
            if (warm) {
                esp_camera_fb_return(warm);
            }
        }
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            *why = "no frame";
        } else {
            if (!frame2jpg(fb, JPEG_QUALITY, &jpg, out_len)) {
                jpg  = nullptr;
                *why = "jpeg conversion failed";
            }
            esp_camera_fb_return(fb);
        }
        esp_camera_deinit();
    }

    M5.In_I2C.begin();
    if (!internal_bus_ok()) {
        bus_failed = true;
        free(jpg);
        jpg  = nullptr;
        *why = "internal bus did not recover; camera, mic and movement disabled until restart";
    }
    return jpg;
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

    char body[720];
    snprintf(body, sizeof(body),
             "{\"sketch\":\"%s\",\"version\":\"%s\",\"uptime_s\":%lu,"
             "\"battery_v\":%.2f,\"battery_ma\":%.1f,\"wifi_rssi_dbm\":%d,"
             "\"free_heap\":%u,\"servo_power\":false,\"speaker_volume\":%u,"
             "\"clips_spoken\":%lu,\"moves_done\":%lu,\"angles_known\":%s,"
             "\"last_yaw\":%d,\"last_pitch\":%d,\"servo_zero_ok\":%s,\"free_psram\":%u,"
             "\"photos_taken\":%lu,\"camera_errors\":%lu,\"bus_failed\":%s,\"internal_bus_ok\":%s,"
             "\"recordings_made\":%lu,\"mic_errors\":%lu,\"requests_served\":%lu}",
             SKETCH_NAME, SKETCH_VERSION, (unsigned long)(millis() / 1000), voltage, current_ma,
             (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), SPEAKER_VOLUME,
             (unsigned long)clips_spoken, (unsigned long)moves_done, angles_known ? "true" : "false",
             last_yaw, last_pitch, zero_ok ? "true" : "false", (unsigned)ESP.getFreePsram(),
             (unsigned long)photos_taken, (unsigned long)camera_errors, bus_failed ? "true" : "false",
             internal_bus_ok() ? "true" : "false", (unsigned long)recordings_made, (unsigned long)mic_errors,
             (unsigned long)(requests_served + 1));

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

    float baseline_ma = M5StackChan.getBatteryCurrent() * 1000.0f;

    servo_power_on();
    if (!servos_ready()) {
        servo_power_off();
        send_not_ready();
        return;
    }

    int start_yaw, start_pitch;
    if (!read_angles(start_yaw, start_pitch)) {
        servo_power_off();
        send_json_error(503, "servo position read failed; power off");
        return;
    }

    problem = move_rules::check_move(start_yaw, start_pitch, yaw, pitch);
    if (problem) {
        servo_power_off();
        char body[200];
        snprintf(body, sizeof(body), "{\"error\":\"%s\",\"start_yaw\":%d,\"start_pitch\":%d}", problem, start_yaw,
                 start_pitch);
        server.send(400, "application/json", body);
        return;
    }

    /* Travel time from the larger axis change, in whole degrees */
    int step_deg     = max(abs(yaw - start_yaw), abs(pitch - start_pitch)) / 10;
    uint32_t move_ms = max(MOVE_MIN_MS, (uint32_t)step_deg * MOVE_MS_PER_DEG);

    uint32_t t0          = millis();
    float peak_delta_ma  = 0.0f;
    uint32_t over_since  = 0;
    bool timed_out       = false;
    bool stall_abort     = false;
    bool cmd_failed      = false;

    /* One command per servo: target position plus travel time; speed 0 lets time govern */
    if (!bus.WritePos(YAW_ID, angle_to_raw(yaw_zero, yaw), move_ms, 0) ||
        !bus.WritePos(PITCH_ID, angle_to_raw(pitch_zero, pitch), move_ms, 0)) {
        cmd_failed = true;
    }
    delay(40);  // let the servos raise their "moving" flag

    while (!cmd_failed && !servos_arrived()) {
        float delta = M5StackChan.getBatteryCurrent() * 1000.0f - baseline_ma;
        if (delta > peak_delta_ma) {
            peak_delta_ma = delta;
        }
        if (delta > STALL_DELTA_MA) {
            if (over_since == 0) {
                over_since = millis();
            } else if (millis() - over_since > STALL_HOLD_MS) {
                stall_abort = true;
                break;
            }
        } else {
            over_since = 0;
        }
        if (millis() - t0 > MOVE_TIMEOUT_MS) {
            timed_out = true;
            break;
        }
        delay(20);
    }

    bool ok = !timed_out && !stall_abort && !cmd_failed;
    uint32_t duration = millis() - t0;
    int end_yaw = 0, end_pitch = 0;
    bool end_known = false;
    if (ok) {
        delay(MOVE_HOLD_MS);
        end_known = read_angles(end_yaw, end_pitch);
    } else {
        /* Abort: cut power right away rather than command anything else */
        angles_known = false;
    }
    servo_power_off();

    last_move_end_ms = millis();
    has_moved        = true;
    if (ok) {
        moves_done++;
        requests_served++;
    }

    char end_buf[48];
    if (end_known) {
        snprintf(end_buf, sizeof(end_buf), "\"end_yaw\":%d,\"end_pitch\":%d", end_yaw, end_pitch);
    } else {
        snprintf(end_buf, sizeof(end_buf), "\"end_yaw\":null,\"end_pitch\":null");
    }

    char body[420];
    snprintf(body, sizeof(body),
             "{\"moved\":%s,\"target_yaw\":%d,\"target_pitch\":%d,\"start_yaw\":%d,\"start_pitch\":%d,"
             "%s,\"move_ms\":%lu,\"duration_ms\":%lu,\"peak_current_delta_ma\":%.0f,"
             "\"timed_out\":%s,\"stall_abort\":%s,\"cmd_failed\":%s,\"servo_power\":false,\"moves_done\":%lu}",
             ok ? "true" : "false", yaw, pitch, start_yaw, start_pitch, end_buf, (unsigned long)move_ms,
             (unsigned long)duration, peak_delta_ma, timed_out ? "true" : "false",
             stall_abort ? "true" : "false", cmd_failed ? "true" : "false", (unsigned long)moves_done);
    server.send(ok ? 200 : 500, "application/json", body);
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
    if (has_snapped && millis() - last_snap_ms < SNAP_COOLDOWN_MS) {
        send_json_error(429, "cooldown");
        return;
    }
    if (M5.Speaker.isPlaying()) {
        send_json_error(409, "busy: audio playing");
        return;
    }

    capturing = true;
    draw_camera_banner();

    uint32_t t0     = millis();
    size_t len      = 0;
    const char* why = nullptr;
    uint8_t* jpg    = capture_jpeg(&len, &why);
    uint32_t took   = millis() - t0;

    capturing    = false;
    last_snap_ms = millis();
    has_snapped  = true;
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
    auto& d = M5StackChan.Display();
    d.fillRect(0, 0, d.width(), 40, TFT_RED);
    d.setTextColor(TFT_WHITE, TFT_RED);
    d.setCursor(8, 12);
    d.printf("MIC REC  %d s ", secs_left);
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
    if (has_recorded && millis() - last_rec_ms < REC_COOLDOWN_MS) {
        send_json_error(429, "cooldown");
        return;
    }
    if (M5.Speaker.isPlaying()) {
        send_json_error(409, "busy: audio playing");
        return;
    }

    capturing = true;  // keeps draw_screen() off the banner
    draw_mic_banner(secs);

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
    if (!M5.Mic.begin()) {
        why = "mic begin failed";
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
        }
    }
    M5.Mic.end();  // resets the codec and powers its inputs down
    uint32_t took = millis() - t0;

    M5.Speaker.begin();
    M5.Speaker.setVolume(SPEAKER_VOLUME);

    if (!internal_bus_ok()) {
        bus_failed = true;
        why        = "internal bus did not recover; camera, mic and movement disabled until restart";
    }

    capturing    = false;
    last_rec_ms  = millis();
    has_recorded = true;
    last_draw_ms = millis();
    draw_screen();

    if (why) {
        mic_errors++;
        send_json_error(500, why);
        return;
    }

    /* Signal level only, so silence can be detected without listening */
    int16_t* pcm  = rec_buf + MIC_WARMUP;
    int32_t peak  = 0;
    double sum_sq = 0.0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        if (v > peak) {
            peak = v;
        }
        sum_sq += (double)pcm[i] * pcm[i];
    }
    int rms = (int)sqrt(sum_sq / samples);

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

static void handle_not_found()
{
    send_json_error(404, "not found");
}

static void draw_screen()
{
    if (capturing) {
        return;
    }
    auto& d = M5StackChan.Display();
    d.fillScreen(TFT_BLACK);
    d.setCursor(0, 0);
    d.setTextColor(TFT_GREENYELLOW);
    d.printf("%s v%s\n\n", SKETCH_NAME, SKETCH_VERSION);
    d.setTextColor(TFT_WHITE);

    if (WiFi.status() == WL_CONNECTED) {
        d.printf("IP %s\nport %u\n\n", WiFi.localIP().toString().c_str(), HTTP_PORT);
    } else {
        d.printf("Wi-Fi connecting...\n\n");
    }

    d.printf("Bat %.2fV %.0fmA\n", M5StackChan.getBatteryVoltage(), M5StackChan.getBatteryCurrent() * 1000.0f);
    d.printf("served   %lu\nrejected %lu\nspoken   %lu\nmoves    %lu\nphotos   %lu\nheard    %lu\n",
             (unsigned long)requests_served, (unsigned long)requests_rejected, (unsigned long)clips_spoken,
             (unsigned long)moves_done, (unsigned long)photos_taken, (unsigned long)recordings_made);
    if (angles_known) {
        d.printf("head %d,%d\n", last_yaw, last_pitch);
    }

    if (!token_ok) {
        d.setTextColor(TFT_RED);
        d.printf("TOKEN NOT SET (secrets.h)\n");
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

    M5StackChan.Display().setTextSize(2);

    token_ok = strlen(HERMES_STATUS_TOKEN) >= MIN_TOKEN_LEN &&
               strcmp(HERMES_STATUS_TOKEN, "REPLACE_WITH_A_LONG_RANDOM_TOKEN") != 0;

    wav_buf = (uint8_t*)heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
    }

    /* Only the Authorization header is needed; WebServer discards others */
    const char* header_keys[] = {"Authorization"};
    server.collectHeaders(header_keys, 1);
    server.on("/status", HTTP_GET, handle_status);
    server.on("/say", HTTP_POST, handle_say_done, handle_say_upload);
    server.on("/head/read", HTTP_POST, handle_head_read);
    server.on("/head/move", HTTP_POST, handle_head_move);
    server.on("/camera/snap", HTTP_POST, handle_snap);
    server.on("/mic/record", HTTP_POST, handle_mic_record);
    server.onNotFound(handle_not_found);
    server.begin();

    /* Never print the token or Wi-Fi password */
    Serial.printf("[R5] %s ready, ip=%s port=%u token_ok=%d wav_buf=%d\n", SKETCH_NAME,
                  WiFi.localIP().toString().c_str(), HTTP_PORT, token_ok, wav_buf != nullptr);
    draw_screen();
}

void loop()
{
    M5StackChan.update();
    server.handleClient();

    if (millis() - last_draw_ms > 2000) {
        last_draw_ms = millis();
        draw_screen();
    }
    delay(2);
}
