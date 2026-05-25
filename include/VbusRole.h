// include/VbusRole.h
//
// Per-board identity + priority constants for the virtual-bus role
// election (see docs/adr/0009-wifi-role-election.md). The build flag
// -DVBUS_BOARD_KIND=N selects which of the three boards this firmware
// is, and the per-kind tables below derive the priority / peer name
// / kind name from it.
//
// Header-only. Both the TX (src_tx/), RX (src/), and converter
// (src_converter/) trees include this directly.

#pragma once

#include <stdint.h>

#ifndef VBUS_BOARD_KIND
#error "VBUS_BOARD_KIND must be defined in platformio.ini (0=TX, 1=RX, 2=converter)"
#endif

namespace vbus {

enum BoardKind : uint8_t {
    BK_TX        = 0,
    BK_RX        = 1,
    BK_CONVERTER = 2,
    BK_IOS       = 3,
};

// Compile-time priority table per ADR-0009. Higher wins election.
constexpr uint8_t kPriorityTx        = 100;
constexpr uint8_t kPriorityRx        = 200;
constexpr uint8_t kPriorityConverter = 150;
constexpr uint8_t kPriorityIos       = 0;     // never elects (STA-only)

// What this build is.
constexpr BoardKind kBoardKind = (BoardKind)VBUS_BOARD_KIND;

constexpr uint8_t kBoardPriority =
    (kBoardKind == BK_TX)        ? kPriorityTx :
    (kBoardKind == BK_RX)        ? kPriorityRx :
    (kBoardKind == BK_CONVERTER) ? kPriorityConverter :
                                   kPriorityIos;

// Peer name strings used in the JSON 'peer' field on every multicast
// packet (data PGNs + control PGN 65500). Renamed round 83b — the
// "TX"/"RX" labels were direction-from-the-perspective-of-BLE-round-79
// and stopped making sense once the radios elected roles dynamically.
// The boards are now identified by what they physically are: the
// AMOLED-1.75-G with the LC76G GPS (LCD-GPS) and the Waveshare 2.1"
// touch display (LCD-2.1). Env names / folder names retain the legacy
// "tx" / "rx" tokens to avoid MAC-registration churn in scripts/flash.sh.
constexpr const char* kPeerNameTx        = "lcd-gps";
constexpr const char* kPeerNameRx        = "lcd-2.1";
constexpr const char* kPeerNameConverter = "nmea-converter";
constexpr const char* kPeerNameIos       = "ios-app";

constexpr const char* kBoardPeerName =
    (kBoardKind == BK_TX)        ? kPeerNameTx :
    (kBoardKind == BK_RX)        ? kPeerNameRx :
    (kBoardKind == BK_CONVERTER) ? kPeerNameConverter :
                                   kPeerNameIos;

constexpr const char* kBoardKindName =
    (kBoardKind == BK_TX)        ? "LCD-GPS" :
    (kBoardKind == BK_RX)        ? "LCD-2.1" :
    (kBoardKind == BK_CONVERTER) ? "converter" :
                                   "iOS";

// Control PGN for heartbeat / takeover / going-down. In the N2K
// manufacturer-proprietary single-frame range (65280-65535) so it
// can never collide with a real-bus PGN once the converter starts
// mirroring physical traffic.
constexpr uint32_t kControlPgn = 65500;

}  // namespace vbus
