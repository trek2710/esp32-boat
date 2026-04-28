#include "NmeaBridge.h"

#include <Arduino.h>
#include <cmath>
#include <cstdio>

#include "magnetic_variation.h"

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
                auto snap = s.snapshot();
                s.setDepth(depth_below_transducer_m, snap.water_temp_c);
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
                    auto snap = s.snapshot();
                    s.setDepth(snap.depth_m, temp_k - 273.15);
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

#endif // !SIMULATED_DATA


// ============================================================================
// Simulated build — fake values, no NMEA 2000 stack.
// ============================================================================
#if SIMULATED_DATA

bool NmeaBridge::begin() {
    Serial.println(F("[sim] NMEA bridge running in SIMULATED_DATA mode"));
    return true;
}

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

    state_.setGps(lat, lon);
    state_.setApparentWind(awa, aws);
    state_.setMagneticHeading(hdg_m);
    state_.setStw(stw);
    state_.setDepth(depth, temp_c);

    // Magnetic variation from the WMM-on-SD lookup (round-49 stub).
    const double var_deg = navmath::lookupMagneticVariation(lat, lon);
    if (!std::isnan(var_deg)) state_.setMagneticVariation(var_deg);

    // Fire simulated PGN log entries on their per-PGN intervals so the
    // debug screen sees a realistic-looking trickle.
    char summary[56];
    for (auto& e : sim_events) {
        if (now - e.last_ms < e.interval_ms) continue;
        e.last_ms = now;
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
