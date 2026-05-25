// src_converter/WifiPublisher.cpp
//
// Mirrors src_tx/WifiPublisher.cpp's role-election + heartbeat plumbing.
// Differences from the TX version:
//   - No publishX functions yet — converter doesn't publish data PGNs
//     onto multicast this round (deferred).
//   - Drain only cares about the control PGN (peer discovery) — it
//     doesn't dispatch into any local state.
//   - Built against Arduino-ESP32 3.x via the pioarduino platform fork.
//     Same WiFi.h / esp_wifi.h / lwip/sockets.h APIs as 2.x for the
//     subset we use (softAP, scanNetworks, begin, esp_wifi_set_config,
//     sendto, IP_MULTICAST_IF, WiFiUDP::beginMulticast).

#include "WifiPublisher.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <string.h>

#include <VirtualBusJson.h>
#include <wifi_credentials.h>
#include <Settings.h>          // round 85 v1.6 step 1
#include <VbusRole.h>
#include <RoleNegotiator.h>

namespace {

vbus::RoleNegotiator g_neg;

int         g_send_sock        = -1;
int         g_rx_sock           = -1;
bool        g_rx_udp_open      = false;     // alias
sockaddr_in g_dest             = {};        // AP IP:kBusPort
IPAddress   g_iface_ip;
char        g_iface_ip_str[16] = "0.0.0.0";

bool        g_scan_attempted   = false;
bool        g_sta_was_connected = false;
uint32_t    g_last_ap_scan_ms  = 0;         // task #36: dual-AP detect cadence

// Round 85 (ADR-0013): mirror of the AP-broadcast settings snapshot.
settings::SettingsStore g_settings;

void closeSendSocket() {
    if (g_send_sock >= 0) { close(g_send_sock); g_send_sock = -1; }
}

bool openSendSocket(IPAddress /*iface*/) {
    // Round 84 (ADR-0010): unicast to AP IP.
    closeSendSocket();
    g_send_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_send_sock < 0) return false;
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
    // Round 84 (ADR-0010): plain unicast bind on bus port; no
    // IP_ADD_MEMBERSHIP since there's no multicast group any more.
    closeRxSocket();
    g_rx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_rx_sock < 0) {
        Serial.printf("[conv-wifi] rx socket() failed errno=%d\r\n", errno);
        return false;
    }
    int yes = 1;
    setsockopt(g_rx_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(vbus::kBusPort);
    if (bind(g_rx_sock, (sockaddr*)&local, sizeof(local)) < 0) {
        Serial.printf("[conv-wifi] rx bind failed errno=%d\r\n", errno);
        close(g_rx_sock); g_rx_sock = -1; return false;
    }
    g_rx_udp_open = true;
    Serial.printf("[conv-wifi] rx: bound to %s:%u (unicast)\r\n",
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

bool bringUpAp() {
    Serial.println("[conv-wifi] bringing up softAP");
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(WIFI_AP_SSID, nullptr, 1, 0, 4);
    if (!ok) { Serial.println("[conv-wifi] softAP failed"); return false; }
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
    Serial.printf("[conv-wifi] AP up: SSID=%s IP=%s\r\n",
                  WIFI_AP_SSID, g_iface_ip_str);
    return true;
}

void dropAp() {
    Serial.println("[conv-wifi] dropping AP");
    closeSendSocket(); closeRxSocket();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool bringUpSta(int8_t channel_or_minus1) {
    Serial.printf("[conv-wifi] bringing up STA (ch=%d)\r\n",
                  (int)channel_or_minus1);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    if (channel_or_minus1 > 0)
        WiFi.begin(WIFI_AP_SSID, WIFI_AP_PASSWORD, channel_or_minus1);
    else
        WiFi.begin(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    g_sta_was_connected = false;
    return true;
}

void dropSta() {
    Serial.println("[conv-wifi] dropping STA");
    closeSendSocket(); closeRxSocket();
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    g_sta_was_connected = false;
}

void doScanAndRecord() {
    Serial.println("[conv-wifi] scanning for AP SSID...");
    int n = WiFi.scanNetworks(false, false, false, 200);
    int8_t found_ch = -1;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == WIFI_AP_SSID) {
            found_ch = (int8_t)WiFi.channel(i);
            Serial.printf("[conv-wifi] found AP on ch %d\r\n", (int)found_ch);
            break;
        }
    }
    if (found_ch < 0) Serial.println("[conv-wifi] no AP found, will host");
    WiFi.scanDelete();
    g_neg.recordScanResult(found_ch);
    g_scan_attempted = true;
}

// Round 84 (task #36): see src_tx/WifiPublisher.cpp for rationale —
// periodic AP-side scan for a competing _wifi_nmea2k. On detection,
// drop AP + force re-election; cold-boot election path then re-joins
// the survivor as STA. RX (priority 200) always outranks converter
// (150) and TX (100), so in practice this converges with RX as AP.
constexpr uint32_t kApDualDetectMs = 30000;
void detectDualApAndStepDown(uint32_t now) {
    if (now - g_last_ap_scan_ms < kApDualDetectMs) return;
    g_last_ap_scan_ms = now;
    Serial.println("[conv-wifi] AP: scanning for competing _wifi_nmea2k...");
    int n = WiFi.scanNetworks(false, false, false, 200);
    uint8_t our_bssid[6] = {};
    WiFi.softAPmacAddress(our_bssid);
    bool dual = false;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) != WIFI_AP_SSID) continue;
        const uint8_t* other = WiFi.BSSID(i);
        if (other && memcmp(our_bssid, other, 6) == 0) continue;
        Serial.printf("[conv-wifi] dual-AP detected on ch %d RSSI %d — "
                      "stepping down\r\n",
                      (int)WiFi.channel(i), (int)WiFi.RSSI(i));
        dual = true;
        break;
    }
    WiFi.scanDelete();
    if (dual) {
        dropAp();
        g_neg.forceReelect(now);
        g_scan_attempted = false;
    }
}

bool onNetworkLocal() {
    auto r = g_neg.role();
    if (r == vbus::RoleNegotiator::Role::AP)  return g_send_sock >= 0;
    if (r == vbus::RoleNegotiator::Role::STA) return g_send_sock >= 0
                                                 && WiFi.status() == WL_CONNECTED;
    return false;
}

void sendBytes(const char* json, size_t len) {
    if (g_send_sock < 0 || !onNetworkLocal()) return;
    sendto(g_send_sock, json, len, 0, (sockaddr*)&g_dest, sizeof(g_dest));
}

void sendHeartbeat(vbus::RoleNegotiator::Event ev, uint32_t now) {
    char buf[256];
    int n = g_neg.buildHeartbeatJson(buf, sizeof(buf), ev,
                                     g_iface_ip_str, now);
    if (n > 0) sendBytes(buf, (size_t)n);
}

void drainOnce(uint32_t now) {
    if (g_rx_sock < 0) return;
    char buf[512];
    for (;;) {
        sockaddr_in src; socklen_t srclen = sizeof(src);
        int n = recvfrom(g_rx_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                         (sockaddr*)&src, &srclen);
        if (n <= 0) break;
        buf[n] = 0;
        int64_t pgn = 0;
        if (!vbus::findInt(buf, "pgn", &pgn)) continue;
        if (pgn != (int64_t)vbus::kControlPgn) continue;
        char peer[24]; if (!vbus::findString(buf, "peer", peer, sizeof(peer))) continue;
        const char* fields = nullptr; size_t flen = 0;
        if (!vbus::findFieldsBody(buf, &fields, &flen)) continue;
        char fbuf[256];
        if (flen >= sizeof(fbuf)) continue;
        memcpy(fbuf, fields, flen); fbuf[flen] = 0;
        g_neg.onHeartbeat(peer, fbuf, now);

        // Round 85 (ADR-0013): adopt AP-broadcast settings when STA.
        if (g_neg.role() == vbus::RoleNegotiator::Role::STA) {
            int64_t remote_v = 0;
            if (vbus::findInt(buf, "settings_v", &remote_v) && remote_v > 0) {
                const char* sbody = nullptr; size_t slen = 0;
                if (vbus::findObjectBody(buf, "settings", &sbody, &slen)) {
                    char sbuf[512];
                    if (slen < sizeof(sbuf)) {
                        memcpy(sbuf, sbody, slen);
                        sbuf[slen] = '\0';
                        if (g_settings.adoptFromRemote((uint32_t)remote_v, sbuf)) {
                            Serial.printf("[settings] adopted v%u from AP\n",
                                          (unsigned)remote_v);
                        }
                    }
                }
            }
        }
    }
}

}  // namespace

namespace WifiPublisher {

bool begin() {
    Serial.println("[conv-wifi] init RoleNegotiator");
    g_neg.init(millis());
    if (g_settings.loadFromNvs()) {
        Serial.printf("[settings] loaded v%u from NVS\n",
                      (unsigned)g_settings.version());
    } else {
        Serial.println("[settings] no NVS snapshot — using defaults");
    }
    return true;
}

void tick(uint32_t now) {
    if (g_neg.role() == vbus::RoleNegotiator::Role::ELECTING && !g_scan_attempted) {
        uint32_t backoff =
            (vbus::kBoardPriority >= 200) ? 0    :
            (vbus::kBoardPriority >= 150) ? 4000 :
            (vbus::kBoardPriority >= 100) ? 8000 : 12000;
        static uint32_t s_boot_ms = millis();
        if (now - s_boot_ms >= backoff) doScanAndRecord();
    }
    if (g_neg.role() != vbus::RoleNegotiator::Role::ELECTING) g_scan_attempted = true;

    if (g_neg.role() == vbus::RoleNegotiator::Role::STA) {
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !g_sta_was_connected) {
            Serial.printf("[conv-wifi] STA associated, ip=%s rssi=%d\r\n",
                          WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            reopenSocketsForIface(WiFi.localIP());
        }
        g_sta_was_connected = connected;
    }

    auto a = g_neg.tick(now);
    if (a.drop_ap)            dropAp();
    if (a.drop_sta)           dropSta();
    if (a.start_ap)           bringUpAp();
    if (a.start_sta)          bringUpSta(a.target_channel);
    if (a.publish_heartbeat)
        sendHeartbeat(vbus::RoleNegotiator::Event::HEARTBEAT, now);
    if (a.publish_takeover)
        sendHeartbeat(vbus::RoleNegotiator::Event::TAKEOVER_ANNOUNCE, now);
    if (a.publish_going_down)
        sendHeartbeat(vbus::RoleNegotiator::Event::GOING_DOWN, now);

    drainOnce(now);

    // Task #36: AP-side dual-AP detection. Reset the cadence on non-AP
    // role so the first scan after promotion has a full interval.
    if (g_neg.role() == vbus::RoleNegotiator::Role::AP) {
        detectDualApAndStepDown(now);
    } else {
        g_last_ap_scan_ms = now;
    }
}

const settings::Settings& currentSettings() { return g_settings.view(); }

void publishPgnJson(uint32_t pgn, const char* fields_body) {
    if (g_send_sock < 0 || !fields_body) return;
    // Same envelope as the firmware's other publish helpers (see
    // src/NmeaBridge.cpp::buildPgnJson). src=35 is the converter's
    // claimed N2K source address (NMEA2000.SetMode(..., 35) in main).
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"pgn\":%u,\"src\":35,\"peer\":\"nmea-converter\","
        "\"fields\":{%s}}",
        (unsigned)pgn, fields_body);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    sendBytes(buf, (size_t)n);
}

const char* roleName()       { return vbus::RoleNegotiator::roleName(g_neg.role()); }
const char* currentApPeer()  { return g_neg.currentApPeerName(); }
int  peerCount()             { return g_neg.peerCount(); }
bool onNetwork()             { return onNetworkLocal(); }
const char* apIp()           { return g_iface_ip_str; }
uint32_t stationCount() {
    return (g_neg.role() == vbus::RoleNegotiator::Role::AP)
           ? (uint32_t)WiFi.softAPgetStationNum() : 0;
}

}  // namespace WifiPublisher
