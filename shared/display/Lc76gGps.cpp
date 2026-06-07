#include "Lc76gGps.h"
#include "Tca9554.h"

#include <Arduino.h>
#include <cstdlib>
#include <cstring>

// NMEA GGA parser salvaged verbatim from src_tx/main.cpp (parseNmeaCoord /
// nmeaChecksumOk / nextField / parseNmeaLine / gpsFeedByte). The v1 I2C
// transport is dropped — see the header for why. SOG/COG aren't parsed
// (GGA carries position only); a consumer derives them from position
// deltas, same as the v1 firmware did for own-ship.

namespace gps {
namespace {

constexpr int      kUartRx     = 17;
constexpr int      kUartTx     = 18;
constexpr uint32_t kUartBaud   = 9600;
constexpr uint8_t  kResetExio  = 7;       // LC76G RESET on TCA9554 EXIO7
constexpr uint32_t kFixStaleMs = 5000;

Fix  g_fix;
char lineBuf[120];
int  lineLen = 0;

double parseCoord(const char *s, char hemi) {
    if (!s || !*s) return NAN;
    double raw = atof(s);
    double deg = floor(raw / 100.0);
    double min = raw - deg * 100.0;
    double dec = deg + min / 60.0;
    if (hemi == 'S' || hemi == 'W') dec = -dec;
    return dec;
}

bool checksumOk(const char *line, int len) {
    int star = -1;
    for (int i = 1; i < len; i++) if (line[i] == '*') { star = i; break; }
    if (star < 0 || star + 3 > len) return false;
    uint8_t sum = 0;
    for (int i = 1; i < star; i++) sum ^= (uint8_t)line[i];
    auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int hi = hex(line[star + 1]);
    int lo = hex(line[star + 2]);
    if (hi < 0 || lo < 0) return false;
    return (uint8_t)((hi << 4) | lo) == sum;
}

const char *nextField(char **cursor) {
    if (!*cursor) return nullptr;
    char *start = *cursor;
    char *p = start;
    while (*p && *p != ',' && *p != '*') p++;
    char term = *p;
    *p = '\0';
    *cursor = (term == ',') ? (p + 1) : nullptr;
    return start;
}

// Parse one full NMEA line ('\r'/'\n' stripped). GGA only. Mutates `line`.
void parseLine(char *line, int len, uint32_t now) {
    if (len < 7 || line[0] != '$') return;
    if (!checksumOk(line, len)) return;
    if (!(line[3] == 'G' && line[4] == 'G' && line[5] == 'A')) return;  // $??GGA

    char *cursor = line + 6;
    if (*cursor != ',') return;
    cursor++;

    const char *time_s = nextField(&cursor);  // hhmmss.sss
    const char *lat_s  = nextField(&cursor);  // ddmm.mmmm
    const char *latH_s = nextField(&cursor);  // N/S
    const char *lon_s  = nextField(&cursor);  // dddmm.mmmm
    const char *lonH_s = nextField(&cursor);  // E/W
    const char *fix_s  = nextField(&cursor);  // 0..6
    const char *sats_s = nextField(&cursor);  // count
    (void)time_s;
    if (!fix_s || !sats_s) return;

    g_fix.fixQuality = (uint8_t)atoi(fix_s);
    g_fix.numSats    = (uint8_t)atoi(sats_s);
    if (g_fix.fixQuality >= 1 && lat_s && latH_s && lon_s && lonH_s) {
        g_fix.lat = parseCoord(lat_s, latH_s[0]);
        g_fix.lon = parseCoord(lon_s, lonH_s[0]);
        g_fix.lastFixMs = now;
    }
}

void feedByte(uint8_t b, uint32_t now) {
    if (b == '\n') {
        if (lineLen > 0) {
            if (lineBuf[lineLen - 1] == '\r') lineLen--;
            lineBuf[lineLen] = '\0';
            g_fix.linesParsed++;
            parseLine(lineBuf, lineLen, now);
        }
        lineLen = 0;
        return;
    }
    if (lineLen >= (int)sizeof(lineBuf) - 1) { lineLen = 0; return; }  // overrun
    lineBuf[lineLen++] = (char)b;
}

}  // namespace

bool begin() {
    Serial1.begin(kUartBaud, SERIAL_8N1, kUartRx, kUartTx);
    // Pulse RESET (active-low) via the expander. ~500 ms boot per datasheet.
    if (!tca9554::pinWrite(kResetExio, false)) {
        Serial.println("[gps] TCA9554 RESET-low failed");
        return false;
    }
    delay(15);
    if (!tca9554::pinWrite(kResetExio, true)) {
        Serial.println("[gps] TCA9554 RESET-high failed");
        return false;
    }
    delay(600);
    Serial.printf("[gps] LC76G UART up @ %lu baud (RX=GPIO%d) — needs R15/R16 "
                  "jumpers to stream\n", (unsigned long)kUartBaud, kUartRx);
    return true;
}

void poll(uint32_t now) {
    while (Serial1.available() > 0) {
        g_fix.uartBytes++;
        feedByte((uint8_t)Serial1.read(), now);
    }
}

const Fix& fix() { return g_fix; }

bool hasFreshFix(uint32_t now) {
    return g_fix.fixQuality >= 1 && (now - g_fix.lastFixMs) < kFixStaleMs;
}

}  // namespace gps
