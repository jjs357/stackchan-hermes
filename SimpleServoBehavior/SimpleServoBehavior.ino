/**
 * StackChan head-turn demo
 *   1. go to the home pose
 *   2. turn the head LEFT
 *   3. hold for 2 seconds
 *   4. turn back to centre
 *
 * Board: M5CoreS3   Library: M5StackChan (>= 1.0.0)
 * Motion angle unit: 10 = 1 degree.   Speed range: 0..1000
 */

#include <M5StackChan.h>

namespace {

// ---- tunables -----------------------------------------------------------
constexpr int16_t  kLeftAngle      = 600;    // 60.0° left (900 = 90°, as in the M5Stack docs)
constexpr int16_t  kCenterAngle    = 0;      // the calibrated home position
constexpr int16_t  kSpeed          = 500;    // 0..1000, how fast the servo travels
constexpr uint32_t kTravelMs       = 900;    // estimate of the time to reach the target
constexpr uint32_t kHoldMs         = 2000;   // the requested 2-second dwell

// ---- helpers ------------------------------------------------------------
void showLabel(const char* text)
{
  auto& d = M5StackChan.Display();
  d.startWrite();
  d.fillRect(0, 0, d.width(), 30, TFT_BLACK);
  d.setTextScroll(false);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(top_left);
  d.setTextSize(2);
  d.drawString(text, 6, 5);
  d.endWrite();
}

/** Wait, while still servicing the StackChan runtime (servo sync, sensors, power). */
void keepAlive(uint32_t ms)
{
  const uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    M5StackChan.update();
    delay(5);
  }
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  M5StackChan.begin();

  // Establish a known pose first, so "centre" really is centre.
  showLabel("HOME");
  M5StackChan.Motion.goHome();
  keepAlive(1200);

  // 1) turn the head left
  showLabel("LEFT");
  M5StackChan.Motion.moveX(kLeftAngle, kSpeed);
  keepAlive(kTravelMs);

  // 2) hold for 2 seconds
  keepAlive(kHoldMs);

  // 3) back to centre
  //    (use M5StackChan.Motion.goHome() instead to return BOTH axes to home)
  showLabel("CENTER");
  M5StackChan.Motion.moveX(kCenterAngle, kSpeed);
  keepAlive(kTravelMs);

  showLabel("DONE");
  Serial.println("head-turn sequence complete");
}

void loop()
{
  M5StackChan.update();
  delay(20);
}
