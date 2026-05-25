// src_converter/WifiPublisher.h
//
// Converter-side WiFi virtual-bus participant. This first revision only
// handles the role-election + heartbeat plumbing — enough to make the
// converter a visible peer on the bus. Publishing the AIS PGNs onto
// multicast (alongside the existing TWAI emit) is deferred to a follow-
// up round; it requires hooking tNMEA2000::SendMsg, which means
// subclassing NMEA2000_esp32_twai. Out of scope for the
// "get-the-converter-on-the-network" phase.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <Settings.h>          // round 85 v1.6 step 1

namespace WifiPublisher {

bool begin();
void tick(uint32_t now);

// Status getters for serial logging.
const char* roleName();
const char* currentApPeer();
int  peerCount();
bool onNetwork();
const char* apIp();
uint32_t stationCount();

// Round 85 (ADR-0013): the live settings snapshot mirrored from the
// AP's heartbeat (or the local NVS-loaded copy on cold boot). Consumers
// (e.g. AIS filter in src_converter/Ui.cpp) read fields from this to
// pick up iOS-pushed config changes within ≤1 heartbeat.
const settings::Settings& currentSettings();

// Round 85 v1.6 step 3 (ADR-0012): generic JSON-PGN publish onto the
// virtual bus. Used by main.cpp's AIS replay walker to emit PGN
// 129038/9/40/809/810 from the AisTargetStore. The body is the inner
// `{ ... }` of the "fields" object; we wrap the canonical envelope
// (`{"pgn":N,"src":35,"peer":"nmea-converter","fields":{...}}`)
// before sending. No-op when not associated.
void publishPgnJson(uint32_t pgn, const char* fields_body);

}  // namespace WifiPublisher
