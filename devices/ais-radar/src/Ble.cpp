#include "Ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <cmath>
#include <AisRadarBle.h>

namespace ble {
namespace {

NimBLECharacteristic* g_own = nullptr;
NimBLECharacteristic* g_tgt = nullptr;
bool g_connected = false;

HostGps  g_host;
uint32_t g_hostMs = 0;
constexpr uint32_t kHostFreshMs = 5000;

double e7d(int32_t v)  { return (v == INT32_MIN) ? NAN : v / 1e7; }
double d10d(int16_t v) { return (v == INT16_MIN) ? NAN : v / 10.0; }

class GpsCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        const std::string v = c->getValue();
        if (v.size() < sizeof(BleHostGps)) return;
        BleHostGps g;
        memcpy(&g, v.data(), sizeof(g));
        g_host.lat    = e7d(g.lat_e7);
        g_host.lon    = e7d(g.lon_e7);
        g_host.cogDeg = d10d(g.cog_deg10);
        g_host.sogKn  = d10d(g.sog_kn10);
        g_hostMs = millis();
    }
};
GpsCb g_gpsCb;

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override {
        g_connected = true;
        Serial.println("[ble] central connected");
    }
    void onDisconnect(NimBLEServer*) override {
        g_connected = false;
        Serial.println("[ble] central disconnected — re-advertising");
        NimBLEDevice::startAdvertising();
    }
};
ServerCb g_cb;

int32_t e7(double d)   { return (int32_t)llround(d * 1e7); }
int16_t d10(double v)  { return (int16_t)lround(v * 10.0); }

}  // namespace

void begin() {
    NimBLEDevice::init(AISRADAR_BLE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P3);
    NimBLEServer* srv = NimBLEDevice::createServer();
    srv->setCallbacks(&g_cb);

    NimBLEService* svc = srv->createService(AISRADAR_BLE_SVC_UUID);
    g_own = svc->createCharacteristic(
        AISRADAR_BLE_OWN_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
    g_tgt = svc->createCharacteristic(
        AISRADAR_BLE_TGT_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
    NimBLECharacteristic* gps = svc->createCharacteristic(
        AISRADAR_BLE_GPS_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    gps->setCallbacks(&g_gpsCb);
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(AISRADAR_BLE_SVC_UUID);
    adv->setName(AISRADAR_BLE_NAME);
    NimBLEDevice::startAdvertising();
    Serial.printf("[ble] advertising as \"%s\"\n", AISRADAR_BLE_NAME);
}

void publish(AisTargetStore& store, double ownLat, double ownLon,
             double ownCogDeg, bool haveFix, int threatLevel) {
    if (!g_connected) return;

    AisTarget t[AisTargetStore::CAPACITY];
    const size_t n = store.snapshotByRecency(t, AisTargetStore::CAPACITY);

    BleOwnShip o;
    // Always send the position the radar is centred on (a bench coord when
    // there's no real GPS fix). The flags bit, not a sentinel, marks whether
    // it's a real fix — so the central can still place targets either way.
    o.lat_e7    = e7(ownLat);
    o.lon_e7    = e7(ownLon);
    o.cog_deg10 = std::isnan(ownCogDeg) ? INT16_MIN : d10(ownCogDeg);
    o.flags     = (haveFix ? 0x01 : 0x00)
                | (uint8_t)((threatLevel & 0x03) << 1);
    o.targets   = (uint8_t)n;
    g_own->setValue((uint8_t*)&o, sizeof(o));
    g_own->notify();

    const uint32_t now = millis();
    for (size_t i = 0; i < n; ++i) {
        const bool hasPos = (t[i].lat_deg != 0.0 || t[i].lon_deg != 0.0);
        BleTarget bt;
        bt.mmsi      = t[i].mmsi;
        bt.lat_e7    = hasPos ? e7(t[i].lat_deg) : INT32_MIN;
        bt.lon_e7    = hasPos ? e7(t[i].lon_deg) : INT32_MIN;
        bt.sog_kn10  = (t[i].sog_kn  >= 0) ? d10(t[i].sog_kn)  : INT16_MIN;
        bt.cog_deg10 = (t[i].cog_deg >= 0) ? d10(t[i].cog_deg) : INT16_MIN;
        bt.ship_type = t[i].vessel_type;
        const uint32_t age = (now - t[i].last_seen_ms) / 1000;
        bt.age_s     = age > 255 ? 255 : (uint8_t)age;
        g_tgt->setValue((uint8_t*)&bt, sizeof(bt));
        g_tgt->notify();
        delay(6);   // small gap so back-to-back notifies aren't dropped
    }
}

bool connected() { return g_connected; }

bool hostGps(HostGps* out) {
    if (g_hostMs == 0 || (millis() - g_hostMs) > kHostFreshMs) return false;
    *out = g_host;
    return true;
}

}  // namespace ble
