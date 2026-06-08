#!/usr/bin/env python3
"""Generate verified AIS Type-18 (Class B position) AIVDM test sentences
placed around a centre point, for injecting via the dAISy 2+ `T` menu.

Each sentence is round-trip decoded to confirm the encoded lat/lon/SOG/COG
match the intent, and the NMEA-0183 checksum is computed correctly (the
dAISy silently drops sentences with a bad checksum). Type 18 is 168 bits.
"""
import math

CENTRE = (55.76196, 12.62900)   # the device's assumed own-ship position

# (mmsi, range_nm, bearing_deg, sog_kn, cog_deg)
TARGETS = [
    (211000001, 0.4,  20, 5.2, 200),
    (211000002, 0.8, 110, 7.0, 300),
    (211000003, 1.2, 200, 3.5,  40),
    (211000004, 1.6, 290, 9.0, 120),
    (211000005, 0.6, 340, 4.0,  60),
]


def dest_point(lat, lon, bearing_deg, dist_nm):
    R = 3440.065
    br = math.radians(bearing_deg)
    d = dist_nm / R
    p1 = math.radians(lat)
    l1 = math.radians(lon)
    p2 = math.asin(math.sin(p1) * math.cos(d) + math.cos(p1) * math.sin(d) * math.cos(br))
    l2 = l1 + math.atan2(math.sin(br) * math.sin(d) * math.cos(p1),
                         math.cos(d) - math.sin(p1) * math.sin(p2))
    return math.degrees(p2), math.degrees(l2)


def haversine_nm(a, b):
    R = 3440.065
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    dphi = math.radians(b[0] - a[0])
    dlam = math.radians(b[1] - a[1])
    h = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlam / 2) ** 2
    return R * 2 * math.atan2(math.sqrt(h), math.sqrt(1 - h))


def bits(val, n):
    if val < 0:
        val += (1 << n)
    return format(val & ((1 << n) - 1), '0{}b'.format(n))


def encode_type18(mmsi, lat, lon, sog_kn, cog_deg):
    b = ''
    b += bits(18, 6)                        # message type 18
    b += bits(0, 2)                         # repeat indicator
    b += bits(mmsi, 30)                     # MMSI
    b += bits(0, 8)                         # reserved
    b += bits(round(sog_kn * 10), 10)       # SOG, 0.1 kn
    b += bits(0, 1)                         # position accuracy
    b += bits(round(lon * 600000), 28)      # longitude, 1/600000 min
    b += bits(round(lat * 600000), 27)      # latitude
    b += bits(round(cog_deg * 10), 12)      # COG, 0.1 deg
    b += bits(511, 9)                       # true heading n/a
    b += bits(60, 6)                        # timestamp n/a
    b += bits(0, 2)                         # reserved
    b += bits(1, 1)                         # CS unit
    b += bits(0, 1)                         # display
    b += bits(0, 1)                         # DSC
    b += bits(1, 1)                         # band
    b += bits(1, 1)                         # message 22
    b += bits(0, 1)                         # assigned
    b += bits(0, 1)                         # RAIM
    b += bits(0, 20)                        # radio status
    assert len(b) == 168, len(b)
    payload = ''
    for i in range(0, 168, 6):
        v = int(b[i:i + 6], 2)
        payload += chr(v + 48 if v < 40 else v + 56)
    return payload


def decode_type18(payload):
    b = ''
    for c in payload:
        v = ord(c) - 48
        if v > 40:
            v -= 8
        b += format(v, '06b')

    def g(s, n, signed=False):
        v = int(b[s:s + n], 2)
        if signed and v >= (1 << (n - 1)):
            v -= (1 << n)
        return v
    return dict(mmsi=g(8, 30), sog=g(46, 10) / 10,
                lon=g(57, 28, True) / 600000, lat=g(85, 27, True) / 600000,
                cog=g(112, 12) / 10)


def nmea(payload, channel='A', fill=0):
    body = "AIVDM,1,1,,{},{},{}".format(channel, payload, fill)
    cs = 0
    for c in body:
        cs ^= ord(c)
    return "!{}*{:02X}".format(body, cs)


def main():
    print("# Type-18 AIS test targets around %.5f, %.5f" % CENTRE)
    print("# paste one (or several, one per line) into the dAISy `T` prompt\n")
    for mmsi, rng, brg, sog, cog in TARGETS:
        lat, lon = dest_point(CENTRE[0], CENTRE[1], brg, rng)
        payload = encode_type18(mmsi, lat, lon, sog, cog)
        s = nmea(payload, 'A', 0)
        d = decode_type18(payload)
        err = haversine_nm((lat, lon), (d['lat'], d['lon'])) * 1852.0
        ok = (d['mmsi'] == mmsi and err < 1.0
              and abs(d['sog'] - sog) < 0.1 and abs(d['cog'] - cog) < 0.1)
        print(s)
        print("    # mmsi=%d  %.1fNM brg%03d  sog=%.1f cog=%03d  "
              "round-trip err=%.2fm  %s"
              % (mmsi, rng, brg, sog, cog, err, "OK" if ok else "** FAIL **"))


if __name__ == "__main__":
    main()
