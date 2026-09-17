#include <LittleFS.h>
#include <SD.h>

constexpr uint8_t sdChipSelectPin = 5;
constexpr unsigned long confirmationWindow = 30000;

String baseName(const String& path) {
  const int lastSlash = path.lastIndexOf('/');
  return lastSlash < 0 ? path : path.substring(lastSlash + 1);
}

bool deleteRecursive(FS& storage, const String& path) {
  File target = storage.open(path);
  if (!target) return false;

  if (!target.isDirectory()) {
    target.close();
    return storage.remove(path);
  }

  File entry = target.openNextFile();
  while (entry) {
    const String childName = baseName(String(entry.name()));
    const String childPath = path == "/" ? "/" + childName : path + "/" + childName;
    const bool childIsDirectory = entry.isDirectory();
    entry.close();

    if (childIsDirectory) {
      if (!deleteRecursive(storage, childPath)) {
        target.close();
        return false;
      }
    } else if (!storage.remove(childPath)) {
      target.close();
      return false;
    }

    entry = target.openNextFile();
  }

  target.close();
  return path == "/" || storage.rmdir(path);
}

bool deleteSdContents() {
  if (!SD.begin(sdChipSelectPin)) {
    Serial.println("SD card not detected; nothing to erase on SD.");
    return true;
  }

  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    Serial.println("Could not open SD root.");
    return false;
  }

  File entry = root.openNextFile();
  bool success = true;
  while (entry) {
    const String path = String(entry.name());
    const bool isDirectory = entry.isDirectory();
    entry.close();

    if (isDirectory) success = deleteRecursive(SD, path) && success;
    else success = SD.remove(path) && success;
    entry = root.openNextFile();
  }

  root.close();
  return success;
}

void wipeStorage() {
  Serial.println("Starting storage wipe...");

  if (LittleFS.begin(false)) {
    Serial.println("Formatting LittleFS...");
    Serial.println(LittleFS.format() ? "LittleFS formatted." : "LittleFS format failed.");
    LittleFS.end();
  } else {
    Serial.println("LittleFS could not be mounted; format skipped.");
  }

  Serial.println(deleteSdContents() ? "SD contents removed." : "Some SD contents could not be removed.");
  Serial.println("Wipe complete. You can now upload the demo sketch.");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("CYD storage wipe utility");
  Serial.println("This will erase LittleFS and all files on the SD card.");
  Serial.println("Type WIPE and press Enter within 30 seconds to continue.");

  const unsigned long started = millis();
  while (millis() - started < confirmationWindow) {
    if (Serial.available()) {
      String command = Serial.readStringUntil('\n');
      command.trim();
      if (command == "WIPE") {
        wipeStorage();
        return;
      }
      Serial.println("No action taken. Type WIPE exactly to confirm.");
    }
    delay(10);
  }

  Serial.println("Timed out. No storage was changed.");
}

void loop() {
  delay(1000);
}
