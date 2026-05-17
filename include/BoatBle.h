// include/BoatBle.h — shared wire protocol between the transmitter and the
// receiver over BLE GATT.
//
// One custom 128-bit service. Six characteristics: five `NOTIFY`-only
// channels that the TX pushes boat-state data on, plus one `WRITE`-only
// command channel that lets the RX (or a future phone client) configure
// the TX. Categories were chosen so that:
//   - each PDU fits comfortably inside the default 23-byte ATT MTU
//     (largest current PDU is 13 bytes), so we don't have to negotiate
//     MTU at connect time to stay correct,
//   - they line up 1:1 with the existing receiver UI groups (Wind tile
//     cluster, GPS tile cluster, etc.), so the RX BleClient (step 4)
//     can drop each received payload directly into one BoatState
//     setter family,
//   - a generic BLE explorer (nRF Connect, LightBlue, etc.) on a phone
//     can subscribe to whatever subset it wants and ignore the rest.
//
// Wire format conventions:
//   - All multi-byte integers are little-endian (native to both ESP32-S3
//     ends; if a non-ESP32 client ever joins it can byte-swap).
//   - Every PDU starts with a uint8_t valid_mask whose bits indicate which
//     fields in the rest of the struct carry fresh data. Bit 0 corresponds
//     to the first non-mask field; bit positions are documented next to
//     each struct.
//   - Fixed-point integer encoding for everything but lat/lon, to keep
//     PDUs small and avoid IEEE-754 wire-format pitfalls:
//         *_deg10  = degrees × 10     (e.g. 1234  → 123.4°)
//         *_kt100  = knots × 100      (e.g. 1567  → 15.67 kt)
//         *_m10    = metres × 10      (e.g. 152   → 15.2 m)
//         *_c10    = °C × 10          (e.g. 187   → 18.7 °C)
//         *_e7     = degrees × 1e7    (NMEA convention for lat/lon,
//                                       int32_t fits ±214.7°)
//     The receiver re-multiplies these out into floats inside BoatState.
//   - `__attribute__((packed))` on every PDU so the compiler can't
//     introduce padding; static_asserts below pin the expected sizes so
//     any future toolchain change that breaks packing fails at compile
//     time rather than producing scrambled values on the wire.
//
// Versioning: if we ever change the layout of any PDU, the simplest path
// is to bump the last byte of the service UUID. Clients that pinned to the
// old UUID stop connecting (clean failure) rather than silently
// misinterpreting bytes.

#pragma once

#include <stddef.h>
#include <stdint.h>

// All UUIDs share a base prefix `5b6f1e10-b6e0-4f6a-9c8d-4a3e1d2c9f__`.
// The last byte distinguishes service from each characteristic — easier
// to scan in nRF Connect.
#define BOAT_BLE_BASE_UUID       "5b6f1e10-b6e0-4f6a-9c8d-4a3e1d2c9f"
#define BOAT_BLE_SERVICE_UUID    BOAT_BLE_BASE_UUID "01"
#define BOAT_BLE_WIND_UUID       BOAT_BLE_BASE_UUID "10"
#define BOAT_BLE_GPS_UUID        BOAT_BLE_BASE_UUID "11"
#define BOAT_BLE_HEADING_UUID    BOAT_BLE_BASE_UUID "12"
#define BOAT_BLE_DEPTH_TEMP_UUID BOAT_BLE_BASE_UUID "13"
#define BOAT_BLE_ATTITUDE_UUID   BOAT_BLE_BASE_UUID "14"
#define BOAT_BLE_COMMAND_UUID    BOAT_BLE_BASE_UUID "20"

// Local advertised name (also useful in nRF Connect to spot the device).
// Kept short — BLE advertisement payloads have only 31 bytes total, and
// 18 of those are usually taken by the 128-bit service UUID.
#define BOAT_BLE_DEVICE_NAME     "esp32-boat-tx"

namespace boatble {

// ---- Notification PDUs ----------------------------------------------------

// Wind characteristic. Carries true and apparent wind, optionally true
// wind direction. The mask lets the TX skip fields it hasn't observed
// yet, so a fresh boot doesn't transmit zeros that the RX would mistake
// for real data.
//
//   bit 0: TWA   bit 1: TWS   bit 2: TWD   bit 3: AWA   bit 4: AWS
struct __attribute__((packed)) WindPdu {
    uint8_t  valid_mask;
    int16_t  twa_deg10;
    uint16_t tws_kt100;
    int16_t  twd_deg10;
    int16_t  awa_deg10;
    uint16_t aws_kt100;
};
static_assert(sizeof(WindPdu) == 11, "WindPdu unexpected size");

// GPS characteristic. Position + course-over-ground + speed-over-ground.
// Lat/lon use the NMEA 0183 "degrees × 1e7" int32_t convention — gives
// ~1.1 cm resolution at the equator, which is well below GNSS noise.
//
//   bit 0: LAT   bit 1: LON   bit 2: COG   bit 3: SOG
struct __attribute__((packed)) GpsPdu {
    uint8_t  valid_mask;
    int32_t  lat_e7;
    int32_t  lon_e7;
    int16_t  cog_deg10;
    uint16_t sog_kt100;
};
static_assert(sizeof(GpsPdu) == 13, "GpsPdu unexpected size");

// Heading + boat speed (through water). Combined into one PDU because
// they're produced by the same physical sensor stack on most boats
// (compass + paddlewheel) and tend to update at the same rate.
//
//   bit 0: HDG   bit 1: BSPD
struct __attribute__((packed)) HeadingPdu {
    uint8_t  valid_mask;
    int16_t  hdg_deg10;
    uint16_t bspd_kt100;
};
static_assert(sizeof(HeadingPdu) == 5, "HeadingPdu unexpected size");

// Depth + temperatures. DEP is depth below the transducer in metres.
// AIR-T and SEA-T are signed so frost / icy water still encode correctly.
//
//   bit 0: DEP   bit 1: AIR-T   bit 2: SEA-T
struct __attribute__((packed)) DepthTempPdu {
    uint8_t  valid_mask;
    uint16_t dep_m10;
    int16_t  air_temp_c10;
    int16_t  sea_temp_c10;
};
static_assert(sizeof(DepthTempPdu) == 7, "DepthTempPdu unexpected size");

// Attitude + engine. Reserved slots for hardware we don't have on day
// one (HEEL/PITCH need an attitude PGN, ROT needs PGN 127251, RUD needs
// PGN 127245, ENG-T/OIL-T need PGN 127488/127489). The mask means the
// TX can publish whatever it actually receives and the RX renders only
// the populated tiles.
//
//   bit 0: HEEL    bit 1: PITCH    bit 2: ROT
//   bit 3: RUD     bit 4: ENG-T    bit 5: OIL-T
struct __attribute__((packed)) AttitudePdu {
    uint8_t  valid_mask;
    int16_t  heel_deg10;
    int16_t  pitch_deg10;
    int16_t  rot_deg10s;       // rate of turn, deg/s × 10
    int16_t  rud_deg10;
    int16_t  eng_temp_c10;
    int16_t  oil_temp_c10;
};
static_assert(sizeof(AttitudePdu) == 13, "AttitudePdu unexpected size");

// ---- RX → TX command channel ---------------------------------------------

// The command characteristic is `WRITE` (and `WRITE_NO_RESPONSE` for
// snappiness). The first byte is a CommandType enum; the rest of the
// packet is the payload for that command type.
enum class CommandType : uint8_t {
    Ping             = 0x00,  // empty payload, no side-effects (debug)
    SimSetChannels   = 0x01,  // payload: SimSetChannelsPayload
    SimAllOff        = 0x02,  // empty payload, equivalent to SimSetChannels(0xFFFFFFFF, 0)
    SimAllOn         = 0x03,  // empty payload, equivalent to SimSetChannels(0xFFFFFFFF, 1)
};

// Sim channel bits — these mirror the page-3 toggle UI on the RX. The
// TX simulator gates each publish on the matching bit being set.
enum SimChannelBit : uint32_t {
    SIM_CH_WIND     = 1u << 0,
    SIM_CH_GPS      = 1u << 1,
    SIM_CH_HEADING  = 1u << 2,
    SIM_CH_DEPTH    = 1u << 3,
    SIM_CH_AIR_TEMP = 1u << 4,
    SIM_CH_ATTITUDE = 1u << 5,
};

// SimSetChannels(0x09, 1) means: for channels Wind and Depth, set the
// enabled bit. Channels not selected by the mask are left untouched.
struct __attribute__((packed)) SimSetChannelsPayload {
    uint32_t channel_mask;
    uint8_t  enable;          // 0 = disable selected channels, 1 = enable
};
static_assert(sizeof(SimSetChannelsPayload) == 5,
              "SimSetChannelsPayload unexpected size");

// The full command PDU is a 1-byte type followed by a per-type payload.
// We keep the payload as a tail array so callers can subclass with the
// right struct without needing a tagged union here.
struct __attribute__((packed)) CommandHeader {
    CommandType type;
};
static_assert(sizeof(CommandHeader) == 1, "CommandHeader unexpected size");

}  // namespace boatble
