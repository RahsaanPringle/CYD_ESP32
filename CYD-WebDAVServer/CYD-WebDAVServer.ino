#include <WiFi.h>
#include <WiFiClient.h>
#include <LittleFS.h>
#include <SD.h>
#include <TFT_eSPI.h>

const char* wifiSsid = "WifiSSID";
const char* wifiPassword = "PassWord";
const char* webDavUser = "cyd";
const char* webDavPassword = "upload";
const char* webDavBasicAuth = "Basic Y3lkOnVwbG9hZA==";

constexpr uint16_t serverPort = 80;
constexpr uint8_t backlightPin = 21;
constexpr uint8_t sdChipSelectPin = 5;
constexpr uint8_t logLineCount = 8;
constexpr size_t transferBufferSize = 1024;
constexpr bool requireAuthentication = true;

WiFiServer server(serverPort);
FS* storage = nullptr;
TFT_eSPI display = TFT_eSPI();
String logLines[logLineCount];
IPAddress serverAddress;

struct HttpRequest {
  String method;
  String path;
  String depth;
  String authorization;
  size_t contentLength = 0;
  bool valid = false;
};

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
  display.print("WebDAV server");
  display.drawFastHLine(8, 28, 304, TFT_DARKGREY);

  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(1);
  for (uint8_t index = 0; index < logLineCount; ++index) {
    display.setCursor(8, 38 + index * 23);
    display.print(logLines[index]);
  }
}

int hexValue(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

String urlDecode(const String& value) {
  String decoded;
  decoded.reserve(value.length());

  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character == '%' && index + 2 < value.length()) {
      const int high = hexValue(value[index + 1]);
      const int low = hexValue(value[index + 2]);
      if (high >= 0 && low >= 0) {
        decoded += char((high << 4) | low);
        index += 2;
      }
    } else if (character == '+') {
      decoded += ' ';
    } else {
      decoded += character;
    }
  }

  return decoded;
}

String xmlEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length());

  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character == '&') escaped += F("&amp;");
    else if (character == '<') escaped += F("&lt;");
    else if (character == '>') escaped += F("&gt;");
    else if (character == '\"') escaped += F("&quot;");
    else escaped += character;
  }

  return escaped;
}

String safePath(String requestPath) {
  const int queryStart = requestPath.indexOf('?');
  if (queryStart >= 0) requestPath = requestPath.substring(0, queryStart);
  requestPath = urlDecode(requestPath);
  requestPath.replace("\\", "/");

  while (requestPath.startsWith("/")) requestPath = requestPath.substring(1);

  String cleaned;
  int segmentStart = 0;
  while (segmentStart < requestPath.length()) {
    int segmentEnd = requestPath.indexOf('/', segmentStart);
    if (segmentEnd < 0) segmentEnd = requestPath.length();
    const String segment = requestPath.substring(segmentStart, segmentEnd);

    if (!segment.isEmpty() && segment != "." && segment != "..") {
      String cleanSegment;
      for (size_t index = 0; index < segment.length(); ++index) {
        const char character = segment[index];
        if (isalnum(character) || character == '.' || character == '-' || character == '_' || character == ' ') {
          cleanSegment += character;
        }
      }
      if (!cleanSegment.isEmpty()) {
        if (!cleaned.isEmpty()) cleaned += '/';
        cleaned += cleanSegment;
      }
    }

    segmentStart = segmentEnd + 1;
  }

  return cleaned;
}

String storageName(const String& path) {
  return path.isEmpty() ? "/" : "/" + path;
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

bool deleteRecursive(const String& path) {
  File target = storage->open(storageName(path));
  if (!target) return false;

  if (!target.isDirectory()) {
    target.close();
    return storage->remove(storageName(path));
  }

  File entry = target.openNextFile();
  while (entry) {
    String childName = baseName(String(entry.name()));
    String childPath = path.isEmpty() ? childName : path + "/" + childName;
    bool childIsDirectory = entry.isDirectory();
    entry.close();

    if (childIsDirectory) {
      if (!deleteRecursive(childPath)) {
        target.close();
        return false;
      }
    } else if (!storage->remove(storageName(childPath))) {
      target.close();
      return false;
    }

    entry = target.openNextFile();
  }

  target.close();
  return path.isEmpty() || storage->rmdir(storageName(path)) || storage->remove(storageName(path));
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
  if (lowerName.endsWith(".svg")) return "image/svg+xml";
  if (lowerName.endsWith(".pdf")) return "application/pdf";
  return "application/octet-stream";
}

const char* listingIconFor(const String& fileName, bool isDirectory) {
  if (isDirectory) return "&#128193;";

  String lowerName = fileName;
  lowerName.toLowerCase();
  if (lowerName.endsWith(".jpg") || lowerName.endsWith(".jpeg") || lowerName.endsWith(".png") || lowerName.endsWith(".gif") || lowerName.endsWith(".webp") || lowerName.endsWith(".svg")) return "&#128444;";
  if (lowerName.endsWith(".mp3") || lowerName.endsWith(".wav") || lowerName.endsWith(".ogg") || lowerName.endsWith(".m4a")) return "&#127925;";
  if (lowerName.endsWith(".mp4") || lowerName.endsWith(".mov") || lowerName.endsWith(".avi") || lowerName.endsWith(".mkv") || lowerName.endsWith(".webm")) return "&#127909;";
  if (lowerName.endsWith(".zip") || lowerName.endsWith(".rar") || lowerName.endsWith(".7z") || lowerName.endsWith(".gz") || lowerName.endsWith(".tar")) return "&#128230;";
  if (lowerName.endsWith(".html") || lowerName.endsWith(".htm") || lowerName.endsWith(".css") || lowerName.endsWith(".js") || lowerName.endsWith(".json") || lowerName.endsWith(".ino") || lowerName.endsWith(".cpp") || lowerName.endsWith(".h")) return "&#128187;";
  if (lowerName.endsWith(".txt") || lowerName.endsWith(".md") || lowerName.endsWith(".pdf") || lowerName.endsWith(".doc") || lowerName.endsWith(".docx")) return "&#128196;";
  return "&#128196;";
}

void sendResponse(WiFiClient& client, int code, const String& status, const String& contentType, const String& body, const String& extraHeaders = "") {
  client.print("HTTP/1.1 ");
  client.print(code);
  client.print(' ');
  client.print(status);
  client.print("\r\nConnection: close\r\nContent-Length: ");
  client.print(body.length());
  client.print("\r\nContent-Type: ");
  client.print(contentType);
  client.print("\r\n");
  if (!extraHeaders.isEmpty()) client.print(extraHeaders);
  client.print("\r\n");
  client.print(body);
}

void sendEmptyResponse(WiFiClient& client, int code, const String& status, const String& extraHeaders = "") {
  client.print("HTTP/1.1 ");
  client.print(code);
  client.print(' ');
  client.print(status);
  client.print("\r\nConnection: close\r\nContent-Length: 0\r\n");
  if (!extraHeaders.isEmpty()) client.print(extraHeaders);
  client.print("\r\n");
}

bool readRequest(WiFiClient& client, HttpRequest& request) {
  client.setTimeout(5000);
  String requestLine = client.readStringUntil('\n');
  requestLine.trim();
  if (requestLine.isEmpty()) return false;

  const int firstSpace = requestLine.indexOf(' ');
  const int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  if (firstSpace < 0 || secondSpace < 0) return false;

  request.method = requestLine.substring(0, firstSpace);
  request.path = safePath(requestLine.substring(firstSpace + 1, secondSpace));
  request.valid = true;

  while (client.connected()) {
    String headerLine = client.readStringUntil('\n');
    headerLine.trim();
    if (headerLine.isEmpty()) break;

    const int colon = headerLine.indexOf(':');
    if (colon < 0) continue;

    String name = headerLine.substring(0, colon);
    String value = headerLine.substring(colon + 1);
    name.toLowerCase();
    value.trim();

    if (name == "content-length") request.contentLength = value.toInt();
    else if (name == "depth") request.depth = value;
    else if (name == "authorization") request.authorization = value;
  }

  return request.valid;
}

bool isAuthorized(const HttpRequest& request) {
  return !requireAuthentication || request.authorization == webDavBasicAuth;
}

void sendUnauthorized(WiFiClient& client) {
  sendResponse(client, 401, "Unauthorized", "text/plain", "WebDAV login required\n", "WWW-Authenticate: Basic realm=\"CYD WebDAV\"\r\n");
}

String webDavHref(const String& path, bool isDirectory) {
  String href = "/" + path;
  href.replace(" ", "%20");
  if (isDirectory && !href.endsWith("/")) href += "/";
  return href;
}

void appendPropResponse(String& xml, const String& path, bool isDirectory, size_t size) {
  xml += F("<d:response><d:href>");
  xml += xmlEscape(webDavHref(path, isDirectory));
  xml += F("</d:href><d:propstat><d:prop><d:resourcetype>");
  if (isDirectory) xml += F("<d:collection/>");
  xml += F("</d:resourcetype>");

  if (!isDirectory) {
    xml += F("<d:getcontentlength>");
    xml += String(size);
    xml += F("</d:getcontentlength><d:getcontenttype>");
    xml += contentTypeFor(path);
    xml += F("</d:getcontenttype>");
  }

  xml += F("</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>");
}

void handleOptions(WiFiClient& client) {
  sendEmptyResponse(client, 200, "OK", "DAV: 1\r\nAllow: OPTIONS, PROPFIND, GET, HEAD, PUT, MKCOL, DELETE, LOCK, UNLOCK\r\n");
}

void handlePropfind(WiFiClient& client, const HttpRequest& request) {
  const String targetPath = request.path;
  File target = storage->open(storageName(targetPath));
  if (!target) {
    sendResponse(client, 404, "Not Found", "text/plain", "Not found\n");
    return;
  }

  const bool targetIsDirectory = target.isDirectory();
  String xml = F("<?xml version=\"1.0\" encoding=\"utf-8\"?><d:multistatus xmlns:d=\"DAV:\">");
  appendPropResponse(xml, targetPath, targetIsDirectory, target.size());

  if (targetIsDirectory && request.depth != "0") {
    File entry = target.openNextFile();
    while (entry) {
      const String childName = baseName(String(entry.name()));
      const String childPath = targetPath.isEmpty() ? childName : targetPath + "/" + childName;
      appendPropResponse(xml, childPath, entry.isDirectory(), entry.size());
      entry.close();
      entry = target.openNextFile();
    }
  }

  target.close();
  xml += F("</d:multistatus>");
  sendResponse(client, 207, "Multi-Status", "application/xml; charset=utf-8", xml);
}

void handleGet(WiFiClient& client, const HttpRequest& request, bool headersOnly) {
  const String path = request.path;
  File target = storage->open(storageName(path), FILE_READ);
  if (!target) {
    sendResponse(client, 404, "Not Found", "text/plain", "Not found\n");
    return;
  }

  if (target.isDirectory()) {
    String page = F("<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>CYD WebDAV</title><style>body{font-family:system-ui,sans-serif;max-width:760px;margin:0 auto;padding:24px;background:#eef3f5;color:#17202a}main{background:white;border:1px solid #d7e0e4;border-radius:8px;padding:24px}a{color:#176b87;overflow-wrap:anywhere}li{display:flex;justify-content:space-between;gap:10px;align-items:center;margin:10px 0}.icon{display:inline-block;width:1.5em;text-align:center}.meta{color:#63727a}.delete{background:#a43d3d;color:#fff;border:0;border-radius:4px;padding:6px 10px;cursor:pointer}</style></head><body><main><h1>CYD WebDAV</h1><p>WebDAV: <strong>/</strong></p><p>User: <strong>cyd</strong> Password: <strong>upload</strong></p><ul>");
    File entry = target.openNextFile();
    while (entry) {
      const String childName = baseName(String(entry.name()));
      const String childPath = path.isEmpty() ? childName : path + "/" + childName;
      const String childHref = webDavHref(childPath, entry.isDirectory());
      page += F("<li><div><a href=\"");
      page += childHref;
      page += F("\"><span class=\"icon\" aria-hidden=\"true\">");
      page += listingIconFor(childName, entry.isDirectory());
      page += F("</span>");
      page += xmlEscape(childName);
      if (entry.isDirectory()) page += F("/");
      page += F("</a>");
      if (!entry.isDirectory()) {
        page += F(" <span class=\"meta\">");
        page += String(entry.size());
        page += F(" bytes</span>");
      }
      page += F("</div><button class=\"delete\" type=\"button\" data-path=\"");
      page += xmlEscape(childHref);
      page += F("\">Delete</button></li>");
      entry.close();
      entry = target.openNextFile();
    }
    target.close();
    page += F("<script>document.querySelectorAll('.delete').forEach((button) => { button.addEventListener('click', async () => { const path = button.dataset.path; if (!confirm('Delete ' + decodeURIComponent(path) + '?')) return; const response = await fetch(path, { method: 'DELETE' }); if (response.ok || response.status === 204) { location.reload(); } else { alert('Delete failed: ' + response.status); } }); });</script>");
    page += F("</ul></main></body></html>");
    if (headersOnly) sendEmptyResponse(client, 200, "OK", "Content-Type: text/html; charset=utf-8\r\n");
    else sendResponse(client, 200, "OK", "text/html; charset=utf-8", page);
    return;
  }

  client.print("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: ");
  client.print(target.size());
  client.print("\r\nContent-Type: ");
  client.print(contentTypeFor(path));
  client.print("\r\n\r\n");

  if (!headersOnly) {
    uint8_t buffer[transferBufferSize];
    while (target.available()) {
      const int bytesRead = target.read(buffer, sizeof(buffer));
      if (bytesRead > 0) client.write(buffer, bytesRead);
    }
  }
  target.close();
}

bool readBodyToFile(WiFiClient& client, File& file, size_t contentLength) {
  uint8_t buffer[transferBufferSize];
  size_t remaining = contentLength;
  const unsigned long started = millis();

  while (remaining > 0) {
    const size_t nextRead = min(remaining, sizeof(buffer));
    int bytesRead = client.read(buffer, nextRead);
    if (bytesRead > 0) {
      file.write(buffer, bytesRead);
      remaining -= bytesRead;
    } else if (!client.connected() || millis() - started > 30000) {
      return false;
    } else {
      delay(1);
    }
  }

  file.flush();
  return true;
}

void handlePut(WiFiClient& client, const HttpRequest& request) {
  if (request.path.isEmpty()) {
    sendResponse(client, 400, "Bad Request", "text/plain", "PUT requires a file path\n");
    return;
  }

  createParentDirectories(request.path);
  const bool existed = storage->exists(storageName(request.path));
  File file = storage->open(storageName(request.path), FILE_WRITE);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    sendResponse(client, 500, "Internal Server Error", "text/plain", "Could not open destination file\n");
    return;
  }

  if (!readBodyToFile(client, file, request.contentLength)) {
    file.close();
    sendResponse(client, 408, "Request Timeout", "text/plain", "Upload timed out\n");
    return;
  }
  file.close();

  logEvent("PUT " + request.path + " " + String(request.contentLength) + "B");
  sendEmptyResponse(client, existed ? 204 : 201, existed ? "No Content" : "Created");
}

void handleMkcol(WiFiClient& client, const HttpRequest& request) {
  if (request.path.isEmpty()) {
    sendResponse(client, 405, "Method Not Allowed", "text/plain", "Root already exists\n");
    return;
  }

  if (storage->exists(storageName(request.path))) {
    sendResponse(client, 405, "Method Not Allowed", "text/plain", "Path already exists\n");
    return;
  }

  createParentDirectories(request.path);
  if (!storage->mkdir(storageName(request.path))) {
    sendResponse(client, 409, "Conflict", "text/plain", "Could not create folder\n");
    return;
  }

  logEvent("MKCOL " + request.path);
  sendEmptyResponse(client, 201, "Created");
}

void handleDelete(WiFiClient& client, const HttpRequest& request) {
  if (request.path.isEmpty()) {
    sendResponse(client, 405, "Method Not Allowed", "text/plain", "Deleting root is disabled\n");
    return;
  }

  if (!storage->exists(storageName(request.path))) {
    sendResponse(client, 404, "Not Found", "text/plain", "Not found\n");
    return;
  }

  if (!deleteRecursive(request.path)) {
    sendResponse(client, 500, "Internal Server Error", "text/plain", "Delete failed\n");
    return;
  }

  logEvent("DELETE " + request.path);
  sendEmptyResponse(client, 204, "No Content");
}

void handleClient(WiFiClient& client) {
  HttpRequest request;
  if (!readRequest(client, request)) return;

  if (!isAuthorized(request)) {
    sendUnauthorized(client);
    return;
  }

  if (request.method == "OPTIONS") handleOptions(client);
  else if (request.method == "PROPFIND") handlePropfind(client, request);
  else if (request.method == "GET") handleGet(client, request, false);
  else if (request.method == "HEAD") handleGet(client, request, true);
  else if (request.method == "PUT") handlePut(client, request);
  else if (request.method == "MKCOL") handleMkcol(client, request);
  else if (request.method == "DELETE") handleDelete(client, request);
  else if (request.method == "LOCK") sendResponse(client, 200, "OK", "application/xml; charset=utf-8", "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:prop xmlns:d=\"DAV:\"><d:lockdiscovery/></d:prop>");
  else if (request.method == "UNLOCK") sendEmptyResponse(client, 204, "No Content");
  else sendResponse(client, 405, "Method Not Allowed", "text/plain", "Method not supported\n", "Allow: OPTIONS, PROPFIND, GET, HEAD, PUT, MKCOL, DELETE, LOCK, UNLOCK\r\n");
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
  serverAddress = WiFi.localIP();
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
    WiFi.softAP("CYD-WebDAV", "cydupload");
    serverAddress = WiFi.softAPIP();
    logEvent("AP CYD-WebDAV");
    logEvent("AP IP " + WiFi.softAPIP().toString());
    logEvent("AP password: cydupload");
  }

  server.begin();
  logEvent("WebDAV http://" + serverAddress.toString());
  logEvent(String("Login ") + webDavUser + "/" + webDavPassword);
}

void loop() {
  WiFiClient client = server.available();
  if (client) {
    handleClient(client);
    client.stop();
  }
}