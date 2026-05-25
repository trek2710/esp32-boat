// include/RoleNegotiator.h
//
// Header-only state machine for the WiFi AP/STA role election among
// virtual-bus peers. See docs/adr/0009-wifi-role-election.md for the
// design.
//
// Usage from each env's WiFi code:
//   1. Construct one RoleNegotiator instance.
//   2. Call init(millis()) once at boot, before any WiFi action.
//   3. Each loop iteration, call tick(millis()). It returns an
//      ActionSet describing what the caller must do this tick
//      (start AP, drop STA, publish a heartbeat, etc.).
//   4. When the caller's UDP drain task receives a packet with
//      PGN == kControlPgn, decode it via parseHeartbeatFields(...)
//      and call onHeartbeat(...).
//
// The negotiator owns NO sockets or WiFi state — it's pure logic over
// timestamps and a small peer table. The caller (TX/RX/converter)
// owns the WiFi mode transitions and the UDP socket.

#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <VbusRole.h>
#include <VirtualBusJson.h>

namespace vbus {

class RoleNegotiator {
public:
    enum class Role : uint8_t {
        ELECTING = 0,
        AP       = 1,
        STA      = 2,
    };

    enum class Event : uint8_t {
        HEARTBEAT          = 0,
        TAKEOVER_ANNOUNCE  = 1,
        GOING_DOWN         = 2,
    };

    // Returned from tick(). Multiple flags can fire in the same tick
    // (e.g. drop_sta + start_ap when a STA takes over as AP).
    struct ActionSet {
        bool start_ap            = false;
        bool start_sta           = false;
        bool drop_ap             = false;
        bool drop_sta            = false;
        bool publish_heartbeat   = false;
        bool publish_takeover    = false;
        bool publish_going_down  = false;
        // When start_sta fires, the caller's WiFi.begin() needs to know
        // which channel the AP is on. -1 = unknown (let the STA scan).
        int8_t target_channel    = -1;
    };

    struct PeerEntry {
        char     name[24];
        uint8_t  kind;
        uint8_t  priority;
        uint8_t  role;            // matches Role enum values
        uint32_t last_seen_ms;    // 0 = slot free
    };

    static constexpr size_t kMaxPeers          = 6;
    static constexpr uint32_t kHeartbeatPeriodMs = 5000;
    static constexpr uint32_t kAckHeartbeatMissThresh = 5;  // 5×5s = 25s silent → re-elect
    static constexpr uint32_t kPeerPruneAfterMs = 30000;
    static constexpr uint32_t kHandoffSettleMs  = 500;
    static constexpr uint32_t kTakeoverGapMs    = 500;      // between the 3 announces
    static constexpr uint32_t kTakeoverWaitMs   = 1500;     // after 3rd announce

    void init(uint32_t now) {
        role_                 = Role::ELECTING;
        electing_started_ms_  = now;
        backoff_ms_           = backoffMsFor(kBoardPriority);
        scan_done_            = false;
        last_hb_sent_ms_      = 0;
        going_down_sent_ms_   = 0;
        takeover_count_       = 0;
        takeover_last_ms_     = 0;
        scan_channel_         = -1;
        for (auto& p : peers_) { p.last_seen_ms = 0; p.name[0] = 0; }
    }

    // Round 84 (task #36): re-enter ELECTING without wiping the peer
    // table. Used when an AP detects a competing AP on the same SSID
    // (dual-AP split brain) and wants to step down + re-elect. The
    // peer table is preserved because any heartbeats we've heard
    // recently are still useful for the immediate re-election (we
    // already know who else is around).
    void forceReelect(uint32_t now) {
        role_                 = Role::ELECTING;
        electing_started_ms_  = now;
        backoff_ms_           = backoffMsFor(kBoardPriority);
        scan_done_            = false;
        last_hb_sent_ms_      = 0;
        going_down_sent_ms_   = 0;
        takeover_count_       = 0;
        takeover_last_ms_     = 0;
        scan_channel_         = -1;
    }

    Role role() const { return role_; }

    // Returned heartbeat-publish actions need a payload; the caller can
    // build it with this helper. Renders into `buf` and returns bytes
    // written (excl. nul), or -1 on overflow.
    int buildHeartbeatJson(char* buf, size_t cap, Event ev,
                           const char* ap_ip_or_empty,
                           uint32_t uptime_ms) const {
        int n = vbus::writeHeader(buf, cap, kControlPgn, kBoardPeerName);
        if (n < 0 || (size_t)n >= cap) return -1;
        int m = snprintf(buf + n, cap - n,
                         "\"event\":\"%s\",\"kind\":\"%s\","
                         "\"priority\":%u,\"role\":\"%s\","
                         "\"ap_ip\":\"%s\",\"uptime_ms\":%lu",
                         eventName(ev), kBoardKindName,
                         (unsigned)kBoardPriority,
                         roleName(role_),
                         (ap_ip_or_empty && *ap_ip_or_empty)
                             ? ap_ip_or_empty : "",
                         (unsigned long)uptime_ms);
        if (m < 0 || (size_t)(n + m) >= cap) return -1;
        int e = vbus::writeFooter(buf + n + m, cap - n - m);
        if (e < 0) return -1;
        return n + m + e;
    }

    // Called by the caller's UDP drain when a PGN==kControlPgn arrives.
    // peer_name comes from the top-level "peer" field; fields is the
    // already-extracted body of the "fields" object.
    void onHeartbeat(const char* peer_name, const char* fields,
                     uint32_t now) {
        if (!peer_name || !*peer_name || !fields) return;
        // Don't track ourselves.
        if (strcmp(peer_name, kBoardPeerName) == 0) return;

        int64_t prio = 0;
        if (!vbus::findInt(fields, "priority", &prio)) return;

        char role_s[12] = {0};
        if (!vbus::findString(fields, "role", role_s, sizeof(role_s))) return;
        uint8_t r = roleFromName(role_s);

        char kind_s[16] = {0};
        vbus::findString(fields, "kind", kind_s, sizeof(kind_s));
        uint8_t k = kindFromName(kind_s);

        char ev_s[24] = {0};
        vbus::findString(fields, "event", ev_s, sizeof(ev_s));

        int8_t idx = findOrAllocPeer(peer_name);
        if (idx < 0) return;  // table full
        auto& p = peers_[idx];
        strncpy(p.name, peer_name, sizeof(p.name) - 1);
        p.name[sizeof(p.name) - 1] = 0;
        p.kind         = k;
        p.priority     = (uint8_t)prio;
        p.role         = r;
        p.last_seen_ms = now;

        // Event-specific reactions.
        if (strcmp(ev_s, "going_down") == 0) {
            // Current AP is stepping down. If we were STA, we'll notice
            // the AP heartbeats stop and re-elect. If we were the one
            // doing takeover, we accelerate our AP bringup.
            if (role_ == Role::STA && takeover_count_ >= 3) {
                takeover_last_ms_ = 0;  // unblock the wait
            }
        }
    }

    // Drive the state machine forward. Call once per loop iteration.
    ActionSet tick(uint32_t now) {
        ActionSet a;
        pruneOldPeers(now);

        // Heartbeat publish (skipped during initial backoff).
        if (role_ != Role::ELECTING ||
            (now - electing_started_ms_ >= backoff_ms_)) {
            if (last_hb_sent_ms_ == 0 ||
                now - last_hb_sent_ms_ >= kHeartbeatPeriodMs) {
                last_hb_sent_ms_ = now;
                a.publish_heartbeat = true;
            }
        }

        switch (role_) {
        case Role::ELECTING: {
            // Wait for BOTH the backoff to elapse AND the caller's scan
            // to complete (recordScanResult sets scan_done_). The caller
            // is responsible for triggering the scan after its own copy
            // of the backoff elapses; until it does, we stay electing.
            if (now - electing_started_ms_ < backoff_ms_) break;
            if (!scan_done_) break;
            int8_t apIdx = currentApPeerIdx();
            if (apIdx >= 0) {
                a.start_sta = true;
                a.target_channel = scan_channel_;
                role_ = Role::STA;
            } else {
                a.start_ap = true;
                role_ = Role::AP;
            }
            break;
        }

        case Role::AP: {
            // Higher-priority peer arrived? Step down — go directly to
            // STA instead of through ELECTING+backoff+scan. The
            // higher-priority peer is already known to be AP and we
            // know its SSID, so joining is fast (no oscillation).
            int8_t higher = highestPriorityPeerIdx();
            if (higher >= 0 && peers_[higher].priority > kBoardPriority) {
                if (going_down_sent_ms_ == 0) {
                    a.publish_going_down = true;
                    going_down_sent_ms_  = now;
                } else if (now - going_down_sent_ms_ >= kHandoffSettleMs) {
                    a.drop_ap        = true;
                    a.start_sta      = true;
                    a.target_channel = -1;       // STA scans for SSID
                    going_down_sent_ms_ = 0;
                    role_            = Role::STA;
                }
            }
            break;
        }

        case Role::STA: {
            int8_t apIdx = currentApPeerIdx();
            // The "(scan)" placeholder (last_seen_ms==1, priority=50)
            // exists only to drive the cold-boot AP/STA decision in
            // ELECTING. Don't let it trigger spurious takeover or
            // re-election in the STA state — wait for a real heartbeat.
            const bool ap_is_real = (apIdx >= 0) &&
                                    (peers_[apIdx].last_seen_ms > 1);

            // STA notices we should be AP (our priority > current AP's).
            // Send 3 takeover_announce over 1.5s, then drop+START_AP.
            if (ap_is_real && peers_[apIdx].priority < kBoardPriority) {
                if (takeover_count_ == 0) {
                    a.publish_takeover = true;
                    takeover_last_ms_  = now;
                    takeover_count_    = 1;
                } else if (takeover_count_ < 3 &&
                           now - takeover_last_ms_ >= kTakeoverGapMs) {
                    a.publish_takeover = true;
                    takeover_last_ms_  = now;
                    takeover_count_++;
                } else if (takeover_count_ >= 3 &&
                           (takeover_last_ms_ == 0 ||
                            now - takeover_last_ms_ >= kTakeoverWaitMs)) {
                    a.drop_sta      = true;
                    a.start_ap      = true;
                    takeover_count_ = 0;
                    role_           = Role::AP;
                }
            } else {
                takeover_count_ = 0;  // no longer applicable
            }

            // AP heartbeats stopped → re-elect. Only counts if we've
            // ever seen a real heartbeat — the placeholder doesn't
            // qualify (it never had a real timestamp to age out from).
            if (ap_is_real &&
                now - peers_[apIdx].last_seen_ms >
                    kAckHeartbeatMissThresh * kHeartbeatPeriodMs) {
                a.drop_sta           = true;
                role_                = Role::ELECTING;
                electing_started_ms_ = now;
                scan_done_           = false;
            }
            break;
        }
        }
        return a;
    }

    // Caller informs the negotiator about scan results before letting
    // tick() pick start_sta vs start_ap. Pass channel >0 if SSID found,
    // -1 if not. Setting scan_done_ here gates the negotiator's
    // transition: tick() won't leave ELECTING until the caller has
    // actually scanned. This avoids a race where the negotiator's
    // internal backoff elapsed before the caller's scan ran, leaving
    // the peer table empty and the negotiator wrongly picking AP.
    void recordScanResult(int8_t channel_or_minus1) {
        scan_channel_ = channel_or_minus1;
        scan_done_    = true;
        if (channel_or_minus1 > 0) {
            // Synthesise a "saw an AP" peer entry so currentApPeerIdx()
            // returns non-negative. The real heartbeat will replace it
            // soon. The placeholder lives in the last slot.
            int8_t idx = -1;
            for (int i = (int)kMaxPeers - 1; i >= 0; i--) {
                if (peers_[i].last_seen_ms == 0) { idx = i; break; }
            }
            if (idx >= 0) {
                strncpy(peers_[idx].name, "(scan)", sizeof(peers_[idx].name) - 1);
                peers_[idx].kind         = 255;
                peers_[idx].priority     = 50;   // low — anyone real overrides
                peers_[idx].role         = (uint8_t)Role::AP;
                peers_[idx].last_seen_ms = 1;    // non-zero but very old; pruner will remove
            }
        }
    }

    int peerCount() const {
        int c = 0;
        for (const auto& p : peers_) if (p.last_seen_ms != 0) c++;
        return c;
    }

    const PeerEntry* peerAt(int idx) const {
        int c = 0;
        for (const auto& p : peers_) {
            if (p.last_seen_ms != 0) {
                if (c == idx) return &p;
                c++;
            }
        }
        return nullptr;
    }

    const char* currentApPeerName() const {
        // When I'm the AP, the "current AP" is me — return my own
        // peer name. Otherwise look for the highest-priority peer in
        // the table claiming AP.
        if (role_ == Role::AP) return kBoardPeerName;
        for (const auto& p : peers_) {
            if (p.last_seen_ms != 0 && p.role == (uint8_t)Role::AP) {
                return p.name;
            }
        }
        return "";
    }

    static const char* roleName(Role r) {
        switch (r) {
        case Role::ELECTING: return "electing";
        case Role::AP:       return "AP";
        case Role::STA:      return "STA";
        }
        return "?";
    }

private:
    Role     role_                = Role::ELECTING;
    uint32_t electing_started_ms_ = 0;
    uint32_t backoff_ms_          = 0;
    bool     scan_done_           = false;
    int8_t   scan_channel_        = -1;
    uint32_t last_hb_sent_ms_     = 0;
    uint32_t going_down_sent_ms_  = 0;
    uint8_t  takeover_count_      = 0;
    uint32_t takeover_last_ms_    = 0;
    PeerEntry peers_[kMaxPeers]   = {};

    static uint32_t backoffMsFor(uint8_t prio) {
        // ADR-0009: RX (200) → 0s, converter (150) → 4s, TX (100) → 8s.
        if (prio >= 200) return 0;
        if (prio >= 150) return 4000;
        if (prio >= 100) return 8000;
        return 12000;
    }

    static const char* eventName(Event e) {
        switch (e) {
        case Event::HEARTBEAT:         return "heartbeat";
        case Event::TAKEOVER_ANNOUNCE: return "takeover_announce";
        case Event::GOING_DOWN:        return "going_down";
        }
        return "?";
    }

    static uint8_t roleFromName(const char* s) {
        if (!s) return (uint8_t)Role::ELECTING;
        if (strcmp(s, "AP")  == 0) return (uint8_t)Role::AP;
        if (strcmp(s, "STA") == 0) return (uint8_t)Role::STA;
        return (uint8_t)Role::ELECTING;
    }

    static uint8_t kindFromName(const char* s) {
        if (!s) return 255;
        if (strcmp(s, "LCD-GPS")   == 0) return BK_TX;
        if (strcmp(s, "LCD-2.1")   == 0) return BK_RX;
        if (strcmp(s, "converter") == 0) return BK_CONVERTER;
        if (strcmp(s, "iOS")       == 0) return BK_IOS;
        return 255;
    }

    int8_t findOrAllocPeer(const char* name) {
        for (int i = 0; i < (int)kMaxPeers; i++) {
            if (peers_[i].last_seen_ms != 0 &&
                strcmp(peers_[i].name, name) == 0) {
                return i;
            }
        }
        for (int i = 0; i < (int)kMaxPeers; i++) {
            if (peers_[i].last_seen_ms == 0) return i;
        }
        return -1;
    }

    int8_t currentApPeerIdx() const {
        // If multiple peers claim AP (transient during handoff), pick
        // the highest-priority one.
        int8_t best = -1;
        uint8_t best_prio = 0;
        for (int i = 0; i < (int)kMaxPeers; i++) {
            if (peers_[i].last_seen_ms == 0) continue;
            if (peers_[i].role != (uint8_t)Role::AP) continue;
            if (peers_[i].priority >= best_prio) {
                best = i;
                best_prio = peers_[i].priority;
            }
        }
        return best;
    }

    int8_t highestPriorityPeerIdx() const {
        int8_t best = -1;
        uint8_t best_prio = 0;
        for (int i = 0; i < (int)kMaxPeers; i++) {
            if (peers_[i].last_seen_ms == 0) continue;
            if (peers_[i].priority > best_prio) {
                best = i;
                best_prio = peers_[i].priority;
            }
        }
        return best;
    }

    void pruneOldPeers(uint32_t now) {
        for (auto& p : peers_) {
            if (p.last_seen_ms == 0) continue;
            // last_seen_ms=1 is the placeholder from recordScanResult;
            // prune it as soon as a real heartbeat arrives elsewhere.
            if (p.last_seen_ms == 1) {
                bool any_real = false;
                for (auto& q : peers_) {
                    if (&q != &p && q.last_seen_ms > 1) { any_real = true; break; }
                }
                if (any_real) p.last_seen_ms = 0;
                continue;
            }
            if (now - p.last_seen_ms > kPeerPruneAfterMs) p.last_seen_ms = 0;
        }
    }
};

}  // namespace vbus
