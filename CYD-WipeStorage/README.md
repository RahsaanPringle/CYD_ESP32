# CYD Storage Wipe Utility

This standalone sketch resets the storage used by the demo server:

- Formats LittleFS.
- Removes all files and folders from the SD card when one is present.

## Use it

1. Open `CYD-WipeStorage.ino` in Arduino IDE.
2. Select the same CYD board and serial port used by the server sketch.
3. Upload the sketch.
4. Open Serial Monitor at `115200` baud with line endings enabled.
5. Type `WIPE` and press Enter within 30 seconds.
6. Wait for `Wipe complete`, then upload `CYD-WebDAVServer.ino` or another demo sketch.

The utility does nothing unless the exact command `WIPE` is received. If the 30-second window expires, storage is left unchanged.

Warning: this permanently deletes the demo data from LittleFS and the SD card. Back up anything important before confirming.
