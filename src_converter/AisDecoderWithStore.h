#pragma once

#include "NMEA0183AIStoNMEA2000.h"
#include "AisTargetStore.h"

// Wraps MyAisDecoder (the vendored AK-Homberger AIS->N2K decoder) so each
// successful AIS callback also (a) populates an AisTargetStore the UI can
// render, and (b) bumps a counter of N2K messages actually sent. The
// parent's method does the SendMsg, so the +1 here matches the wire 1:1
// (whereas counting VDM lines overcounts on multi-fragment messages).

class AisDecoderWithStore : public MyAisDecoder {
public:
    AisTargetStore& store() { return store_; }
    uint32_t n2k_sent() const { return n2k_sent_; }

    // Class A position report (AIS types 1, 2, 3) -> PGN 129038.
    void onType123(unsigned int _uMsgType, unsigned int _uMmsi, unsigned int _uNavstatus,
                   int _iRot, unsigned int _uSog, bool _bPosAccuracy,
                   long _iPosLon, long _iPosLat, int _iCog, int _iHeading,
                   int _Repeat, bool _Raim, unsigned int _timestamp,
                   unsigned int _maneuver_i) override {
        MyAisDecoder::onType123(_uMsgType, _uMmsi, _uNavstatus, _iRot, _uSog, _bPosAccuracy,
                                _iPosLon, _iPosLat, _iCog, _iHeading, _Repeat, _Raim,
                                _timestamp, _maneuver_i);
        const float sog = (_uSog == 1023) ? -1.0f : _uSog / 10.0f;
        const float cog = (_iCog  == 3600) ? -1.0f : _iCog  / 10.0f;
        store_.recordPosition(_uMmsi, 'A', sog, cog);
        store_.recordNavStatus(_uMmsi, static_cast<uint8_t>(_uNavstatus));
        ++n2k_sent_;
    }

    // Class A static / voyage (type 5) -> PGN 129794. Has name and vessel type.
    void onType5(unsigned int _uMsgType, unsigned int _uMmsi, unsigned int _uImo,
                 const std::string &_strCallsign, const std::string &_strName,
                 unsigned int _uType, unsigned int _uToBow, unsigned int _uToStern,
                 unsigned int _uToPort, unsigned int _uToStarboard, unsigned int _uFixType,
                 unsigned int _uEtaMonth, unsigned int _uEtaDay, unsigned int _uEtaHour,
                 unsigned int _uEtaMinute, unsigned int _uDraught,
                 const std::string &_strDestination, unsigned int _ais_version,
                 unsigned int _repeat, bool _dte) override {
        MyAisDecoder::onType5(_uMsgType, _uMmsi, _uImo, _strCallsign, _strName, _uType,
                              _uToBow, _uToStern, _uToPort, _uToStarboard, _uFixType,
                              _uEtaMonth, _uEtaDay, _uEtaHour, _uEtaMinute, _uDraught,
                              _strDestination, _ais_version, _repeat, _dte);
        store_.recordName(_uMmsi, 'A', _strName.c_str());
        store_.recordType(_uMmsi, 'A', static_cast<uint8_t>(_uType));
        ++n2k_sent_;
    }

    // Class B position (type 18) -> PGN 129039.
    void onType18(unsigned int _uMsgType, unsigned int _uMmsi, unsigned int _uSog,
                  bool _bPosAccuracy, long _iPosLon, long _iPosLat, int _iCog, int _iHeading,
                  bool _raim, unsigned int _repeat, bool _unit, bool _diplay, bool _dsc,
                  bool _band, bool _msg22, bool _assigned, unsigned int _timestamp,
                  bool _state) override {
        MyAisDecoder::onType18(_uMsgType, _uMmsi, _uSog, _bPosAccuracy, _iPosLon, _iPosLat,
                               _iCog, _iHeading, _raim, _repeat, _unit, _diplay, _dsc, _band,
                               _msg22, _assigned, _timestamp, _state);
        const float sog = (_uSog == 1023) ? -1.0f : _uSog / 10.0f;
        const float cog = (_iCog  == 3600) ? -1.0f : _iCog  / 10.0f;
        store_.recordPosition(_uMmsi, 'B', sog, cog);
        ++n2k_sent_;
    }

    // Class B extended position (type 19) -> PGN 129040. Has name + type.
    void onType19(unsigned int _uMmsi, unsigned int _uSog, bool _bPosAccuracy,
                  int _iPosLon, int _iPosLat, int _iCog, int _iHeading,
                  const std::string &_strName, unsigned int _uType,
                  unsigned int _uToBow, unsigned int _uToStern, unsigned int _uToPort,
                  unsigned int _uToStarboard, unsigned int _timestamp, unsigned int _fixtype,
                  bool _dte, bool _assigned, unsigned int _repeat, bool _raim) override {
        MyAisDecoder::onType19(_uMmsi, _uSog, _bPosAccuracy, _iPosLon, _iPosLat, _iCog,
                               _iHeading, _strName, _uType, _uToBow, _uToStern, _uToPort,
                               _uToStarboard, _timestamp, _fixtype, _dte, _assigned, _repeat,
                               _raim);
        const float sog = (_uSog == 1023) ? -1.0f : _uSog / 10.0f;
        const float cog = (_iCog  == 3600) ? -1.0f : _iCog  / 10.0f;
        store_.recordPosition(_uMmsi, 'B', sog, cog);
        store_.recordName(_uMmsi, 'B', _strName.c_str());
        store_.recordType(_uMmsi, 'B', static_cast<uint8_t>(_uType));
        ++n2k_sent_;
    }

    // Class B static A (type 24A) -> PGN 129809. MMSI + name only.
    void onType24A(unsigned int _uMsgType, unsigned int _repeat, unsigned int _uMmsi,
                   const std::string &_strName) override {
        MyAisDecoder::onType24A(_uMsgType, _repeat, _uMmsi, _strName);
        store_.recordName(_uMmsi, 'B', _strName.c_str());
        ++n2k_sent_;
    }

    // Class B static B (type 24B) -> PGN 129810. Callsign + vessel type + dimensions.
    void onType24B(unsigned int _uMsgType, unsigned int _repeat, unsigned int _uMmsi,
                   const std::string &_strCallsign, unsigned int _uType,
                   unsigned int _uToBow, unsigned int _uToStern, unsigned int _uToPort,
                   unsigned int _uToStarboard, const std::string &_strVendor) override {
        MyAisDecoder::onType24B(_uMsgType, _repeat, _uMmsi, _strCallsign, _uType,
                                _uToBow, _uToStern, _uToPort, _uToStarboard, _strVendor);
        store_.recordType(_uMmsi, 'B', static_cast<uint8_t>(_uType));
        ++n2k_sent_;
    }

private:
    AisTargetStore store_;
    uint32_t n2k_sent_ = 0;
};
