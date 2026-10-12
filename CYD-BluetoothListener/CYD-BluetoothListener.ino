#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <TFT_eSPI.h>

constexpr char DeviceName[] = "CYD Bluetooth Listener";
constexpr char ServiceUuid[] = "d2e22c28-5d9a-4ff4-8fbd-02f0ad8c6c64";
constexpr char WriteCharacteristicUuid[] = "32a5f9d0-bf73-4ba2-a482-1680474d6e9f";
constexpr uint8_t BacklightPin = 21;
constexpr uint8_t DisplayLogLines = 8;
constexpr uint8_t MaxCallbackEvents = 16;
constexpr size_t MaxWritePreviewBytes = 32;

enum class BluetoothEventType : uint8_t { Connected, Disconnected, Write };

struct BluetoothEvent {
  BluetoothEventType type = BluetoothEventType::Connected;
  uint32_t occurredAt = 0;
  uint16_t byteCount = 0;
  char preview[MaxWritePreviewBytes * 2 + 1] = {};
};

TFT_eSPI display;
BLEServer* bleServer = nullptr;
BLEAdvertising* bleAdvertising = nullptr;
BluetoothEvent callbackEvents[MaxCallbackEvents];
portMUX_TYPE callbackLock = portMUX_INITIALIZER_UNLOCKED;
volatile size_t callbackEventHead = 0;
volatile size_t callbackEventTail = 0;
String displayLines[DisplayLogLines];
uint32_t connectedClients = 0;

const char* eventTypeName(BluetoothEventType type) {
  switch (type) {
    case BluetoothEventType::Connected: return "BLE connected";
    case BluetoothEventType::Disconnected: return "BLE disconnected";
    case BluetoothEventType::Write: return "BLE write";
  }
  return "BLE event";
}

void redrawDisplay() {
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.print("BLE Listener");
  display.drawFastHLine(8, 28, 304, TFT_DARKGREY);
  display.setTextSize(1);
  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setCursor(8, 38);
  display.printf("%s", DeviceName);
  display.setCursor(8, 50);
  display.printf("%lu client%s", static_cast<unsigned long>(connectedClients), connectedClients == 1 ? "" : "s");
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  for (uint8_t index = 0; index < DisplayLogLines; ++index) {
    display.setCursor(8, 64 + index * 21);
    display.print(displayLines[index]);
  }
}

void addDisplayLine(const String& message) {
  Serial.println(message);
  for (uint8_t index = 1; index < DisplayLogLines; ++index) displayLines[index - 1] = displayLines[index];
  String line = String(millis() / 1000) + "s " + message;
  if (line.length() > 38) line.remove(38);
  displayLines[DisplayLogLines - 1] = line;
  redrawDisplay();
}

void enqueueEvent(BluetoothEventType type, const uint8_t* data = nullptr, size_t length = 0) {
  portENTER_CRITICAL(&callbackLock);
  const size_t nextHead = (callbackEventHead + 1) % MaxCallbackEvents;
  if (nextHead != callbackEventTail) {
    BluetoothEvent& event = callbackEvents[callbackEventHead];
    event = {};
    event.type = type;
    event.occurredAt = millis();
    event.byteCount = min(length, static_cast<size_t>(UINT16_MAX));
    const size_t previewLength = min(length, MaxWritePreviewBytes);
    for (size_t index = 0; index < previewLength; ++index) {
      snprintf(event.preview + index * 2, sizeof(event.preview) - index * 2, "%02X", data[index]);
    }
    callbackEventHead = nextHead;
  }
  portEXIT_CRITICAL(&callbackLock);
}

class ListenerServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    enqueueEvent(BluetoothEventType::Connected);
  }

  void onDisconnect(BLEServer*) override {
    enqueueEvent(BluetoothEventType::Disconnected);
  }
};

class ListenerWriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const String value = characteristic->getValue();
    enqueueEvent(BluetoothEventType::Write,
                 reinterpret_cast<const uint8_t*>(value.c_str()), value.length());
  }
};

void processBluetoothEvents() {
  while (true) {
    BluetoothEvent event;
    bool hasEvent = false;
    portENTER_CRITICAL(&callbackLock);
    if (callbackEventTail != callbackEventHead) {
      event = callbackEvents[callbackEventTail];
      callbackEventTail = (callbackEventTail + 1) % MaxCallbackEvents;
      hasEvent = true;
    }
    portEXIT_CRITICAL(&callbackLock);
    if (!hasEvent) return;

    if (event.type == BluetoothEventType::Connected) ++connectedClients;
    if (event.type == BluetoothEventType::Disconnected && connectedClients > 0) --connectedClients;
    String message = eventTypeName(event.type);
    if (event.type == BluetoothEventType::Write) {
      message += " " + String(event.byteCount) + " B";
      if (event.preview[0]) message += " " + String(event.preview);
    }
    addDisplayLine(message);
  }
}

void startBluetoothListener() {
  BLEDevice::init(DeviceName);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ListenerServerCallbacks());
  BLEService* service = bleServer->createService(ServiceUuid);
  BLECharacteristic* writeCharacteristic = service->createCharacteristic(
      WriteCharacteristicUuid, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  writeCharacteristic->setCallbacks(new ListenerWriteCallbacks());
  service->start();
  bleAdvertising = BLEDevice::getAdvertising();
  bleAdvertising->addServiceUUID(ServiceUuid);
  bleAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  addDisplayLine("Advertising started");
}

void setup() {
  Serial.begin(115200);
  pinMode(BacklightPin, OUTPUT);
  digitalWrite(BacklightPin, HIGH);
  display.init();
  display.setRotation(1);
  redrawDisplay();
  startBluetoothListener();
}

void loop() {
  processBluetoothEvents();
}