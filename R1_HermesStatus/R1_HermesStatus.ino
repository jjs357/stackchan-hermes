/*
 * R1_HermesStatus — read-only status endpoint for StackChan (K151 / CoreS3)
 *
 * Milestone R1 of the StackChan + Hermes setup (see ../SETUP-STATUS.md).
 *
 * What it does:
 *   - Joins your Wi-Fi and serves ONE endpoint: GET http://<robot-ip>:8080/status
 *   - Requires header "Authorization: Bearer <token>"; anything else gets 401/404
 *   - Returns battery voltage/current, uptime, Wi-Fi RSSI and free heap as JSON
 *   - Shows IP, battery and a served-request counter on the display
 *
 * What it does NOT do:
 *   - No servo movement (servo power is switched off after init)
 *   - No camera, microphone, speaker, LEDs, IR or NFC
 *   - No writes or commands of any kind
 *
 * Setup:
 *   1. Copy secrets.h.example to secrets.h in this folder and fill it in
 *      yourself (never paste the values into a chat).
 *   2. Board: Espressif "esp32" package -> "M5CoreS3" (or your usual StackChan board).
 *   3. Upload with the Arduino IDE.
 *
 * Libraries: M5StackChan 1.1.1, M5Unified, M5GFX (no others needed).
 * Recovery: re-upload any previous sketch from your sketchbook.
 */
#include <Arduino.h>
#include <M5StackChan.h>
#include <WiFi.h>
#include <WebServer.h>
#include "secrets.h"

static const char* SKETCH_NAME     = "R1_HermesStatus";
static const char* SKETCH_VERSION  = "0.1.0";
static const uint16_t HTTP_PORT    = 8080;
static const size_t MIN_TOKEN_LEN  = 16;

WebServer server(HTTP_PORT);

static bool token_ok              = false;
static uint32_t requests_served   = 0;
static uint32_t requests_rejected = 0;
static uint32_t last_draw_ms      = 0;

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

static void handle_status()
{
    if (!is_authorized()) {
        requests_rejected++;
        server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
        return;
    }

    float voltage    = M5StackChan.getBatteryVoltage();
    float current_ma = M5StackChan.getBatteryCurrent() * 1000.0f;  // positive = discharging

    char body[320];
    snprintf(body, sizeof(body),
             "{\"sketch\":\"%s\",\"version\":\"%s\",\"uptime_s\":%lu,"
             "\"battery_v\":%.2f,\"battery_ma\":%.1f,\"wifi_rssi_dbm\":%d,"
             "\"free_heap\":%u,\"servo_power\":false,\"requests_served\":%lu}",
             SKETCH_NAME, SKETCH_VERSION, (unsigned long)(millis() / 1000), voltage, current_ma,
             (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), (unsigned long)(requests_served + 1));

    requests_served++;
    server.send(200, "application/json", body);
}

static void handle_not_found()
{
    server.send(404, "application/json", "{\"error\":\"not found\"}");
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
    d.printf("served   %lu\nrejected %lu\n", (unsigned long)requests_served,
             (unsigned long)requests_rejected);

    if (!token_ok) {
        d.setTextColor(TFT_RED);
        d.printf("\nTOKEN NOT SET (secrets.h)\n");
    }
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
    server.onNotFound(handle_not_found);
    server.begin();

    /* Never print the token or Wi-Fi password */
    Serial.printf("[R1] %s ready, ip=%s port=%u token_ok=%d\n", SKETCH_NAME,
                  WiFi.localIP().toString().c_str(), HTTP_PORT, token_ok);
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
