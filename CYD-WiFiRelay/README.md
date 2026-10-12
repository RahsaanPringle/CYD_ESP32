# CYD Wi-Fi Relay

This project turns a CYD into a Wi-Fi relay. Set the upstream network name and password in `UpstreamSsid` and `UpstreamPassword` near the top of the sketch, then connect devices to the relay access point.

## Relay network

- SSID: `CYD-WiFi-Relay`
- Password: `CydRelay2026!`
- Relay address: `192.168.4.1`

## Traffic display

The TFT shows upstream connection state, access-point client count, and DNS activity delivered to the relay. It does not save traffic.

## Important compatibility note

Internet sharing requires ESP32 NAPT support (`lwip_napt`) in the installed Arduino-ESP32 core. When unavailable, the sketch still connects to the upstream network and starts the relay AP, but shows `NAT: OFF`; clients cannot reach the Internet through it. NAT routing does not expose every encrypted request to an Arduino sketch, so this project intentionally logs connection and DNS metadata rather than payloads.

The current version does not require a touchscreen library. It reads the configured upstream credentials directly from the sketch.