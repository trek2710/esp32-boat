// include/wifi_credentials.example.h
//
// Template for include/wifi_credentials.h (gitignored). Copy this file to
// wifi_credentials.h before building any WiFi-transport env. Both
// nmea2k_tx_wifi (softAP host) and the RX wifi env (station) include
// this header so the credentials are pinned in one place.
//
// SSID/password are documented in
//   ~/.claude/projects/-Users-jeppekoefoed-Documents-Claude-Projects-esp32-boat/memory/wifi_credentials.md
// and in docs/VIRTUAL_BUS_WIRE.md (Network section).

#pragma once

#define WIFI_AP_SSID      "_wifi_nmea2k"
#define WIFI_AP_PASSWORD  "changeme"        // replace with the real shared password
