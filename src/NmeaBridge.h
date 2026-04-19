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
