#include "NmeaBridge.h"

#include <Arduino.h>
#include <cmath>
#include <cstdio>

#include "magnetic_variation.h"

#if !SIMULATED_DATA && !DATA_SOURCE_BLE && !DATA_SOURCE_WIFI
#include <NMEA2000_CAN.h>   // pulls in the backend for the current platform
#include <N2kMessages.h>    // Parse* helpers + msToKnots / radToDeg already defined here
#endif

namespace {
inline double normalizeDeg(double d) {
    while (d < -180.0) d += 360.0;
    while (d >  180.0) d -= 360.0;
    return d;
}
}  // namespace

NmeaBridge::NmeaBridge(BoatState& state) : state_(state) {}

// ============================================================================
// Production build — real NMEA 2000 stack.
// ============================================================================
#if !SIMULATED_DATA && !DATA_SOURCE_BLE && !DATA_SOURCE_WIFI

NmeaBridge* NmeaBridge::instance_ = nullptr;

bool NmeaBridge::begin() {
    instance_ = this;
    n2k_ = &NMEA2000;

    n2k_->SetProductInformation(
        "ESP32-BOAT-001", N2K_PRODUCT_CODE, N2K_MODEL_ID,
        N2K_SW_VERSION, N2K_MODEL_VERSION);

    n2k_->SetDeviceInformation(
        N2K_SERIAL_CODE,
        130,    // Device function: Display
        120,    // Device class:    Display
        2046);  // Manufacturer ID (2046 = reserved / experimental)

    n2k_->SetMode(tNMEA2000::N2km_ListenOnly);
    n2k_->EnableForward(false);
    n2k_->SetMsgHandler(&NmeaBridge::handleMsg);

    if (!n2k_->Open()) return false;

    xTaskCreatePinnedToCore(&NmeaBridge::taskTrampoline,
                            "n2k", 4096, this, 5, nullptr, 0);
    return true;
}

void NmeaBridge::taskTrampoline(void* arg) {
    static_cast<NmeaBridge*>(arg)->taskLoop();
}

void NmeaBridge::taskLoop() {
    for (;;) {
        n2k_->ParseMessages();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void NmeaBridge::handleMsg(const tN2kMsg& msg) {
    if (!instance_) return;
    auto& s = instance_->state_;
    const uint8_t src = msg.Source;
    char summary[56];
    summary[0] = '\0';

    switch (msg.PGN) {
        // ----------------------------------------------------------------- GPS
        case 129025L: { // Position Rapid Update
            double lat = NAN, lon = NAN;
            if (ParseN2kPGN129025(msg, lat, lon)) {
                // Round 56: setGps() now takes ONLY the position; SOG/COG
                // fall out of the differential between consecutive fixes
                // inside BoatState. PGN 129026 (sensor-supplied COG/SOG)
                // is ignored below.
                s.setGps(lat, lon);
                // Magnetic variation from the WMM-on-SD lookup (round-49
                // stub: Copenhagen-area constant). Re-runs on every fix
                // so a long passage's variation drift gets picked up.
                const double var = navmath::lookupMagneticVariation(lat, lon);
                if (!isnan(var)) s.setMagneticVariation(var);
                snprintf(summary, sizeof(summary), "GPS lat=%.4f lon=%.4f", lat, lon);
            } else {
                snprintf(summary, sizeof(summary), "GPS (parse failed)");
            }
            break;
        }
        case 129026L: { // COG & SOG Rapid Update
            // Round 56: ignored. The device computes its own COG/SOG from
            // PGN 129025 deltas; trusting a sensor-supplied value would
            // double-source the same quantity and mask GPS-fix dropouts.
            unsigned char sid;
            tN2kHeadingReference ref;
            double cog_rad = NAN, sog_ms = NAN;
            if (ParseN2kPGN129026(msg, sid, ref, cog_rad, sog_ms)) {
                snprintf(summary, sizeof(summary),
                         "COG/SOG cog=%.0f sog=%.1fkn (ignored)",
                         RadToDeg(cog_rad), msToKnots(sog_ms));
            } else {
                snprintf(summary, sizeof(summary), "COG/SOG (parse failed)");
            }
            break;
        }

        // ---------------------------------------------------------------- Wind
        case 130306L: { // Wind Data
            unsigned char sid;
            double wind_speed_ms = NAN, wind_angle_rad = NAN;
            tN2kWindReference ref;
            if (ParseN2kPGN130306(msg, sid, wind_speed_ms, wind_angle_rad, ref)) {
                const double speed_kn  = msToKnots(wind_speed_ms);
                const double angle_deg = normalizeDeg(RadToDeg(wind_angle_rad));
                // Round 53: only consume APPARENT wind from sensors. True
                // wind is derived in BoatState from AWA/AWS + boat motion;
                // a chartplotter publishing pre-computed true wind would
                // duplicate that work and disagree with our derivation,
                // so we ignore it here.
                if (ref == N2kWind_Apparent) {
                    s.setApparentWind(angle_deg, speed_kn);
                    snprintf(summary, sizeof(summary), "Wind AWA=%+.0f spd=%.1fkn",
                             angle_deg, speed_kn);
                } else {
                    snprintf(summary, sizeof(summary), "Wind ref=%u (ignored)",
                             static_cast<unsigned>(ref));
                }
            } else {
                snprintf(summary, sizeof(summary), "Wind (parse failed)");
            }
            break;
        }

        // --------------------------------------------------------------- Depth
        case 128267L: { // Water Depth
            unsigned char sid;
            double depth_below_transducer_m = NAN, offset_m = NAN, range_m = NAN;
            if (ParseN2kPGN128267(msg, sid, depth_below_transducer_m, offset_m, range_m)) {
                // Round 78 follow-up: depth-only setter. Sea temp now
                // comes in via PGN 130316 → setSeaTemp() below, with
                // its own timestamp tracking for the honeycomb's Hz.
                s.setDepth(depth_below_transducer_m);
                snprintf(summary, sizeof(summary), "Depth %.1fm", depth_below_transducer_m);
            } else {
                snprintf(summary, sizeof(summary), "Depth (parse failed)");
            }
            break;
        }
        case 130316L: { // Temperature, Extended Range
            unsigned char sid;
            unsigned char temp_instance;
            tN2kTempSource temp_src;
            double temp_k = NAN, set_temp_k = NAN;
            if (ParseN2kPGN130316(msg, sid, temp_instance, temp_src, temp_k, set_temp_k)) {
                if (temp_src == N2kts_SeaTemperature) {
                    // Round 78 follow-up: split-out sea-temp setter.
                    // No more snapshot+setDepth dance — sea temp owns
                    // its own field + timestamp now.
                    s.setSeaTemp(temp_k - 273.15);
                } else if (temp_src == N2kts_OutsideTemperature) {
                    // Round 78 — populate the air-temp hex on the PGN
                    // page when a real outside-temp probe is on the bus.
                    s.setAirTemp(temp_k - 273.15);
                }
                snprintf(summary, sizeof(summary), "Temp src=%u t=%.1fC",
                         static_cast<unsigned>(temp_src), temp_k - 273.15);
            } else {
                snprintf(summary, sizeof(summary), "Temp (parse failed)");
            }
            break;
        }

        // ------------------------------------------------------- Heading / STW
        case 127250L: { // Vessel Heading
            unsigned char sid;
            double heading_rad = NAN, dev_rad = NAN, var_rad = NAN;
            tN2kHeadingReference ref;
            if (ParseN2kPGN127250(msg, sid, heading_rad, dev_rad, var_rad, ref)) {
                // Round 53: store the RAW magnetic heading. True heading is
                // derived in BoatState from heading_mag + variation.
                // Variation can come from this same PGN (when the boat's
                // chartplotter knows it) or from the WMM lookup tied to GPS.
                if (ref == N2khr_magnetic && !isnan(heading_rad)) {
                    s.setMagneticHeading(normalizeDeg(RadToDeg(heading_rad)));
                }
                if (!isnan(var_rad)) {
                    s.setMagneticVariation(RadToDeg(var_rad));
                }
                snprintf(summary, sizeof(summary),
                         "Heading %.0f%s var=%.1f",
                         RadToDeg(heading_rad),
                         ref == N2khr_magnetic ? "M" : "T",
                         RadToDeg(var_rad));
            } else {
                snprintf(summary, sizeof(summary), "Heading (parse failed)");
            }
            break;
        }
        case 128259L: { // Speed Through Water
            unsigned char sid;
            double stw_ms = NAN, sog_ms = NAN;
            tN2kSpeedWaterReferenceType ref;
            if (ParseN2kPGN128259(msg, sid, stw_ms, sog_ms, ref)) {
                s.setStw(msToKnots(stw_ms));
                snprintf(summary, sizeof(summary), "STW %.1fkn", msToKnots(stw_ms));
            } else {
                snprintf(summary, sizeof(summary), "STW (parse failed)");
            }
            break;
        }

        // ---------------------------------------------------------------- AIS
        // TODO(v1.1): AIS PGNs 129038 / 129039 / 129809 / 129810.
        // The Parse signatures in NMEA2000-library 4.22 take many more
        // parameters (tN2kAISRepeat, tN2kAISTransceiverInformation,
        // tN2kAISUnit, Display/DSC/Band/Msg22/Mode/State flags). Wire them up
        // properly once the instrument pages are validated on real data.

        default:
            snprintf(summary, sizeof(summary), "(undecoded)");
            break;
    }

    s.logPgn(static_cast<uint32_t>(msg.PGN), src, summary);
}

#endif // !SIMULATED_DATA && !DATA_SOURCE_BLE && !DATA_SOURCE_WIFI


// ============================================================================
// BLE-bridged build (step 4) — NimBLE central; subscribes to esp32-boat-tx.
// ============================================================================
#if DATA_SOURCE_BLE

#include <NimBLEDevice.h>
#include <BoatBle.h>           // shared wire protocol (include/ on the path)
#include <cstdint>             // INT16_MIN
#include <cstring>             // memcpy

namespace {

// ---- Tunables --------------------------------------------------------------

// We scan continuously when there's no link. setActiveScan() makes the
// peripheral respond with its scan response (which carries the name) — we
// need that to match by name. 100 ms / 100 ms = 100% duty cycle.
constexpr uint16_t kScanIntervalMs = 100;
constexpr uint16_t kScanWindowMs   = 100;

// On disconnect, NimBLE delivers a callback first; we then restart the
// scan from bleTick() (not from the callback — re-entering NimBLE host
// task from a callback is dicey on 1.4.x). The flag below routes the
// work over to the main loop.
constexpr uint32_t kReconnectGuardMs = 250;

// ---- File-scope state ------------------------------------------------------
//
// NimBLE's host task drives the writers (advertising, connect, notify
// callbacks). The main loop (UI / status getters) is the reader. We keep
// reader-visible state in plain `volatile` variables; 32-bit reads/writes
// on the S3 are atomic, so this is safe without explicit barriers. The
// peer-MAC string is a fixed-size buffer and a slightly racy read can at
// worst show the previous link's address for one frame — acceptable.

BoatState*        g_bleState        = nullptr;  // shortcut into bridge.state_
NimBLEClient*     g_client          = nullptr;
NimBLEAdvertisedDevice* g_targetDev = nullptr;  // populated by advert callback
volatile bool     g_wantConnect     = false;    // hand-off flag
volatile bool     g_wantReconnect   = false;    // restart scan after a drop
uint32_t          g_reconnectAfter  = 0;        // millis() guard before reconnect
volatile bool     g_connected       = false;
volatile int      g_rssi            = INT16_MIN;
volatile uint32_t g_notifyCount[NmeaBridge::BLE_CH_COUNT] = {0};
char              g_peerMac[18]     = {0};      // "AA:BB:CC:DD:EE:FF\0"

// Handles to the five NOTIFY characteristics, captured on connect.
NimBLERemoteCharacteristic* g_charWind      = nullptr;
NimBLERemoteCharacteristic* g_charGps       = nullptr;
NimBLERemoteCharacteristic* g_charHeading   = nullptr;
NimBLERemoteCharacteristic* g_charDepthTemp = nullptr;
NimBLERemoteCharacteristic* g_charAttitude  = nullptr;

// Convert raw fixed-point integers from BoatBle.h PDUs to the doubles
// BoatState wants. Trivial scales — kept as inlines for clarity.
inline double deg10ToDeg(int16_t v)     { return v * 0.1; }
inline double deg10ToDegU(uint16_t v)   { return v * 0.1; }
inline double kt100ToKt(uint16_t v)     { return v * 0.01; }
inline double m10ToM(uint16_t v)        { return v * 0.1; }
inline double c10ToC(int16_t v)         { return v * 0.1; }
inline double e7ToDeg(int32_t v)        { return v * 1e-7; }

// ---- Notification handlers (one per characteristic) -----------------------
//
// Each runs on the NimBLE host task. We decode the PDU, run the masked
// fields through BoatState's setters (which take the mutex internally),
// and bump the per-channel counter. If the PDU length doesn't match, we
// drop it silently — partial frames aren't actionable here.

// BoatBle.h valid_mask bits (verified against include/BoatBle.h):
//   Wind:        bit 0 TWA, 1 TWS, 2 TWD, 3 AWA, 4 AWS
//   Gps:         bit 0 LAT, 1 LON, 2 COG, 3 SOG
//   Heading:     bit 0 HDG, 1 BSPD
//   DepthTemp:   bit 0 DEP, 1 AIR-T, 2 SEA-T
//   Attitude:    bit 0 HEEL, 1 PITCH, 2 ROT (unused on RX for v1)

void onNotifyWind(NimBLERemoteCharacteristic*,
                  uint8_t* data, size_t len, bool) {
    if (len != sizeof(boatble::WindPdu) || !g_bleState) return;
    boatble::WindPdu pdu;
    memcpy(&pdu, data, sizeof(pdu));
    const bool haveAwa = (pdu.valid_mask & (1 << 3)) != 0;
    const bool haveAws = (pdu.valid_mask & (1 << 4)) != 0;
    if (haveAwa && haveAws) {
        g_bleState->setApparentWind(deg10ToDeg(pdu.awa_deg10),
                                    kt100ToKt(pdu.aws_kt100));
    }
    g_notifyCount[NmeaBridge::BLE_CH_WIND]++;
}

void onNotifyGps(NimBLERemoteCharacteristic*,
                 uint8_t* data, size_t len, bool) {
    if (len != sizeof(boatble::GpsPdu) || !g_bleState) return;
    boatble::GpsPdu pdu;
    memcpy(&pdu, data, sizeof(pdu));
    const bool haveLat = (pdu.valid_mask & (1 << 0)) != 0;
    const bool haveLon = (pdu.valid_mask & (1 << 1)) != 0;
    if (haveLat && haveLon) {
        g_bleState->setGps(e7ToDeg(pdu.lat_e7), e7ToDeg(pdu.lon_e7));
    }
    g_notifyCount[NmeaBridge::BLE_CH_GPS]++;
}

void onNotifyHeading(NimBLERemoteCharacteristic*,
                     uint8_t* data, size_t len, bool) {
    if (len != sizeof(boatble::HeadingPdu) || !g_bleState) return;
    boatble::HeadingPdu pdu;
    memcpy(&pdu, data, sizeof(pdu));
    if (pdu.valid_mask & (1 << 0)) {
        g_bleState->setMagneticHeading(deg10ToDegU(pdu.hdg_deg10));
    }
    if (pdu.valid_mask & (1 << 1)) {
        g_bleState->setStw(kt100ToKt(pdu.bspd_kt100));
    }
    g_notifyCount[NmeaBridge::BLE_CH_HEADING]++;
}

void onNotifyDepthTemp(NimBLERemoteCharacteristic*,
                       uint8_t* data, size_t len, bool) {
    if (len != sizeof(boatble::DepthTempPdu) || !g_bleState) return;
    boatble::DepthTempPdu pdu;
    memcpy(&pdu, data, sizeof(pdu));
    if (pdu.valid_mask & (1 << 0)) g_bleState->setDepth(m10ToM(pdu.dep_m10));
    if (pdu.valid_mask & (1 << 1)) g_bleState->setAirTemp(c10ToC(pdu.air_temp_c10));
    if (pdu.valid_mask & (1 << 2)) g_bleState->setSeaTemp(c10ToC(pdu.sea_temp_c10));
    g_notifyCount[NmeaBridge::BLE_CH_DEPTH_TEMP]++;
}

void onNotifyAttitude(NimBLERemoteCharacteristic*,
                      uint8_t* data, size_t /*len*/, bool) {
    // We don't push heel/pitch/etc. into BoatState yet — the existing UI
    // doesn't render them. Counter increment + drop is enough to prove
    // the channel is alive on the Communication page.
    (void)data;
    if (!g_bleState) return;
    g_notifyCount[NmeaBridge::BLE_CH_ATTITUDE]++;
}

// ---- Advertising scan callback --------------------------------------------
class AdvertCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveName() || dev->getName() != BOAT_BLE_DEVICE_NAME) return;
        // Match — stop scanning and hand the device off to bleTick().
        NimBLEDevice::getScan()->stop();
        g_targetDev   = dev;
        g_wantConnect = true;
    }
};
AdvertCallbacks g_advertCb;

// ---- Client connect/disconnect --------------------------------------------
class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
        g_connected = true;
        const auto addr = c->getPeerAddress();
        // toString() returns "aa:bb:cc:dd:ee:ff" lowercase; we copy in
        // upper-case for the UI to be a bit more readable.
        std::string s = addr.toString();
        size_t i = 0;
        for (; i < s.size() && i < sizeof(g_peerMac) - 1; ++i) {
            char ch = s[i];
            if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
            g_peerMac[i] = ch;
        }
        g_peerMac[i] = '\0';
    }
    void onDisconnect(NimBLEClient*) override {
        g_connected      = false;
        g_rssi           = INT16_MIN;
        g_peerMac[0]     = '\0';
        g_charWind = g_charGps = g_charHeading = nullptr;
        g_charDepthTemp = g_charAttitude = nullptr;
        if (g_bleState) g_bleState->invalidateLiveData();
        // Hand the reconnect to bleTick() — re-entering NimBLE from this
        // callback context is unsafe on 1.4.x.
        g_reconnectAfter = millis() + kReconnectGuardMs;
        g_wantReconnect  = true;
    }
};
ClientCallbacks g_clientCb;

// ---- One-shot connect (driven from bleTick) -------------------------------
//
// 1. Connect to the device captured by the advert callback.
// 2. Walk the service, find each of the five NOTIFY chars by UUID.
// 3. Subscribe each with its notify callback.
// 4. If anything fails along the way, disconnect and let the disconnect
//    callback restart the scan.

bool subscribeAll() {
    if (!g_client) return false;
    NimBLERemoteService* svc = g_client->getService(BOAT_BLE_SERVICE_UUID);
    if (!svc) return false;

    struct Slot {
        const char*                   uuid;
        NimBLERemoteCharacteristic**  out;
        void (*cb)(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool);
    } slots[] = {
        { BOAT_BLE_WIND_UUID,        &g_charWind,      onNotifyWind        },
        { BOAT_BLE_GPS_UUID,         &g_charGps,       onNotifyGps         },
        { BOAT_BLE_HEADING_UUID,     &g_charHeading,   onNotifyHeading     },
        { BOAT_BLE_DEPTH_TEMP_UUID,  &g_charDepthTemp, onNotifyDepthTemp   },
        { BOAT_BLE_ATTITUDE_UUID,    &g_charAttitude,  onNotifyAttitude    },
    };
    for (auto& slot : slots) {
        NimBLERemoteCharacteristic* ch = svc->getCharacteristic(slot.uuid);
        if (!ch || !ch->canNotify() || !ch->subscribe(true, slot.cb)) {
            Serial.printf("[ble] subscribe failed for %s\n", slot.uuid);
            return false;
        }
        *slot.out = ch;
    }
    return true;
}

void startScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&g_advertCb, false);
    scan->setActiveScan(true);
    scan->setInterval(kScanIntervalMs);
    scan->setWindow(kScanWindowMs);
    scan->start(0 /*duration: continuous*/, nullptr, false);
}

}  // namespace

bool NmeaBridge::begin() {
    Serial.println(F("[ble] NmeaBridge starting as BLE central"));
    g_bleState = &state_;

    NimBLEDevice::init("");  // we don't advertise; empty local name is fine
    NimBLEDevice::setPower(ESP_PWR_LVL_P3);

    g_client = NimBLEDevice::createClient();
    g_client->setClientCallbacks(&g_clientCb, false);

    startScan();
    Serial.println(F("[ble] scanning for esp32-boat-tx..."));
    return true;
}

// Called from main loop. Drives the connect / reconnect state machine —
// NimBLE callbacks can't safely re-enter the host task, so connect()
// and rescan() happen here on the Arduino main task.
void NmeaBridge::bleTick() {
    if (g_wantConnect) {
        g_wantConnect = false;
        if (!g_client || !g_targetDev) return;
        Serial.printf("[ble] connecting to %s\n",
                      g_targetDev->getAddress().toString().c_str());
        if (g_client->connect(g_targetDev)) {
            if (!subscribeAll()) {
                Serial.println(F("[ble] subscribe-all failed; disconnecting"));
                g_client->disconnect();
                // disconnect callback handles the rescan.
                return;
            }
            // Cache RSSI once at connect time; we don't poll it because
            // every getRssi() is a synchronous L2CAP round trip.
            g_rssi = g_client->getRssi();
            Serial.printf("[ble] connected (rssi=%d dBm)\n", g_rssi);
        } else {
            Serial.println(F("[ble] connect failed; rescanning"));
            startScan();
        }
        return;
    }
    if (g_wantReconnect && millis() >= g_reconnectAfter) {
        g_wantReconnect = false;
        Serial.println(F("[ble] rescanning"));
        startScan();
    }
}

bool NmeaBridge::bleConnected() {
    return g_connected;
}

const char* NmeaBridge::blePeerMac() {
    return g_peerMac;
}

int NmeaBridge::bleRssi() {
    return g_rssi;
}

uint32_t NmeaBridge::bleNotifyCount(BleChannel ch) {
    if (ch >= BLE_CH_COUNT) return 0;
    return g_notifyCount[ch];
}

#endif // DATA_SOURCE_BLE


// ============================================================================
// Simulated build — fake values, no NMEA 2000 stack.
// ============================================================================
#if SIMULATED_DATA

bool NmeaBridge::begin() {
    Serial.println(F("[sim] NMEA bridge running in SIMULATED_DATA mode"));
    return true;
}

// Round 72 — definition for the extern declared in NmeaBridge.h. Defaults
// match pre-round-72 behaviour (everything publishing); the UI's simulator-
// page checkboxes flip these at runtime.
SimEnables g_sim_enables;

// Synthesises one fake PGN event at a time so the debug screen sees a steady,
// human-readable trickle instead of a blur. Real bus traffic is rarely above
// ~50 msg/s; aim for roughly that.
namespace {
struct SimEvent {
    uint32_t pgn;
    uint32_t last_ms;
    uint32_t interval_ms;
};
SimEvent sim_events[] = {
    {129025L, 0,  100},   // Position Rapid Update: 10 Hz typical
    {129026L, 0,  250},   // COG/SOG: 4 Hz
    {130306L, 0,  100},   // Wind: 10 Hz (apparent only — true wind is derived)
    {128267L, 0, 1000},   // Depth: 1 Hz
    {130316L, 0, 2000},   // Water temp: 0.5 Hz
    {127250L, 0,  100},   // Heading (magnetic): 10 Hz
    {128259L, 0,  250},   // STW: 4 Hz
};
}  // namespace

void NmeaBridge::simulateTick() {
    static uint32_t t0 = millis();
    const uint32_t now = millis();
    const double t = (now - t0) / 1000.0;

    // Round 53/56: simulator outputs ONLY what real boat sensors publish:
    //   * GPS:                  lat, lon            (position only — SOG/COG
    //                                                fall out of consecutive
    //                                                fixes inside BoatState)
    //   * Apparent wind:        awa, aws           (masthead anemometer in
    //                                                boat-relative frame)
    //   * Magnetic compass:     hdg_m
    //   * Speed through water:  stw                 (paddlewheel)
    //   * Depth + sea temp:     depth, temp_c
    //
    // Round 56 — instead of hardcoding sog/cog we INTEGRATE a real boat
    // trajectory: the sim picks a slowly-varying compass bearing and
    // boat speed, then walks (lat, lon) along that vector at each tick.
    // The device's setGps() then derives SOG/COG from the differential,
    // and they come out matching the integrator's intent (within the
    // ±0.1 % of equirectangular vs. great-circle for our 100 ms / few-
    // metre baselines).
    static double sim_lat = 55.6761;
    static double sim_lon = 12.5683;
    static uint32_t prev_tick_ms = now;
    const double tick_dt_s = (now - prev_tick_ms) / 1000.0;
    prev_tick_ms = now;

    // True boat motion the sim is "intending":
    const double sim_speed_kn = 6.0 + 0.5 * sin(t * 0.30);   // ~5.5..6.5 kn
    const double sim_cog_deg  = 120.0 + 5.0 * sin(t * 0.05); // ~115..125° true

    // Walk the position. 1° lat ≈ 111 km; 1° lon ≈ 111 km × cos(lat).
    constexpr double kEarthR_m  = 6371008.8;
    constexpr double kKnToMS    = 1852.0 / 3600.0;
    const double speed_ms       = sim_speed_kn * kKnToMS;
    const double cog_rad        = sim_cog_deg  * M_PI / 180.0;
    const double dx_m = speed_ms * sin(cog_rad) * tick_dt_s;  // east
    const double dy_m = speed_ms * cos(cog_rad) * tick_dt_s;  // north
    const double mid_lat_rad    = sim_lat * M_PI / 180.0;
    sim_lat += dy_m / kEarthR_m * 180.0 / M_PI;
    sim_lon += dx_m / (kEarthR_m * cos(mid_lat_rad)) * 180.0 / M_PI;

    const double lat   = sim_lat;
    const double lon   = sim_lon;

    // Other raw sensor outputs.
    const double awa   = 45.0 + 10.0 * sin(t * 0.2);
    const double aws   = 12.0 +  2.0 * cos(t * 0.4);
    const double depth = 8.5 + 0.7 * sin(t * 0.1);
    const double temp_c = 12.3;
    const double hdg_m = normalizeDeg(115.0 + 5.0 * sin(t * 0.05));  // magnetic
    const double stw   = 5.9 + 0.4 * sin(t * 0.25);

    // Round 72 — per-channel enable gating. Skipped if the user
    // unchecked the channel's toggle button on the Sim page.
    //
    // Round 73 — per-channel publish CADENCE matching the NMEA 2000
    // spec transmission intervals. Before round 73 the setters fired
    // on every loop iteration (~30 Hz from the LVGL tick), which made
    // the labels visibly flicker and didn't reflect any real boat. The
    // intervals below are the standard NMEA 2000 rates (cross-checked
    // against canboat / Maretron / Garmin docs):
    //
    //   PGN 129025 Position Rapid Update     100 ms (10 Hz)
    //   PGN 130306 Wind Data                 100 ms (10 Hz)
    //   PGN 127250 Vessel Heading            100 ms (10 Hz)
    //   PGN 128259 Speed, Water Referenced  1000 ms (1 Hz)
    //   PGN 128267 Water Depth              1000 ms (1 Hz)
    //
    // (PGN 130316 Temperature is 2 s in the spec but setDepth() is
    // atomic over depth+temp, so we publish temp at the depth cadence
    // and let the 130316 log entry below keep its own 2 s cadence.)
    constexpr uint32_t kGpsIntervalMs     = 100;
    constexpr uint32_t kWindIntervalMs    = 100;
    constexpr uint32_t kHeadingIntervalMs = 100;
    constexpr uint32_t kStwIntervalMs     = 1000;
    constexpr uint32_t kDepthIntervalMs   = 1000;

    static uint32_t last_gps_pub_ms     = 0;
    static uint32_t last_wind_pub_ms    = 0;
    static uint32_t last_heading_pub_ms = 0;
    static uint32_t last_stw_pub_ms     = 0;
    static uint32_t last_depth_pub_ms   = 0;

    // Magnetic variation lookup is cheap; compute it unconditionally
    // (when heading is enabled) so the PGN 127250 log summary further
    // down can read it whether or not we actually published this tick.
    const double var_deg = g_sim_enables.heading
        ? navmath::lookupMagneticVariation(lat, lon)
        : NAN;

    if (g_sim_enables.gps && (now - last_gps_pub_ms >= kGpsIntervalMs)) {
        state_.setGps(lat, lon);
        last_gps_pub_ms = now;
    }
    if (g_sim_enables.wind && (now - last_wind_pub_ms >= kWindIntervalMs)) {
        state_.setApparentWind(awa, aws);
        last_wind_pub_ms = now;
    }
    if (g_sim_enables.heading &&
        (now - last_heading_pub_ms >= kHeadingIntervalMs)) {
        state_.setMagneticHeading(hdg_m);
        if (!std::isnan(var_deg)) state_.setMagneticVariation(var_deg);
        last_heading_pub_ms = now;
    }
    if (g_sim_enables.stw && (now - last_stw_pub_ms >= kStwIntervalMs)) {
        state_.setStw(stw);
        last_stw_pub_ms = now;
    }
    if (g_sim_enables.depth && (now - last_depth_pub_ms >= kDepthIntervalMs)) {
        state_.setDepth(depth);
        last_depth_pub_ms = now;
    }

    // Round 78 follow-up — sea temp now publishes at PGN 130316's
    // 2 s spec cadence (was piggybacked on depth's 1 Hz). Still gated
    // on g_sim_enables.depth: that toggle on the Sim page silences the
    // whole "water-instruments" group (depth + sea temp).
    constexpr uint32_t kSeaTempIntervalMs = 2000;
    static uint32_t last_sea_temp_pub_ms = 0;
    if (g_sim_enables.depth &&
        (now - last_sea_temp_pub_ms >= kSeaTempIntervalMs)) {
        state_.setSeaTemp(temp_c);
        last_sea_temp_pub_ms = now;
    }

    // Round 78 — outdoor air temperature. ENG-T / OIL-T are deliberately
    // NOT simulated (per round-78 spec: "make room for engine but don't
    // simulate it"); their hexes on the PGN page stay grey until a real
    // engine bridge lands. Air temp drifts slowly around 18 °C, published
    // every 2 s to match the NMEA 2000 PGN 130316 transmission interval.
    constexpr uint32_t kAirTempIntervalMs = 2000;
    static uint32_t last_air_pub_ms = 0;
    if (now - last_air_pub_ms >= kAirTempIntervalMs) {
        const double air_temp_c = 18.0 + 2.0 * sin(t * 0.05);
        state_.setAirTemp(air_temp_c);
        last_air_pub_ms = now;
    }

    // Fire simulated PGN log entries on their per-PGN intervals so the
    // debug screen sees a realistic-looking trickle.
    char summary[56];
    for (auto& e : sim_events) {
        if (now - e.last_ms < e.interval_ms) continue;
        e.last_ms = now;
        // Round 72 — suppress the log entry when the channel is off so
        // the Debug page reflects the same "channel went silent" state
        // the data labels show.
        bool channel_on = true;
        switch (e.pgn) {
            case 129025L: case 129026L: channel_on = g_sim_enables.gps;     break;
            case 130306L:               channel_on = g_sim_enables.wind;    break;
            case 128267L: case 130316L: channel_on = g_sim_enables.depth;   break;
            case 127250L:               channel_on = g_sim_enables.heading; break;
            case 128259L:               channel_on = g_sim_enables.stw;     break;
        }
        if (!channel_on) continue;
        switch (e.pgn) {
            case 129025L:
                snprintf(summary, sizeof(summary), "GPS lat=%.4f lon=%.4f", lat, lon);
                break;
            case 129026L:
                // Round 56: COG/SOG are now DERIVED inside BoatState from
                // GPS-position deltas; the sim used to publish them as
                // raw inputs but no longer does. Read what BoatState
                // computed for the log entry.
                {
                    auto snap = state_.snapshot();
                    snprintf(summary, sizeof(summary),
                             "COG/SOG (derived) cog=%.0f sog=%.1fkn",
                             snap.cog, snap.sog);
                }
                break;
            case 130306L:
                snprintf(summary, sizeof(summary), "Wind AWA=%+.0f spd=%.1fkn", awa, aws);
                break;
            case 128267L:
                snprintf(summary, sizeof(summary), "Depth %.1fm", depth);
                break;
            case 130316L:
                snprintf(summary, sizeof(summary), "Temp sea t=%.1fC", temp_c);
                break;
            case 127250L:
                snprintf(summary, sizeof(summary), "Heading %.0fM var=%.1f",
                         hdg_m, var_deg);
                break;
            case 128259L:
                snprintf(summary, sizeof(summary), "STW %.1fkn", stw);
                break;
            default:
                snprintf(summary, sizeof(summary), "(sim)");
                break;
        }
        state_.logPgn(e.pgn, /*src=*/0, summary);
    }
}

#endif // SIMULATED_DATA


// ============================================================================
// WiFi-bridged build (round 81) — STA on _wifi_nmea2k; consumes the virtual
// N2K bus multicast. See docs/adr/ and docs/VIRTUAL_BUS_WIRE.md for the
// wire spec.
// ============================================================================
#if DATA_SOURCE_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <cstdint>
#include <cstring>

#include <VirtualBusJson.h>
#include <wifi_credentials.h>
#include <VbusRole.h>
#include <RoleNegotiator.h>
#include <Settings.h>          // round 85 v1.6 step 1
#include <WebServer.h>
#include <DNSServer.h>         // round 85 v1.6 step 3 — captive-portal stub

namespace {

// Reading staleness — if no packets within this window, the UI treats
// the link as dead and we invalidate BoatState so the honeycomb shows "—".
constexpr uint32_t kStaleMs       = 3000;
constexpr size_t   kRxBufSize     = 512;

vbus::RoleNegotiator g_neg;

BoatState*       g_wifiState     = nullptr;
int              g_rx_sock       = -1;          // BSD UDP RX socket
int              g_send_sock     = -1;          // BSD UDP TX socket
bool             g_udpOpen       = false;       // proxy for g_rx_sock open
sockaddr_in      g_dest          = {};          // STA mode: AP IP:kBusPort
IPAddress        g_iface_ip;
char             g_iface_ip_str[16] = "0.0.0.0";

// Round 85 (v1.6 step 1, ADR-0013): canonical AP-owned settings + the
// HTTP server we expose on port 80 so iOS can POST /settings. The store
// is loaded from NVS in begin(); the server is spun up on bringUpAp()
// and torn down on dropAp() so a non-AP peer doesn't hold port 80.
settings::SettingsStore g_settings;
WebServer*              g_http = nullptr;

// Round 85 v1.6 step 3 — captive-portal stub. iOS aggressively drops
// WiFi networks it thinks have no internet (it then prefers cellular
// even with WiFi connected, which breaks our app since 192.168.4.1
// isn't routable over cellular). We make `_wifi_nmea2k` look like a
// normal internet network by (1) running a DNS server that answers
// every hostname with our AP IP, and (2) serving the small set of
// HTTP probes iOS uses to test connectivity. The probes return the
// exact "Success" payload Apple expects, so iOS classifies us as
// "online" and stops bouncing to cellular.
DNSServer* g_dns = nullptr;

// Round 84 (ADR-0010): when we're the AP, we maintain a small table of
// associated STA IPs (learned from the source address of every unicast
// packet that reaches us). On receive we fan out to every entry except
// the source (topology-based loop prevention). On local publish (sim,
// heartbeat) we fan out to all entries. Sized for 4 peers (softAP
// max_connection); +2 headroom for transient entries during handoff.
constexpr int    kMaxPeers       = 6;
constexpr uint32_t kPeerStaleMs  = 30000;       // ≥6 heartbeat intervals
struct PeerAddr {
    uint32_t addr;       // sin_addr.s_addr in network byte order; 0 = empty
    uint32_t last_seen;
};
PeerAddr         g_peers[kMaxPeers] = {};

bool             g_scan_attempted = false;
uint32_t         g_last_ap_scan_ms = 0;     // task #36: dual-AP detect cadence
volatile bool    g_sta_was_connected = false;
volatile bool    g_dataFresh     = false;
volatile uint32_t g_lastPacketMs = 0;
volatile uint32_t g_pktCount[NmeaBridge::WIFI_CH_COUNT] = {0};
char             g_lastPeer[24]  = {0};
char             g_rxBuf[kRxBufSize];

// Cross-task flags. wifiTick() (Arduino main task, core 1) sets these
// on WiFi state transitions; drainTask (core 0) consumes them.
volatile bool    g_wantOpenUdp   = false;
volatile bool    g_wantCloseUdp  = false;
TaskHandle_t     g_drainTask     = nullptr;

NmeaBridge::WifiChannel channelForPgn(int64_t pgn) {
    switch (pgn) {
        case 130306: return NmeaBridge::WIFI_CH_WIND;
        case 129025: case 129026: return NmeaBridge::WIFI_CH_GPS;
        case 127250: case 128259: return NmeaBridge::WIFI_CH_HEADING;
        case 128267: case 130316: return NmeaBridge::WIFI_CH_DEPTH_TEMP;
        case 127257: case 127251: case 127245: case 127489:
            return NmeaBridge::WIFI_CH_ATTITUDE;
        default:     return NmeaBridge::WIFI_CH_COUNT;  // sentinel: unknown
    }
}

// Per-PGN dispatch into BoatState. Mirrors the BLE handlers' field set —
// we only consume what the existing UI actually renders. Unknown / unused
// fields are ignored without warning so the wire format can grow without
// rebuilding the RX.
void dispatch(int64_t pgn, const char* fields) {
    if (!g_wifiState) return;
    switch (pgn) {
        case 130306: {
            char ref[24] = {0};
            double speedMs = 0, angleRad = 0;
            if (!vbus::findString(fields, "reference", ref, sizeof(ref))) return;
            if (strcmp(ref, "Apparent") != 0) return;  // RX only uses apparent
            if (!vbus::findDouble(fields, "windSpeed", &speedMs)) return;
            if (!vbus::findDouble(fields, "windAngle", &angleRad)) return;
            g_wifiState->setApparentWind(vbus::radToDeg(angleRad),
                                         vbus::msToKnots(speedMs));
            break;
        }
        case 129025: {
            double lat = 0, lon = 0;
            if (!vbus::findDouble(fields, "latitude",  &lat)) return;
            if (!vbus::findDouble(fields, "longitude", &lon)) return;
            g_wifiState->setGps(lat, lon);
            // Magnetic variation isn't on the wire — derive locally from
            // position so heading_true = heading_mag + variation isn't NaN.
            // Without this the main page's heading reads "---" even with
            // PGN 127250 arriving normally.
            const double var = navmath::lookupMagneticVariation(lat, lon);
            if (!std::isnan(var)) g_wifiState->setMagneticVariation(var);
            break;
        }
        case 127250: {
            char ref[24] = {0};
            double hdgRad = 0;
            if (!vbus::findDouble(fields, "heading", &hdgRad)) return;
            // BoatState only consumes magnetic heading; if the publisher
            // sends a true-heading row we drop it (the simulator emits
            // Magnetic per the wire spec, so this is the common case).
            if (vbus::findString(fields, "reference", ref, sizeof(ref))
                && strcmp(ref, "Magnetic") != 0) return;
            g_wifiState->setMagneticHeading(vbus::radToDeg(hdgRad));
            break;
        }
        case 128259: {
            double speedMs = 0;
            if (!vbus::findDouble(fields, "speedWaterReferenced", &speedMs))
                return;
            g_wifiState->setStw(vbus::msToKnots(speedMs));
            break;
        }
        case 128267: {
            double depthM = 0;
            if (!vbus::findDouble(fields, "depth", &depthM)) return;
            g_wifiState->setDepth(depthM);
            break;
        }
        case 130316: {
            char src[40] = {0};
            double tK = 0;
            if (!vbus::findString(fields, "source", src, sizeof(src))) return;
            if (!vbus::findDouble(fields, "actualTemperature", &tK)) return;
            const double tC = vbus::kelvinToCelsius(tK);
            if      (strcmp(src, "Sea Temperature")     == 0) g_wifiState->setSeaTemp(tC);
            else if (strcmp(src, "Outside Temperature") == 0) g_wifiState->setAirTemp(tC);
            break;
        }
        // Round 85 (v1.5b step 4): AIS dispatch. Wire format follows
        // CANboat field names; the converter publishes these onto the
        // virtual bus via its ADR-0012 replay (in-flight — until that
        // ships, this branch is exercised only via fan-out from another
        // peer that publishes AIS, including the iOS app).
        //
        // Position reports — 129038 (Class A), 129039 (Class B), 129040
        // (Class B extended) share the same lat/lon/sog/cog shape. We
        // upsert against MMSI and let BoatState's LRU handle eviction.
        case 129038:
        case 129039:
        case 129040: {
            int64_t mmsi = 0;
            double  lat = NAN, lon = NAN, sog_ms = NAN, cog_rad = NAN;
            if (!vbus::findInt(fields, "userId", &mmsi) || mmsi <= 0) break;
            vbus::findDouble(fields, "latitude",  &lat);
            vbus::findDouble(fields, "longitude", &lon);
            vbus::findDouble(fields, "sog",       &sog_ms);
            vbus::findDouble(fields, "cog",       &cog_rad);
            AisTarget t{};
            t.mmsi         = (uint32_t)mmsi;
            t.lat          = lat;
            t.lon          = lon;
            t.sog          = std::isnan(sog_ms)  ? NAN : vbus::msToKnots(sog_ms);
            t.cog          = std::isnan(cog_rad) ? NAN : vbus::radToDeg(cog_rad);
            t.last_seen_ms = millis();
            // PGN 129040 carries vessel name too — peek for it before upsert.
            if (pgn == 129040) {
                char nm[20] = {0};
                if (vbus::findString(fields, "name", nm, sizeof(nm))) {
                    strncpy(t.name, nm, sizeof(t.name) - 1);
                }
            }
            g_wifiState->upsertAisTarget(t);
            break;
        }
        // Static reports: name (129809) and ship type (129810). These
        // carry MMSI + a single descriptive field — upsert what we have
        // and rely on the existing record to keep position fields alive.
        case 129809: {
            int64_t mmsi = 0;
            char    nm[20] = {0};
            if (!vbus::findInt(fields, "userId", &mmsi) || mmsi <= 0) break;
            if (!vbus::findString(fields, "name", nm, sizeof(nm))) break;
            AisTarget t{};
            t.mmsi         = (uint32_t)mmsi;
            strncpy(t.name, nm, sizeof(t.name) - 1);
            t.last_seen_ms = millis();
            g_wifiState->upsertAisTarget(t);
            break;
        }
        case 129810: {
            int64_t mmsi = 0;
            int64_t st_i = 0;
            if (!vbus::findInt(fields, "userId", &mmsi) || mmsi <= 0) break;
            if (!vbus::findInt(fields, "typeOfShip", &st_i)) break;
            AisTarget t{};
            t.mmsi         = (uint32_t)mmsi;
            t.ship_type    = (uint8_t)st_i;
            t.last_seen_ms = millis();
            g_wifiState->upsertAisTarget(t);
            break;
        }
        // 127257 (Attitude), 127251 (Rate of Turn), 127245 (Rudder),
        // 127489 (Engine) — accepted into the per-channel counter but not
        // routed to a setter (the UI doesn't render these yet, matching
        // the BLE backend's behaviour).
        default: break;
    }
}

// ---- send socket / interface plumbing -----------------------------------

void closeSendSocket() {
    if (g_send_sock >= 0) { close(g_send_sock); g_send_sock = -1; }
}

bool openSendSocket(IPAddress /*iface*/) {
    // Round 84 (ADR-0010): plain unicast socket. STA mode uses g_dest =
    // AP IP. AP mode sets g_dest per call from the peer table (see
    // sendBytes()).
    closeSendSocket();
    g_send_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_send_sock < 0) return false;
    memset(&g_dest, 0, sizeof(g_dest));
    g_dest.sin_family      = AF_INET;
    g_dest.sin_port        = htons(vbus::kBusPort);
    IPAddress ap; ap.fromString(vbus::kApIp);
    g_dest.sin_addr.s_addr = (uint32_t)ap;
    return true;
}

void setIfaceIp(IPAddress ip) {
    g_iface_ip = ip;
    snprintf(g_iface_ip_str, sizeof(g_iface_ip_str), "%u.%u.%u.%u",
             ip[0], ip[1], ip[2], ip[3]);
}

// Insert/refresh a peer entry. Returns true if newly added.
bool peerSeen(uint32_t addr, uint32_t now) {
    int empty = -1;
    int oldest = 0;
    for (int i = 0; i < kMaxPeers; i++) {
        if (g_peers[i].addr == addr && addr != 0) {
            g_peers[i].last_seen = now;
            return false;
        }
        if (g_peers[i].addr == 0 && empty < 0) empty = i;
        if (g_peers[i].last_seen < g_peers[oldest].last_seen) oldest = i;
    }
    const int slot = (empty >= 0) ? empty : oldest;
    g_peers[slot].addr      = addr;
    g_peers[slot].last_seen = now;
    return true;
}

void peerExpire(uint32_t now) {
    for (int i = 0; i < kMaxPeers; i++) {
        if (g_peers[i].addr != 0 &&
            (now - g_peers[i].last_seen) >= kPeerStaleMs) {
            g_peers[i].addr = 0;
        }
    }
}

// Send to a specific destination IP at kBusPort. Used by AP fanout.
void sendBytesToAddr(const char* json, size_t len, uint32_t addr) {
    if (g_send_sock < 0 || len == 0 || addr == 0) return;
    sockaddr_in d = {};
    d.sin_family      = AF_INET;
    d.sin_port        = htons(vbus::kBusPort);
    d.sin_addr.s_addr = addr;
    sendto(g_send_sock, json, len, 0, (sockaddr*)&d, sizeof(d));
}

// Role-aware publish:
//  - AP role: fanout to every known peer.
//  - STA role: unicast to AP IP (g_dest).
void sendBytes(const char* json, size_t len) {
    if (g_send_sock < 0 || len == 0) return;
    if (g_neg.role() == vbus::RoleNegotiator::Role::AP) {
        for (int i = 0; i < kMaxPeers; i++) {
            if (g_peers[i].addr != 0)
                sendBytesToAddr(json, len, g_peers[i].addr);
        }
    } else {
        sendto(g_send_sock, json, len, 0, (sockaddr*)&g_dest, sizeof(g_dest));
    }
}

// AP fanout of a received packet to every known peer except the source.
void relayToOthers(const char* json, size_t len, uint32_t src_addr) {
    if (g_send_sock < 0 || len == 0) return;
    for (int i = 0; i < kMaxPeers; i++) {
        if (g_peers[i].addr != 0 && g_peers[i].addr != src_addr)
            sendBytesToAddr(json, len, g_peers[i].addr);
    }
}

void sendHeartbeat(vbus::RoleNegotiator::Event ev, uint32_t now) {
    // Buffer sized for header + body + settings block. The settings JSON
    // alone is ~280 bytes; bump well above that to cover future keys.
    char buf[768];
    int n = g_neg.buildHeartbeatJson(buf, sizeof(buf), ev,
                                     g_iface_ip_str, now);
    if (n <= 0) return;

    // Round 85 (ADR-0013): if we're AP, splice the settings snapshot into
    // the heartbeat. The negotiator emits a JSON of shape
    //   {"pgn":65500,"src":255,"peer":"X","fields":{<kv>...}}
    // and we want
    //   {"pgn":65500,"src":255,"peer":"X","fields":{<kv>...,"settings_v":N,"settings":{...}}}
    // — i.e. inject `,"settings_v":N,"settings":{...}` immediately before
    // the trailing `}}`. STAs without this block continue to work; the
    // adoption code is gated on the field being present.
    if (g_neg.role() == vbus::RoleNegotiator::Role::AP && n >= 2 &&
        buf[n - 2] == '}' && buf[n - 1] == '}') {
        const int splice_at = n - 2;
        const int remaining = (int)sizeof(buf) - splice_at;
        // Need 1 byte for the leading comma + the settings block + 2 for "}}".
        if (remaining > 8) {
            buf[splice_at] = ',';
            const int settings_len = g_settings.exportJson(
                buf + splice_at + 1, (size_t)(remaining - 1 - 2));
            if (settings_len > 0) {
                buf[splice_at + 1 + settings_len + 0] = '}';
                buf[splice_at + 1 + settings_len + 1] = '}';
                buf[splice_at + 1 + settings_len + 2] = '\0';
                n = splice_at + 1 + settings_len + 2;
            }
        }
    }
    sendBytes(buf, (size_t)n);
}

// ---- Simulator + multicast publisher (round 83b: moved here from the
// TX board so the data source is the AP). The ESP32 softAP doesn't
// deliver STA-originated multicast to its own lwIP, so placing the
// publisher on the AP avoids that limitation entirely — multicast
// goes AP → STAs (which is the working direction).
// Generates the same waveforms the old TX simulator did, feeds the
// local BoatState directly (so the RX honeycomb displays the values),
// AND publishes JSON PGN packets onto the multicast group so STAs
// (TX, converter, iOS) can consume too.
// ------------------------------------------------------------------

int buildPgnJson(char* buf, size_t cap, uint32_t pgn, const char* body) {
    int n = vbus::writeHeader(buf, cap, pgn, vbus::kBoardPeerName);
    if (n < 0 || (size_t)n >= cap) return -1;
    int m = snprintf(buf + n, cap - n, "%s", body);
    if (m < 0 || (size_t)(n + m) >= cap) return -1;
    int e = vbus::writeFooter(buf + n + m, cap - n - m);
    if (e < 0) return -1;
    return n + m + e;
}

void publishPosition(double lat, double lon) {
    char buf[256], body[128];
    snprintf(body, sizeof(body),
             "\"latitude\":%.7f,\"longitude\":%.7f", lat, lon);
    int len = buildPgnJson(buf, sizeof(buf), 129025, body);
    if (len > 0) sendBytes(buf, (size_t)len);
}

void publishApparentWind(double awa_deg, double aws_kn, uint8_t sid) {
    char buf[256], body[160];
    snprintf(body, sizeof(body),
             "\"sid\":%u,\"windSpeed\":%.3f,\"windAngle\":%.4f,"
             "\"reference\":\"Apparent\"",
             (unsigned)sid,
             aws_kn  * 0.514444,         // knots → m/s
             awa_deg * 0.0174532925);    // degrees → radians
    int len = buildPgnJson(buf, sizeof(buf), 130306, body);
    if (len > 0) sendBytes(buf, (size_t)len);
}

void publishMagHeading(double hdg_deg, uint8_t sid) {
    char buf[256], body[128];
    snprintf(body, sizeof(body),
             "\"sid\":%u,\"heading\":%.4f,\"reference\":\"Magnetic\"",
             (unsigned)sid, hdg_deg * 0.0174532925);
    int len = buildPgnJson(buf, sizeof(buf), 127250, body);
    if (len > 0) sendBytes(buf, (size_t)len);
}

void publishStw(double stw_kn, uint8_t sid) {
    char buf[256], body[160];
    snprintf(body, sizeof(body),
             "\"sid\":%u,\"speedWaterReferenced\":%.3f,"
             "\"speedWaterReferencedType\":\"Paddle wheel\"",
             (unsigned)sid, stw_kn * 0.514444);
    int len = buildPgnJson(buf, sizeof(buf), 128259, body);
    if (len > 0) sendBytes(buf, (size_t)len);
}

void publishDepthPgn(double depth_m, uint8_t sid) {
    char buf[256], body[128];
    snprintf(body, sizeof(body),
             "\"sid\":%u,\"depth\":%.2f,\"offset\":0.0",
             (unsigned)sid, depth_m);
    int len = buildPgnJson(buf, sizeof(buf), 128267, body);
    if (len > 0) sendBytes(buf, (size_t)len);
}

void publishSeaTemp(double temp_c, uint8_t sid) {
    char buf[256], body[160];
    snprintf(body, sizeof(body),
             "\"sid\":%u,\"instance\":1,\"source\":\"Sea Temperature\","
             "\"actualTemperature\":%.2f",
             (unsigned)sid, temp_c + 273.15);
    int len = buildPgnJson(buf, sizeof(buf), 130316, body);
    if (len > 0) sendBytes(buf, (size_t)len);
}

void publishAirTemp(double temp_c, uint8_t sid) {
    char buf[256], body[160];
    snprintf(body, sizeof(body),
             "\"sid\":%u,\"instance\":0,\"source\":\"Outside Temperature\","
             "\"actualTemperature\":%.2f",
             (unsigned)sid, temp_c + 273.15);
    int len = buildPgnJson(buf, sizeof(buf), 130316, body);
    if (len > 0) sendBytes(buf, (size_t)len);
}

// Runs the simulator and publishes. Called from wifiTick().
// Identical waveform shape to the old TX-side simulator + the
// existing SIMULATED_DATA simulateTick(). Cadences match ADR-0006
// (10 Hz wind/GPS/heading, 1 Hz STW/depth, 2 s sea/air temp).
void wifiSimAndPublish(BoatState& state, uint32_t now) {
    static uint32_t t0 = millis();
    const double t = (now - t0) / 1000.0;

    // Walking position — same integrator as src/NmeaBridge.cpp's
    // simulateTick (round 56) so movement looks identical.
    static double sim_lat = 55.6761;
    static double sim_lon = 12.5683;
    static uint32_t prev_tick_ms = now;
    const double tick_dt_s = (now - prev_tick_ms) / 1000.0;
    prev_tick_ms = now;
    const double sim_speed_kn = 6.0 + 0.5 * sin(t * 0.30);
    const double sim_cog_deg  = 120.0 + 5.0 * sin(t * 0.05);
    constexpr double kEarthR_m = 6371008.8;
    constexpr double kKnToMS   = 1852.0 / 3600.0;
    const double speed_ms      = sim_speed_kn * kKnToMS;
    const double cog_rad       = sim_cog_deg  * M_PI / 180.0;
    const double dx_m = speed_ms * sin(cog_rad) * tick_dt_s;
    const double dy_m = speed_ms * cos(cog_rad) * tick_dt_s;
    const double mid_lat_rad   = sim_lat * M_PI / 180.0;
    sim_lat += dy_m / kEarthR_m * 180.0 / M_PI;
    sim_lon += dx_m / (kEarthR_m * cos(mid_lat_rad)) * 180.0 / M_PI;

    const double lat   = sim_lat;
    const double lon   = sim_lon;
    const double awa   = 45.0 + 10.0 * sin(t * 0.2);
    const double aws   = 12.0 +  2.0 * cos(t * 0.4);
    const double depth = 8.5  + 0.7 * sin(t * 0.1);
    const double temp_c = 12.3;
    const double air_c = 18.0 + 2.0 * sin(t * 0.05);
    double hdg_m = 115.0 + 5.0 * sin(t * 0.05);
    if (hdg_m < 0) hdg_m += 360.0;
    if (hdg_m >= 360.0) hdg_m -= 360.0;
    const double stw   = 5.9 + 0.4 * sin(t * 0.25);

    static uint8_t sid = 0;
    static uint32_t last_gps   = 0, last_wind = 0, last_hdg = 0;
    static uint32_t last_stw   = 0, last_dep  = 0;
    static uint32_t last_sea_t = 0, last_air_t = 0;

    const double var = navmath::lookupMagneticVariation(lat, lon);

    // Round 85 v1.6 step 4: per-channel sim toggles. When a toggle is
    // OFF, the sim simply doesn't publish and doesn't touch local
    // state — leaving room for OTHER sources (iPhone GPS via PGN
    // 129025, future LC76G on TX, real boat instruments) to write
    // into BoatState. The previous round actively NaN-cleared local
    // state to force the LCD to blank, but that drowned out alternate
    // sources publishing at a slower cadence. Blanking is now done by
    // per-channel staleness in the consuming UIs (TX already does this;
    // RX Main page does this too as of round 85 step 4).
    const settings::Settings& cfg = g_settings.view();

    if (cfg.sim_gps && now - last_gps >= 100) {
        last_gps = now;
        state.setGps(lat, lon);
        if (!std::isnan(var)) state.setMagneticVariation(var);
        publishPosition(lat, lon);
    }
    if (cfg.sim_wind && now - last_wind >= 100) {
        last_wind = now;
        state.setApparentWind(awa, aws);
        publishApparentWind(awa, aws, sid++);
    }
    if (cfg.sim_heading && now - last_hdg >= 100) {
        last_hdg = now;
        state.setMagneticHeading(hdg_m);
        publishMagHeading(hdg_m, sid++);
    }
    if (cfg.sim_depth && now - last_stw >= 1000) {
        last_stw = now;
        state.setStw(stw);
        publishStw(stw, sid++);
    }
    if (cfg.sim_depth && now - last_dep >= 1000) {
        last_dep = now;
        state.setDepth(depth);
        publishDepthPgn(depth, sid++);
    }
    if (cfg.sim_sea_temp && now - last_sea_t >= 2000) {
        last_sea_t = now;
        state.setSeaTemp(temp_c);
        publishSeaTemp(temp_c, sid++);
    }
    if (cfg.sim_air_temp && now - last_air_t >= 2000) {
        last_air_t = now;
        state.setAirTemp(air_c);
        publishAirTemp(air_c, sid++);
    }
}

// ---- WiFi mode transitions ----------------------------------------------

// Round 85 (ADR-0013): HTTP server bring-up. Only runs while AP. POST
// applies + bumps version + persists; GET returns the canonical snapshot
// wrapped in `{<settings_v + settings>}` (same shape as the heartbeat
// embed, so the iOS app can decode either source with one schema).
void startHttpServer() {
    if (g_http) return;
    g_http = new WebServer(80);
    g_http->on("/settings", HTTP_GET, []() {
        char buf[512];
        buf[0] = '{';
        int n = g_settings.exportJson(buf + 1, sizeof(buf) - 3);
        if (n <= 0) { g_http->send(500, "text/plain", "overflow"); return; }
        buf[1 + n]     = '}';
        buf[1 + n + 1] = '\0';
        g_http->send(200, "application/json", buf);
    });
    g_http->on("/settings", HTTP_POST, []() {
        if (!g_http->hasArg("plain")) {
            g_http->send(400, "text/plain", "missing body");
            return;
        }
        String body = g_http->arg("plain");
        const int changes = g_settings.applyFromJson(body.c_str());
        log_i("[settings] POST applied %d changes, v=%u",
              changes, (unsigned)g_settings.version());
        char buf[512];
        buf[0] = '{';
        int n = g_settings.exportJson(buf + 1, sizeof(buf) - 3);
        if (n <= 0) { g_http->send(500, "text/plain", "overflow"); return; }
        buf[1 + n]     = '}';
        buf[1 + n + 1] = '\0';
        g_http->send(200, "application/json", buf);
    });
    // Captive-portal probes. Apple's hotspot detection hits these and
    // looks for an HTTP 200 with exact "Success" content. Anything else
    // (timeout, 4xx, wrong body) triggers iOS's "no internet" verdict.
    auto sendCaptiveSuccess = []() {
        g_http->send(200, "text/html",
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
            "<BODY>Success</BODY></HTML>");
    };
    g_http->on("/hotspot-detect.html",          HTTP_GET, sendCaptiveSuccess);
    g_http->on("/library/test/success.html",    HTTP_GET, sendCaptiveSuccess);
    // Android / Chrome probe.
    g_http->on("/generate_204",                 HTTP_GET, []() {
        g_http->send(204, "text/plain", "");
    });
    // Microsoft / Windows.
    g_http->on("/connecttest.txt",              HTTP_GET, []() {
        g_http->send(200, "text/plain", "Microsoft Connect Test");
    });
    g_http->on("/ncsi.txt",                     HTTP_GET, []() {
        g_http->send(200, "text/plain", "Microsoft NCSI");
    });
    g_http->onNotFound([]() { g_http->send(404, "text/plain", "not found"); });
    g_http->begin();
    log_i("[wifi] http: /settings + captive probes up on :80");

    // DNS hairpin — answer every A query with 192.168.4.1. iOS resolves
    // captive.apple.com / www.apple.com / etc. through whichever DNS
    // server its current network hands out (the softAP's DHCP gives out
    // 192.168.4.1 as DNS by default). Without a DNS server, those
    // lookups time out and iOS declares the network broken.
    if (!g_dns) g_dns = new DNSServer();
    g_dns->setErrorReplyCode(DNSReplyCode::NoError);
    g_dns->start(53, "*", IPAddress(192, 168, 4, 1));
    log_i("[wifi] dns: hairpin server up on :53");
}

void stopHttpServer() {
    if (g_http) {
        g_http->stop();
        delete g_http;
        g_http = nullptr;
        log_w("[wifi] http: server stopped");
    }
    if (g_dns) {
        g_dns->stop();
        delete g_dns;
        g_dns = nullptr;
        log_w("[wifi] dns: server stopped");
    }
}

bool bringUpAp() {
    log_i("[wifi] bringing up softAP");
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(WIFI_AP_SSID, nullptr, 1, 0, 4);
    if (!ok) { log_e("[wifi] softAP failed"); return false; }
    wifi_config_t cfg = {};
    strncpy((char*)cfg.ap.ssid, WIFI_AP_SSID, sizeof(cfg.ap.ssid) - 1);
    strncpy((char*)cfg.ap.password, WIFI_AP_PASSWORD,
            sizeof(cfg.ap.password) - 1);
    cfg.ap.ssid_len        = strlen(WIFI_AP_SSID);
    cfg.ap.channel         = 1;
    cfg.ap.authmode        = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection  = 4;
    cfg.ap.beacon_interval = 100;
    cfg.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    WiFi.setSleep(false);
    setIfaceIp(WiFi.softAPIP());
    openSendSocket(g_iface_ip);
    g_wantOpenUdp = true;
    startHttpServer();
    log_i("[wifi] AP up: SSID=%s IP=%s", WIFI_AP_SSID, g_iface_ip_str);
    return true;
}

void dropAp() {
    log_w("[wifi] dropping AP");
    stopHttpServer();
    closeSendSocket();
    g_wantCloseUdp = true;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool bringUpSta(int8_t channel_or_minus1) {
    log_i("[wifi] bringing up STA (ch=%d)", (int)channel_or_minus1);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    if (channel_or_minus1 > 0)
        WiFi.begin(WIFI_AP_SSID, WIFI_AP_PASSWORD, channel_or_minus1);
    else
        WiFi.begin(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    g_sta_was_connected = false;
    return true;
}

void dropSta() {
    log_w("[wifi] dropping STA");
    closeSendSocket();
    g_wantCloseUdp = true;
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    g_sta_was_connected = false;
}

// ---- cold-boot scan ------------------------------------------------------

void doScanAndRecord() {
    log_i("[wifi] scanning for AP SSID...");
    int n = WiFi.scanNetworks(false, false, false, 200);
    int8_t found_ch = -1;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == WIFI_AP_SSID) {
            found_ch = (int8_t)WiFi.channel(i);
            log_i("[wifi] found AP on ch %d", (int)found_ch);
            break;
        }
    }
    if (found_ch < 0) log_i("[wifi] no AP found, will host");
    WiFi.scanDelete();
    g_neg.recordScanResult(found_ch);
    g_scan_attempted = true;
}

// Round 84 (task #36): see src_tx/WifiPublisher.cpp for rationale.
// RX has the highest priority (200) so in practice it stays AP after
// the cold-boot split-brain resolves — but the logic is symmetric so
// a future peer with priority > 200 would also converge.
constexpr uint32_t kApDualDetectMs = 30000;
void detectDualApAndStepDown(uint32_t now) {
    if (now - g_last_ap_scan_ms < kApDualDetectMs) return;
    g_last_ap_scan_ms = now;
    log_i("[wifi] AP: scanning for competing _wifi_nmea2k...");
    int n = WiFi.scanNetworks(false, false, false, 200);
    uint8_t our_bssid[6] = {};
    WiFi.softAPmacAddress(our_bssid);
    bool dual = false;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) != WIFI_AP_SSID) continue;
        const uint8_t* other = WiFi.BSSID(i);
        if (other && memcmp(our_bssid, other, 6) == 0) continue;
        log_w("[wifi] dual-AP detected on ch %d RSSI %d — stepping down",
              (int)WiFi.channel(i), (int)WiFi.RSSI(i));
        dual = true;
        break;
    }
    WiFi.scanDelete();
    if (dual) {
        dropAp();
        g_neg.forceReelect(now);
        g_scan_attempted = false;
    }
}

}  // namespace

static void drainTask(void*);

bool NmeaBridge::begin() {
    log_i("[wifi] begin: enter (RoleNegotiator)");
    g_wifiState = &state_;
    g_neg.init(millis());
    // Round 85 (ADR-0013): seed the settings store from NVS so a power
    // cycle survives the most recent snapshot. Returns false on first
    // boot — we just keep the compile-time defaults in that case.
    if (g_settings.loadFromNvs()) {
        log_i("[settings] loaded v%u from NVS",
              (unsigned)g_settings.version());
    } else {
        log_i("[settings] no NVS snapshot — using defaults");
    }
    xTaskCreatePinnedToCore(drainTask, "wifi-drain", 4096, nullptr,
                            5, &g_drainTask, 0);
    return true;
}

void NmeaBridge::wifiTick() {
    const uint32_t now = millis();

    // Cold-boot scan: priority-weighted delay, then scan + record.
    if (g_neg.role() == vbus::RoleNegotiator::Role::ELECTING && !g_scan_attempted) {
        uint32_t backoff =
            (vbus::kBoardPriority >= 200) ? 0    :
            (vbus::kBoardPriority >= 150) ? 4000 :
            (vbus::kBoardPriority >= 100) ? 8000 : 12000;
        static uint32_t s_boot_ms = millis();
        if (now - s_boot_ms >= backoff) doScanAndRecord();
    }
    if (g_neg.role() != vbus::RoleNegotiator::Role::ELECTING) g_scan_attempted = true;

    // Edge: STA just got WL_CONNECTED → open send socket + signal drain.
    if (g_neg.role() == vbus::RoleNegotiator::Role::STA) {
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !g_sta_was_connected) {
            log_i("[wifi] STA associated, ip=%s rssi=%d dBm",
                  WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            setIfaceIp(WiFi.localIP());
            openSendSocket(g_iface_ip);
            g_wantOpenUdp = true;
        }
        g_sta_was_connected = connected;
    }

    auto a = g_neg.tick(now);
    if (a.drop_ap)            dropAp();
    if (a.drop_sta)           dropSta();
    if (a.start_ap)           bringUpAp();
    if (a.start_sta)          bringUpSta(a.target_channel);
    if (a.publish_heartbeat)
        sendHeartbeat(vbus::RoleNegotiator::Event::HEARTBEAT, now);
    if (a.publish_takeover)
        sendHeartbeat(vbus::RoleNegotiator::Event::TAKEOVER_ANNOUNCE, now);
    if (a.publish_going_down)
        sendHeartbeat(vbus::RoleNegotiator::Event::GOING_DOWN, now);

    // Round 83b: when we're AP, run the simulator and publish onto
    // the virtual bus. Placed on the AP because STA-originated
    // multicast didn't reach the AP's own lwIP on ESP32 — round 84
    // pivoted to unicast (ADR-0010) but the AP-as-publisher
    // arrangement stayed.
    if (g_neg.role() == vbus::RoleNegotiator::Role::AP && g_send_sock >= 0) {
        wifiSimAndPublish(*g_wifiState, now);
    }

    // Round 85 (ADR-0013): drive the AP-side HTTP server. Cheap when
    // there's no connected client; safe to call every tick.
    if (g_http) g_http->handleClient();
    // Round 85 v1.6 step 3: drive the DNS hairpin too.
    if (g_dns)  g_dns->processNextRequest();

    // Task #36: AP-side dual-AP detection. Reset cadence on non-AP
    // role so the first scan after promotion has a full interval.
    if (g_neg.role() == vbus::RoleNegotiator::Role::AP) {
        detectDualApAndStepDown(now);
    } else {
        g_last_ap_scan_ms = now;
    }

    // 5-second heartbeat log: WL state + per-channel pkt counts.
    static uint32_t lastStatusLog   = 0;
    static uint32_t lastTotalSample = 0;
    if (now - lastStatusLog >= 5000) {
        lastStatusLog = now;
        uint32_t total = 0;
        for (int i = 0; i < WIFI_CH_COUNT; i++) total += g_pktCount[i];
        const uint32_t windowRx = total - lastTotalSample;
        lastTotalSample = total;
        log_i("[wifi] role=%s ip=%s ap=%s peers=%d "
              "pkts_rx=%u/5s by_ch w=%u g=%u h=%u d=%u a=%u",
              vbus::RoleNegotiator::roleName(g_neg.role()),
              g_iface_ip_str,
              g_neg.currentApPeerName(),
              g_neg.peerCount(),
              (unsigned)windowRx,
              (unsigned)g_pktCount[WIFI_CH_WIND],
              (unsigned)g_pktCount[WIFI_CH_GPS],
              (unsigned)g_pktCount[WIFI_CH_HEADING],
              (unsigned)g_pktCount[WIFI_CH_DEPTH_TEMP],
              (unsigned)g_pktCount[WIFI_CH_ATTITUDE]);
    }

    // Staleness detection (data PGNs, not heartbeats).
    if (g_dataFresh && (now - g_lastPacketMs >= kStaleMs)) {
        g_dataFresh = false;
        if (g_wifiState) g_wifiState->invalidateLiveData();
        log_w("[wifi] data stale — blanking live values");
    }
}

// drainTask — pinned to core 0 (LVGL + Arduino loop run on core 1). Owns
// g_udp completely; main task signals open/close intent via g_wantOpenUdp
// and g_wantCloseUdp. Without this separation, the main loop's LVGL
// flush (~50 ms when it runs) stalls UDP draining and lwIP's small
// UDP_RECVMBOX_SIZE (default 6) overflows, dropping ~40% of packets.
static void drainTask(void*) {
    for (;;) {
        if (g_wantOpenUdp && g_rx_sock < 0) {
            // Round 84 (ADR-0010): plain unicast bind on kBusPort.
            // No multicast group, no per-iface binding — STAs unicast
            // to our AP IP and lwIP routes the packet to this socket
            // regardless of which netif it arrived on.
            g_rx_sock = socket(AF_INET, SOCK_DGRAM, 0);
            bool ok = false;
            if (g_rx_sock >= 0) {
                int yes = 1;
                setsockopt(g_rx_sock, SOL_SOCKET, SO_REUSEADDR,
                           &yes, sizeof(yes));
                sockaddr_in local = {};
                local.sin_family = AF_INET;
                local.sin_addr.s_addr = htonl(INADDR_ANY);
                local.sin_port        = htons(vbus::kBusPort);
                if (bind(g_rx_sock, (sockaddr*)&local, sizeof(local)) == 0) {
                    ok = true;
                    log_i("[wifi] drain: bound to %s:%u (unicast)",
                          g_iface_ip_str, (unsigned)vbus::kBusPort);
                } else {
                    log_e("[wifi] drain: bind failed errno=%d", errno);
                }
            }
            if (!ok) {
                if (g_rx_sock >= 0) { close(g_rx_sock); g_rx_sock = -1; }
            }
            g_udpOpen = ok;
            g_wantOpenUdp = false;
        }
        if (g_wantCloseUdp && g_rx_sock >= 0) {
            close(g_rx_sock); g_rx_sock = -1;
            g_udpOpen = false;
            g_wantCloseUdp = false;
            log_w("[wifi] drain: rx socket closed");
        }
        if (g_rx_sock >= 0) {
            // Drain everything available without yielding. MSG_DONTWAIT
            // makes recvfrom non-blocking; loop until EAGAIN.
            const bool amAp =
                (g_neg.role() == vbus::RoleNegotiator::Role::AP);
            const uint32_t tnow = millis();
            for (;;) {
                sockaddr_in src;
                socklen_t srclen = sizeof(src);
                int n = recvfrom(g_rx_sock, g_rxBuf, sizeof(g_rxBuf) - 1,
                                 MSG_DONTWAIT,
                                 (sockaddr*)&src, &srclen);
                if (n <= 0) break;
                g_rxBuf[n] = '\0';

                static int dumpCount = 0;
                if (dumpCount < 3) {
                    dumpCount++;
                    log_i("[wifi] rx pkt %d (%d bytes): %s",
                          dumpCount, n, g_rxBuf);
                }

                // Round 84 (ADR-0010): when AP, learn the source IP
                // and fan the packet out to every other known peer.
                // Source-IP skip is the only loop-prevention we need
                // because the star topology has exactly one hop.
                if (amAp) {
                    peerSeen(src.sin_addr.s_addr, tnow);
                    relayToOthers(g_rxBuf, (size_t)n, src.sin_addr.s_addr);
                }

                int64_t pgn = 0;
                if (!vbus::findInt(g_rxBuf, "pgn", &pgn)) continue;
                const char* fields = nullptr;
                size_t flen = 0;
                if (!vbus::findFieldsBody(g_rxBuf, &fields, &flen)) continue;

                char fbuf[kRxBufSize];
                if (flen >= sizeof(fbuf)) continue;
                memcpy(fbuf, fields, flen);
                fbuf[flen] = '\0';

                char peer[sizeof(g_lastPeer)];
                bool have_peer = vbus::findString(g_rxBuf, "peer", peer,
                                                  sizeof(peer));

                // Control PGN → negotiator + settings adoption when STA.
                // Data PGN → BoatState dispatch.
                if (pgn == (int64_t)vbus::kControlPgn) {
                    if (have_peer) g_neg.onHeartbeat(peer, fbuf, millis());

                    // Round 85 (ADR-0013): if this heartbeat is from the
                    // AP and carries a settings snapshot, adopt it on a
                    // version bump. We only act in STA role — when we ARE
                    // the AP, our local store is canonical and the loop-
                    // back of our own heartbeat shouldn't disturb it.
                    if (g_neg.role() == vbus::RoleNegotiator::Role::STA) {
                        int64_t remote_v = 0;
                        if (vbus::findInt(g_rxBuf, "settings_v", &remote_v)
                            && remote_v > 0) {
                            const char* sbody = nullptr;
                            size_t      slen  = 0;
                            if (vbus::findObjectBody(g_rxBuf, "settings",
                                                     &sbody, &slen)) {
                                char sbuf[512];
                                if (slen < sizeof(sbuf)) {
                                    memcpy(sbuf, sbody, slen);
                                    sbuf[slen] = '\0';
                                    if (g_settings.adoptFromRemote(
                                            (uint32_t)remote_v, sbuf)) {
                                        log_i("[settings] adopted v%u from AP",
                                              (unsigned)remote_v);
                                    }
                                }
                            }
                        }
                    }
                    continue;
                }

                if (have_peer) memcpy(g_lastPeer, peer, sizeof(peer));

                dispatch(pgn, fbuf);

                const NmeaBridge::WifiChannel ch = channelForPgn(pgn);
                if (ch != NmeaBridge::WIFI_CH_COUNT) g_pktCount[ch]++;

                g_lastPacketMs = millis();
                g_dataFresh = true;
            }
            if (amAp) peerExpire(tnow);
        }
        // 1 ms tick yields the core but stays responsive — at TX's 100 ms
        // publish cadence this drains within 1-2 task slices of arrival.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

bool NmeaBridge::wifiConnected() {
    // "Connected" = we have an active network role. Whether data is
    // flowing or peers are present is shown separately on the comm
    // page (stations / peers / packet counters).
    auto r = g_neg.role();
    if (r == vbus::RoleNegotiator::Role::AP)  return true;   // we're hosting
    if (r == vbus::RoleNegotiator::Role::STA) return WiFi.status() == WL_CONNECTED;
    return false;
}

const char* NmeaBridge::wifiPeerName() {
    // For UI: show the AP we're connected to (if STA), or our own
    // identity (if AP). Falls back to last data-publisher's peer name.
    auto r = g_neg.role();
    if (r == vbus::RoleNegotiator::Role::STA) return g_neg.currentApPeerName();
    if (r == vbus::RoleNegotiator::Role::AP)  return vbus::kBoardPeerName;
    return g_lastPeer;
}

int NmeaBridge::wifiRssi() {
    auto r = g_neg.role();
    if (r == vbus::RoleNegotiator::Role::STA && WiFi.status() == WL_CONNECTED)
        return (int)WiFi.RSSI();
    return INT16_MIN;
}

uint32_t NmeaBridge::wifiPacketCount(WifiChannel ch) {
    if (ch >= WIFI_CH_COUNT) return 0;
    return g_pktCount[ch];
}

const char* NmeaBridge::wifiRoleName() {
    return vbus::RoleNegotiator::roleName(g_neg.role());
}

int NmeaBridge::wifiPeerCount() {
    return g_neg.peerCount();
}

int NmeaBridge::wifiStationCount() {
    return (g_neg.role() == vbus::RoleNegotiator::Role::AP)
           ? (int)WiFi.softAPgetStationNum() : 0;
}

const char* NmeaBridge::wifiLocalIp() {
    return g_iface_ip_str;
}

int NmeaBridge::wifiChannel() {
    return (int)WiFi.channel();
}

uint32_t NmeaBridge::settingsVersion() {
    return g_settings.version();
}

const settings::Settings& NmeaBridge::currentSettings() {
    return g_settings.view();
}

#endif // DATA_SOURCE_WIFI
