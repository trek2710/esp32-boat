#include "BoatState.h"

#include <cmath>
#include <cstring>

namespace {
struct Lock {
    SemaphoreHandle_t m;
    explicit Lock(SemaphoreHandle_t mtx) : m(mtx) { xSemaphoreTake(m, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m); }
};

inline double normalizeDeg(double d) {
    while (d < 0.0)    d += 360.0;
    while (d >= 360.0) d -= 360.0;
    return d;
}
inline double normalizeSignedDeg(double d) {
    while (d <= -180.0) d += 360.0;
    while (d >   180.0) d -= 360.0;
    return d;
}
}  // namespace

BoatState::BoatState() : mutex_(xSemaphoreCreateMutex()) {}

Instruments BoatState::snapshot() {
    Lock l(mutex_);
    return i_;
}

// ---- raw sensor setters ----------------------------------------------------

void BoatState::setGps(double lat, double lon, double sog, double cog) {
    Lock l(mutex_);
    i_.lat = lat;
    i_.lon = lon;
    i_.sog = sog;
    i_.cog = cog;
    i_.gps_last_ms = millis();
    recomputeDerived_locked();
}

void BoatState::setApparentWind(double awa, double aws) {
    Lock l(mutex_);
    i_.awa = awa;
    i_.aws = aws;
    i_.wind_last_ms = millis();
    recomputeDerived_locked();
}

void BoatState::setMagneticHeading(double heading_mag_deg) {
    Lock l(mutex_);
    i_.heading_mag_deg = heading_mag_deg;
    i_.hdg_last_ms     = millis();
    recomputeDerived_locked();
}

void BoatState::setMagneticVariation(double variation_deg) {
    Lock l(mutex_);
    i_.magnetic_variation_deg = variation_deg;
    i_.var_last_ms             = millis();
    recomputeDerived_locked();
}

void BoatState::setStw(double stw) {
    Lock l(mutex_);
    i_.stw          = stw;
    i_.stw_last_ms  = millis();
    recomputeDerived_locked();
}

void BoatState::setDepth(double depth_m, double water_temp_c) {
    Lock l(mutex_);
    i_.depth_m      = depth_m;
    i_.water_temp_c = water_temp_c;
    i_.depth_last_ms = millis();
    // Depth doesn't feed any derived value, but keep the call here for
    // consistency in case future derived fields depend on it.
    recomputeDerived_locked();
}

// ---- derive ---------------------------------------------------------------

void BoatState::recomputeDerived_locked() {
    // (1) True heading = magnetic heading + variation. NaN-propagating: if
    //     either input is missing the result is NaN and the UI shows "--".
    if (!std::isnan(i_.heading_mag_deg) &&
        !std::isnan(i_.magnetic_variation_deg)) {
        i_.heading_true_deg =
            normalizeDeg(i_.heading_mag_deg + i_.magnetic_variation_deg);
    } else {
        i_.heading_true_deg = NAN;
    }

    // (2) True wind from apparent wind + boat motion. The classic vector
    //     calculation in the boat-relative frame (bow = +y, starboard = +x):
    //         apparent wind velocity vector  V_a = -AWS · (sin AWA, cos AWA)
    //                                              (negated because AWA is
    //                                               the angle the wind comes
    //                                               FROM, opposite of motion)
    //         boat velocity vector           V_b = (0, STW)
    //         true wind velocity vector      V_t = V_a + V_b
    //     and the "from" angle of the true wind is atan2(-V_t.x, -V_t.y).
    //     Falling back to SOG (course-over-ground speed) if STW is missing
    //     gives the right answer in slack water; in current it underestimates
    //     wind triangles by the current vector — fine for v1.
    const double boat_v = !std::isnan(i_.stw) ? i_.stw : i_.sog;
    if (!std::isnan(i_.awa) && !std::isnan(i_.aws) && !std::isnan(boat_v)) {
        const double awa_rad = i_.awa * M_PI / 180.0;
        const double ax = i_.aws * std::sin(awa_rad);   // starboard component (+ east in boat frame)
        const double ay = i_.aws * std::cos(awa_rad);   // forward component  (+ bow   in boat frame)
        // V_t = V_a + V_b = ((-AWS·sin) + 0, (-AWS·cos) + STW) → but we
        // want the FROM angle, so we work with (ax, ay) directly and recover
        // the "from" sign at the end. Algebraically equivalent to the
        // formula above:
        const double tx = ax;
        const double ty = ay - boat_v;
        i_.tws = std::sqrt(tx * tx + ty * ty);
        const double twa_rad = std::atan2(tx, ty);
        i_.twa = normalizeSignedDeg(twa_rad * 180.0 / M_PI);
    } else {
        i_.twa = NAN;
        i_.tws = NAN;
    }

    // (3) True wind direction = boat's true heading + TWA, normalised 0..360.
    if (!std::isnan(i_.heading_true_deg) && !std::isnan(i_.twa)) {
        i_.twd = normalizeDeg(i_.heading_true_deg + i_.twa);
    } else {
        i_.twd = NAN;
    }

    // (4) VMG (velocity made good upwind) — boat speed projected onto the
    //     true-wind axis. Negative VMG = sailing downwind.
    if (!std::isnan(i_.stw) && !std::isnan(i_.twa)) {
        i_.vmg = i_.stw * std::cos(i_.twa * M_PI / 180.0);
    } else {
        i_.vmg = NAN;
    }
}

// ---- AIS / PGN log (unchanged from round 52) -------------------------------

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

void BoatState::logPgn(uint32_t pgn, uint8_t src, const char* summary) {
    Lock l(mutex_);
    PgnEvent& slot = pgn_log_[pgn_log_head_];
    slot.pgn   = pgn;
    slot.src   = src;
    slot.t_ms  = millis();
    if (summary) {
        std::strncpy(slot.summary, summary, sizeof(slot.summary) - 1);
        slot.summary[sizeof(slot.summary) - 1] = '\0';
    } else {
        slot.summary[0] = '\0';
    }
    pgn_log_head_ = (pgn_log_head_ + 1) % kPgnLogSize;
    pgn_log_total_++;
}

std::array<PgnEvent, BoatState::kPgnLogSize> BoatState::pgnLogSnapshot() {
    Lock l(mutex_);
    // Return newest-first: start from head-1 and walk backwards.
    std::array<PgnEvent, kPgnLogSize> out{};
    for (size_t i = 0; i < kPgnLogSize; ++i) {
        size_t idx = (pgn_log_head_ + kPgnLogSize - 1 - i) % kPgnLogSize;
        out[i] = pgn_log_[idx];
    }
    return out;
}

uint32_t BoatState::pgnLogTotal() {
    Lock l(mutex_);
    return pgn_log_total_;
}

void BoatState::pruneStaleAis_locked() {
    const uint32_t now = millis();
    for (auto& slot : ais_) {
        if (slot.mmsi != 0 && now - slot.last_seen_ms > kAisStaleMs) {
            slot = AisTarget{};
        }
    }
}
