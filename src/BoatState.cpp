#include "BoatState.h"

#include <cstring>

namespace {
struct Lock {
    SemaphoreHandle_t m;
    explicit Lock(SemaphoreHandle_t mtx) : m(mtx) { xSemaphoreTake(m, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m); }
};
}  // namespace

BoatState::BoatState() : mutex_(xSemaphoreCreateMutex()) {}

Instruments BoatState::snapshot() {
    Lock l(mutex_);
    return i_;
}

void BoatState::setGps(double lat, double lon, double sog, double cog) {
    Lock l(mutex_);
    i_.lat = lat;
    i_.lon = lon;
    i_.sog = sog;
    i_.cog = cog;
    i_.gps_last_ms = millis();
}

void BoatState::setWindApparent(double awa, double aws) {
    Lock l(mutex_);
    i_.awa = awa;
    i_.aws = aws;
    i_.wind_last_ms = millis();
}

void BoatState::setWindTrue(double twa, double tws) {
    Lock l(mutex_);
    i_.twa = twa;
    i_.tws = tws;
    i_.wind_last_ms = millis();
}

void BoatState::setDepth(double depth_m, double water_temp_c) {
    Lock l(mutex_);
    i_.depth_m = depth_m;
    i_.water_temp_c = water_temp_c;
    i_.depth_last_ms = millis();
}

void BoatState::setHeading(double heading_true_deg) {
    Lock l(mutex_);
    i_.heading_true_deg = heading_true_deg;
    i_.hdg_last_ms = millis();
}

void BoatState::setStw(double stw) {
    Lock l(mutex_);
    i_.stw = stw;
    i_.hdg_last_ms = millis();
}

void BoatState::upsertAisTarget(const AisTarget& t) {
    Lock l(mutex_);
    pruneStaleAis_locked();

    // Update existing slot if MMSI already known.
    for (auto& slot : ais_) {
        if (slot.mmsi == t.mmsi && slot.mmsi != 0) {
            // Preserve name if the incoming update doesn't include one.
            char saved_name[sizeof(slot.name)];
            std::memcpy(saved_name, slot.name, sizeof(saved_name));
            slot = t;
            if (t.name[0] == 0) {
                std::memcpy(slot.name, saved_name, sizeof(saved_name));
            }
            return;
        }
    }
    // Else insert into the first empty slot.
    for (auto& slot : ais_) {
        if (slot.mmsi == 0) {
            slot = t;
            return;
        }
    }
    // Table full — overwrite the oldest entry.
    uint32_t oldest_idx = 0;
    uint32_t oldest_ms  = ais_[0].last_seen_ms;
    for (size_t i = 1; i < ais_.size(); ++i) {
        if (ais_[i].last_seen_ms < oldest_ms) {
            oldest_ms = ais_[i].last_seen_ms;
            oldest_idx = static_cast<uint32_t>(i);
        }
    }
    ais_[oldest_idx] = t;
}

std::array<AisTarget, 32> BoatState::aisSnapshot() {
    Lock l(mutex_);
    pruneStaleAis_locked();
    return ais_;
}

void BoatState::pruneStaleAis_locked() {
    const uint32_t now = millis();
    for (auto& slot : ais_) {
        if (slot.mmsi != 0 && now - slot.last_seen_ms > kAisStaleMs) {
            slot = AisTarget{};
        }
    }
}
