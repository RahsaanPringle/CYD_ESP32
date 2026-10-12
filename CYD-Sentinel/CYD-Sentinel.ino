#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <SD.h>
#include <TFT_eSPI.h>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#else
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

constexpr char AccessPointName[] = "CYD-IoT-Monitor";
constexpr char AccessPointPassword[] = "cydmesh1";
constexpr uint8_t BacklightPin = 21;
constexpr uint8_t SdChipSelectPin = 5;
constexpr char EventLogPath[] = "/sentinel-events.ndjson";
constexpr uint16_t DashboardPort = 8080;
constexpr uint16_t HttpSinkholePort = 80;
constexpr uint16_t DnsPort = 53;
constexpr size_t MaxDevices = 16;
constexpr size_t MaxTrafficEvents = 100;
constexpr size_t MaxCallbackEvents = 16;
constexpr uint8_t DisplayLogLines = 7;
constexpr uint32_t DuplicateWindowMs = 1000;
constexpr uint8_t SdFlushEventCount = 5;
constexpr uint32_t SdFlushIntervalMs = 5000;

enum class EventType : uint8_t {
  DeviceConnected, DeviceDisconnected, DnsQuery, HttpRequest, System
};

struct DeviceRecord {
  uint8_t mac[6] = {};
  IPAddress ip;
  char label[25] = {};
  unsigned long connectedAt = 0;
  unsigned long lastSeenAt = 0;
  uint32_t requestCount = 0;
  bool connected = false;
  bool used = false;
};

struct TrafficEvent {
  uint32_t sequence = 0;
  unsigned long occurredAt = 0;
  EventType type = EventType::System;
  IPAddress sourceIp;
  IPAddress destinationIp;
  uint16_t sourcePort = 0;
  uint16_t destinationPort = 0;
  char hostname[65] = {};
  char method[9] = {};
  char path[97] = {};
  uint32_t byteCount = 0;
};

struct CallbackEvent {
  bool connected = false;
  uint8_t mac[6] = {};
};

TFT_eSPI display;
WiFiServer dashboardServer(DashboardPort);
WiFiServer httpSinkhole(HttpSinkholePort);
WiFiUDP dnsServer;
File eventLogFile;
DeviceRecord devices[MaxDevices];
TrafficEvent trafficEvents[MaxTrafficEvents];
CallbackEvent callbackEvents[MaxCallbackEvents];
portMUX_TYPE callbackLock = portMUX_INITIALIZER_UNLOCKED;
volatile size_t callbackEventHead = 0;
volatile size_t callbackEventTail = 0;
uint32_t trafficSequence = 0;
size_t trafficEventCount = 0;
size_t nextTrafficEvent = 0;
String displayLines[DisplayLogLines];
bool displayUpdatesPaused = false;
char lastEventSignature[180] = {};
uint32_t lastEventAt = 0;
bool sdAvailable = false;
uint8_t pendingSdEvents = 0;
uint32_t lastSdFlushAt = 0;

const char* eventTypeName(EventType type) {
  switch (type) {
    case EventType::DeviceConnected: return "connected";
    case EventType::DeviceDisconnected: return "disconnected";
    case EventType::DnsQuery: return "dns";
    case EventType::HttpRequest: return "http";
    case EventType::System: return "system";
  }
  return "unknown";
}

String macAddress(const uint8_t mac[6]) {
  char value[18];
  snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return value;
}

String jsonEscape(const char* value) {
  String escaped;
  for (; *value; ++value) {
    if (*value == '"' || *value == '\\') escaped += '\\';
    if (static_cast<uint8_t>(*value) >= 32) escaped += *value;
  }
  return escaped;
}

String urlDecode(const String& value) {
  String decoded;
  decoded.reserve(value.length());
  for (size_t index = 0; index < value.length(); ++index) {
    if (value[index] == '+' ) {
      decoded += ' ';
    } else if (value[index] == '%' && index + 2 < value.length()) {
      char hex[3] = {value[index + 1], value[index + 2], '\0'};
      decoded += static_cast<char>(strtoul(hex, nullptr, 16));
      index += 2;
    } else {
      decoded += value[index];
    }
  }
  return decoded;
}

String trafficEventJson(const TrafficEvent& event) {
  String json;
  json.reserve(260);
  json += "{\"sequence\":" + String(event.sequence);
  json += ",\"time\":" + String(event.occurredAt / 1000);
  json += ",\"type\":\"" + String(eventTypeName(event.type));
  json += "\",\"source\":\"" + event.sourceIp.toString();
  json += "\",\"sourcePort\":" + String(event.sourcePort);
  json += ",\"destinationPort\":" + String(event.destinationPort);
  json += ",\"host\":\"" + jsonEscape(event.hostname);
  json += "\",\"method\":\"" + jsonEscape(event.method);
  json += "\",\"path\":\"" + jsonEscape(event.path);
  json += "\",\"bytes\":" + String(event.byteCount) + "}";
  return json;
}

void closeSdLog() {
  if (eventLogFile) eventLogFile.close();
  sdAvailable = false;
  pendingSdEvents = 0;
}

void appendSdEvent(const TrafficEvent& event) {
  if (!sdAvailable || !eventLogFile) return;
  const String line = trafficEventJson(event);
  if (eventLogFile.println(line) == 0) {
    closeSdLog();
    addDisplayLine("SD write failed");
    return;
  }
  ++pendingSdEvents;
}

void flushSdLog(bool force = false) {
  if (!sdAvailable || !eventLogFile) return;
  if (!force && pendingSdEvents < SdFlushEventCount && millis() - lastSdFlushAt < SdFlushIntervalMs) return;
  eventLogFile.flush();
  pendingSdEvents = 0;
  lastSdFlushAt = millis();
}

void initializeSdLog() {
  if (!SD.begin(SdChipSelectPin)) {
    addDisplayLine("SD not present");
    return;
  }
  eventLogFile = SD.open(EventLogPath, FILE_APPEND);
  if (!eventLogFile) {
    addDisplayLine("SD log unavailable");
    return;
  }
  sdAvailable = true;
  lastSdFlushAt = millis();
  addDisplayLine("SD logging enabled");
}

void redrawDisplay() {
  if (displayUpdatesPaused) return;
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.print("IoT Monitor");
  display.drawFastHLine(8, 28, 304, TFT_DARKGREY);
  display.setTextSize(1);
  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setCursor(8, 38);
  display.printf("%s  %u device%s", WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum(), WiFi.softAPgetStationNum() == 1 ? "" : "s");
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  for (uint8_t index = 0; index < DisplayLogLines; ++index) {
    display.setCursor(8, 58 + index * 24);
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

int findDeviceByIp(const IPAddress& ip) {
  for (size_t index = 0; index < MaxDevices; ++index) {
    if (devices[index].used && devices[index].ip == ip) return index;
  }
  return -1;
}

int findDeviceByMac(const uint8_t mac[6]) {
  for (size_t index = 0; index < MaxDevices; ++index) {
    if (devices[index].used && memcmp(devices[index].mac, mac, 6) == 0) return index;
  }
  return -1;
}

int findOrCreateDevice(const uint8_t mac[6]) {
  int deviceIndex = findDeviceByMac(mac);
  if (deviceIndex >= 0) return deviceIndex;
  for (size_t index = 0; index < MaxDevices; ++index) {
    if (!devices[index].used) {
      devices[index].used = true;
      memcpy(devices[index].mac, mac, 6);
      return static_cast<int>(index);
    }
  }
  return 0;
}

void touchDevice(const IPAddress& ip) {
  int deviceIndex = findDeviceByIp(ip);
  if (deviceIndex < 0) {
    for (size_t index = 0; index < MaxDevices; ++index) {
      if (devices[index].used && devices[index].connected && devices[index].ip == IPAddress(0, 0, 0, 0)) {
        devices[index].ip = ip;
        deviceIndex = static_cast<int>(index);
        break;
      }
    }
  }
  if (deviceIndex >= 0) {
    devices[deviceIndex].lastSeenAt = millis();
    ++devices[deviceIndex].requestCount;
  }
}

bool isDuplicate(const TrafficEvent& event) {
  char signature[sizeof(lastEventSignature)];
  snprintf(signature, sizeof(signature), "%u|%s|%s|%s|%u", static_cast<unsigned>(event.type), event.sourceIp.toString().c_str(), event.hostname, event.path, event.destinationPort);
  const bool duplicate = strcmp(signature, lastEventSignature) == 0 && millis() - lastEventAt < DuplicateWindowMs;
  snprintf(lastEventSignature, sizeof(lastEventSignature), "%s", signature);
  lastEventAt = millis();
  return duplicate;
}

void logTraffic(TrafficEvent event) {
  event.sequence = ++trafficSequence;
  event.occurredAt = millis();
  touchDevice(event.sourceIp);
  if (isDuplicate(event)) return;
  trafficEvents[nextTrafficEvent] = event;
  nextTrafficEvent = (nextTrafficEvent + 1) % MaxTrafficEvents;
  if (trafficEventCount < MaxTrafficEvents) ++trafficEventCount;
  appendSdEvent(event);
  String description = String(eventTypeName(event.type)) + " " + event.sourceIp.toString();
  if (event.hostname[0]) description += " " + String(event.hostname);
  else if (event.path[0]) description += " " + String(event.path);
  addDisplayLine(description);
}

void enqueueStationEvent(bool connected, const uint8_t mac[6]) {
  portENTER_CRITICAL(&callbackLock);
  const size_t nextHead = (callbackEventHead + 1) % MaxCallbackEvents;
  if (nextHead != callbackEventTail) {
    callbackEvents[callbackEventHead].connected = connected;
    memcpy(callbackEvents[callbackEventHead].mac, mac, 6);
    callbackEventHead = nextHead;
  }
  portEXIT_CRITICAL(&callbackLock);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) enqueueStationEvent(true, info.wifi_ap_staconnected.mac);
  if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) enqueueStationEvent(false, info.wifi_ap_stadisconnected.mac);
}
#else
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == SYSTEM_EVENT_AP_STACONNECTED) enqueueStationEvent(true, info.wifi_ap_staconnected.mac);
  if (event == SYSTEM_EVENT_AP_STADISCONNECTED) enqueueStationEvent(false, info.wifi_ap_stadisconnected.mac);
}
#endif

void processStationEvents() {
  while (true) {
    CallbackEvent callbackEvent;
    bool hasEvent = false;
    portENTER_CRITICAL(&callbackLock);
    if (callbackEventTail != callbackEventHead) {
      callbackEvent = callbackEvents[callbackEventTail];
      callbackEventTail = (callbackEventTail + 1) % MaxCallbackEvents;
      hasEvent = true;
    }
    portEXIT_CRITICAL(&callbackLock);
    if (!hasEvent) return;

    const int deviceIndex = findOrCreateDevice(callbackEvent.mac);
    DeviceRecord& device = devices[deviceIndex];
    device.connected = callbackEvent.connected;
    device.lastSeenAt = millis();
    if (callbackEvent.connected) device.connectedAt = millis();

    TrafficEvent event;
    event.type = callbackEvent.connected ? EventType::DeviceConnected : EventType::DeviceDisconnected;
    logTraffic(event);
    addDisplayLine(macAddress(callbackEvent.mac) + (callbackEvent.connected ? " joined" : " left"));
  }
}

bool readDnsQuestion(uint8_t* packet, size_t length, char hostname[65], size_t& questionEnd, uint16_t& questionType) {
  if (length < 17) return false;
  size_t offset = 12;
  size_t output = 0;
  while (offset < length && packet[offset] != 0) {
    const uint8_t labelLength = packet[offset++];
    if (labelLength == 0 || labelLength > 63 || offset + labelLength > length) return false;
    if (output && output < 64) hostname[output++] = '.';
    for (uint8_t index = 0; index < labelLength && output < 64; ++index) hostname[output++] = static_cast<char>(packet[offset++]);
    if (offset + labelLength > length) return false;
  }
  if (offset + 5 > length) return false;
  hostname[output] = '\0';
  ++offset;
  questionType = (static_cast<uint16_t>(packet[offset]) << 8) | packet[offset + 1];
  questionEnd = offset + 4;
  return true;
}

void handleDns() {
  const int packetLength = dnsServer.parsePacket();
  if (packetLength <= 0) return;
  uint8_t packet[512];
  const int length = dnsServer.read(packet, min(packetLength, static_cast<int>(sizeof(packet))));
  char hostname[65] = {};
  size_t questionEnd = 0;
  uint16_t questionType = 0;
  if (length < 0 || !readDnsQuestion(packet, length, hostname, questionEnd, questionType)) return;

  TrafficEvent event;
  event.type = EventType::DnsQuery;
  event.sourceIp = dnsServer.remoteIP();
  event.destinationIp = WiFi.softAPIP();
  event.sourcePort = dnsServer.remotePort();
  event.destinationPort = DnsPort;
  snprintf(event.hostname, sizeof(event.hostname), "%s", hostname);
  logTraffic(event);

  const bool isARecord = questionType == 1;
  uint8_t response[512];
  if (questionEnd + (isARecord ? 16 : 0) > sizeof(response)) return;
  memcpy(response, packet, questionEnd);
  response[2] = 0x81;
  response[3] = isARecord ? 0x80 : 0x84;
  response[6] = 0;
  response[7] = isARecord ? 1 : 0;
  response[8] = response[9] = response[10] = response[11] = 0;
  size_t responseLength = questionEnd;
  if (isARecord) {
    response[responseLength++] = 0xC0;
    response[responseLength++] = 0x0C;
    response[responseLength++] = 0;
    response[responseLength++] = 1;
    response[responseLength++] = 0;
    response[responseLength++] = 1;
    response[responseLength++] = 0;
    response[responseLength++] = 0;
    response[responseLength++] = 0;
    response[responseLength++] = 30;
    response[responseLength++] = 0;
    response[responseLength++] = 4;
    const IPAddress address = WiFi.softAPIP();
    for (uint8_t index = 0; index < 4; ++index) response[responseLength++] = address[index];
  }
  dnsServer.beginPacket(dnsServer.remoteIP(), dnsServer.remotePort());
  dnsServer.write(response, responseLength);
  dnsServer.endPacket();
}

String readHttpLine(WiFiClient& client) {
  String line = client.readStringUntil('\n');
  line.trim();
  return line;
}

void handleHttpSinkhole() {
  WiFiClient client = httpSinkhole.available();
  if (!client) return;
  client.setTimeout(800);
  const String requestLine = readHttpLine(client);
  if (requestLine.isEmpty()) {
    client.stop();
    return;
  }
  const int firstSpace = requestLine.indexOf(' ');
  const int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  const String method = firstSpace > 0 ? requestLine.substring(0, firstSpace) : "UNKNOWN";
  const String path = firstSpace > 0 && secondSpace > firstSpace ? requestLine.substring(firstSpace + 1, secondSpace) : "/";
  String host;
  uint32_t contentLength = 0;
  while (client.connected()) {
    const String header = readHttpLine(client);
    if (header.isEmpty()) break;
    if (header.startsWith("Host:")) {
      host = header.substring(5);
      host.trim();
    } else if (header.startsWith("Content-Length:")) {
      contentLength = header.substring(15).toInt();
    }
  }
  TrafficEvent event;
  event.type = EventType::HttpRequest;
  event.sourceIp = client.remoteIP();
  event.destinationIp = WiFi.softAPIP();
  event.sourcePort = client.remotePort();
  event.destinationPort = HttpSinkholePort;
  event.byteCount = contentLength;
  snprintf(event.hostname, sizeof(event.hostname), "%s", host.c_str());
  snprintf(event.method, sizeof(event.method), "%s", method.c_str());
  snprintf(event.path, sizeof(event.path), "%s", path.c_str());
  logTraffic(event);
  client.print("HTTP/1.1 204 No Content\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  client.stop();
}

void writeJsonEvent(WiFiClient& client, const TrafficEvent& event) {
  client.print(trafficEventJson(event));
}

void sendResponse(WiFiClient& client, const char* contentType, const String& body, int status = 200, const char* statusText = "OK") {
  client.printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", status, statusText, contentType, static_cast<unsigned>(body.length()));
  client.print(body);
}

String dashboardPage() {
  return F(R"HTML(<!doctype html><html lang="en"><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>CYD IoT Monitor</title><style>body{margin:0;background:#eef2ed;color:#17231e;font:16px Georgia,serif}main{max-width:980px;margin:auto;padding:24px}h1{margin:0;color:#a83b20;font-size:2rem}header{border-bottom:3px solid #d5a13c;padding-bottom:16px}section{margin-top:24px}h2{font-size:1.2rem;margin-bottom:8px}table{width:100%;border-collapse:collapse;background:#fff}th,td{padding:9px;text-align:left;border-bottom:1px solid #dce2da;vertical-align:top}th{background:#234a3b;color:#fff}code{font-family:Consolas,monospace}button,input{font:inherit;padding:7px;border:1px solid #79897c}button{background:#d5a13c;color:#251805;cursor:pointer}#status{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}#status div{background:#fff;padding:10px;border-left:4px solid #d5a13c}@media(max-width:620px){main{padding:14px}table{font-size:.82rem}th,td{padding:6px}}</style></head><body><main><header><h1>CYD IoT Monitor</h1><p>Isolated network activity</p></header><section id="status"></section><section><h2>Devices</h2><table><thead><tr><th>Label / MAC</th><th>IP</th><th>State</th><th>Requests</th><th>Label</th></tr></thead><tbody id="devices"></tbody></table></section><section><h2>Recent Activity</h2><button onclick="clearEvents()">Clear RAM log</button><table><thead><tr><th>Time</th><th>Source</th><th>Type</th><th>Destination</th><th>Detail</th></tr></thead><tbody id="events"></tbody></table></section></main><script>let last=0;const text=v=>document.createTextNode(v);function cell(r,v){let d=document.createElement('td');d.append(text(v));r.append(d)}async function refresh(){let s=await (await fetch('/api/status')).json();let st=document.querySelector('#status');st.replaceChildren();for(let [k,v] of Object.entries(s)){let d=document.createElement('div');d.append(text(k+': '+v));st.append(d)}let ds=await (await fetch('/api/devices')).json(),db=document.querySelector('#devices');db.replaceChildren();ds.forEach(d=>{let r=document.createElement('tr');cell(r,(d.label?d.label+' / ':'')+d.mac);cell(r,d.ip||'-');cell(r,d.connected?'connected':'recent');cell(r,d.requests);let c=document.createElement('td'),i=document.createElement('input'),b=document.createElement('button');i.value=d.label;i.maxLength=24;b.textContent='Save';b.onclick=async()=>{await fetch('/api/device-label',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({mac:d.mac,label:i.value})});refresh()};c.append(i,b);r.append(c);db.append(r)});let es=await (await fetch('/api/events?after='+last)).json(),eb=document.querySelector('#events');es.forEach(e=>{last=Math.max(last,e.sequence);let r=document.createElement('tr');cell(r,e.time+'s');cell(r,e.source+(e.sourcePort?':'+e.sourcePort:''));cell(r,e.type);cell(r,e.host||('port '+e.destinationPort));cell(r,[e.method,e.path,e.bytes?'('+e.bytes+' bytes)':''].filter(Boolean).join(' '));eb.prepend(r)});while(eb.rows.length>100)eb.deleteRow(-1)}async function clearEvents(){await fetch('/api/clear',{method:'POST'});last=0;document.querySelector('#events').replaceChildren()}refresh();setInterval(refresh,2000)</script></body></html>)HTML");
}

void handleDashboard() {
  WiFiClient client = dashboardServer.available();
  if (!client) return;
  client.setTimeout(1000);
  const String requestLine = readHttpLine(client);
  if (requestLine.isEmpty()) { client.stop(); return; }
  const int firstSpace = requestLine.indexOf(' ');
  const int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  const String method = requestLine.substring(0, firstSpace);
  String target = requestLine.substring(firstSpace + 1, secondSpace);
  uint32_t contentLength = 0;
  while (client.connected()) {
    const String header = readHttpLine(client);
    if (header.isEmpty()) break;
    if (header.startsWith("Content-Length:")) contentLength = header.substring(15).toInt();
  }
  String requestBody;
  if (contentLength > 0 && contentLength <= 128) {
    requestBody.reserve(contentLength);
    while (requestBody.length() < contentLength && client.connected()) requestBody += static_cast<char>(client.read());
  }
  const int queryIndex = target.indexOf('?');
  const String path = queryIndex < 0 ? target : target.substring(0, queryIndex);
  const String query = queryIndex < 0 ? "" : target.substring(queryIndex + 1);
  if (path == "/" || path == "/dashboard") sendResponse(client, "text/html; charset=utf-8", dashboardPage());
  else if (path == "/api/status") {
    String body = "{\"uptime\":\"" + String(millis() / 1000) + "s\",\"ap\":\"" + WiFi.softAPIP().toString() + "\",\"connected\":" + String(WiFi.softAPgetStationNum()) + ",\"events\":" + String(trafficEventCount) + ",\"sd\":\"" + String(sdAvailable ? "logging" : "not available") + "\",\"freeHeap\":" + String(ESP.getFreeHeap()) + "}";
    sendResponse(client, "application/json", body);
  } else if (path == "/api/devices") {
    String body = "[";
    for (size_t index = 0; index < MaxDevices; ++index) if (devices[index].used) {
      if (body.length() > 1) body += ',';
      body += "{\"mac\":\"" + macAddress(devices[index].mac) + "\",\"ip\":\"" + devices[index].ip.toString() + "\",\"label\":\"" + jsonEscape(devices[index].label) + "\",\"connected\":" + String(devices[index].connected ? "true" : "false") + ",\"requests\":" + String(devices[index].requestCount) + "}";
    }
    sendResponse(client, "application/json", body + "]");
  } else if (path == "/api/events") {
    const int afterIndex = query.indexOf("after=");
    const uint32_t after = afterIndex < 0 ? 0 : query.substring(afterIndex + 6).toInt();
    client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n[");
    bool first = true;
    const size_t firstEvent = trafficEventCount == MaxTrafficEvents ? nextTrafficEvent : 0;
    for (size_t index = 0; index < trafficEventCount; ++index) {
      const TrafficEvent& event = trafficEvents[(firstEvent + index) % MaxTrafficEvents];
      if (event.sequence <= after) continue;
      if (!first) client.print(',');
      writeJsonEvent(client, event);
      first = false;
    }
    client.print(']');
  } else if (path == "/api/clear" && method == "POST") {
    trafficEventCount = 0;
    nextTrafficEvent = 0;
    sendResponse(client, "application/json", "{\"cleared\":true}");
  } else if (path == "/api/device-label" && method == "POST") {
    const String form = requestBody.isEmpty() ? query : requestBody;
    const int macStart = form.indexOf("mac=");
    const int labelStart = form.indexOf("label=");
    if (macStart >= 0 && labelStart >= 0) {
      const int macEnd = form.indexOf('&', macStart);
      const String mac = urlDecode(form.substring(macStart + 4, macEnd < 0 ? form.length() : macEnd));
      const String label = urlDecode(form.substring(labelStart + 6));
      for (size_t index = 0; index < MaxDevices; ++index) if (devices[index].used && macAddress(devices[index].mac) == mac) snprintf(devices[index].label, sizeof(devices[index].label), "%s", label.c_str());
    }
    sendResponse(client, "application/json", "{\"saved\":true}");
  } else sendResponse(client, "application/json", "{\"error\":\"Not found\"}", 404, "Not Found");
  client.stop();
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  const IPAddress address(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(address, address, subnet);
  WiFi.softAP(AccessPointName, AccessPointPassword);
  WiFi.onEvent(onWiFiEvent);
  dashboardServer.begin();
  httpSinkhole.begin();
  dnsServer.begin(DnsPort);
  addDisplayLine("AP " + WiFi.softAPIP().toString());
  addDisplayLine("Dashboard :8080");
}

void setup() {
  Serial.begin(115200);
  pinMode(BacklightPin, OUTPUT);
  digitalWrite(BacklightPin, HIGH);
  display.init();
  display.setRotation(1);
  redrawDisplay();
  initializeSdLog();
  startAccessPoint();
}

void loop() {
  processStationEvents();
  handleDns();
  handleHttpSinkhole();
  handleDashboard();
  flushSdLog();
}