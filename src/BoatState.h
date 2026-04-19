// BoatState — a thread-safe snapshot of the latest values we've seen on the bus.
//
// The NMEA bridge task writes to it when PGN frames arrive; the UI task reads
// from it when redrawing instruments. A single FreeRTOS mutex guards the whole
// struct — access is low-frequency enough that finer locking isn't worth it.

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <cstdint>

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
    // GPS
    double   lat            = NAN;   // degrees
    double   lon            = NAN;   // degrees
    double   sog            = NAN;   // knots (Speed Over Ground)
    double   cog            = NAN;   // degrees true (Course Over Ground)
    uint32_t gps_last_ms    = 0;

    // Wind
    double   awa            = NAN;   // Apparent Wind Angle, degrees (-180..180, + starboard)
    double   aws            = NAN;   // Apparent Wind Speed, knots
    double   twa            = NAN;   // True Wind Angle
    double   tws            = NAN;   // True Wind Speed
    uint32_t wind_last_ms   = 0;

    // Depth / water
    double   depth_m        = NAN;   // metres below transducer
    double   water_temp_c   = NAN;   // °C
    uint32_t depth_last_ms  = 0;

    // Heading / speed through water
    double   heading_true_deg = NAN;
    double   stw              = NAN; // Speed Through Water, knots
    uint32_t hdg_last_ms      = 0;
};

class BoatState {
public:
    BoatState();

    // Whole-struct snapshot — cheap copy, safe for the UI task to read.
    Instruments snapshot();

    // Mutators called by the NMEA bridge.
    void setGps(double lat, double lon, double sog, double cog);
    void setWindApparent(double awa, double aws);
    void setWindTrue(double twa, double tws);
    void setDepth(double depth_m, double water_temp_c);
    void setHeading(double heading_true_deg);
    void setStw(double stw);

    // AIS target book-keeping.
    // Targets older than kAisStaleMs are dropped on each update.
    void upsertAisTarget(const AisTarget& t);
    std::array<AisTarget, 32> aisSnapshot();

    static constexpr uint32_t kAisStaleMs = 10UL * 60UL * 1000UL; // 10 minutes

private:
    SemaphoreHandle_t              mutex_;
    Instruments                    i_;
    std::array<AisTarget, 32>      ais_{};

    void pruneStaleAis_locked();
};
