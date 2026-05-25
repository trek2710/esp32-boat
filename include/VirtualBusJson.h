// include/VirtualBusJson.h
//
// Small helpers for the canboat-style JSON used on the WiFi virtual N2K
// bus. See docs/VIRTUAL_BUS_WIRE.md for the wire spec.
//
// Header-only — kept small and inline so the same code compiles cleanly
// into both the TX (src_tx/) and RX (src/) builds without any
// shared-library glue. ArduinoJson is intentionally NOT a dependency:
// the schema is fixed and tiny, hand-rolled snprintf/strstr is enough.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace vbus {

// ---- transport constants --------------------------------------------------

// Round 84 (ADR-0010): unicast star with AP-as-relay. STAs sendto AP IP;
// AP fans out to STAs.
constexpr const char* kApIp            = "192.168.4.1";
constexpr uint16_t    kBusPort         = 60001;     // shared UDP port
constexpr uint8_t     kSrcOffBus       = 255;       // N2K "unclaimed/null"

// ---- formatting (TX side) -------------------------------------------------
//
// Each format* function writes a full UTF-8 JSON document of the shape
//   {"pgn":N,"src":255,"peer":"...","fields":{...}}
// into `buf` (size `cap`). Returns the number of bytes written (excluding
// the trailing nul) or a negative value if the buffer is too small.

inline int writeHeader(char* buf, size_t cap, uint32_t pgn,
                       const char* peer) {
    // N2K PGNs are 18-bit (0..262143), not 16-bit — using uint16_t here
    // truncates large PGNs like 130306 → 64770, which caused the RX
    // dispatch to silently skip every packet during bring-up.
    return snprintf(buf, cap,
                    "{\"pgn\":%u,\"src\":%u,\"peer\":\"%s\",\"fields\":{",
                    (unsigned)pgn, (unsigned)kSrcOffBus, peer);
}

inline int writeFooter(char* buf, size_t cap) {
    if (cap < 3) return -1;
    buf[0] = '}'; buf[1] = '}'; buf[2] = '\0';
    return 2;
}

// ---- parsing (RX side) ----------------------------------------------------
//
// Lightweight extractors for the fixed wire shape. The whole JSON document
// is small (< 200 chars typical), so we just walk the string looking for
// "key":<value> patterns. Assumes no nested objects inside `fields` and no
// escaped characters in string values — both true for our schema.

// Locate "key": in `json` and return a pointer to the first character of
// the value (the char after the colon, with leading whitespace skipped).
// Returns nullptr if the key is not found.
inline const char* findValue(const char* json, const char* key) {
    if (!json || !key) return nullptr;
    // Build "key": pattern on the stack; keys are short.
    char needle[40];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return nullptr;
    const char* p = strstr(json, needle);
    if (!p) return nullptr;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return nullptr;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

inline bool findInt(const char* json, const char* key, int64_t* out) {
    const char* p = findValue(json, key);
    if (!p) return false;
    if (strncmp(p, "null", 4) == 0) return false;
    char* end = nullptr;
    long long v = strtoll(p, &end, 10);
    if (end == p) return false;
    *out = (int64_t)v;
    return true;
}

inline bool findDouble(const char* json, const char* key, double* out) {
    const char* p = findValue(json, key);
    if (!p) return false;
    if (strncmp(p, "null", 4) == 0) return false;
    char* end = nullptr;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

inline bool findString(const char* json, const char* key,
                       char* buf, size_t cap) {
    const char* p = findValue(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) buf[i++] = *p++;
    if (i + 1 >= cap || *p != '"') { if (cap > 0) buf[0] = 0; return false; }
    buf[i] = 0;
    return true;
}

// Get the body of any "<key>":{ ... } sub-object as a (start, length) span.
// We don't fully nest-parse — we just find the opening brace and walk
// forward counting braces until balanced.
inline bool findObjectBody(const char* json, const char* key,
                           const char** out_start, size_t* out_len) {
    const char* p = findValue(json, key);
    if (!p || *p != '{') return false;
    p++;
    const char* start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        if (depth == 0) break;
        p++;
    }
    if (depth != 0) return false;
    *out_start = start;
    *out_len = (size_t)(p - start);
    return true;
}

// Backwards-compat shim — `findFieldsBody(json, ...)` was the only call
// site before round 85; new code uses findObjectBody directly.
inline bool findFieldsBody(const char* json,
                           const char** out_start, size_t* out_len) {
    return findObjectBody(json, "fields", out_start, out_len);
}

// ---- conversion helpers (canboat units ↔ display units) -------------------

inline double knotsToMs(double kt)        { return kt * 0.514444; }
inline double msToKnots(double ms)        { return ms * 1.943844; }
inline double degToRad(double deg)        { return deg * 0.0174532925199433; }
inline double radToDeg(double rad)        { return rad * 57.29577951308232;  }
inline double celsiusToKelvin(double c)   { return c + 273.15; }
inline double kelvinToCelsius(double k)   { return k - 273.15; }

}  // namespace vbus
