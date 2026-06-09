#pragma once

#include <AisTargetStore.h>

// BLE peripheral for the AIS-radar device. Advertises one service and
// notifies own-ship state + AIS targets to the iOS central. See
// shared/ble/AisRadarBle.h for the wire contract.

namespace ble {

// Host (phone) GPS, decoded from the last BLE write. Valid only if recent.
struct HostGps {
    double lat;
    double lon;
    double cogDeg;   // NAN = n/a
    double sogKn;    // NAN = n/a
};

void begin();

// Returns the phone GPS if a write arrived within the freshness window.
bool hostGps(HostGps* out);

// Notify own ship + every live target. No-op when no central is connected.
// threatLevel (0-3, from radar::assessWorst) is packed into the own-ship
// flags so the app shows the same background colour.
void publish(AisTargetStore& store, double ownLat, double ownLon,
             double ownCogDeg, double ownSogKn, bool haveFix, int threatLevel);

bool connected();

}  // namespace ble
