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

uint8_t* g_buf   = nullptr;
size_t   g_size  = 0;
uint32_t g_cell  = 0;          // currently-loaded cell id (0 = none)
bool     g_valid = false;
uint32_t g_feat  = 0;          // feature count
size_t   g_cur   = 0;          // iterator cursor (byte offset)
uint32_t g_idx   = 0;

// CM93 cell index for scale C — matches tools/chart-transcode cell_index().
uint32_t cellIndexC(double lat, double lon) {
    double lon1 = (lon + 360.0) * 3.0;
    while (lon1 >= 1080.0) lon1 -= 1080.0;
    int lon3 = (int)std::floor(lon1 / kScaleDval) * kScaleDval;
    double lat1 = lat * 3.0 + 270.0 - 30.0;
    int lat3 = (int)std::floor(lat1 / kScaleDval) * kScaleDval;
    return (uint32_t)((lat3 + 30) * 10000 + lon3);
}

template <typename T>
T rd(size_t off) { T v; std::memcpy(&v, g_buf + off, sizeof(T)); return v; }

}  // namespace

bool ensureCell(double lat, double lon) {
    const uint32_t id = cellIndexC(lat, lon);
    if (id == g_cell) return g_valid;     // already attempted this cell
    g_cell = id;
    g_valid = false;
    if (g_buf) { heap_caps_free(g_buf); g_buf = nullptr; }

    // Path is relative to the card root — SD_MMC adds the "/sdcard" mount.
    char path[32];
    snprintf(path, sizeof(path), "/tiles/%08u.c93t", (unsigned)id);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) { Serial.printf("[chart] no tile %s\n", path); return false; }

    g_size = f.size();
    g_buf = (uint8_t*)heap_caps_malloc(g_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_buf) { Serial.println("[chart] PSRAM alloc failed"); f.close(); return false; }
    f.read(g_buf, g_size);
    f.close();

    if (g_size < kHdr || rd<uint32_t>(0) != kMagic) {
        Serial.printf("[chart] bad tile %s\n", path);
        heap_caps_free(g_buf); g_buf = nullptr; return false;
    }
    g_feat = rd<uint32_t>(24);
    g_valid = true;
    Serial.printf("[chart] loaded %s: %u features, %u KB\n",
                  path, (unsigned)g_feat, (unsigned)(g_size / 1024));
    return true;
}

bool valid() { return g_valid; }
uint32_t cellId() { return g_cell; }
uint32_t featureCount() { return g_valid ? g_feat : 0; }

void rewind() { g_cur = kHdr; g_idx = 0; }

bool next(Feature& out) {
    if (!g_valid || g_idx >= g_feat || g_cur + 8 > g_size) return false;
    out.layer = g_buf[g_cur];
    out.flags = g_buf[g_cur + 1];
    out.depth = rd<float>(g_cur + 2);
    out.npts  = rd<uint16_t>(g_cur + 6);
    out.pts   = reinterpret_cast<const float*>(g_buf + g_cur + 8);  // 4-byte aligned
    g_cur += 8 + (size_t)out.npts * 8;
    g_idx++;
    return out.npts > 0;
}

}  // namespace chart
