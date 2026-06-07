#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <cmath>

// Small fixed-size table of recently seen AIS targets. The UI reads it; the
// decoder writes it. Indexed only by MMSI; on overflow the oldest target by
// last_seen_ms is evicted. Not thread-safe — both the decoder and the UI
// run on the same loop, single-core C6 with no preemption between them.

struct AisTarget {
    uint32_t mmsi;           // 0 means slot is empty
    char     name[21];        // 20-char NMEA AIS name, NUL-padded; "" if unknown
    char     klass;           // 'A', 'B', or '?'
    float    sog_kn;          // < 0 means unknown
    float    cog_deg;         // < 0 means unknown
    uint8_t  vessel_type;     // 0 = unknown / not available
    uint8_t  nav_status;      // 15 = not defined / unknown (AIS default)
    // Round 85 v1.6 step 3: position. NaN means "no fix received yet."
    // Populated by recordPosition() and consumed by the WiFi publisher
    // when it emits PGN 129038 / 129039 / 129040 onto the virtual bus.
    double   lat_deg;
    double   lon_deg;
    uint32_t last_seen_ms;
    uint16_t hits;
};

class AisTargetStore {
public:
    static constexpr size_t   CAPACITY      = 16;
    static constexpr uint32_t STALE_AFTER_MS = 5UL * 60UL * 1000UL;  // 5 min

    // Drop targets we haven't heard from in STALE_AFTER_MS. Call once per UI
    // tick — cheap; the table is 16 slots wide.
    void evictStale() {
        const uint32_t now = millis();
        for (size_t i = 0; i < CAPACITY; ++i) {
            if (targets_[i].mmsi == 0) continue;
            if (now - targets_[i].last_seen_ms > STALE_AFTER_MS) {
                targets_[i] = AisTarget{};
            }
        }
    }

    void recordPosition(uint32_t mmsi, char klass, float sog_kn, float cog_deg) {
        AisTarget& t = slotFor(mmsi);
        if (klass != '?') t.klass = klass;
        if (sog_kn  >= 0) t.sog_kn  = sog_kn;
        if (cog_deg >= 0) t.cog_deg = cog_deg;
        bump(t);
    }

    // Round 85 v1.6 step 3: position update. NaN values are ignored
    // (decoder may receive a partial report with only sog/cog).
    void recordLatLon(uint32_t mmsi, double lat_deg, double lon_deg) {
        AisTarget& t = slotFor(mmsi);
        if (!std::isnan(lat_deg)) t.lat_deg = lat_deg;
        if (!std::isnan(lon_deg)) t.lon_deg = lon_deg;
        bump(t);
    }

    void recordName(uint32_t mmsi, char klass, const char* name) {
        AisTarget& t = slotFor(mmsi);
        if (klass != '?') t.klass = klass;
        if (name && *name) {
            strncpy(t.name, name, sizeof(t.name) - 1);
            t.name[sizeof(t.name) - 1] = 0;
        }
        bump(t);
    }

    void recordType(uint32_t mmsi, char klass, uint8_t vessel_type) {
        AisTarget& t = slotFor(mmsi);
        if (klass != '?') t.klass = klass;
        if (vessel_type != 0) t.vessel_type = vessel_type;
        bump(t);
    }

    void recordNavStatus(uint32_t mmsi, uint8_t nav_status) {
        AisTarget& t = slotFor(mmsi);
        t.klass = 'A';  // only Class A reports nav status
        t.nav_status = nav_status;
        bump(t);
    }

    size_t size() const {
        size_t n = 0;
        for (size_t i = 0; i < CAPACITY; ++i) if (targets_[i].mmsi) ++n;
        return n;
    }

    size_t snapshotByRecency(AisTarget* out, size_t out_cap) const {
        size_t indices[CAPACITY];
        size_t n = 0;
        for (size_t i = 0; i < CAPACITY; ++i) {
            if (targets_[i].mmsi) indices[n++] = i;
        }
        for (size_t i = 1; i < n; ++i) {
            size_t j = i;
            while (j > 0 && targets_[indices[j-1]].last_seen_ms < targets_[indices[j]].last_seen_ms) {
                size_t tmp = indices[j-1]; indices[j-1] = indices[j]; indices[j] = tmp;
                --j;
            }
        }
        const size_t m = (n < out_cap) ? n : out_cap;
        for (size_t i = 0; i < m; ++i) out[i] = targets_[indices[i]];
        return m;
    }

private:
    AisTarget targets_[CAPACITY] = {};

    AisTarget& slotFor(uint32_t mmsi) {
        for (size_t i = 0; i < CAPACITY; ++i) {
            if (targets_[i].mmsi == mmsi) return targets_[i];
        }
        size_t pick = 0;
        for (size_t i = 0; i < CAPACITY; ++i) {
            if (targets_[i].mmsi == 0) { pick = i; break; }
            if (targets_[i].last_seen_ms < targets_[pick].last_seen_ms) pick = i;
        }
        targets_[pick] = AisTarget{};
        targets_[pick].mmsi        = mmsi;
        targets_[pick].klass       = '?';
        targets_[pick].sog_kn      = -1.0f;
        targets_[pick].cog_deg     = -1.0f;
        targets_[pick].vessel_type = 0;
        targets_[pick].nav_status  = 15;  // AIS "not defined / not available"
        return targets_[pick];
    }

    static void bump(AisTarget& t) {
        t.last_seen_ms = millis();
        ++t.hits;
    }
};
