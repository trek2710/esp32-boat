// NMEA0183-AIS -> NMEA2000 converter for the Waveshare ESP32-C6-LCD-1.47.
//
// Reads !AIVDM sentences from a Daisy 2+ AIS receiver on UART1 (38400 baud,
// TTL 3.3 V) and re-emits them as NMEA 2000 AIS PGNs on the CAN bus via an
// SN65HVD230 transceiver. The 1.47" 172x320 ST7789 LCD shows decoded
// targets and CAN link status.

// CAN pins MUST be defined before any NMEA2000 header is included so the
// sergei/NMEA2000_esp32_twai library picks them up.
#define ESP32_CAN_TX_PIN GPIO_NUM_4
#define ESP32_CAN_RX_PIN GPIO_NUM_5
#define NMEA0183_UART_RX_PIN 19

#include <Arduino.h>
#include <NMEA2000.h>
#include <NMEA2000_esp32_twai.h>
#include <N2kMessages.h>

// The ttlappalainen NMEA2000_CAN.h auto-picker only knows about the
// classic-ESP32 backend, so we instantiate the global NMEA2000 object
// ourselves. The vendored AK-Homberger decoder references the symbol
// `NMEA2000` by name; declaring it here makes that work.
NMEA2000_esp32_twai NMEA2000;

#include <NMEA0183.h>
#include <NMEA0183Msg.h>

#include "AisDecoderWithStore.h"
#include "Ui.h"
#include "WifiPublisher.h"

static constexpr size_t MAX_NMEA0183_MESSAGE_SIZE = 150;

static tNMEA0183 NMEA0183;
static tNMEA0183Msg NMEA0183Msg;

static AisDecoderWithStore decoder;
static AIS::DefaultSentenceParser parser;

static uint32_t nmea0183_sentences_seen = 0;
static uint32_t last_nmea0183_ms       = 0;
static uint32_t last_tx_ms             = 0;
static uint32_t last_ui_refresh_ms     = 0;
static uint32_t last_hb_ms             = 0;

static void parseOneAivdm() {
    char buf[MAX_NMEA0183_MESSAGE_SIZE];

    if (!NMEA0183.GetMessage(NMEA0183Msg)) return;
    ++nmea0183_sentences_seen;
    last_nmea0183_ms = millis();
    if (!NMEA0183Msg.IsMessageCode("VDM")) return;
    if (!NMEA0183Msg.GetMessage(buf, MAX_NMEA0183_MESSAGE_SIZE)) return;
    strcat(buf, "\n");

    const uint32_t before = decoder.n2k_sent();
    int i = 0;
    do {
        i = decoder.decodeMsg(buf, strlen(buf), i, parser);
    } while (i != 0);
    if (decoder.n2k_sent() != before) last_tx_ms = millis();
}

void setup() {
    // Drop CPU from 160 to 80 MHz — halves dynamic power. Our workload
    // (UART 38400, TWAI 250 kbit/s, 250 ms LCD refresh) doesn't need the
    // headroom. Must run before Serial.begin so the UART baud divisor is
    // calculated against the new APB clock.
    setCpuFrequencyMhz(80);

    Serial.begin(115200);
    delay(100);
    Serial.println("[boot] NMEA0183-AIS -> N2K converter (C6 + 1.47\" LCD)");

    Serial1.begin(38400, SERIAL_8N1, NMEA0183_UART_RX_PIN, -1);
    NMEA0183.SetMessageStream(&Serial1);
    NMEA0183.Open();

    NMEA2000.SetProductInformation("AIS-N2K-1", 100,
        "Daisy-to-N2K AIS bridge", "1.0.0", "1.0.0");
    NMEA2000.SetDeviceInformation(1, 135, 25, 2046);
    NMEA2000.SetMode(tNMEA2000::N2km_NodeOnly, 35);

    const unsigned long txPgns[] = {
        129038UL, 129039UL, 129040UL,
        129794UL, 129802UL, 129809UL, 129810UL, 0UL
    };
    NMEA2000.ExtendTransmitMessages(txPgns);
    NMEA2000.EnableForward(false);
    NMEA2000.Open();

    ui::begin();
    // Bring up the WiFi virtual-bus participant. Role (AP vs STA) is
    // decided at runtime by RoleNegotiator. AIS multicast publish on
    // top of this is a follow-up — first goal is just to be a visible
    // peer with heartbeats so the bus knows the converter is alive.
    WifiPublisher::begin();
    Serial.println("[boot] ready");
}

// ---- Fake-AIS simulator (bench-only, until a Daisy is wired) ---------------
//
// Three pretend boats drifting in slow circles off Aarhus. Updates the
// AisTargetStore every 6 s (close to a real Class B position-report
// cadence). The WiFi replay loop below converts these into PGNs.

static void simAisTick(uint32_t now) {
    static uint32_t last_tick = 0;
    if (now - last_tick < 6000) return;
    last_tick = now;

    struct FakeAis {
        uint32_t mmsi;
        const char* name;
        uint8_t  ship_type;   // AIS message-24B typeOfShip code
        uint8_t  nav_status;  // 0 = under way using engine, 15 = not defined
        double   base_lat;
        double   base_lon;
        double   speed_kn;
        double   bearing_deg;
    };
    static const FakeAis fakes[] = {
        { 244010001, "LADY ROSE",   60, 0,  56.150, 10.220, 6.5,  45 }, // PAX
        { 244010002, "SEA WOLF",    70, 0,  56.165, 10.215, 8.2,  92 }, // CRG
        { 244010003, "EVA",         80, 15, 56.140, 10.245, 0.3,   0 }, // TNK, drifting
    };
    static double phase = 0.0;
    phase += 0.10;
    auto& store = decoder.store();
    for (auto& f : fakes) {
        // Circle ~0.001° radius around the base point.
        const double dlat = sin(phase + f.mmsi * 0.001) * 0.0010;
        const double dlon = cos(phase + f.mmsi * 0.001) * 0.0010;
        store.recordLatLon(f.mmsi, f.base_lat + dlat, f.base_lon + dlon);
        store.recordPosition(f.mmsi, 'B',
                             (float)f.speed_kn,
                             (float)f.bearing_deg);
        store.recordName(f.mmsi, 'B', f.name);
        store.recordType(f.mmsi, 'B', f.ship_type);
        if (f.nav_status != 15) {
            store.recordNavStatus(f.mmsi, f.nav_status);
        }
    }
}

// ---- AIS-to-WiFi replay (ADR-0012) ----------------------------------------
//
// Walks the AisTargetStore every 5 s and emits each non-empty target as
// PGN 129039 (Class B position) + 129809 (name) + 129810 (ship type).
// Shorter than the ADR-0012 "30 s" target because no per-update push
// path exists yet; this gets new + moving targets onto iOS/LCDs within
// 5 s without the architectural lift of hooking the decoder's emit
// path. Real-time push lands when a Daisy is wired.

static void aisReplayTick(uint32_t now) {
    static uint32_t last_tick = 0;
    if (now - last_tick < 5000) return;
    last_tick = now;

    AisTarget snap[AisTargetStore::CAPACITY];
    const size_t n = decoder.store().snapshotByRecency(snap, AisTargetStore::CAPACITY);
    char body[300];
    for (size_t i = 0; i < n; ++i) {
        const AisTarget& t = snap[i];
        if (t.mmsi == 0) continue;
        // Position report. CANboat field names match what RX dispatch +
        // iOS PgnDispatch expect. SOG -> m/s, COG -> radians.
        if (!std::isnan(t.lat_deg) && !std::isnan(t.lon_deg)) {
            const double sog_ms  = (t.sog_kn  >= 0) ? t.sog_kn  * 0.514444 : 0.0;
            const double cog_rad = (t.cog_deg >= 0) ? t.cog_deg * (M_PI / 180.0) : 0.0;
            snprintf(body, sizeof(body),
                "\"userId\":%lu,\"latitude\":%.6f,\"longitude\":%.6f,"
                "\"sog\":%.3f,\"cog\":%.4f",
                (unsigned long)t.mmsi, t.lat_deg, t.lon_deg, sog_ms, cog_rad);
            WifiPublisher::publishPgnJson(129039, body);
        }
        // Name (PGN 129809). Empty names are skipped.
        if (t.name[0] != 0) {
            snprintf(body, sizeof(body),
                "\"userId\":%lu,\"name\":\"%s\"",
                (unsigned long)t.mmsi, t.name);
            WifiPublisher::publishPgnJson(129809, body);
        }
        // Ship type (PGN 129810). 0 = unknown, skip.
        if (t.vessel_type != 0) {
            snprintf(body, sizeof(body),
                "\"userId\":%lu,\"typeOfShip\":%u",
                (unsigned long)t.mmsi, (unsigned)t.vessel_type);
            WifiPublisher::publishPgnJson(129810, body);
        }
    }
}

void loop() {
    NMEA2000.ParseMessages();
    parseOneAivdm();
    const uint32_t now = millis();
    WifiPublisher::tick(now);
    simAisTick(now);
    aisReplayTick(now);

    if (now - last_ui_refresh_ms >= 250) {
        last_ui_refresh_ms = now;
        decoder.store().evictStale();
        ui::Stats s{
            decoder.n2k_sent(),
            nmea0183_sentences_seen,
            last_tx_ms,
            NMEA2000.GetN2kSource(),
            last_nmea0183_ms,
        };
        ui::refresh(decoder.store(), s);
    }

    if (now - last_hb_ms >= 5000) {
        last_hb_ms = now;
        Serial.printf("[hb] up=%lus  raw=%lu  n2k=%lu  src=%d  "
                      "wifi=%s ap=%s peers=%d ip=%s\n",
                      (unsigned long)(now / 1000),
                      (unsigned long)nmea0183_sentences_seen,
                      (unsigned long)decoder.n2k_sent(),
                      NMEA2000.GetN2kSource(),
                      WifiPublisher::roleName(),
                      WifiPublisher::currentApPeer(),
                      WifiPublisher::peerCount(),
                      WifiPublisher::apIp());
    }
}
