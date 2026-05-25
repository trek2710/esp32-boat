// src_tx/WifiPublisher.cpp
//
// TX-side WiFi virtual-bus participant. Role (AP vs STA) decided at
// runtime by vbus::RoleNegotiator (see docs/adr/0009). On cold boot:
//   1. begin() inits negotiator + parses multicast IP; no WiFi yet
//   2. tick() polls negotiator. After the priority-weighted backoff
//      it does a WiFi.scanNetworks() for the SSID and records the
//      result. Negotiator's next tick returns start_ap or start_sta.
//   3. WiFi mode transitions happen inline in tick(). Socket open/
//      close on every transition (interface IP changes between AP
//      192.168.4.1 and STA 192.168.4.x).
//
// Other implementation notes:
//   - Multicast send uses a BSD socket with IP_MULTICAST_IF pinned to
//     the current interface IP — required because softAP-only ESP32
//     has no default lwIP netif and beginPacket() fails with ERR_IF
//     (-12). Same trick works for STA mode.
//   - Heartbeat receive uses WiFiUDP::beginMulticast() polled from
//     tick(). Volume is tiny (~0.6 pkt/s of control traffic at
//     steady state) so a dedicated task isn't justified.
//   - SoftAP forced to WPA2-PSK + CCMP-only via esp_wifi_set_config()
//     to dodge the ESP32-S3↔S3 4WAY_HANDSHAKE_TIMEOUT bug
//     (memory/s3_s3_wpa_handshake_bug.md).

#include "WifiPublisher.h"

#if TRANSPORT_WIFI

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <string.h>

#include <VirtualBusJson.h>
#include <wifi_credentials.h>

// Round 85 (v1.5b step 7 fix): BoatState + navmath for data dispatch.
#include "BoatState.h"
#include "magnetic_variation.h"
#include <cmath>

// Round 85 (v1.6 step 1, ADR-0013): adopt settings broadcast by the AP.
#include <Settings.h>
#include <VbusRole.h>
#include <RoleNegotiator.h>

// Per-channel packet counters defined in src_tx/main.cpp; the PGN page
// reads them to show measured Hz. Pre-round-80 they were bumped by the
// publishX() functions; post-WiFi-migration TX is a consumer, so we
// bump them in dispatchData() on receive. Declared at global scope (not
// inside the anonymous namespace below) so the linker binds them to the
// main.cpp definitions.
extern volatile uint32_t notifyCountWind;
extern volatile uint32_t notifyCountGps;
extern volatile uint32_t notifyCountHeading;
extern volatile uint32_t notifyCountDepthTemp;
extern volatile uint32_t notifyCountAttitude;

namespace {

vbus::RoleNegotiator g_neg;

int         g_send_sock        = -1;        // unicast send (to AP)
int         g_rx_sock           = -1;        // unicast RX (from AP)
bool        g_rx_udp_open      = false;     // alias for g_rx_sock open
sockaddr_in g_dest             = {};        // AP IP:kBusPort
IPAddress   g_iface_ip;
char        g_iface_ip_str[16] = "0.0.0.0";

bool        g_scan_attempted   = false;     // for the cold-boot ELECTING scan
bool        g_sta_was_connected = false;    // edge detector for STA bring-up
uint32_t    g_last_ap_scan_ms  = 0;         // task #36: dual-AP detect cadence

uint32_t    g_packets_total    = 0;
uint32_t    g_packets_window   = 0;
uint32_t    g_packets_fail_w   = 0;
uint32_t    g_packets_fail_t   = 0;
uint32_t    g_packets_rx_total = 0;       // diagnostic — incoming UDP count
uint8_t     g_sid              = 0;

struct ChannelCache {
    char     last[280];
    size_t   last_len;
    uint32_t last_sent_ms;
    bool     valid;
};
enum Channel : uint8_t {
    CH_WIND_TRUE = 0, CH_WIND_TRUE_GROUND, CH_WIND_APPARENT,
    CH_GPS_POS, CH_GPS_COG_SOG,
    CH_HEADING, CH_BOAT_SPEED,
    CH_DEPTH, CH_AIR_TEMP, CH_SEA_TEMP,
    CH_ATTITUDE, CH_ROT, CH_RUDDER, CH_ENG_TEMP, CH_OIL_TEMP,
    CH_COUNT
};
ChannelCache g_cache[CH_COUNT] = {};

uint8_t nextSid() {
    g_sid = (uint8_t)((g_sid + 1) % 253);
    return g_sid;
}

// True when WiFi link is up enough to publish.
bool onNetwork() {
    auto r = g_neg.role();
    if (r == vbus::RoleNegotiator::Role::AP)  return g_send_sock >= 0;
    if (r == vbus::RoleNegotiator::Role::STA) return g_send_sock >= 0
                                                 && WiFi.status() == WL_CONNECTED;
    return false;
}

// ---- socket lifecycle ----------------------------------------------------

void closeSendSocket() {
    if (g_send_sock >= 0) { close(g_send_sock); g_send_sock = -1; }
}

bool openSendSocket(IPAddress /*iface*/) {
    // Round 84 (ADR-0010): unicast to AP IP. No more IP_MULTICAST_IF —
    // plain SOCK_DGRAM; lwIP's default route is fine for unicast on
    // the STA interface. g_dest is the AP's IP/port; we send the same
    // sockaddr_in every time.
    closeSendSocket();
    g_send_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_send_sock < 0) {
        Serial.printf("[tx-wifi] socket() failed errno=%d\r\n", errno);
        return false;
    }
    memset(&g_dest, 0, sizeof(g_dest));
    g_dest.sin_family      = AF_INET;
    g_dest.sin_port        = htons(vbus::kBusPort);
    IPAddress ap; ap.fromString(vbus::kApIp);
    g_dest.sin_addr.s_addr = (uint32_t)ap;
    return true;
}

void closeRxSocket() {
    if (g_rx_sock >= 0) { close(g_rx_sock); g_rx_sock = -1; }
    g_rx_udp_open = false;
}

bool openRxSocket(IPAddress /*iface*/) {
    // Round 84 (ADR-0010): receive plain unicast on the bus port.
    // No IP_ADD_MEMBERSHIP needed; lwIP will deliver any UDP packet
    // destined for our STA IP on port kBusPort to this socket.
    closeRxSocket();
    g_rx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rx_sock < 0) {
        Serial.printf("[tx-wifi] rx socket() failed errno=%d\r\n", errno);
        return false;
    }
    int yes = 1;
    setsockopt(g_rx_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(vbus::kBusPort);
    if (bind(g_rx_sock, (sockaddr*)&local, sizeof(local)) < 0) {
        Serial.printf("[tx-wifi] rx bind failed errno=%d\r\n", errno);
        close(g_rx_sock); g_rx_sock = -1; return false;
    }
    g_rx_udp_open = true;
    Serial.printf("[tx-wifi] rx: bound to %s:%u (unicast)\r\n",
                  g_iface_ip_str, (unsigned)vbus::kBusPort);
    return true;
}

void reopenSocketsForIface(IPAddress new_iface) {
    g_iface_ip = new_iface;
    snprintf(g_iface_ip_str, sizeof(g_iface_ip_str), "%u.%u.%u.%u",
             new_iface[0], new_iface[1], new_iface[2], new_iface[3]);
    openSendSocket(new_iface);
    openRxSocket(new_iface);
}

// ---- WiFi mode transitions ----------------------------------------------

bool bringUpAp() {
    Serial.println("[tx-wifi] bringing up softAP");
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(WIFI_AP_SSID, nullptr, 1, 0, 4);
    if (!ok) { Serial.println("[tx-wifi] softAP failed"); return false; }
    // Force WPA2-PSK + CCMP per memory/s3_s3_wpa_handshake_bug.md.
    wifi_config_t cfg = {};
    strncpy((char*)cfg.ap.ssid, WIFI_AP_SSID, sizeof(cfg.ap.ssid) - 1);
    strncpy((char*)cfg.ap.password, WIFI_AP_PASSWORD, sizeof(cfg.ap.password) - 1);
    cfg.ap.ssid_len        = strlen(WIFI_AP_SSID);
    cfg.ap.channel         = 1;
    cfg.ap.authmode        = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection  = 4;
    cfg.ap.beacon_interval = 100;
    cfg.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    WiFi.setSleep(false);
    reopenSocketsForIface(WiFi.softAPIP());
    Serial.printf("[tx-wifi] AP up: SSID=%s IP=%s\r\n",
                  WIFI_AP_SSID, g_iface_ip_str);
    return true;
}

void dropAp() {
    Serial.println("[tx-wifi] dropping AP");
    closeSendSocket();
    closeRxSocket();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool bringUpSta(int8_t channel_or_minus1) {
    Serial.printf("[tx-wifi] bringing up STA (ch=%d)\r\n", (int)channel_or_minus1);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    if (channel_or_minus1 > 0) {
        WiFi.begin(WIFI_AP_SSID, WIFI_AP_PASSWORD, channel_or_minus1);
    } else {
        WiFi.begin(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    }
    g_sta_was_connected = false;
    return true;
}

void dropSta() {
    Serial.println("[tx-wifi] dropping STA");
    closeSendSocket();
    closeRxSocket();
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    g_sta_was_connected = false;
}

// ---- cold-boot scan ------------------------------------------------------

void doScanAndRecord() {
    Serial.println("[tx-wifi] scanning for AP SSID...");
    int n = WiFi.scanNetworks(false /*async*/, false, false, 200);
    int8_t found_ch = -1;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == WIFI_AP_SSID) {
            found_ch = (int8_t)WiFi.channel(i);
            Serial.printf("[tx-wifi] found AP on ch %d, RSSI %d\r\n",
                          (int)found_ch, WiFi.RSSI(i));
            break;
        }
    }
    if (found_ch < 0) Serial.println("[tx-wifi] no AP found, will host");
    WiFi.scanDelete();
    g_neg.recordScanResult(found_ch);
    g_scan_attempted = true;
}

// Round 84 (task #36): while in AP role, periodically scan for another
// peer broadcasting the same SSID (dual-AP split brain). If detected,
// step down and re-elect; the negotiator's cold-boot election path will
// scan again as STA and join the surviving AP. RX has the highest
// priority so it could stay AP unconditionally, but it shares this
// helper anyway — when the user adds a future peer with priority > 200,
// the same code converges without a special case.
//
// Scan is ~200 ms and may briefly drop softAP beacons; STAs reconnect
// automatically on association loss.
constexpr uint32_t kApDualDetectMs = 30000;
void detectDualApAndStepDown(uint32_t now) {
    if (now - g_last_ap_scan_ms < kApDualDetectMs) return;
    g_last_ap_scan_ms = now;
    Serial.println("[tx-wifi] AP: scanning for competing _wifi_nmea2k...");
    int n = WiFi.scanNetworks(false /*async*/, false, false, 200);
    uint8_t our_bssid[6] = {};
    WiFi.softAPmacAddress(our_bssid);
    bool dual = false;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) != WIFI_AP_SSID) continue;
        const uint8_t* other = WiFi.BSSID(i);
        if (other && memcmp(our_bssid, other, 6) == 0) continue;  // self
        Serial.printf("[tx-wifi] dual-AP detected on ch %d RSSI %d "
                      "(BSSID %02x:%02x:%02x:%02x:%02x:%02x) — stepping down\r\n",
                      (int)WiFi.channel(i), (int)WiFi.RSSI(i),
                      other[0], other[1], other[2],
                      other[3], other[4], other[5]);
        dual = true;
        break;
    }
    WiFi.scanDelete();
    if (dual) {
        dropAp();
        g_neg.forceReelect(now);
        g_scan_attempted = false;  // re-trigger cold-boot scan path
    }
}

// ---- send helpers --------------------------------------------------------

void sendBytes(const char* json, size_t len) {
    if (g_send_sock < 0 || !onNetwork()) return;
    ssize_t r = sendto(g_send_sock, json, len, 0,
                       (sockaddr*)&g_dest, sizeof(g_dest));
    if (r < 0) { g_packets_fail_t++; g_packets_fail_w++; }
    g_packets_total++; g_packets_window++;
}

void sendHeartbeat(vbus::RoleNegotiator::Event ev, uint32_t now) {
    char buf[256];
    int n = g_neg.buildHeartbeatJson(buf, sizeof(buf), ev,
                                     g_iface_ip_str, now);
    if (n > 0) sendBytes(buf, (size_t)n);
}

void sendCached(Channel ch, uint32_t now) {
    if (!g_cache[ch].valid || g_cache[ch].last_len == 0) return;
    sendBytes(g_cache[ch].last, g_cache[ch].last_len);
    g_cache[ch].last_sent_ms = now;
}

void sendAndCache(Channel ch, const char* json, size_t len, uint32_t now) {
    if (len == 0 || len >= sizeof(g_cache[ch].last)) return;
    sendBytes(json, len);
    memcpy(g_cache[ch].last, json, len);
    g_cache[ch].last[len] = 0;
    g_cache[ch].last_len  = len;
    g_cache[ch].last_sent_ms = now;
    g_cache[ch].valid = true;
}

int buildPacket(char* buf, size_t cap, uint32_t pgn, const char* body) {
    int n = vbus::writeHeader(buf, cap, pgn, vbus::kBoardPeerName);
    if (n < 0 || (size_t)n >= cap) return -1;
    int m = snprintf(buf + n, cap - n, "%s", body);
    if (m < 0 || (size_t)(n + m) >= cap) return -1;
    int e = vbus::writeFooter(buf + n + m, cap - n - m);
    if (e < 0) return -1;
    return n + m + e;
}

// ---- drain (heartbeat + round-85 data dispatch) -------------------------

// Round 85 (v1.5b step 7 fix): consumer for incoming data PGNs. Set by
// main.cpp via setDataConsumer(); nullptr disables data dispatch (only
// heartbeats reach the negotiator, matching pre-round-85 behaviour).
static BoatState* g_data_state = nullptr;


// Round 85 (ADR-0013): AP-broadcast settings snapshot we mirror locally.
// Loaded from NVS in begin(); updated by adoptFromRemote() when the AP
// heartbeat carries a higher settings_v.
static settings::SettingsStore g_settings;

// Dispatch one already-parsed PGN's fields into BoatState. Mirrors the
// equivalent switch in src/NmeaBridge.cpp::dispatch(); kept inline here
// to avoid pulling the whole RX-side dispatcher into the TX build.
static void dispatchData(int64_t pgn, const char* fields, uint32_t now) {
    if (!g_data_state) return;
    // Bump per-channel counter first so the PGN page sees activity
    // regardless of whether the field parse below succeeds (a malformed
    // packet on a known channel is still "a packet on that channel").
    switch (pgn) {
        case 130306: notifyCountWind++; break;
        case 129025: case 129026: notifyCountGps++; break;
        case 127250: case 128259: notifyCountHeading++; break;
        case 128267: case 130316: notifyCountDepthTemp++; break;
        case 127257: case 127251: case 127245: case 127489:
            notifyCountAttitude++; break;
        default: break;
    }
    switch (pgn) {
        case 130306: {  // wind
            char ref[24] = {0};
            double speedMs = 0, angleRad = 0;
            if (!vbus::findString(fields, "reference", ref, sizeof(ref))) return;
            if (strcmp(ref, "Apparent") != 0) return;
            if (!vbus::findDouble(fields, "windSpeed", &speedMs)) return;
            if (!vbus::findDouble(fields, "windAngle", &angleRad)) return;
            g_data_state->setApparentWind(vbus::radToDeg(angleRad),
                                          vbus::msToKnots(speedMs));
            break;
        }
        case 129025: {  // GPS position
            double lat = 0, lon = 0;
            if (!vbus::findDouble(fields, "latitude",  &lat)) return;
            if (!vbus::findDouble(fields, "longitude", &lon)) return;
            g_data_state->setGps(lat, lon);
            const double var = navmath::lookupMagneticVariation(lat, lon);
            if (!std::isnan(var)) g_data_state->setMagneticVariation(var);
            break;
        }
        case 127250: {  // heading (magnetic)
            char ref[24] = {0};
            double hdgRad = 0;
            if (!vbus::findDouble(fields, "heading", &hdgRad)) return;
            if (vbus::findString(fields, "reference", ref, sizeof(ref))
                && strcmp(ref, "Magnetic") != 0) return;
            g_data_state->setMagneticHeading(vbus::radToDeg(hdgRad));
            break;
        }
        case 128259: {  // STW
            double speedMs = 0;
            if (!vbus::findDouble(fields, "speedWaterReferenced", &speedMs)) return;
            g_data_state->setStw(vbus::msToKnots(speedMs));
            break;
        }
        case 128267: {  // depth
            double depthM = 0;
            if (!vbus::findDouble(fields, "depth", &depthM)) return;
            g_data_state->setDepth(depthM);
            break;
        }
        case 130316: {  // temp (sea or outside)
            char src[40] = {0};
            double tK = 0;
            if (!vbus::findString(fields, "source", src, sizeof(src))) return;
            if (!vbus::findDouble(fields, "actualTemperature", &tK)) return;
            const double tC = vbus::kelvinToCelsius(tK);
            if      (strcmp(src, "Sea Temperature")     == 0) g_data_state->setSeaTemp(tC);
            else if (strcmp(src, "Outside Temperature") == 0) g_data_state->setAirTemp(tC);
            break;
        }
        // AIS — same wire shape as the RX dispatcher.
        case 129038:
        case 129039:
        case 129040: {
            int64_t mmsi = 0;
            double  lat = NAN, lon = NAN, sog_ms = NAN, cog_rad = NAN;
            if (!vbus::findInt(fields, "userId", &mmsi) || mmsi <= 0) break;
            vbus::findDouble(fields, "latitude",  &lat);
            vbus::findDouble(fields, "longitude", &lon);
            vbus::findDouble(fields, "sog",       &sog_ms);
            vbus::findDouble(fields, "cog",       &cog_rad);
            AisTarget t{};
            t.mmsi         = (uint32_t)mmsi;
            t.lat          = lat;
            t.lon          = lon;
            t.sog          = std::isnan(sog_ms)  ? NAN : vbus::msToKnots(sog_ms);
            t.cog          = std::isnan(cog_rad) ? NAN : vbus::radToDeg(cog_rad);
            t.last_seen_ms = now;
            if (pgn == 129040) {
                char nm[20] = {0};
                if (vbus::findString(fields, "name", nm, sizeof(nm))) {
                    strncpy(t.name, nm, sizeof(t.name) - 1);
                }
            }
            g_data_state->upsertAisTarget(t);
            break;
        }
        case 129809: {
            int64_t mmsi = 0;
            char    nm[20] = {0};
            if (!vbus::findInt(fields, "userId", &mmsi) || mmsi <= 0) break;
            if (!vbus::findString(fields, "name", nm, sizeof(nm))) break;
            AisTarget t{};
            t.mmsi         = (uint32_t)mmsi;
            strncpy(t.name, nm, sizeof(t.name) - 1);
            t.last_seen_ms = now;
            g_data_state->upsertAisTarget(t);
            break;
        }
        case 129810: {
            int64_t mmsi = 0;
            int64_t st_i = 0;
            if (!vbus::findInt(fields, "userId", &mmsi) || mmsi <= 0) break;
            if (!vbus::findInt(fields, "typeOfShip", &st_i)) break;
            AisTarget t{};
            t.mmsi         = (uint32_t)mmsi;
            t.ship_type    = (uint8_t)st_i;
            t.last_seen_ms = now;
            g_data_state->upsertAisTarget(t);
            break;
        }
        default: break;
    }
}

void drainOnce(uint32_t now) {
    if (g_rx_sock < 0) return;
    char buf[512];
    for (;;) {
        sockaddr_in src; socklen_t srclen = sizeof(src);
        int n = recvfrom(g_rx_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                         (sockaddr*)&src, &srclen);
        if (n <= 0) break;
        g_packets_rx_total++;
        buf[n] = 0;
        int64_t pgn = 0;
        if (!vbus::findInt(buf, "pgn", &pgn)) continue;
        const char* fields = nullptr; size_t flen = 0;
        if (!vbus::findFieldsBody(buf, &fields, &flen)) continue;
        char fbuf[256];
        if (flen >= sizeof(fbuf)) continue;
        memcpy(fbuf, fields, flen); fbuf[flen] = 0;

        if (pgn == (int64_t)vbus::kControlPgn) {
            char peer[24];
            if (vbus::findString(buf, "peer", peer, sizeof(peer))) {
                g_neg.onHeartbeat(peer, fbuf, now);
            }
            // Round 85 (ADR-0013): if STA, adopt the AP's settings
            // snapshot when its version is higher than ours.
            if (g_neg.role() == vbus::RoleNegotiator::Role::STA) {
                int64_t remote_v = 0;
                if (vbus::findInt(buf, "settings_v", &remote_v) && remote_v > 0) {
                    const char* sbody = nullptr; size_t slen = 0;
                    if (vbus::findObjectBody(buf, "settings", &sbody, &slen)) {
                        char sbuf[512];
                        if (slen < sizeof(sbuf)) {
                            memcpy(sbuf, sbody, slen);
                            sbuf[slen] = '\0';
                            if (g_settings.adoptFromRemote(
                                    (uint32_t)remote_v, sbuf)) {
                                Serial.printf("[settings] adopted v%u "
                                              "from AP\r\n",
                                              (unsigned)remote_v);
                            }
                        }
                    }
                }
            }
        } else {
            dispatchData(pgn, fbuf, now);
        }
    }
}

}  // namespace

namespace WifiPublisher {

void setDataConsumer(BoatState* state) {
    g_data_state = state;
}

bool begin() {
    Serial.println("[tx-wifi] init RoleNegotiator (role decided in tick())");
    g_neg.init(millis());
    if (g_settings.loadFromNvs()) {
        Serial.printf("[settings] loaded v%u from NVS\r\n",
                      (unsigned)g_settings.version());
    } else {
        Serial.println("[settings] no NVS snapshot — using defaults");
    }
    return true;
}

void tick(uint32_t now) {
    // Cold-boot scan: once backoff elapses, scan and record result. The
    // negotiator's next tick will transition to AP or STA.
    if (g_neg.role() == vbus::RoleNegotiator::Role::ELECTING && !g_scan_attempted) {
        // Approximate backoff check using same formula as negotiator.
        uint32_t backoff =
            (vbus::kBoardPriority >= 200) ? 0    :
            (vbus::kBoardPriority >= 150) ? 4000 :
            (vbus::kBoardPriority >= 100) ? 8000 : 12000;
        static uint32_t s_boot_ms = millis();
        if (now - s_boot_ms >= backoff) doScanAndRecord();
    }
    if (g_neg.role() != vbus::RoleNegotiator::Role::ELECTING) g_scan_attempted = true;

    // Edge: STA just got WL_CONNECTED → open sockets bound to STA IP.
    if (g_neg.role() == vbus::RoleNegotiator::Role::STA) {
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !g_sta_was_connected) {
            Serial.printf("[tx-wifi] STA associated, ip=%s rssi=%d\r\n",
                          WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            reopenSocketsForIface(WiFi.localIP());
        }
        g_sta_was_connected = connected;
    }

    auto a = g_neg.tick(now);
    if (a.drop_ap)  dropAp();
    if (a.drop_sta) dropSta();
    if (a.start_ap) bringUpAp();
    if (a.start_sta) bringUpSta(a.target_channel);
    if (a.publish_heartbeat)
        sendHeartbeat(vbus::RoleNegotiator::Event::HEARTBEAT, now);
    if (a.publish_takeover)
        sendHeartbeat(vbus::RoleNegotiator::Event::TAKEOVER_ANNOUNCE, now);
    if (a.publish_going_down)
        sendHeartbeat(vbus::RoleNegotiator::Event::GOING_DOWN, now);

    drainOnce(now);

    // Task #36: if we ended up AP, periodically check for a competing
    // AP on the same SSID. Reset the cadence timer on the AP→STA edge
    // so STA→AP transitions get a full interval before the first scan.
    if (g_neg.role() == vbus::RoleNegotiator::Role::AP) {
        detectDualApAndStepDown(now);
    } else {
        g_last_ap_scan_ms = now;
    }
}

// ---- per-PDU publishers (unchanged from round 81) ------------------------

void publishWind(const boatble::WindPdu& pdu, uint32_t now) {
    if (!onNetwork()) return;
    const uint8_t sid = nextSid();
    char buf[256], body[160];
    if ((pdu.valid_mask & (1u << 0)) && (pdu.valid_mask & (1u << 1))) {
        double tws = vbus::knotsToMs(pdu.tws_kt100 / 100.0);
        double twa = vbus::degToRad(pdu.twa_deg10 / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"windSpeed\":%.3f,\"windAngle\":%.4f,"
                 "\"reference\":\"True\"", (unsigned)sid, tws, twa);
        int len = buildPacket(buf, sizeof(buf), 130306, body);
        if (len > 0) sendAndCache(CH_WIND_TRUE, buf, (size_t)len, now);
    }
    if ((pdu.valid_mask & (1u << 2)) && (pdu.valid_mask & (1u << 1))) {
        double tws = vbus::knotsToMs(pdu.tws_kt100 / 100.0);
        double twd = vbus::degToRad(pdu.twd_deg10 / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"windSpeed\":%.3f,\"windAngle\":%.4f,"
                 "\"reference\":\"True (ground referenced to North)\"",
                 (unsigned)sid, tws, twd);
        int len = buildPacket(buf, sizeof(buf), 130306, body);
        if (len > 0) sendAndCache(CH_WIND_TRUE_GROUND, buf, (size_t)len, now);
    }
    if ((pdu.valid_mask & (1u << 3)) && (pdu.valid_mask & (1u << 4))) {
        double aws = vbus::knotsToMs(pdu.aws_kt100 / 100.0);
        double awa = vbus::degToRad(pdu.awa_deg10 / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"windSpeed\":%.3f,\"windAngle\":%.4f,"
                 "\"reference\":\"Apparent\"", (unsigned)sid, aws, awa);
        int len = buildPacket(buf, sizeof(buf), 130306, body);
        if (len > 0) sendAndCache(CH_WIND_APPARENT, buf, (size_t)len, now);
    }
}

void publishGps(const boatble::GpsPdu& pdu, uint32_t now) {
    if (!onNetwork()) return;
    const uint8_t sid = nextSid();
    char buf[256], body[160];
    if ((pdu.valid_mask & (1u << 0)) && (pdu.valid_mask & (1u << 1))) {
        double lat = pdu.lat_e7 / 1e7;
        double lon = pdu.lon_e7 / 1e7;
        snprintf(body, sizeof(body),
                 "\"latitude\":%.7f,\"longitude\":%.7f", lat, lon);
        int len = buildPacket(buf, sizeof(buf), 129025, body);
        if (len > 0) sendAndCache(CH_GPS_POS, buf, (size_t)len, now);
    }
    if ((pdu.valid_mask & (1u << 2)) && (pdu.valid_mask & (1u << 3))) {
        double cog = vbus::degToRad(pdu.cog_deg10 / 10.0);
        double sog = vbus::knotsToMs(pdu.sog_kt100 / 100.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"cogReference\":\"True\","
                 "\"cog\":%.4f,\"sog\":%.3f", (unsigned)sid, cog, sog);
        int len = buildPacket(buf, sizeof(buf), 129026, body);
        if (len > 0) sendAndCache(CH_GPS_COG_SOG, buf, (size_t)len, now);
    }
}

void publishHeading(const boatble::HeadingPdu& pdu, uint32_t now) {
    if (!onNetwork()) return;
    const uint8_t sid = nextSid();
    char buf[256], body[160];
    if (pdu.valid_mask & (1u << 0)) {
        double hdg = vbus::degToRad(pdu.hdg_deg10 / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"heading\":%.4f,\"reference\":\"Magnetic\"",
                 (unsigned)sid, hdg);
        int len = buildPacket(buf, sizeof(buf), 127250, body);
        if (len > 0) sendAndCache(CH_HEADING, buf, (size_t)len, now);
    }
    if (pdu.valid_mask & (1u << 1)) {
        double sw = vbus::knotsToMs(pdu.bspd_kt100 / 100.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"speedWaterReferenced\":%.3f,"
                 "\"speedWaterReferencedType\":\"Paddle wheel\"",
                 (unsigned)sid, sw);
        int len = buildPacket(buf, sizeof(buf), 128259, body);
        if (len > 0) sendAndCache(CH_BOAT_SPEED, buf, (size_t)len, now);
    }
}

void publishDepthTemp(const boatble::DepthTempPdu& pdu, uint32_t now) {
    if (!onNetwork()) return;
    const uint8_t sid = nextSid();
    char buf[256], body[160];
    if (pdu.valid_mask & (1u << 0)) {
        double d = pdu.dep_m10 / 10.0;
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"depth\":%.2f,\"offset\":0.0",
                 (unsigned)sid, d);
        int len = buildPacket(buf, sizeof(buf), 128267, body);
        if (len > 0) sendAndCache(CH_DEPTH, buf, (size_t)len, now);
    }
    if (pdu.valid_mask & (1u << 1)) {
        double k = vbus::celsiusToKelvin(pdu.air_temp_c10 / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"instance\":0,"
                 "\"source\":\"Outside Temperature\","
                 "\"actualTemperature\":%.2f", (unsigned)sid, k);
        int len = buildPacket(buf, sizeof(buf), 130316, body);
        if (len > 0) sendAndCache(CH_AIR_TEMP, buf, (size_t)len, now);
    }
    if (pdu.valid_mask & (1u << 2)) {
        double k = vbus::celsiusToKelvin(pdu.sea_temp_c10 / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"instance\":1,"
                 "\"source\":\"Sea Temperature\","
                 "\"actualTemperature\":%.2f", (unsigned)sid, k);
        int len = buildPacket(buf, sizeof(buf), 130316, body);
        if (len > 0) sendAndCache(CH_SEA_TEMP, buf, (size_t)len, now);
    }
}

void publishAttitude(const boatble::AttitudePdu& pdu, uint32_t now) {
    if (!onNetwork()) return;
    const uint8_t sid = nextSid();
    char buf[256], body[160];
    if (pdu.valid_mask & ((1u << 0) | (1u << 1))) {
        const bool hasHeel  = pdu.valid_mask & (1u << 0);
        const bool hasPitch = pdu.valid_mask & (1u << 1);
        double roll  = hasHeel  ? vbus::degToRad(pdu.heel_deg10  / 10.0) : 0.0;
        double pitch = hasPitch ? vbus::degToRad(pdu.pitch_deg10 / 10.0) : 0.0;
        int written = snprintf(body, sizeof(body),
                               "\"sid\":%u,\"yaw\":null", (unsigned)sid);
        written += snprintf(body + written, sizeof(body) - written,
                            hasPitch ? ",\"pitch\":%.4f" : ",\"pitch\":null", pitch);
        written += snprintf(body + written, sizeof(body) - written,
                            hasHeel  ? ",\"roll\":%.4f"  : ",\"roll\":null",  roll);
        int len = buildPacket(buf, sizeof(buf), 127257, body);
        if (len > 0) sendAndCache(CH_ATTITUDE, buf, (size_t)len, now);
    }
    if (pdu.valid_mask & (1u << 2)) {
        double rot = vbus::degToRad(pdu.rot_deg10s / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"rate\":%.5f", (unsigned)sid, rot);
        int len = buildPacket(buf, sizeof(buf), 127251, body);
        if (len > 0) sendAndCache(CH_ROT, buf, (size_t)len, now);
    }
    if (pdu.valid_mask & (1u << 3)) {
        double rud = vbus::degToRad(pdu.rud_deg10 / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"instance\":0,\"position\":%.4f,"
                 "\"directionOrder\":\"No Order\"", (unsigned)sid, rud);
        int len = buildPacket(buf, sizeof(buf), 127245, body);
        if (len > 0) sendAndCache(CH_RUDDER, buf, (size_t)len, now);
    }
    if (pdu.valid_mask & (1u << 4)) {
        double k = vbus::celsiusToKelvin(pdu.eng_temp_c10 / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"instance\":0,"
                 "\"engineCoolantTemperature\":%.2f", (unsigned)sid, k);
        int len = buildPacket(buf, sizeof(buf), 127489, body);
        if (len > 0) sendAndCache(CH_ENG_TEMP, buf, (size_t)len, now);
    }
    if (pdu.valid_mask & (1u << 5)) {
        double k = vbus::celsiusToKelvin(pdu.oil_temp_c10 / 10.0);
        snprintf(body, sizeof(body),
                 "\"sid\":%u,\"instance\":0,"
                 "\"engineOilTemperature\":%.2f", (unsigned)sid, k);
        int len = buildPacket(buf, sizeof(buf), 127489, body);
        if (len > 0) sendAndCache(CH_OIL_TEMP, buf, (size_t)len, now);
    }
}

uint32_t stationCount() {
    return (g_neg.role() == vbus::RoleNegotiator::Role::AP)
           ? (uint32_t)WiFi.softAPgetStationNum() : 0;
}
const char* apIp()                 { return g_iface_ip_str; }
uint32_t packetsSent()             { return g_packets_total; }
uint32_t packetsInWindow()         { return g_packets_window; }
uint32_t packetsFailedInWindow()   { return g_packets_fail_w; }
uint32_t packetsRxTotal()          { return g_packets_rx_total; }
void resetWindow() { g_packets_window = 0; g_packets_fail_w = 0; }
const char* roleName()             { return vbus::RoleNegotiator::roleName(g_neg.role()); }
const char* currentApPeer()        { return g_neg.currentApPeerName(); }
int  peerCount()                   { return g_neg.peerCount(); }
bool onNetwork()                   { return ::onNetwork(); }

}  // namespace WifiPublisher

#else  // !TRANSPORT_WIFI — BLE TX env still pulls this file via build_src_filter

namespace WifiPublisher {
void setDataConsumer(BoatState*) {}
bool begin() { return false; }
void tick(uint32_t) {}
void publishWind     (const boatble::WindPdu&,     uint32_t) {}
void publishGps      (const boatble::GpsPdu&,      uint32_t) {}
void publishHeading  (const boatble::HeadingPdu&,  uint32_t) {}
void publishDepthTemp(const boatble::DepthTempPdu&, uint32_t) {}
void publishAttitude (const boatble::AttitudePdu&, uint32_t) {}
uint32_t stationCount()          { return 0; }
const char* apIp()               { return "0.0.0.0"; }
uint32_t packetsSent()           { return 0; }
uint32_t packetsInWindow()       { return 0; }
uint32_t packetsFailedInWindow() { return 0; }
uint32_t packetsRxTotal()        { return 0; }
void resetWindow() {}
const char* roleName()           { return "n/a"; }
const char* currentApPeer()      { return ""; }
int peerCount()                  { return 0; }
bool onNetwork()                 { return false; }
}  // namespace WifiPublisher

#endif  // TRANSPORT_WIFI
