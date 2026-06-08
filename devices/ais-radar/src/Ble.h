#pragma once

#include <AisTargetStore.h>

// BLE peripheral for the AIS-radar device. Advertises one service and
// notifies own-ship state + AIS targets to the iOS central. See
// shared/ble/AisRadarBle.h for the wire contract.

namespace ble {

void begin();

// Notify own ship + every live target. No-op when no central is connected.
// threatLevel (0-3, from radar::assessWorst) is packed into the own-ship
// flags so the app shows the same background colour.
void publish(AisTargetStore& store, double ownLat, double ownLon,
             double ownCogDeg, bool haveFix, int threatLevel);

bool connected();

}  // namespace ble
