/*
 * R2_HermesGreeting — R1 status endpoint + one token-protected "play this WAV" endpoint
 *
 * Milestone R2 of the StackChan + Hermes setup (see ../SETUP-STATUS.md).
 *
 * Endpoints (both require header "Authorization: Bearer <token>"):
 *   GET  /status  — same as R1, plus spoken count and speaker volume
 *   POST /say     — multipart upload of ONE short WAV, played once at low volume
 *
 * WAV rules (checked against the bytes actually received before playing):
 *   PCM, mono, 16-bit, 8000–24000 Hz, at most 6 s and 192 KB.
 *   One clip at a time; 5 s cooldown between clips.
 *
 * What it does NOT do:
 *   - No servo movement (servo power is switched off after init)
 *   - No camera, microphone, LEDs, IR or NFC
 *   - The speech audio is generated on the Mac (macOS `say`); the robot only plays it
 *
 * Setup:
 *   1. Copy your secrets.h from ../R1_HermesStatus/ into this folder (same token).
 *   2. Board: Espressif "esp32" package -> "M5CoreS3", PSRAM enabled (the default).
 *   3. Upload with the Arduino IDE.
 *
 * Libraries: M5StackChan 1.1.1, M5Unified, M5GFX (no others needed).
 * Recovery: re-upload R1_HermesStatus or any previous sketch from your sketchbook.
 */
#include <Arduino.h>
#include <M5StackChan.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include "secrets.h"

static const char* SKETCH_NAME      = "R2_HermesGreeting";
static const char* SKETCH_VERSION   = "0.2.0";
static const uint16_t HTTP_PORT     = 8080;
static const size_t MIN_TOKEN_LEN   = 16;
static const size_t MAX_WAV_BYTES   = 192 * 1024;
static const uint32_t MAX_WAV_MS    = 6000;
static const uint32_t COOLDOWN_MS   = 5000;
static const uint8_t SPEAKER_VOLUME = 64;  // of 255: deliberately quiet

WebServer server(HTTP_PORT);

static bool token_ok              = false;
static uint32_t requests_served   = 0;
static uint32_t requests_rejected = 0;
static uint32_t clips_spoken      = 0;
static uint32_t last_draw_ms      = 0;
static uint32_t last_play_ms      = 0;
static bool has_played            = false;

static uint8_t* wav_buf = nullptr;
static size_t wav_len   = 0;

/* State of the current /say upload, decided when the upload starts */
enum class SayState { None, Accepting, Unauthorized, Busy, Cooldown, NoBuffer, TooLarge, Aborted };
static SayState say_state = SayState::None;

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

static void send_json_error(int code, const char* msg)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    server.send(code, "application/json", body);
}

static void handle_status()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }

    float voltage    = M5StackChan.getBatteryVoltage();
    float current_ma = M5StackChan.getBatteryCurrent() * 1000.0f;  // positive = discharging

    char body[384];
    snprintf(body, sizeof(body),
             "{\"sketch\":\"%s\",\"version\":\"%s\",\"uptime_s\":%lu,"
             "\"battery_v\":%.2f,\"battery_ma\":%.1f,\"wifi_rssi_dbm\":%d,"
             "\"free_heap\":%u,\"servo_power\":false,\"speaker_volume\":%u,"
             "\"clips_spoken\":%lu,\"requests_served\":%lu}",
             SKETCH_NAME, SKETCH_VERSION, (unsigned long)(millis() / 1000), voltage, current_ma,
             (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), SPEAKER_VOLUME,
             (unsigned long)clips_spoken, (unsigned long)(requests_served + 1));

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

static void handle_not_found()
{
    send_json_error(404, "not found");
}

static void draw_screen()
{
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

    d.printf("Bat %.2fV %.0fmA\n\n", M5StackChan.getBatteryVoltage(),
             M5StackChan.getBatteryCurrent() * 1000.0f);
    d.printf("served   %lu\nrejected %lu\nspoken   %lu\n", (unsigned long)requests_served,
             (unsigned long)requests_rejected, (unsigned long)clips_spoken);

    if (!token_ok) {
        d.setTextColor(TFT_RED);
        d.printf("\nTOKEN NOT SET (secrets.h)\n");
    }
    if (!wav_buf) {
        d.setTextColor(TFT_RED);
        d.printf("\nNO AUDIO BUFFER\n");
    }
}

void setup()
{
    Serial.begin(115200);

    /* Init StackChan, then cut servo power: this sketch never moves the head */
    M5StackChan.begin();
    M5StackChan.setServoPowerEnabled(false);

    M5.Speaker.begin();
    M5.Speaker.setVolume(SPEAKER_VOLUME);

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
    server.onNotFound(handle_not_found);
    server.begin();

    /* Never print the token or Wi-Fi password */
    Serial.printf("[R2] %s ready, ip=%s port=%u token_ok=%d wav_buf=%d\n", SKETCH_NAME,
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
