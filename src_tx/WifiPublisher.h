// src_tx/WifiPublisher.h
//
// TX-side WiFi virtual-bus publisher. Brings up softAP _wifi_nmea2k,
// opens a UDP socket, and converts each BoatBle PDU into one or more
// canboat-style JSON packets sent to multicast 239.255.78.85:60001.
//
// See docs/VIRTUAL_BUS_WIRE.md for the wire spec and
// ~/.claude/projects/.../memory/virtual_bus_architecture.md for the
// architectural context (BLE → WiFi migration).

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <BoatBle.h>           // for the PDU structs we accept as input

class BoatState;  // round 85 fix — incoming-PGN consumer (optional)

namespace WifiPublisher {

// Round 85 (v1.5b step 7 fix): give TX a local mirror of the bus state.
// When set, drainOnce() routes incoming data PGNs (wind / GPS / heading
// / depth-temp / AIS) into the supplied BoatState. Heartbeats (PGN
// 65500) continue to feed the role negotiator independently. Pass
// nullptr to disable (default).
void setDataConsumer(BoatState* state);

// Bring up softAP and open the UDP socket. Returns true on success.
// Logs to Serial regardless.
bool begin();

// Call once per loop() iteration. Handles the 30 s republish heartbeat.
void tick(uint32_t now);

// One publish entry point per BoatBle channel. Each call may emit
// multiple JSON packets (e.g. WindPdu with both true and apparent set
// → two PGN 130306 packets). The current `now` (millis) is used to
// stamp the last-publish time for republish bookkeeping.
void publishWind     (const boatble::WindPdu&     pdu, uint32_t now);
void publishGps      (const boatble::GpsPdu&      pdu, uint32_t now);
void publishHeading  (const boatble::HeadingPdu&  pdu, uint32_t now);
void publishDepthTemp(const boatble::DepthTempPdu& pdu, uint32_t now);
void publishAttitude (const boatble::AttitudePdu& pdu, uint32_t now);

// Status getters for the UI / heartbeat. All cheap.
uint32_t stationCount();      // number of associated WiFi stations
const char* apIp();           // dotted-quad string, or "0.0.0.0"
uint32_t packetsSent();       // monotonic counter (TX)
uint32_t packetsInWindow();   // counter since last resetWindow()
uint32_t packetsFailedInWindow(); // sendto() < 0 returns since last resetWindow()
uint32_t packetsRxTotal();    // monotonic counter (RX-side diag)
void resetWindow();           // call from the serial heartbeat

// Role-election state — for UI display only.
const char* roleName();       // "electing" / "AP" / "STA"
const char* currentApPeer();  // peer name of current AP, or ""
int         peerCount();      // number of known peers (incl. self if we're AP)
bool        onNetwork();      // true if we have a working WiFi link

}  // namespace WifiPublisher
