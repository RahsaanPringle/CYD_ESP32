# CYD Document Server

This sketch connects the ESP32 CYD to the `FiveNet` Wi-Fi network and serves a small document manager over HTTP.

## Use

1. Open `CYD-DocumentServer.ino` in Arduino IDE.
2. Select **ESP32 Dev Module** and the board's serial port.
3. Upload the sketch.
4. Open Serial Monitor at `115200` baud.
5. Browse to the printed `http://` address from a device on FiveNet.
6. Use the page to upload, download, or delete documents.

Files are stored in the ESP32's LittleFS flash storage. The available space depends on the board's partition scheme and is not a substitute for an SD card for a large document collection.

The Wi-Fi password is currently stored in the sketch as requested. Anyone with access to the network can reach the server, and this version has no login or HTTPS, so use it only on a trusted private network.
