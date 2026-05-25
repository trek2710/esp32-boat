#pragma once

#include <stdint.h>

class AisTargetStore;

namespace ui {

struct Stats {
    uint32_t n2k_sent;                   // count of PGNs transmitted
    uint32_t nmea0183_sentences_seen;    // count of NMEA0183 lines seen on UART
    uint32_t last_tx_ms;                 // millis() of last N2K send
    int      n2k_src_addr;
    uint32_t last_rx_ms;                 // millis() of last NMEA0183 sentence
};

void begin();                                  // init LCD + draw initial layout
void refresh(const AisTargetStore& store,
             const Stats& stats);              // call ~2 Hz from loop()

}  // namespace ui
