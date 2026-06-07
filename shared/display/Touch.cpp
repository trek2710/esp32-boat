#include "Touch.h"
#include "Tca9554.h"
#include "AmoledDisplay.h"   // amoled::kI2cSda / kI2cScl

#include <Arduino.h>
#include <Wire.h>
#include <TouchDrv.hpp>

// Salvaged from src_tx/main.cpp (initTouch + the getPointCount-gated read).
// The v1 swipe-nav state machine is left behind — that was page navigation;
// the radar builds its own zoom/pan gesture on top of readPoint().

namespace touch {
namespace {

constexpr uint8_t kAddr      = 0x5a;
constexpr uint8_t kResetExio = 6;     // CST9217 RESET on TCA9554 EXIO6

TouchDrvCST92xx drv;
bool ready = false;

}  // namespace

bool begin() {
    // Reset pulse via the expander: ≥10 ms low, ≥50 ms high (datasheet).
    if (!tca9554::pinWrite(kResetExio, false)) {
        Serial.println("[touch] TCA9554 RESET-low failed");
        return false;
    }
    delay(20);
    if (!tca9554::pinWrite(kResetExio, true)) {
        Serial.println("[touch] TCA9554 RESET-high failed");
        return false;
    }
    delay(60);

    // RESET already pulsed by us; INT line unused — pass -1/-1 so the
    // library skips GPIO config and polls.
    drv.setPins(-1, -1);
    if (!drv.begin(Wire, kAddr, amoled::kI2cSda, amoled::kI2cScl)) {
        Serial.println("[touch] CST9217 begin() failed");
        return false;
    }
    ready = true;
    Serial.printf("[touch] CST9217 up (%s)\n", drv.getModelName());
    return true;
}

bool readPoint(int16_t &x, int16_t &y) {
    if (!ready) return false;
    // Gate on getPointCount(), not hasPoints(): the latter is true for any
    // chip activity (proximity/partial reports) that carries no valid coord.
    TouchPoints pts = drv.getTouchPoints();
    if (pts.getPointCount() > 0) {
        const TouchPoint &p = pts.getPoint(0);
        x = p.x;
        y = p.y;
        return true;
    }
    return false;
}

}  // namespace touch
