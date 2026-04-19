// NmeaBridge — bridges the ttlappalainen NMEA 2000 library to BoatState.
//
// Owns the tNMEA2000 instance, registers handlers for the PGNs we care about,
// and runs `ParseMessages()` from its own FreeRTOS task so the UI task never
// stalls. All decoded values are pushed into the shared BoatState.

#pragma once

#include "BoatState.h"

class tNMEA2000;

class NmeaBridge {
public:
    // BoatState lifetime must outlive the bridge.
    explicit NmeaBridge(BoatState& state);

    // Initialise NMEA2000 stack + spawn the parse task pinned to core 0.
    // Returns false if the TWAI driver failed to start.
    bool begin();

#if SIMULATED_DATA
    // Drive BoatState with fake values. Called from a separate task when
    // SIMULATED_DATA is set — lets you exercise the UI without the boat.
    void simulateTick();
#endif

private:
    BoatState&  state_;
    tNMEA2000*  n2k_ = nullptr;

    static void taskTrampoline(void* arg);
    void taskLoop();

    // Static handler used by the ttlappalainen lib; forwards to the
    // singleton instance set up in begin().
    static NmeaBridge* instance_;
    static void handleMsg(const class tN2kMsg& msg);
};
