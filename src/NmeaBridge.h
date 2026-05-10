// NmeaBridge — bridges the ttlappalainen NMEA 2000 library to BoatState.
//
// In production builds it owns a tNMEA2000 instance, registers PGN handlers,
// and runs ParseMessages() on its own FreeRTOS task.
//
// In SIMULATED_DATA builds, the NMEA 2000 stack is not compiled in at all —
// begin() becomes a no-op that returns true, and simulateTick() is the only
// thing that ever touches BoatState. This keeps the sim firmware buildable on
// ESP32-S3 boards even when the CAN backend library isn't S3-compatible yet.

#pragma once

#include "BoatState.h"
#include "config.h"

#if !SIMULATED_DATA
class tN2kMsg;
class tNMEA2000;
#endif

class NmeaBridge {
public:
    // BoatState lifetime must outlive the bridge.
    explicit NmeaBridge(BoatState& state);

    // Initialise NMEA2000 stack + spawn the parse task pinned to core 0.
    // In SIMULATED_DATA mode this is a no-op that returns true.
    bool begin();

#if SIMULATED_DATA
    // Drive BoatState with fake values. Called from the main loop when
    // SIMULATED_DATA is set — lets you exercise the UI without the boat.
    void simulateTick();
#endif

private:
    BoatState&  state_;

#if !SIMULATED_DATA
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
