#pragma once

#include <cstdint>
#include <cmath>

// LC76G GNSS on the ESP32-S3-Touch-AMOLED-1.75-G, UART path only.
//
// Salvaged from src_tx/main.cpp, dropping the dead I2C path entirely: on
// this board variant the LC76G is NOT wired to the ESP32 I2C bus (the
// "0x50 ACK" the v1 debugging chased was the AT24Cxx EEPROM). The chip's
// UART (TXD1/RXD1) reaches GPIO17/18 only through the R15/R16 0Ω jumpers,
// which are UNPOPULATED from the factory. So poll() reads nothing until
// those jumpers are bridged — at which point GGA fixes light up with no
// code change.

namespace gps {

struct Fix {
    double   lat        = NAN;   // decimal degrees, NaN until first fix
    double   lon        = NAN;
    uint8_t  fixQuality = 0;     // NMEA GGA fix quality (0 = no fix)
    uint8_t  numSats    = 0;
    uint32_t lastFixMs  = 0;
    uint32_t uartBytes  = 0;     // diagnostic: bytes seen on the UART
    uint32_t linesParsed = 0;
};

// Bring up Serial1 @ 9600 on GPIO17/18 and pulse the LC76G reset via the
// TCA9554 (EXIO7). Requires Wire already up. Returns false only on a reset
// I2C error; UART-silent (unsoldered jumpers) is a normal success.
bool begin();

// Drain whatever the UART has buffered into the NMEA parser. Call often.
void poll(uint32_t now);

const Fix& fix();
bool hasFreshFix(uint32_t now);   // fixQuality >= 1 && age < 5 s

}  // namespace gps
