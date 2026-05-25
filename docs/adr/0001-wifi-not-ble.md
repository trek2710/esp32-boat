# WiFi (not BLE) for the cross-device data plane

A BLE bridge direction was parked on 2026-05-18 as the future path for
sharing AIS data with the esp32-boat RX board and a phone. When the goal
broadened on 2026-05-22 to "a virtual N2K backbone shared by every ESP and
an iOS app," BLE's constraints became disqualifying: ≈50 kbit/s per link,
iOS can only be a central (no peripheral / multi-peer mesh role), and ~10 m
practical range through a fibreglass hull. WiFi with UDP multicast on a
shared LAN matches the peer-to-peer broadcast semantics of N2K natively and
has none of those limits.
