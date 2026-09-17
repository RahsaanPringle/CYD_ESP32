# CYD WebDAV Server

This is a separate test sketch for the ESP32 CYD. It leaves `CYD-DocumentServer` unchanged and runs a small WebDAV-style file server so a desktop WebDAV client or VS Code WebDAV extension can upload multiple files or folders in one operation.

## What it does

- Connects to the `FiveNet` Wi-Fi network.
- Uses the same CYD display/backlight pins as the working document server.
- Stores files on the SD card when one is present, otherwise on LittleFS.
- Supports these WebDAV methods: `OPTIONS`, `PROPFIND`, `GET`, `HEAD`, `PUT`, `MKCOL`, and `DELETE`.
- Uses basic authentication.

Default login:

- User: `cyd`
- Password: `upload`

## Upload from VS Code

VS Code does not include a built-in WebDAV explorer, but you can use either of these workflows:

1. Install a WebDAV-capable VS Code extension, then connect to the URL shown on the CYD screen, usually `http://<CYD-IP>/`, using user `cyd` and password `upload`.
2. Mount the WebDAV location with a desktop client such as Cyberduck, Mountain Duck, WinSCP WebDAV mode, or your operating system's network-location tool, then open that mounted folder in VS Code.

After connecting, copy or drag whole folders into the mounted/WebDAV location. The sketch creates folders with `MKCOL` and writes files with `PUT`.

## Upload and run

1. Open `CYD-WebDAVServer.ino` in Arduino IDE.
2. Select **ESP32 Dev Module** and the board's serial port.
3. Upload the sketch.
4. Open Serial Monitor at `115200` baud or read the CYD display.
5. Connect your WebDAV client to the printed `http://` address.

If Wi-Fi connection fails, the CYD starts an access point named `CYD-WebDAV` with password `cydupload`.

## Notes

This is intentionally simple and meant for a trusted private network. It uses HTTP with Basic authentication, not HTTPS. Change `webDavUser`, `webDavPassword`, and `webDavBasicAuth` together if you want different credentials. The current `webDavBasicAuth` value is the Base64 form of `cyd:upload`.