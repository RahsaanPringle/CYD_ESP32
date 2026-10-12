# CYD Bluetooth Listener

A separate CYD BLE GATT listener that accepts BLE connections to its advertised service and displays activity on the TFT.

## What it accepts

The board advertises as `CYD Bluetooth Listener` and exposes one writable BLE GATT characteristic:

- Service UUID: `d2e22c28-5d9a-4ff4-8fbd-02f0ad8c6c64`
- Writable characteristic UUID: `32a5f9d0-bf73-4ba2-a482-1680474d6e9f`

Use a BLE tool such as nRF Connect or LightBlue to connect and write to that characteristic. The TFT and serial log record connections, disconnections, write size, and a bounded hexadecimal preview of the first 32 bytes.

## Scope

Bluetooth devices only connect when they support the advertised BLE GATT service. This sketch does not impersonate arbitrary Bluetooth profiles, pair with clients, or capture traffic for other applications. Use it only with devices and environments you are authorized to test.