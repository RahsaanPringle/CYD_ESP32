# ESP32 Cheap Yellow Display: Hello World

This starter targets the common 2.8-inch ESP32 CYD board, usually sold as `ESP32-2432S028` with an ILI9341 display.

## Arduino IDE setup

1. Install the Arduino IDE.
2. In **File > Preferences**, add this ESP32 board URL to **Additional Boards Manager URLs**:

   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`

3. In **Tools > Board > Boards Manager**, install **esp32 by Espressif Systems**.
4. In **Sketch > Include Library > Manage Libraries**, install **TFT_eSPI by Bodmer**.
5. Open the `CYD-HelloWorld.ino` sketch.
6. Select **Tools > Board > ESP32 Arduino > ESP32 Dev Module**.
7. Select the USB serial port for the board.
8. Click **Upload**. If upload waits indefinitely, hold the board's **BOOT** button while it starts uploading, then release it.

## TFT_eSPI configuration

For the common `ESP32-2432S028` board, open the installed `TFT_eSPI` library folder and edit `User_Setup.h`. Make sure it contains these settings (comment out conflicting driver and pin definitions first):

```cpp
#define ILI9341_2_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
```

Do not enable another driver setup at the same time. The library is usually installed under `Documents\Arduino\libraries\TFT_eSPI` on Windows.

The sketch also enables the common CYD backlight on GPIO 21. If the screen remains completely dark after upload, briefly shine a flashlight at the panel: if faint text is visible, the board's backlight pin or board variant differs from this mapping.

## Expected result

After reset, the display shows:

- `Hello World!` in cyan
- `ESP32 CYD` in white

If the screen is blank or the colors are wrong, the board variant or TFT_eSPI setup is likely different. Check the controller and display size printed on the board before changing the sketch.
