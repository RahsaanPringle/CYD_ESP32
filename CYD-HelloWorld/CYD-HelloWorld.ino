#include <TFT_eSPI.h>

constexpr uint8_t BacklightPin = 21;
TFT_eSPI display = TFT_eSPI();

void setup() {
  pinMode(BacklightPin, OUTPUT);
  digitalWrite(BacklightPin, HIGH);

  display.init();
  display.setRotation(1);
  display.fillScreen(TFT_BLACK);

  display.setTextColor(TFT_CYAN, TFT_RED);
  display.setTextSize(3);
  display.setCursor(34, 78);
  display.println("Hello World!");

  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(58, 125);
  display.println("ESP32 CYD");
}

void loop() {
}
