// NmeaBridge — bridges a data source to BoatState.
//
// Four mutually-exclusive backends, selected at compile time:
//
//   * Production NMEA 2000 stack (default; all DATA_SOURCE_* off)
//     Owns a tNMEA2000 instance, registers PGN handlers, and runs
//     ParseMessages() on its own FreeRTOS task pinned to core 0.
//
//   * SIMULATED_DATA=1 — no NMEA 2000 stack at all. begin() is a no-op;
//     simulateTick() (called from the main loop) feeds fake values into
//     BoatState. Useful before the transceiver is wired up, and keeps the
//     sim firmware buildable on ESP32-S3 even when the CAN backend isn't
//     S3-compatible.
//
//   * DATA_SOURCE_BLE=1 (step 4) — NmeaBridge becomes a NimBLE central:
//     it scans for "esp32-boat-tx", subscribes to the five telemetry
//     NOTIFY characteristics from include/BoatBle.h, parses each PDU,
//     and pushes values into BoatState through the same setters. On
//     disconnect it calls state_.invalidateLiveData() so the UI shows
//     "—" instead of stale numbers.
//
//   * DATA_SOURCE_WIFI=1 (round 81) — NmeaBridge joins WiFi softAP
//     _wifi_nmea2k as a station, subscribes to the virtual N2K bus
//     multicast group (239.255.78.85:60001), parses canboat-style
//     JSON packets (one PGN per packet) and routes them into the same
//     BoatState setters. Supersedes the BLE path per
//     docs/adr/0001-wifi-not-ble.md.

#pragma once

#include <cstdint>

#include "BoatState.h"
#include "config.h"
#include "Settings.h"   // round 85 v1.6 step 1 (ADR-0013)

#if !SIMULATED_DATA && !DATA_SOURCE_BLE && !DATA_SOURCE_WIFI
class tN2kMsg;
class tNMEA2000;
#endif

class NmeaBridge {
public:
    // BoatState lifetime must outlive the bridge.
    explicit NmeaBridge(BoatState& state);

    // Bring up the configured backend. NMEA2000 mode opens the bus +
    // spawns a parse task; sim mode is a no-op; BLE mode starts a scan.
    bool begin();

#if SIMULATED_DATA
    // Drive BoatState with fake values. Called from the main loop when
    // SIMULATED_DATA is set — lets you exercise the UI without the boat.
    void simulateTick();
#endif

#if DATA_SOURCE_BLE
    // Channels for bleNotifyCount(). Order matches the order the
    // BoatBle.h characteristics are subscribed to.
    enum BleChannel : uint8_t {
        BLE_CH_WIND        = 0,
        BLE_CH_GPS         = 1,
        BLE_CH_HEADING     = 2,
        BLE_CH_DEPTH_TEMP  = 3,
        BLE_CH_ATTITUDE    = 4,
        BLE_CH_COUNT       = 5,
    };

    // Status query API for the Communication page. Thread-safe by being
    // word-atomic reads of file-scope volatile state — see NmeaBridge.cpp
    // for the writer side (NimBLE host task) and reader side (UI / main).
    bool         bleConnected();        // true while a peripheral is linked
    const char*  blePeerMac();          // "" when disconnected
    int          bleRssi();             // INT16_MIN when no link / unknown
    uint32_t     bleNotifyCount(BleChannel ch);

    // Called from the main loop — drives scan→connect→subscribe and the
    // post-disconnect rescan, which can't run safely from inside NimBLE
    // host-task callbacks on the 1.4.x line.
    void bleTick();
#endif

#if DATA_SOURCE_WIFI
    // Per-PGN-group counters for the Communication page. Mirrors the
    // BLE channel layout so the page renders identically.
    enum WifiChannel : uint8_t {
        WIFI_CH_WIND        = 0,    // PGN 130306
        WIFI_CH_GPS         = 1,    // PGN 129025 + 129026
        WIFI_CH_HEADING     = 2,    // PGN 127250 + 128259
        WIFI_CH_DEPTH_TEMP  = 3,    // PGN 128267 + 130316
        WIFI_CH_ATTITUDE    = 4,    // PGN 127257 + 127251 + 127245 + 127489
        WIFI_CH_COUNT       = 5,
    };

    bool         wifiConnected();          // true while STA is associated AND
                                           // at least one packet was seen
                                           // within the staleness window
    const char*  wifiPeerName();           // last 'peer' field seen, or ""
    int          wifiRssi();               // dBm or INT16_MIN
    uint32_t     wifiPacketCount(WifiChannel ch);

    // Role-election state (round 83).
    const char*  wifiRoleName();           // "AP" / "STA" / "electing"
    int          wifiPeerCount();          // peers in the negotiator's table
    int          wifiStationCount();       // when AP, # of associated STAs

    // Round 85 — round-85 Comm page additions. Local interface IP
    // string ("0.0.0.0" before bound) and the radio channel we're
    // on (1..13 typically, 0 if not yet associated).
    const char*  wifiLocalIp();
    int          wifiChannel();

    // Round 85 v1.6 step 1 (ADR-0013): the live settings snapshot. Same
    // contract as src_converter/WifiPublisher.cpp's currentSettings() —
    // consumers read fields here to react to iOS-pushed config changes.
    uint32_t                  settingsVersion();
    const settings::Settings& currentSettings();

    // Called from the main loop. Pumps the UDP receive buffer, dispatches
    // each JSON packet to a setter, and watches for staleness so the UI
    // can blank when the transmitter goes away.
    void wifiTick();
#endif

private:
    BoatState&  state_;

#if !SIMULATED_DATA && !DATA_SOURCE_BLE && !DATA_SOURCE_WIFI
    tNMEA2000*  n2k_ = nullptr;

    static void taskTrampoline(void* arg);
    void taskLoop();

    static NmeaBridge* instance_;
    static void handleMsg(const tN2kMsg& msg);
#endif
};

#if SIMULATED_DATA
// Round 72 — per-channel enables for the sim. Five logical streams map onto
// the five state_.set*() calls inside simulateTick():
//
//   gps     → setGps(lat, lon)               (also gates SOG/COG since they
//                                             are derived from GPS deltas)
//   wind    → setApparentWind(awa, aws)      (also gates derived TWA/TWS/
//                                             TWD/VMG)
//   heading → setMagneticHeading + setMagneticVariation
//   stw     → setStw(stw)
//   depth   → setDepth(depth_m) + setSeaTemp(water_temp_c)   (round 78
//             follow-up split: depth at 1 Hz, sea temp at 0.5 Hz, both
//             gated on this same flag)
//
// When a flag is false the setter is skipped AND the matching PGN log
// entries are suppressed, so the Debug page also shows the channel going
// silent. Default is all-true (matches pre-round-72 behaviour). The
// simulator page wires checkboxes to these so you can verify "what does the
// UI do when GPS drops?" on the bench.
struct SimEnables {
    bool gps     = true;
    bool wind    = true;
    bool heading = true;
    bool stw     = true;
    bool depth   = true;
};
extern SimEnables g_sim_enables;
#endif
