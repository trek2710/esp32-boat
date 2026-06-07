// include/Settings.h
//
// Round 85 (v1.6 step 1) — the global settings snapshot the AP broadcasts
// in every heartbeat (ADR-0013). Shared by all three firmware envs.
//
// The data model is a fixed struct (Settings) with one field per key
// defined in ADR-0013. The runtime container (SettingsStore) tracks the
// version, owns NVS persistence, and serialises to / from JSON so the
// heartbeat embedder and the HTTP POST handler can use the same code.
//
// Header-only. Pulls Preferences (Arduino-ESP32) for NVS; that's already
// linked into every env. ArduinoJson is intentionally NOT a dependency —
// the wire format is fixed and tiny, hand-rolled snprintf / strstr is
// enough (same pattern as VirtualBusJson.h).

#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "VirtualBusJson.h"

namespace settings {

// ---- data model --------------------------------------------------------
//
// Defaults match the ADR-0013 v1 table. Bools are 0/1 in NVS for simplicity.

struct Settings {
    // Master gate over ALL simulator publishing (RX sim + converter AIS
    // sim). Off = field mode: only real sources flow. ADR-0015.
    bool sim_master    = true;
    // Simulator channel mask — what RX's wifiSimAndPublish() emits.
    bool sim_wind      = true;
    bool sim_gps       = true;
    bool sim_heading   = true;
    bool sim_depth     = true;
    bool sim_sea_temp  = true;
    bool sim_air_temp  = true;
    bool sim_ais       = true;            // converter's fake-AIS target sim

    // Navigation parameters.
    uint8_t  nav_no_go_deg = 35;          // Main page no-go-zone half-angle

    // AIS filters (applied by LCD + converter).
    uint8_t  ais_range_nm     = 12;       // distance cap (LCDs use; converter ignores — no own-lat/lon)
    bool     ais_hide_anchored = true;    // hide NavStatus ∈ {1, 5, 6}
    uint16_t ais_stale_s      = 600;      // drop targets last_seen > N seconds

    // Display.
    uint8_t  ui_brightness     = 80;      // 0..100
    uint16_t ui_idle_dim_after_s = 300;   // 0 = never dim
};

// ---- store -------------------------------------------------------------
//
// Single instance per peer. Holds the active Settings + a monotonically
// increasing version number. The version bumps on any local apply() call
// (typically: AP receiving an HTTP POST, or a STA adopting a higher
// snapshot from a heartbeat).

class SettingsStore {
public:
    Settings& mutate() { return data_; }
    const Settings& view() const { return data_; }
    uint32_t version() const { return version_; }

    // Load both the data and version from NVS. Returns true if NVS had a
    // saved snapshot; false on first-boot (defaults will be used).
    bool loadFromNvs() {
        Preferences p;
        if (!p.begin(kNvsNamespace, /*readOnly=*/true)) return false;
        const uint32_t v = p.getUInt("v", 0);
        if (v == 0) { p.end(); return false; }
        version_ = v;
        data_.sim_master          = p.getBool("sm",   data_.sim_master);
        data_.sim_wind            = p.getBool("sw",   data_.sim_wind);
        data_.sim_gps             = p.getBool("sg",   data_.sim_gps);
        data_.sim_heading         = p.getBool("sh",   data_.sim_heading);
        data_.sim_depth           = p.getBool("sd",   data_.sim_depth);
        data_.sim_sea_temp        = p.getBool("sst",  data_.sim_sea_temp);
        data_.sim_air_temp        = p.getBool("sat",  data_.sim_air_temp);
        data_.sim_ais             = p.getBool("sia",  data_.sim_ais);
        data_.nav_no_go_deg       = p.getUChar("ng",  data_.nav_no_go_deg);
        data_.ais_range_nm        = p.getUChar("ar",  data_.ais_range_nm);
        data_.ais_hide_anchored   = p.getBool("ah",   data_.ais_hide_anchored);
        data_.ais_stale_s         = p.getUShort("as", data_.ais_stale_s);
        data_.ui_brightness       = p.getUChar("ub",  data_.ui_brightness);
        data_.ui_idle_dim_after_s = p.getUShort("ud", data_.ui_idle_dim_after_s);
        p.end();
        return true;
    }

    // Write the current snapshot to NVS. Called after every successful
    // apply() so a power cycle survives the most recent settings.
    void saveToNvs() const {
        Preferences p;
        if (!p.begin(kNvsNamespace, /*readOnly=*/false)) return;
        p.putUInt  ("v",   version_);
        p.putBool  ("sm",  data_.sim_master);
        p.putBool  ("sw",  data_.sim_wind);
        p.putBool  ("sg",  data_.sim_gps);
        p.putBool  ("sh",  data_.sim_heading);
        p.putBool  ("sd",  data_.sim_depth);
        p.putBool  ("sst", data_.sim_sea_temp);
        p.putBool  ("sat", data_.sim_air_temp);
        p.putBool  ("sia", data_.sim_ais);
        p.putUChar ("ng",  data_.nav_no_go_deg);
        p.putUChar ("ar",  data_.ais_range_nm);
        p.putBool  ("ah",  data_.ais_hide_anchored);
        p.putUShort("as",  data_.ais_stale_s);
        p.putUChar ("ub",  data_.ui_brightness);
        p.putUShort("ud",  data_.ui_idle_dim_after_s);
        p.end();
    }

    // Serialise the settings block to JSON, matching the wire shape in
    // ADR-0013: `"settings_v":N,"settings":{...}`. Includes both the
    // version line and the keys object so the caller can splice it into
    // a heartbeat or HTTP response unchanged. Returns bytes written
    // (excluding nul) or -1 on overflow.
    int exportJson(char* buf, size_t cap) const {
        int n = snprintf(buf, cap,
            "\"settings_v\":%u,"
            "\"settings\":{"
            "\"sim.master\":%s,"
            "\"sim.wind\":%s,\"sim.gps\":%s,\"sim.heading\":%s,"
            "\"sim.depth\":%s,\"sim.sea_temp\":%s,\"sim.air_temp\":%s,"
            "\"sim.ais\":%s,"
            "\"nav.no_go_deg\":%u,"
            "\"ais.range_nm\":%u,\"ais.hide_anchored\":%s,\"ais.stale_s\":%u,"
            "\"ui.brightness\":%u,\"ui.idle_dim_after_s\":%u"
            "}",
            (unsigned)version_,
            data_.sim_master   ? "true" : "false",
            data_.sim_wind     ? "true" : "false",
            data_.sim_gps      ? "true" : "false",
            data_.sim_heading  ? "true" : "false",
            data_.sim_depth    ? "true" : "false",
            data_.sim_sea_temp ? "true" : "false",
            data_.sim_air_temp ? "true" : "false",
            data_.sim_ais      ? "true" : "false",
            (unsigned)data_.nav_no_go_deg,
            (unsigned)data_.ais_range_nm,
            data_.ais_hide_anchored ? "true" : "false",
            (unsigned)data_.ais_stale_s,
            (unsigned)data_.ui_brightness,
            (unsigned)data_.ui_idle_dim_after_s);
        if (n < 0 || (size_t)n >= cap) return -1;
        return n;
    }

    // Apply a JSON body (the `{...}` value of "settings", NOT the wrapper).
    // Bumps version, persists to NVS. Returns the number of keys that
    // changed; 0 means a no-op POST. Unknown keys are silently ignored.
    int applyFromJson(const char* body, bool bump_version = true) {
        int changes = 0;
        changes += applyBool  (body, "sim.master",        &data_.sim_master);
        changes += applyBool  (body, "sim.wind",          &data_.sim_wind);
        changes += applyBool  (body, "sim.gps",           &data_.sim_gps);
        changes += applyBool  (body, "sim.heading",       &data_.sim_heading);
        changes += applyBool  (body, "sim.depth",         &data_.sim_depth);
        changes += applyBool  (body, "sim.sea_temp",      &data_.sim_sea_temp);
        changes += applyBool  (body, "sim.air_temp",      &data_.sim_air_temp);
        changes += applyBool  (body, "sim.ais",           &data_.sim_ais);
        changes += applyU8    (body, "nav.no_go_deg",     &data_.nav_no_go_deg);
        changes += applyU8    (body, "ais.range_nm",      &data_.ais_range_nm);
        changes += applyBool  (body, "ais.hide_anchored", &data_.ais_hide_anchored);
        changes += applyU16   (body, "ais.stale_s",       &data_.ais_stale_s);
        changes += applyU8    (body, "ui.brightness",     &data_.ui_brightness);
        changes += applyU16   (body, "ui.idle_dim_after_s", &data_.ui_idle_dim_after_s);
        if (changes > 0 && bump_version) {
            version_++;
            saveToNvs();
        }
        return changes;
    }

    // STA-side adoption: replace the snapshot wholesale with the AP's
    // (received via heartbeat). Caller pre-extracts the inner
    // `settings_v` int + the inner `settings` body. We only apply if the
    // remote version is strictly greater than ours. Returns true on bump.
    bool adoptFromRemote(uint32_t remote_v, const char* settings_body) {
        if (remote_v <= version_) return false;
        // Apply WITHOUT bumping the version — we want to match the AP's
        // version exactly so subsequent heartbeats don't oscillate.
        applyFromJson(settings_body, /*bump_version=*/false);
        version_ = remote_v;
        saveToNvs();
        return true;
    }

private:
    static constexpr const char* kNvsNamespace = "vbus-set";

    Settings data_;
    uint32_t version_ = 0;

    // Small typed apply helpers. Each returns 1 if the value changed
    // (post-clamping for ranged ints), 0 otherwise.
    static int applyBool(const char* json, const char* key, bool* out) {
        int64_t v;
        if (vbus::findInt(json, key, &v)) {
            bool nv = (v != 0);
            if (nv != *out) { *out = nv; return 1; }
            return 0;
        }
        // Accept literal true/false too.
        const char* p = vbus::findValue(json, key);
        if (!p) return 0;
        if (strncmp(p, "true", 4) == 0)  { if (!*out) { *out = true;  return 1; } return 0; }
        if (strncmp(p, "false", 5) == 0) { if (*out)  { *out = false; return 1; } return 0; }
        return 0;
    }
    static int applyU8(const char* json, const char* key, uint8_t* out) {
        int64_t v;
        if (!vbus::findInt(json, key, &v)) return 0;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        uint8_t nv = (uint8_t)v;
        if (nv != *out) { *out = nv; return 1; }
        return 0;
    }
    static int applyU16(const char* json, const char* key, uint16_t* out) {
        int64_t v;
        if (!vbus::findInt(json, key, &v)) return 0;
        if (v < 0)     v = 0;
        if (v > 65535) v = 65535;
        uint16_t nv = (uint16_t)v;
        if (nv != *out) { *out = nv; return 1; }
        return 0;
    }
};

}  // namespace settings
