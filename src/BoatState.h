// BoatState — a thread-safe snapshot of the latest values we've seen on the
// bus, plus the derived navigation values the device computes from them.
//
// >>> Full math + worked examples: see docs/NAVIGATION_MATH.md (round 56). <<<
//
// Round 53 split: the Instruments struct now distinguishes RAW SENSOR INPUTS
// (the things a real boat sensor publishes) from DERIVED values (the things
// the device computes from the raw inputs + stored data like the magnetic
// variation table on SD). Setters mutate raw fields only; derived fields
// are recomputed automatically inside each setter (recomputeDerived_locked).
//
// Raw inputs (what real sensors send):
//   * GPS:                 lat, lon, sog, cog
//   * Apparent wind:       awa, aws (masthead sensor — measures relative to
//                                    the boat's motion)
//   * Magnetic compass:    heading_mag_deg
//   * Magnetic variation:  magnetic_variation_deg (from WMM stored on SD,
//                                                  or PGN 127250 if a
//                                                  chartplotter publishes it)
//   * Speed through water: stw   (paddlewheel / ultrasonic log)
//   * Depth + temp:        depth_m, water_temp_c
//
// Derived (what the device computes):
//   * heading_true_deg = heading_mag + magnetic_variation
//   * twa, tws         — true wind from apparent wind + boat motion (vector
//                        subtraction in the boat-relative frame)
//   * twd              = heading_true + twa  (true wind direction, °T)
//   * vmg              = stw · cos(twa)      (velocity made good upwind)
//
// Things we explicitly DO NOT know from sensors (for now):
//   * sea current / set & drift  (would need both SOG/COG and STW/heading and
//                                 still gives only an estimate; v1 leaves
//                                 the "DRIFT" pill showing STW)
//   * AIS targets                (PGN handlers are stubbed in NmeaBridge;
//                                 will land in a later round)

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <cstdint>

// One entry in the rolling PGN log. The NMEA bridge calls BoatState::logPgn()
// every time a frame arrives so the debug screen can show bus traffic. In
// SIMULATED_DATA builds, the sim tick synthesises entries so the debug screen
// is still populated on the bench.
struct PgnEvent {
    uint32_t pgn      = 0;    // e.g. 129025
    uint8_t  src      = 0;    // source address on the N2K bus (0 = sim)
    uint32_t t_ms     = 0;    // millis() at receive
    char     summary[56] = {0}; // short decoded/"undecoded" string
};

struct AisTarget {
    uint32_t mmsi = 0;
    char     name[20] = {0};   // up to 20 ASCII chars; may be empty until we see PGN 129809
    double   lat = NAN;        // degrees
    double   lon = NAN;        // degrees
    double   sog = NAN;        // knots
    double   cog = NAN;        // degrees true
    uint32_t last_seen_ms = 0; // millis() at last update
};

struct Instruments {
    // ===== RAW SENSOR INPUTS =====

    // GPS — position and motion-over-ground.
    double   lat            = NAN;   // degrees
    double   lon            = NAN;   // degrees
    double   sog            = NAN;   // knots (Speed Over Ground)
    double   cog            = NAN;   // degrees true (Course Over Ground)
    uint32_t gps_last_ms    = 0;

    // Apparent wind — what a masthead anemometer measures relative to the
    // boat (so AWA = 0 means dead-ahead wind, regardless of true wind
    // direction). For TRUE wind we subtract boat velocity (see derived
    // block below).
    double   awa            = NAN;   // Apparent Wind Angle, degrees (-180..180, + starboard)
    double   aws            = NAN;   // Apparent Wind Speed, knots
    uint32_t wind_last_ms   = 0;

    // Depth / water (depth sounder + temperature probe).
    // Round 78 follow-up: depth and sea temp now have INDEPENDENT
    // timestamps because they ride on different NMEA 2000 PGNs at
    // different spec cadences — PGN 128267 Water Depth at 1 Hz,
    // PGN 130316 Temperature (Sea) at 0.5 Hz. The honeycomb PGN
    // page reads each hex's Hz from its own *_last_ms.
    double   depth_m            = NAN;   // metres below transducer
    uint32_t depth_last_ms      = 0;
    double   water_temp_c       = NAN;   // °C (sea temp, PGN 130316)
    uint32_t water_temp_last_ms = 0;

    // Outdoor (air) temperature. Round 78 — populated by the simulator
    // for v1; production reads it from PGN 130316 with temp_src =
    // OutsideTemperature (handler added in round 78). ENG_T / OIL_T
    // are intentionally not represented here yet — their hexes on the
    // round-78 PGN page stay grey until a real engine bridge lands.
    double   air_temp_c       = NAN; // °C
    uint32_t air_temp_last_ms = 0;

    // Magnetic compass heading. NMEA 2000 PGN 127250 publishes magnetic by
    // default; the device adds the variation to derive true heading below.
    double   heading_mag_deg = NAN;
    uint32_t hdg_last_ms     = 0;

    // Magnetic variation (declination) — sign convention: positive = east.
    // Looked up from the WMM table (eventually on SD; round-53 stub returns
    // a Copenhagen-area constant). Some chartplotters also publish this in
    // PGN 127250 — when they do we use that value directly.
    double   magnetic_variation_deg = NAN;
    uint32_t var_last_ms     = 0;

    // Speed through water (paddlewheel / ultrasonic log).
    double   stw             = NAN;   // knots
    uint32_t stw_last_ms     = 0;

    // ===== DERIVED VALUES =====
    // Recomputed automatically by BoatState whenever a raw input changes.
    // Read-only from the UI's point of view.

    double   heading_true_deg = NAN;  // = heading_mag + magnetic_variation
    double   twa              = NAN;  // True Wind Angle relative to bow, degrees
    double   tws              = NAN;  // True Wind Speed, knots
    double   twd              = NAN;  // True Wind Direction, degrees true
    double   vmg              = NAN;  // Velocity Made Good upwind, knots
};

class BoatState {
public:
    BoatState();

    // Whole-struct snapshot — cheap copy, safe for the UI task to read.
    Instruments snapshot();

    // ---- RAW SENSOR SETTERS -------------------------------------------------
    // Each one mutates the corresponding raw field(s) and then runs
    // recomputeDerived_locked() so the snapshot's derived fields stay
    // consistent with the raw inputs.
    // Round 56: setGps takes ONLY raw position now. SOG / COG are
    // computed inside BoatState from the differential between
    // consecutive fixes (see BoatState.cpp). This matches the user's
    // "moving direction from a GPS difference calculation" rule —
    // the device doesn't trust a sensor-supplied SOG/COG, it derives
    // its own.
    void setGps(double lat, double lon);
    void setApparentWind(double awa, double aws);
    void setMagneticHeading(double heading_mag_deg);
    void setMagneticVariation(double variation_deg);
    void setStw(double stw);
    // Round 78 follow-up: depth-only setter; sea temp moved to
    // setSeaTemp() so it can publish at the PGN 130316 2 s spec
    // cadence independent of depth's 1 Hz.
    void setDepth(double depth_m);
    void setSeaTemp(double water_temp_c);
    void setAirTemp(double air_temp_c);  // round 78 — outdoor air temp

    // AIS target book-keeping.
    // Targets older than kAisStaleMs are dropped on each update.
    void upsertAisTarget(const AisTarget& t);
    std::array<AisTarget, 32> aisSnapshot();

    static constexpr uint32_t kAisStaleMs = 10UL * 60UL * 1000UL; // 10 minutes

    // ---- PGN log (for the debug screen) -----------------------------------
    // Fixed-size ring buffer. pgnLogSnapshot() returns a copy in newest-first
    // order; empty slots have pgn == 0 and should be skipped by the UI.
    static constexpr size_t kPgnLogSize = 64;
    void logPgn(uint32_t pgn, uint8_t src, const char* summary);
    std::array<PgnEvent, kPgnLogSize> pgnLogSnapshot();
    uint32_t pgnLogTotal();  // total events seen since boot (not just buffered)

private:
    SemaphoreHandle_t                       mutex_;
    Instruments                             i_;
    std::array<AisTarget, 32>               ais_{};
    std::array<PgnEvent, kPgnLogSize>       pgn_log_{};
    size_t                                  pgn_log_head_  = 0; // next write index
    uint32_t                                pgn_log_total_ = 0;

    // Recomputes heading_true / twa / tws / twd / vmg from the raw fields
    // currently in i_. Called at the end of every raw-value setter while
    // the mutex is held.
    void recomputeDerived_locked();

    void pruneStaleAis_locked();
};
