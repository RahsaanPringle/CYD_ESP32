#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <SD.h>
#include <TFT_eSPI.h>

const char* wifiSsid = "wifiSsid";
const char* wifiPassword = "wifiPassword";
constexpr uint8_t backlightPin = 21;
constexpr uint8_t sdChipSelectPin = 5;
constexpr uint8_t logLineCount = 8;

WebServer server(80);
File uploadFile;
size_t uploadBytes = 0;
FS* storage = nullptr;
TFT_eSPI display = TFT_eSPI();
String logLines[logLineCount];

void logEvent(const String& message) {
  Serial.println(message);

  for (uint8_t index = 1; index < logLineCount; ++index) {
    logLines[index - 1] = logLines[index];
  }

  String line = String(millis() / 1000) + "s " + message;
  if (line.length() > 38) line = line.substring(0, 38);
  logLines[logLineCount - 1] = line;

  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.print("Document server");
  display.drawFastHLine(8, 28, 304, TFT_DARKGREY);

  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(1);
  for (uint8_t index = 0; index < logLineCount; ++index) {
    display.setCursor(8, 38 + index * 23);
    display.print(logLines[index]);
  }
}

String htmlEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length());

  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character == '&') escaped += F("&amp;");
    else if (character == '<') escaped += F("&lt;");
    else if (character == '>') escaped += F("&gt;");
    else if (character == '"') escaped += F("&quot;");
    else escaped += character;
  }

  return escaped;
}

String safePath(String fileName) {
  fileName.replace("\\", "/");

  String cleaned;
  int segmentStart = 0;
  while (segmentStart < fileName.length()) {
    int segmentEnd = fileName.indexOf('/', segmentStart);
    if (segmentEnd < 0) segmentEnd = fileName.length();
    const String segment = fileName.substring(segmentStart, segmentEnd);
    if (!segment.isEmpty() && segment != "." && segment != "..") {
      String cleanSegment;
      for (size_t index = 0; index < segment.length(); ++index) {
        const char character = segment[index];
        if (isalnum(character) || character == '.' || character == '-' || character == '_' || character == ' ') {
          cleanSegment += character;
        }
      }
      if (!cleanSegment.isEmpty()) {
        if (cleaned.length() > 0) cleaned += '/';
        cleaned += cleanSegment;
      }
    }
    segmentStart = segmentEnd + 1;
  }

  return cleaned;
}

String safeFileName(String fileName) {
  const String path = safePath(fileName);
  const int lastSlash = path.lastIndexOf('/');
  return lastSlash < 0 ? path : path.substring(lastSlash + 1);
}

String uploadPath(String fileName) {
  const String path = safePath(fileName);
  const int firstSlash = path.indexOf('/');
  return firstSlash < 0 ? path : path.substring(firstSlash + 1);
}

String baseName(const String& path) {
  const int lastSlash = path.lastIndexOf('/');
  return lastSlash < 0 ? path : path.substring(lastSlash + 1);
}

void createParentDirectories(const String& path) {
  int slash = path.indexOf('/');
  while (slash >= 0) {
    storage->mkdir("/" + path.substring(0, slash));
    slash = path.indexOf('/', slash + 1);
  }
}

String storagePathForRequest(String requestPath) {
  if (!requestPath.startsWith("/")) requestPath = "/" + requestPath;
  return safePath(requestPath);
}

String resolveStoragePath(String requestPath) {
  requestPath = safePath(requestPath);
  if (requestPath.isEmpty() || storage->exists("/" + requestPath)) return requestPath;

  int slash = requestPath.indexOf('/');
  while (slash >= 0) {
    const String suffix = requestPath.substring(slash + 1);
    if (storage->exists("/" + suffix)) return suffix;
    slash = requestPath.indexOf('/', slash + 1);
  }

  return requestPath;
}

String fileSize(size_t bytes) {
  if (bytes < 1024) return String(bytes) + " B";
  if (bytes < 1024 * 1024) return String(bytes / 1024.0f, 1) + " KB";
  return String(bytes / (1024.0f * 1024.0f), 1) + " MB";
}

const char* contentTypeFor(const String& fileName) {
  String lowerName = fileName;
  lowerName.toLowerCase();
  if (lowerName.endsWith(".html") || lowerName.endsWith(".htm")) return "text/html; charset=utf-8";
  if (lowerName.endsWith(".css")) return "text/css; charset=utf-8";
  if (lowerName.endsWith(".js")) return "application/javascript; charset=utf-8";
  if (lowerName.endsWith(".json")) return "application/json; charset=utf-8";
  if (lowerName.endsWith(".txt")) return "text/plain; charset=utf-8";
  if (lowerName.endsWith(".jpg") || lowerName.endsWith(".jpeg")) return "image/jpeg";
  if (lowerName.endsWith(".png")) return "image/png";
  if (lowerName.endsWith(".gif")) return "image/gif";
  if (lowerName.endsWith(".webp")) return "image/webp";
  if (lowerName.endsWith(".avif")) return "image/avif";
  if (lowerName.endsWith(".ico")) return "image/x-icon";
  if (lowerName.endsWith(".svg")) return "image/svg+xml";
  if (lowerName.endsWith(".pdf")) return "application/pdf";
  return "application/octet-stream";
}

bool deleteDirectory(const String& path) {
  File directory = storage->open(path);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return false;
  }

  File entry = directory.openNextFile();
  while (entry) {
    String entryPath = String(entry.name());
    if (!entryPath.startsWith("/")) {
      entryPath = path + "/" + entryPath;
    }
    const bool isDirectory = entry.isDirectory();
    entry.close();
    if (isDirectory) {
      if (!deleteDirectory(entryPath)) {
        directory.close();
        return false;
      }
    } else if (!storage->remove(entryPath)) {
      directory.close();
      return false;
    }
    entry = directory.openNextFile();
  }
  directory.close();
  return storage->rmdir(path) || storage->remove(path);
}

void appendStoredFiles(File directory, const String& currentPath, String& page, bool& hasFiles) {
  File file = directory.openNextFile();
  while (file) {
    const String name = baseName(String(file.name()));
    const String storedPath = currentPath.isEmpty() ? name : currentPath + "/" + name;
    if (file.isDirectory()) {
      page += F("<li><a href=\"/");
      page += storedPath;
      page += F("/\">📁 ");
      page += htmlEscape(name);
      page += F("/</a><a href=\"/manage?path=");
      page += storedPath;
      page += F("\">Open</a><form action=\"/delete\" method=\"post\" style=\"margin:0\"><input type=\"hidden\" name=\"name\" value=\"");
      page += htmlEscape(storedPath);
      page += F("\"><input type=\"hidden\" name=\"folder\" value=\"1\"><button class=\"delete\" type=\"submit\">Delete folder</button></form></li>");
    } else {
      hasFiles = true;
      page += F("<li><span><a href=\"/");
      page += storedPath;
      page += F("\">");
      page += htmlEscape(storedPath);
      page += F("</a> <span class=\"meta\">");
      page += fileSize(file.size());
      page += F("</span></span><form action=\"/delete\" method=\"post\" style=\"margin:0\"><input type=\"hidden\" name=\"name\" value=\"");
      page += htmlEscape(storedPath);
      page += F("\"><button class=\"delete\" type=\"submit\">Delete</button></form></li>");
    }
    file = directory.openNextFile();
  }
}

void handleManager() {
  const String currentPath = safePath(server.arg("path"));
  logEvent("GET /manage");
  String page = F(R"HTML(<!doctype html>
<html lang="en">
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FiveNet Documents</title>
<style>
:root { color-scheme: light; font-family: system-ui, sans-serif; }
body { max-width: 760px; margin: 0 auto; padding: 24px; color: #17202a; background: #eef3f5; }
main { background: white; padding: 24px; border: 1px solid #d7e0e4; border-radius: 8px; }
h1 { margin-top: 0; }
form { display: flex; gap: 12px; flex-wrap: wrap; align-items: center; margin-bottom: 24px; }
button, input::file-selector-button { padding: 9px 13px; border: 0; border-radius: 4px; background: #176b87; color: white; cursor: pointer; }
input[type=file] { max-width: 100%; }
ul { padding: 0; list-style: none; }
li { display: flex; gap: 12px; justify-content: space-between; align-items: center; padding: 12px 0; border-top: 1px solid #e2e8eb; }
a { color: #176b87; overflow-wrap: anywhere; }
.meta { color: #63727a; font-size: .9rem; }
.delete { background: #a43d3d; }
.empty { color: #63727a; }
</style>
</head>
<body>
<main>
<h1>FiveNet Documents</h1>
<p>Folder: <a href="/manage">/</a>)HTML");
  page += currentPath;
  page += F(R"HTML(</p>
<p><a href="/manage">Manage root</a>)HTML");
  if (!currentPath.isEmpty()) {
    const int parentSlash = currentPath.lastIndexOf('/');
    const String parentPath = parentSlash < 0 ? "" : currentPath.substring(0, parentSlash);
    page += F(" | <a href=\"/manage?path=");
    page += parentPath;
    page += F("\">Open parent</a>");
  }
  page += F(R"HTML(</p>
<form action="/mkdir" method="post">
<input type="hidden" name="path" value=")HTML");
  page += htmlEscape(currentPath);
  page += F(R"HTML(">
<input type="text" name="name" placeholder="New folder" required>
<button type="submit">Create folder</button>
</form>
<form action="/upload?path=)HTML");
  page += currentPath;
  page += F(R"HTML(" method="post" enctype="multipart/form-data">
<input type="hidden" name="path" value=")HTML");
  page += htmlEscape(currentPath);
  page += F(R"HTML(">
<input type="file" name="document" multiple required>
<button type="submit">Choose Files and Upload</button>
</form>
<form action="/upload?path=)HTML");
  page += currentPath;
  page += F(R"HTML(" method="post" enctype="multipart/form-data">
<input type="hidden" name="path" value=")HTML");
  page += htmlEscape(currentPath);
  page += F(R"HTML(">
<input type="file" name="document" webkitdirectory directory required>
<button type="submit">Choose Folder and Upload</button>
</form>
<h2>Stored files</h2>
<ul>)HTML");

  bool hasFiles = false;
  File root = storage->open(currentPath.isEmpty() ? "/" : "/" + currentPath);
  appendStoredFiles(root, currentPath, page, hasFiles);
  root.close();

  if (!hasFiles) page += F("<li class=\"empty\">No documents uploaded yet.</li>");
  page += F(R"HTML(</ul>
</main>
</body>
</html>)HTML");
  server.send(200, "text/html; charset=utf-8", page);
}

void handleHome() {
  handleManager();
}

void handleStorageRequest() {
  String requestPath = resolveStoragePath(storagePathForRequest(server.uri()));
  if (requestPath.isEmpty()) requestPath = "index.html";
  if (storage->exists("/" + requestPath)) {
    File requestedFile = storage->open("/" + requestPath);
    const bool isDirectory = requestedFile.isDirectory();
    requestedFile.close();
    if (isDirectory) {
      if (!server.uri().endsWith("/")) {
        server.sendHeader("Location", server.uri() + "/");
        server.send(301);
        return;
      }
      requestPath += "/index.html";
    }
  }

  if (storage->exists("/" + requestPath)) {
    File requestedFile = storage->open("/" + requestPath);
    const bool isDirectory = requestedFile.isDirectory();
    requestedFile.close();
    if (isDirectory && server.uri().endsWith("/")) requestPath += "/index.html";
  }

  if (!storage->exists("/" + requestPath) && !requestPath.startsWith("/")) {
    const String referrer = server.header("Referer");
    const int hostEnd = referrer.indexOf('/', referrer.indexOf("://") + 3);
    if (hostEnd >= 0) {
      String referrerPath = referrer.substring(hostEnd);
      const int lastSlash = referrerPath.lastIndexOf('/');
      if (lastSlash >= 0) {
        referrerPath = referrerPath.substring(0, lastSlash);
        const String relativePath = safePath(referrerPath + "/" + requestPath);
        if (storage->exists("/" + relativePath)) requestPath = relativePath;
      }
    }
  }

  if (!storage->exists("/" + requestPath)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File file = storage->open("/" + requestPath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain", "File not found");
    return;
  }

  const size_t fileLength = file.size();
  logEvent("GET " + requestPath + " " + String(fileLength) + "B");
  server.sendHeader("Content-Length", String(fileLength));
  server.streamFile(file, contentTypeFor(requestPath));
  file.close();
}

void handleDownload() {
  const String fileName = safePath(server.arg("name"));
  logEvent("GET " + fileName);
  if (fileName.isEmpty()) {
    server.send(400, "text/plain", "Invalid file name");
    return;
  }

  const String path = "/" + fileName;
  if (!storage->exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File file = storage->open(path, FILE_READ);
  server.streamFile(file, contentTypeFor(fileName));
  file.close();
}

void handleDelete() {
  const String fileName = safePath(server.arg("name"));
  logEvent("DELETE " + fileName);
  if (fileName.isEmpty()) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  const String path = "/" + fileName;
  bool deleted = false;
  File target = storage->open(path);
  if (target) {
    if (target.isDirectory() && server.arg("folder") == "1") {
      target.close();
      deleted = deleteDirectory(path);
    } else if (!target.isDirectory() && server.arg("folder") != "1") {
      target.close();
      deleted = storage->remove(path);
    } else {
      target.close();
    }
  }

  if (!deleted) {
    server.send(404, "text/plain", "File not found or folder is not empty");
    return;
  }

  server.sendHeader("Location", "/manage");
  server.send(303);
}

void handleUpload() {
  logEvent("Upload complete");
  server.sendHeader("Location", "/manage?path=" + safePath(server.arg("path")));
  server.send(303);
}

void handleCreateFolder() {
  const String parentPath = safePath(server.arg("path"));
  const String folderName = safePath(server.arg("name"));
  if (folderName.isEmpty() || folderName.indexOf('/') >= 0) {
    server.send(400, "text/plain", "Invalid folder name");
    return;
  }

  const String folderPath = parentPath.isEmpty() ? folderName : parentPath + "/" + folderName;
  if (!storage->mkdir("/" + folderPath)) {
    server.send(409, "text/plain", "Folder could not be created");
    return;
  }

  server.sendHeader("Location", "/manage?path=" + parentPath);
  server.send(303);
}

void handleUploadData() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    const String relativePath = uploadPath(upload.filename);
    const String destinationPath = safePath(server.arg("path"));
    const String fileName = destinationPath.isEmpty() ? relativePath : destinationPath + "/" + relativePath;
    if (fileName.isEmpty()) return;
    logEvent("Upload " + fileName);
    createParentDirectories(fileName);
    uploadFile = storage->open("/" + fileName, FILE_WRITE);
    uploadBytes = 0;
    if (!uploadFile) logEvent("Upload open failed");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadBytes += uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.flush();
      uploadFile.close();
      logEvent("Upload saved " + String(uploadBytes) + "B");
    }
  }
}

bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPassword);

  Serial.print("Connecting to FiveNet");
  const unsigned long connectionStarted = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectionStarted < 20000) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    logEvent("Wi-Fi failed: " + String(WiFi.status()));
    return false;
  }

  logEvent("Wi-Fi " + WiFi.localIP().toString());
  return true;
}

void setup() {
  Serial.begin(115200);
  pinMode(backlightPin, OUTPUT);
  digitalWrite(backlightPin, HIGH);
  display.init();
  display.setRotation(1);
  display.fillScreen(TFT_BLACK);
  logEvent("Starting...");

  if (!LittleFS.begin(true)) {
    logEvent("LittleFS mount failed");
    while (true) delay(1000);
  }

  storage = &LittleFS;
  logEvent("Storage: LittleFS");
  if (SD.begin(sdChipSelectPin)) {
    storage = &SD;
    logEvent("Storage: SD card");
  } else {
    logEvent("Storage: no SD card");
  }

  if (!connectToWiFi()) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("CYD-DocumentServer", "cydserver");
    logEvent("AP CYD-DocumentServer");
    logEvent("AP IP " + WiFi.softAPIP().toString());
    logEvent("AP password: cydserver");
  } else {
    logEvent("HTTP server ready");
  }

  server.on("/", HTTP_GET, handleHome);
  server.on("/manage", HTTP_GET, handleManager);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/mkdir", HTTP_POST, handleCreateFolder);
  server.on("/upload", HTTP_POST, handleUpload, handleUploadData);
  const char* requestHeaders[] = {"Referer"};
  server.collectHeaders(requestHeaders, 1);
  server.onNotFound(handleStorageRequest);
  server.begin();
  logEvent("HTTP server started");
}

void loop() {
  server.handleClient();
}
