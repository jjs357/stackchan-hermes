/*
 * R4_HermesCamera — status plus ONE camera frame per request (StackChan K151 / CoreS3)
 *
 * Milestone R4 of the StackChan + Hermes setup (see ../SETUP-STATUS.md).
 *
 * Endpoints (all require header "Authorization: Bearer <token>"):
 *   GET  /status        — as R1, plus camera counters and bus health
 *   POST /camera/snap   — capture one 320x240 frame and return it as a JPEG
 *
 * Camera use:
 *   - No frames are captured except during a /camera/snap request; nothing is
 *     stored on the robot, streamed, or sent anywhere except the HTTP reply
 *   - The screen shows a red CAMERA banner while a capture is in progress
 *   - 10 s cooldown between captures
 *   - The GC0308 sensor chip is powered whenever the robot is on (board design);
 *     "off" here means its driver is shut down and no frames are read
 *
 * Shared bus: the camera's control bus (SCCB) and M5Unified's internal I2C bus
 * (servo power switch, battery monitor, touch, power management) are the same
 * pins and the same I2C port, so they take turns:
 *   servo power off -> release internal I2C -> camera init -> one frame ->
 *   camera deinit -> re-take internal I2C -> health check (battery reading)
 * If the health check fails, the camera is disabled until the next restart.
 *
 * What it does NOT do: no servo movement (servo power off), no speaker,
 * microphone, LEDs, IR or NFC.
 *
 * Setup:
 *   1. Copy your secrets.h (same token) into this folder.
 *   2. Board: Espressif "esp32" package -> "M5CoreS3", PSRAM enabled (the default).
 *   3. Upload with the Arduino IDE.
 *
 * Camera pins and settings follow M5Stack's official M5CoreS3 library
 * (src/utility/GC0308.cpp, MIT licence).
 *
 * Libraries: M5StackChan 1.1.1, M5Unified, M5GFX; esp32-camera ships with the esp32 core.
 * Recovery: re-upload R3_HermesMove or any sketch from your sketchbook.
 */
#include <Arduino.h>
#include <M5StackChan.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_camera.h>
#include <img_converters.h>
#include "secrets.h"

static const char* SKETCH_NAME      = "R4_HermesCamera";
static const char* SKETCH_VERSION   = "0.5.0";
static const uint16_t HTTP_PORT     = 8080;
static const size_t MIN_TOKEN_LEN   = 16;

static const uint32_t SNAP_COOLDOWN_MS = 10000;
static const int WARMUP_FRAMES         = 3;     // let auto exposure settle; these are discarded
static const uint8_t JPEG_QUALITY      = 80;    // 0..100
static const float BATTERY_V_MIN       = 2.5f;  // health check: a sane reading means the bus works
static const float BATTERY_V_MAX       = 4.6f;

WebServer server(HTTP_PORT);

static bool token_ok              = false;
static uint32_t requests_served   = 0;
static uint32_t requests_rejected = 0;
static uint32_t photos_taken      = 0;
static uint32_t camera_errors     = 0;
static uint32_t last_draw_ms      = 0;
static uint32_t last_snap_ms      = 0;
static bool has_snapped           = false;
static bool camera_disabled       = false;  // set if the internal bus did not come back
static bool capturing             = false;

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

static void send_json_error(int code, const char* message)
{
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    server.send(code, "application/json", body);
}

/* ---------- camera ---------- */

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

    M5StackChan.setServoPowerEnabled(false);  // already off; make sure before giving up the bus
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
        camera_disabled = true;
        free(jpg);
        jpg  = nullptr;
        *why = "internal bus did not recover; camera disabled until restart";
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

    char body[400];
    snprintf(body, sizeof(body),
             "{\"sketch\":\"%s\",\"version\":\"%s\",\"uptime_s\":%lu,"
             "\"battery_v\":%.2f,\"battery_ma\":%.1f,\"wifi_rssi_dbm\":%d,"
             "\"free_heap\":%u,\"free_psram\":%u,\"servo_power\":false,"
             "\"photos_taken\":%lu,\"camera_errors\":%lu,\"camera_disabled\":%s,"
             "\"internal_bus_ok\":%s,\"requests_served\":%lu}",
             SKETCH_NAME, SKETCH_VERSION, (unsigned long)(millis() / 1000), voltage, current_ma,
             (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
             (unsigned long)photos_taken, (unsigned long)camera_errors, camera_disabled ? "true" : "false",
             internal_bus_ok() ? "true" : "false", (unsigned long)(requests_served + 1));

    requests_served++;
    server.send(200, "application/json", body);
}

static void handle_snap()
{
    if (!is_authorized()) {
        requests_rejected++;
        send_json_error(401, "unauthorized");
        return;
    }
    if (camera_disabled) {
        send_json_error(503, "camera disabled until restart (internal bus check failed)");
        return;
    }
    if (has_snapped && millis() - last_snap_ms < SNAP_COOLDOWN_MS) {
        send_json_error(429, "cooldown");
        return;
    }

    capturing = true;
    draw_camera_banner();

    uint32_t t0 = millis();
    size_t len  = 0;
    const char* why = nullptr;
    uint8_t* jpg    = capture_jpeg(&len, &why);
    uint32_t took   = millis() - t0;

    capturing    = false;
    last_snap_ms = millis();
    has_snapped  = true;
    draw_screen_now();

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

static void handle_not_found()
{
    send_json_error(404, "not found");
}

/* ---------- display ---------- */

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
    d.printf("served   %lu\nrejected %lu\nphotos   %lu\nerrors   %lu\n", (unsigned long)requests_served,
             (unsigned long)requests_rejected, (unsigned long)photos_taken, (unsigned long)camera_errors);

    if (!token_ok) {
        d.setTextColor(TFT_RED);
        d.printf("TOKEN NOT SET (secrets.h)\n");
    }
    if (camera_disabled) {
        d.setTextColor(TFT_RED);
        d.printf("CAMERA DISABLED - restart\n");
    }
}

static void draw_screen_now()
{
    last_draw_ms = millis();
    draw_screen();
}

void setup()
{
    Serial.begin(115200);

    /* Init StackChan, then cut servo power: this sketch never moves the head */
    M5StackChan.begin();
    M5StackChan.setServoPowerEnabled(false);

    M5StackChan.Display().setTextSize(2);

    token_ok = strlen(HERMES_STATUS_TOKEN) >= MIN_TOKEN_LEN &&
               strcmp(HERMES_STATUS_TOKEN, "REPLACE_WITH_A_LONG_RANDOM_TOKEN") != 0;

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
    server.on("/camera/snap", HTTP_POST, handle_snap);
    server.onNotFound(handle_not_found);
    server.begin();

    /* Never print the token or Wi-Fi password */
    Serial.printf("[R4] %s ready, ip=%s port=%u token_ok=%d\n", SKETCH_NAME,
                  WiFi.localIP().toString().c_str(), HTTP_PORT, token_ok);
    draw_screen_now();
}

void loop()
{
    M5StackChan.update();
    server.handleClient();

    if (millis() - last_draw_ms > 2000) {
        draw_screen_now();
    }
    delay(2);
}
