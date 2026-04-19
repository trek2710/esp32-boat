#include "NmeaBridge.h"

#include <Arduino.h>
#include <cmath>

#if !SIMULATED_DATA
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
#if !SIMULATED_DATA

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

    switch (msg.PGN) {
        // ----------------------------------------------------------------- GPS
        case 129025L: { // Position Rapid Update
            double lat = NAN, lon = NAN;
            if (ParseN2kPGN129025(msg, lat, lon)) {
                auto snap = s.snapshot();
                s.setGps(lat, lon, snap.sog, snap.cog);
            }
            break;
        }
        case 129026L: { // COG & SOG Rapid Update
            unsigned char sid;
            tN2kHeadingReference ref;
            double cog_rad = NAN, sog_ms = NAN;
            if (ParseN2kPGN129026(msg, sid, ref, cog_rad, sog_ms)) {
                auto snap = s.snapshot();
                s.setGps(snap.lat, snap.lon, msToKnots(sog_ms), RadToDeg(cog_rad));
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
                if (ref == N2kWind_Apparent) {
                    s.setWindApparent(angle_deg, speed_kn);
                } else if (ref == N2kWind_True_boat || ref == N2kWind_True_water) {
                    s.setWindTrue(angle_deg, speed_kn);
                }
            }
            break;
        }

        // --------------------------------------------------------------- Depth
        case 128267L: { // Water Depth
            unsigned char sid;
            double depth_below_transducer_m = NAN, offset_m = NAN, range_m = NAN;
            if (ParseN2kPGN128267(msg, sid, depth_below_transducer_m, offset_m, range_m)) {
                auto snap = s.snapshot();
                s.setDepth(depth_below_transducer_m, snap.water_temp_c);
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
                    auto snap = s.snapshot();
                    s.setDepth(snap.depth_m, temp_k - 273.15);
                }
            }
            break;
        }

        // ------------------------------------------------------- Heading / STW
        case 127250L: { // Vessel Heading
            unsigned char sid;
            double heading_rad = NAN, dev_rad = NAN, var_rad = NAN;
            tN2kHeadingReference ref;
            if (ParseN2kPGN127250(msg, sid, heading_rad, dev_rad, var_rad, ref)) {
                double deg = RadToDeg(heading_rad);
                if (ref == N2khr_magnetic && !isnan(var_rad)) deg += RadToDeg(var_rad);
                s.setHeading(normalizeDeg(deg));
            }
            break;
        }
        case 128259L: { // Speed Through Water
            unsigned char sid;
            double stw_ms = NAN, sog_ms = NAN;
            tN2kSpeedWaterReferenceType ref;
            if (ParseN2kPGN128259(msg, sid, stw_ms, sog_ms, ref)) {
                s.setStw(msToKnots(stw_ms));
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
            break;
    }
}

#endif // !SIMULATED_DATA


// ============================================================================
// Simulated build — fake values, no NMEA 2000 stack.
// ============================================================================
#if SIMULATED_DATA

bool NmeaBridge::begin() {
    Serial.println(F("[sim] NMEA bridge running in SIMULATED_DATA mode"));
    return true;
}

void NmeaBridge::simulateTick() {
    static uint32_t t0 = millis();
    const double t = (millis() - t0) / 1000.0;

    state_.setGps(55.6761 + 0.0001 * sin(t * 0.1),
                  12.5683 + 0.0001 * cos(t * 0.1),
                  6.2 + 0.5 * sin(t * 0.3),
                  120.0 + 5.0 * sin(t * 0.05));
    state_.setWindApparent(45.0 + 10.0 * sin(t * 0.2), 12.0 + 2.0 * cos(t * 0.4));
    state_.setWindTrue(   60.0 +  5.0 * sin(t * 0.15), 14.0 + 1.5 * cos(t * 0.25));
    state_.setDepth(8.5 + 0.7 * sin(t * 0.1), 12.3);
    state_.setHeading(normalizeDeg(120.0 + 5.0 * sin(t * 0.05)));
    state_.setStw(5.9 + 0.4 * sin(t * 0.25));
}

#endif // SIMULATED_DATA
