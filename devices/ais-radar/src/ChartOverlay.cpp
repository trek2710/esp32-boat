#include "ChartOverlay.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <cmath>
#include <cstring>

namespace chart {
namespace {

constexpr uint32_t kMagic = 0x54333943;   // 'C93T'
constexpr int      kHdr   = 28;           // bytes before the first feature
constexpr int      kScaleDval = 12;       // CM93 scale C
constexpr int      kMaxTiles = 4;         // own cell + up-to-3 neighbours (a 2×2)

struct Tile { uint32_t id; uint8_t* buf; size_t size; uint32_t feat; };
Tile     g_tiles[kMaxTiles];
int      g_nTiles = 0;
uint32_t g_center = 0;          // own cell id, for the status / NO-CHART check

// iterator state
int    g_it  = 0;
size_t g_cur = 0;
uint32_t g_idx = 0;

template <typename T>
T rd(const uint8_t* p, size_t off) { T v; std::memcpy(&v, p + off, sizeof(T)); return v; }

uint32_t cellIndexC(double lat, double lon) {
    double lon1 = (lon + 360.0) * 3.0;
    while (lon1 >= 1080.0) lon1 -= 1080.0;
    int lon3 = (int)std::floor(lon1 / kScaleDval) * kScaleDval;
    double lat1 = lat * 3.0 + 240.0;
    int lat3 = (int)std::floor(lat1 / kScaleDval) * kScaleDval;
    return (uint32_t)((lat3 + 30) * 10000 + lon3);
}

// The own cell plus any neighbour within ~0.6° of an edge (radar range is far
// less than a 4° cell, so this keeps the chart continuous near boundaries).
int neededCells(double lat, double lon, uint32_t* out) {
    const int dv = kScaleDval;
    double lon1 = (lon + 360.0) * 3.0;
    while (lon1 >= 1080.0) lon1 -= 1080.0;
    int lon3 = (int)std::floor(lon1 / dv) * dv;
    double lat1 = lat * 3.0 + 240.0;
    int lat3 = (int)std::floor(lat1 / dv) * dv;
    double flat = (lat1 - lat3) / 3.0, flon = (lon1 - lon3) / 3.0;   // 0..4 in cell
    const double m = 0.6;
    int dla[2] = {0, 0}, ndl = 1, dlo[2] = {0, 0}, ndo = 1;
    if (flat < m)      { dla[1] = -dv; ndl = 2; }
    else if (flat > 4 - m) { dla[1] = dv; ndl = 2; }
    if (flon < m)      { dlo[1] = -dv; ndo = 2; }
    else if (flon > 4 - m) { dlo[1] = dv; ndo = 2; }
    int n = 0;
    for (int a = 0; a < ndl; ++a)
        for (int b = 0; b < ndo; ++b) {
            int la = lat3 + dla[a], lo = lon3 + dlo[b];
            while (lo < 0) lo += 1080;
            while (lo >= 1080) lo -= 1080;
            out[n++] = (uint32_t)((la + 30) * 10000 + lo);
        }
    return n;
}

bool loadTile(uint32_t id, Tile& t) {
    char path[32];
    snprintf(path, sizeof(path), "/tiles/%08u.c93t", (unsigned)id);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    size_t sz = f.size();
    uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { f.close(); return false; }
    f.read(buf, sz); f.close();
    if (sz < kHdr || rd<uint32_t>(buf, 0) != kMagic) { heap_caps_free(buf); return false; }
    t = { id, buf, sz, rd<uint32_t>(buf, 24) };
    return true;
}

}  // namespace

bool ensureCell(double lat, double lon) {
    g_center = cellIndexC(lat, lon);
    uint32_t need[kMaxTiles]; int nn = neededCells(lat, lon, need);

    // Already exactly the loaded set? (order-independent)
    bool same = (nn == g_nTiles);
    for (int j = 0; same && j < nn; ++j) {
        bool found = false;
        for (int i = 0; i < g_nTiles; ++i) if (g_tiles[i].id == need[j]) { found = true; break; }
        same = found;
    }
    if (same && g_nTiles > 0) return true;

    Tile keep[kMaxTiles]; int nk = 0;
    for (int i = 0; i < g_nTiles; ++i) {                 // keep still-needed, free rest
        bool used = false;
        for (int j = 0; j < nn; ++j) if (g_tiles[i].id == need[j]) { used = true; break; }
        if (used) keep[nk++] = g_tiles[i];
        else if (g_tiles[i].buf) heap_caps_free(g_tiles[i].buf);
    }
    for (int j = 0; j < nn; ++j) {                       // load newly-needed
        bool have = false;
        for (int i = 0; i < nk; ++i) if (keep[i].id == need[j]) { have = true; break; }
        if (!have) { Tile t; if (loadTile(need[j], t) && nk < kMaxTiles) keep[nk++] = t; }
    }
    for (int i = 0; i < nk; ++i) g_tiles[i] = keep[i];
    g_nTiles = nk;
    Serial.printf("[chart] center %08u, %d tiles loaded\n", (unsigned)g_center, g_nTiles);
    return g_nTiles > 0;
}

bool valid() { return g_nTiles > 0; }
uint32_t cellId() { return g_center; }
uint32_t featureCount() {
    uint32_t n = 0;
    for (int i = 0; i < g_nTiles; ++i) n += g_tiles[i].feat;
    return n;
}

void rewind() { g_it = 0; g_cur = kHdr; g_idx = 0; }

bool next(Feature& out) {
    while (g_it < g_nTiles) {
        const Tile& t = g_tiles[g_it];
        if (g_idx >= t.feat || g_cur + 8 > t.size) {     // end of this tile
            ++g_it; g_cur = kHdr; g_idx = 0; continue;
        }
        out.layer = t.buf[g_cur];
        out.flags = t.buf[g_cur + 1];
        out.depth = rd<float>(t.buf, g_cur + 2);
        out.npts  = rd<uint16_t>(t.buf, g_cur + 6);
        out.pts   = reinterpret_cast<const float*>(t.buf + g_cur + 8);
        g_cur += 8 + (size_t)out.npts * 8;
        g_idx++;
        return out.npts > 0;
    }
    return false;
}

}  // namespace chart
