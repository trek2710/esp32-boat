#include "Tca9554.h"

#include <Arduino.h>
#include <Wire.h>

// Salvaged from src_tx/main.cpp (tcaReadReg/tcaWriteReg/tcaPinWrite).

namespace tca9554 {
namespace {

constexpr uint8_t kAddr      = 0x20;
constexpr uint8_t kRegOutput = 0x01;
constexpr uint8_t kRegConfig = 0x03;

bool readReg(uint8_t reg, uint8_t *val) {
    Wire.beginTransmission(kAddr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(kAddr, (uint8_t)1) != 1) return false;
    *val = Wire.read();
    return true;
}

bool writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(kAddr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

}  // namespace

bool pinWrite(uint8_t pin, bool high) {
    uint8_t cfg = 0, out = 0;
    if (!readReg(kRegConfig, &cfg)) return false;
    if (!readReg(kRegOutput, &out)) return false;
    cfg &= ~(1 << pin);                       // 0 in config reg = output
    if (high) out |= (1 << pin); else out &= ~(1 << pin);
    if (!writeReg(kRegConfig, cfg)) return false;
    if (!writeReg(kRegOutput, out)) return false;
    return true;
}

}  // namespace tca9554
