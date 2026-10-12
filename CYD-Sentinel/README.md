# CYD Sentinel

An isolated Wi-Fi monitor for the ESP32 CYD board. It creates an access point without an Internet route, returns the ESP32 address for DNS A-record queries, observes HTTP requests sent to port 80, and shows events on the TFT and dashboard.

## Connect

- SSID: `CYD-IoT-Monitor`
- Password: `CydSentinel2026!`
- Dashboard: `http://192.168.4.1:8080/`

The dashboard remains on port 8080 so port 80 is free to capture sinkholed device HTTP requests.

## Included first release features

- Fixed-size 100-event RAM ring buffer with duplicate suppression.
- Wi-Fi station connect/disconnect tracking and device labels.
- DNS sinkhole for A-record queries, logged with source IP and hostname.
- HTTP sinkhole that logs the method, host, path, and declared body length, then replies `204 No Content`.
- TFT event summary and JSON dashboard API.
- Append-only SD-card event history at `/sentinel-events.ndjson` when a card is present. Events are flushed every five records or five seconds.

TLS metadata and raw packet observation for hard-coded IPs are intentionally deferred. Use this only with devices and networks you own or are authorized to test.