#pragma once

#include "ais_decoder.h"
#include "AisTargetStore.h"

// Store-only AIS decoder for the standalone devices (ADR-0016).
//
// Extends the pure AIVDM decoder (AIS::AisDecoder) directly and records
// decoded targets into an AisTargetStore. Unlike the v1 AisDecoderWithStore,
// this carries NO NMEA 2000 dependency — there is no bus to emit PGNs onto;
// a device just needs the target table to draw radar.
//
// Fix vs v1: the v1 decoder recorded SOG/COG/name/type but never the
// position (recordLatLon was only ever called by the simulator). Real
// decoded reports now record lat/lon too — the whole point of the device.
//
// AIS field encodings: position is in 1/600000 minutes (÷600000 → degrees);
// SOG in 0.1 kn (1023 = unknown); COG in 0.1° (3600 = unknown).

class AisTargetDecoder : public AIS::AisDecoder {
public:
    AisTargetStore& store() { return store_; }
    const AisTargetStore& store() const { return store_; }
    uint64_t messages() const { return decoded_; }

protected:
    static double posDeg(long raw)  { return raw / 600000.0; }
    static float  sogKn(unsigned int s) { return (s == 1023) ? -1.0f : s / 10.0f; }
    static float  cogDeg(int c)         { return (c  == 3600) ? -1.0f : c  / 10.0f; }

    // ---- Class A position (types 1/2/3) ----
    void onType123(unsigned int, unsigned int _uMmsi, unsigned int _uNavstatus,
                   int, unsigned int _uSog, bool, long _iPosLon, long _iPosLat,
                   int _iCog, int, int, bool, unsigned int, unsigned int) override {
        store_.recordLatLon(_uMmsi, posDeg(_iPosLat), posDeg(_iPosLon));
        store_.recordPosition(_uMmsi, 'A', sogKn(_uSog), cogDeg(_iCog));
        store_.recordNavStatus(_uMmsi, static_cast<uint8_t>(_uNavstatus));
        ++decoded_;
    }

    // ---- Class A static/voyage (type 5): name + type ----
    void onType5(unsigned int, unsigned int _uMmsi, unsigned int,
                 const std::string &, const std::string &_strName,
                 unsigned int _uType, unsigned int, unsigned int, unsigned int,
                 unsigned int, unsigned int, unsigned int, unsigned int,
                 unsigned int, unsigned int, unsigned int, const std::string &,
                 unsigned int, unsigned int, bool) override {
        store_.recordName(_uMmsi, 'A', _strName.c_str());
        store_.recordType(_uMmsi, 'A', static_cast<uint8_t>(_uType));
        ++decoded_;
    }

    // ---- Class B position (type 18) ----
    void onType18(unsigned int, unsigned int _uMmsi, unsigned int _uSog, bool,
                  long _iPosLon, long _iPosLat, int _iCog, int, bool, unsigned int,
                  bool, bool, bool, bool, bool, bool, unsigned int, bool) override {
        store_.recordLatLon(_uMmsi, posDeg(_iPosLat), posDeg(_iPosLon));
        store_.recordPosition(_uMmsi, 'B', sogKn(_uSog), cogDeg(_iCog));
        ++decoded_;
    }

    // ---- Class B extended position (type 19): position + name + type ----
    void onType19(unsigned int _uMmsi, unsigned int _uSog, bool, int _iPosLon,
                  int _iPosLat, int _iCog, int, const std::string &_strName,
                  unsigned int _uType, unsigned int, unsigned int, unsigned int,
                  unsigned int, unsigned int, unsigned int, bool, bool,
                  unsigned int, bool) override {
        store_.recordLatLon(_uMmsi, posDeg(_iPosLat), posDeg(_iPosLon));
        store_.recordPosition(_uMmsi, 'B', sogKn(_uSog), cogDeg(_iCog));
        store_.recordName(_uMmsi, 'B', _strName.c_str());
        store_.recordType(_uMmsi, 'B', static_cast<uint8_t>(_uType));
        ++decoded_;
    }

    // ---- Class B static A (type 24A): name ----
    void onType24A(unsigned int, unsigned int, unsigned int _uMmsi,
                   const std::string &_strName) override {
        store_.recordName(_uMmsi, 'B', _strName.c_str());
        ++decoded_;
    }

    // ---- Class B static B (type 24B): vessel type ----
    void onType24B(unsigned int, unsigned int, unsigned int _uMmsi,
                   const std::string &, unsigned int _uType, unsigned int,
                   unsigned int, unsigned int, unsigned int,
                   const std::string &) override {
        store_.recordType(_uMmsi, 'B', static_cast<uint8_t>(_uType));
        ++decoded_;
    }

    // ---- Long-range position (type 27): position only ----
    void onType27(unsigned int _uMmsi, unsigned int _uNavstatus, unsigned int _uSog,
                  bool, int _iPosLon, int _iPosLat, int _iCog) override {
        store_.recordLatLon(_uMmsi, posDeg(_iPosLat), posDeg(_iPosLon));
        store_.recordPosition(_uMmsi, '?', sogKn(_uSog), cogDeg(_iCog));
        store_.recordNavStatus(_uMmsi, static_cast<uint8_t>(_uNavstatus));
        ++decoded_;
    }

    // ---- Reports the device doesn't render: required overrides, no-ops ----
    void onType411(unsigned int, unsigned int, unsigned int, unsigned int,
                   unsigned int, unsigned int, unsigned int, unsigned int,
                   bool, int, int) override {}
    void onType9(unsigned int, unsigned int, bool, int, int, int,
                 unsigned int) override {}
    void onType14(unsigned int, unsigned int, const std::string &, int) override {}
    void onType21(unsigned int, unsigned int, const std::string &, bool, int, int,
                  unsigned int, unsigned int, unsigned int, unsigned int) override {}

    // ---- Lifecycle / error hooks: not needed here ----
    void onSentence(const AIS::StringRef &) override {}
    void onMessage(const AIS::StringRef &, const AIS::StringRef &,
                   const AIS::StringRef &) override {}
    void onNotDecoded(const AIS::StringRef &, int) override {}
    void onDecodeError(const AIS::StringRef &, const std::string &) override {}
    void onParseError(const AIS::StringRef &, const std::string &) override {}

private:
    AisTargetStore store_;
    uint64_t       decoded_ = 0;
};
