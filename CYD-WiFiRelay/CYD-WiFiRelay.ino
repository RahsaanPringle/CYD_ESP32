#include <WiFi.h>
#include <WiFiUdp.h>
#include <TFT_eSPI.h>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#else
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#if __has_include(<lwip/lwip_napt.h>)
#include <lwip/lwip_napt.h>
#define CYD_HAS_NAPT 1
#else
#define CYD_HAS_NAPT 0
#endif

constexpr char RelaySsid[] = "CYD-WiFi-Relay";
constexpr char RelayPassword[] = "cydmesh1";
constexpr char UpstreamSsid[] = "Fivenet";
constexpr char UpstreamPassword[] = "jump70lake";
constexpr uint8_t BacklightPin = 21;
constexpr uint8_t MaxNetworks = 8;
constexpr uint8_t LogLineCount = 6;
constexpr uint16_t DnsPort = 53;
constexpr uint32_t DisplayRefreshIntervalMs = 1000;

enum class Screen : uint8_t { NetworkList, Password, Relay };

TFT_eSPI display;
WiFiUDP dnsServer;
WiFiUDP upstreamDns;
Screen screen = Screen::NetworkList;
String networkNames[MaxNetworks];
int networkCount = 0;
int selectedNetwork = -1;
String passwordInput;
String logLines[LogLineCount];
bool relayStarted = false;
bool natEnabled = false;
uint32_t lastConnectionAttemptAt = 0;
bool upstreamWasConnected = false;
bool dnsRequestPending = false;
IPAddress dnsClientAddress;
uint16_t dnsClientPort = 0;
uint32_t dnsRequestStartedAt = 0;
volatile uint8_t pendingClientConnects = 0;
volatile uint8_t pendingClientDisconnects = 0;
uint32_t lastDisplayAt = 0;
bool displayDirty = true;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) ++pendingClientConnects;
  if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) ++pendingClientDisconnects;
}
#else
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t) {
  if (event == SYSTEM_EVENT_AP_STACONNECTED) ++pendingClientConnects;
  if (event == SYSTEM_EVENT_AP_STADISCONNECTED) ++pendingClientDisconnects;
}
#endif

void logActivity(const String& message) {
  Serial.println(message);
  for (uint8_t index = 1; index < LogLineCount; ++index) logLines[index - 1] = logLines[index];
  String line = String(millis() / 1000) + "s " + message;
  if (line.length() > 38) line.remove(38);
  logLines[LogLineCount - 1] = line;
  displayDirty = true;
}

void drawButton(int16_t x, int16_t y, int16_t width, int16_t height, const String& label, uint16_t color) {
  display.fillRoundRect(x, y, width, height, 4, color);
  display.setTextColor(TFT_BLACK, color);
  display.setTextSize(1);
  const int16_t labelWidth = display.textWidth(label);
  display.setCursor(x + (width - labelWidth) / 2, y + (height - 8) / 2);
  display.print(label);
}

void drawNetworkList() {
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.print("Choose Wi-Fi");
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display.setTextSize(1);
  display.setCursor(8, 32);
  display.print("Tap a network, then enter its password");
  if (networkCount == 0) {
    display.setCursor(8, 62);
    display.print("No networks found. Tap RESCAN.");
  }
  for (int index = 0; index < networkCount; ++index) {
    const int16_t y = 48 + index * 22;
    display.fillRect(8, y, 304, 19, TFT_DARKGREY);
    display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    display.setCursor(13, y + 5);
    display.print(networkNames[index]);
  }
  drawButton(8, 225, 90, 30, "RESCAN", TFT_CYAN);
}

void drawPasswordScreen() {
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.print("Wi-Fi Password");
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(1);
  display.setCursor(8, 32);
  display.print(networkNames[selectedNetwork]);
  display.setCursor(8, 46);
  display.print("Password: ");
  for (size_t index = 0; index < passwordInput.length(); ++index) display.print('*');
  const char* keys = "ABCDEFGHIJKLM\nNOPQRSTUVWXYZ\n0123456789";
  int16_t x = 8;
  int16_t y = 64;
  for (const char* key = keys; *key; ++key) {
    if (*key == '\n') { x = 8; y += 31; continue; }
    drawButton(x, y, 21, 25, String(*key), TFT_LIGHTGREY);
    x += 24;
  }
  drawButton(8, 163, 72, 28, "SPACE", TFT_LIGHTGREY);
  drawButton(88, 163, 72, 28, "DELETE", TFT_YELLOW);
  drawButton(168, 163, 72, 28, "CANCEL", TFT_DARKGREY);
  drawButton(248, 163, 64, 28, "JOIN", TFT_GREEN);
}

void drawRelayScreen() {
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setTextSize(2);
  display.setCursor(8, 8);
  display.print("Wi-Fi Relay");
  display.setTextSize(1);
  display.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED, TFT_BLACK);
  display.setCursor(8, 32);
  display.print(WiFi.status() == WL_CONNECTED ? "UPSTREAM CONNECTED" : "UPSTREAM CONNECTING");
  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setCursor(8, 45);
  display.printf("Up: %s  %s", UpstreamSsid, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-");
  display.setCursor(8, 57);
  display.printf("AP: %s  %s", RelaySsid, WiFi.softAPIP().toString().c_str());
  display.setCursor(8, 69);
  display.printf("Pass: %s", RelayPassword);
  display.setCursor(8, 81);
  display.printf("Clients: %u  NAT: %s", WiFi.softAPgetStationNum(), natEnabled ? "ON" : "OFF");
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  for (uint8_t index = 0; index < LogLineCount; ++index) {
    display.setCursor(8, 98 + index * 20);
    display.print(logLines[index]);
  }
  lastDisplayAt = millis();
  displayDirty = false;
}

void scanNetworks() {
  networkCount = 0;
  const int found = WiFi.scanNetworks();
  for (int index = 0; index < found && networkCount < MaxNetworks; ++index) {
    const String name = WiFi.SSID(index);
    if (name.isEmpty()) continue;
    bool duplicate = false;
    for (int saved = 0; saved < networkCount; ++saved) duplicate |= networkNames[saved] == name;
    if (!duplicate) networkNames[networkCount++] = name;
  }
  WiFi.scanDelete();
  drawNetworkList();
}

void startRelay() {
  const IPAddress apAddress(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apAddress, apAddress, subnet);
  WiFi.softAP(RelaySsid, RelayPassword);
  WiFi.onEvent(onWiFiEvent);
  dnsServer.begin(DnsPort);
#if CYD_HAS_NAPT
  ip_napt_enable_no(WIFI_IF_AP, 1);
  natEnabled = true;
#else
  natEnabled = false;
#endif
  relayStarted = true;
  logActivity("Relay AP started");
  logActivity("Connect: " + String(RelaySsid));
  drawRelayScreen();
}

void connectUpstream() {
  const String ssid = networkNames[selectedNetwork];
  WiFi.begin(ssid.c_str(), passwordInput.c_str());
  lastConnectionAttemptAt = millis();
  logActivity("Joining " + ssid);
  startRelay();
  screen = Screen::Relay;
}

String dnsHostname(const uint8_t* packet, size_t length) {
  if (length < 13) return "";
  String hostname;
  size_t offset = 12;
  while (offset < length && packet[offset] != 0) {
    const uint8_t labelLength = packet[offset++];
    if (labelLength == 0 || labelLength > 63 || offset + labelLength > length) return "";
    if (!hostname.isEmpty()) hostname += '.';
    for (uint8_t index = 0; index < labelLength; ++index) hostname += static_cast<char>(packet[offset++]);
  }
  return hostname;
}

void handleDnsRequests() {
  uint8_t packet[512];
  const int responseLength = upstreamDns.parsePacket();
  if (dnsRequestPending && responseLength > 0) {
    const int length = upstreamDns.read(packet, min(responseLength, static_cast<int>(sizeof(packet))));
    if (length > 0) {
      dnsServer.beginPacket(dnsClientAddress, dnsClientPort);
      dnsServer.write(packet, length);
      dnsServer.endPacket();
    }
    dnsRequestPending = false;
  }
  if (dnsRequestPending && millis() - dnsRequestStartedAt > 3000) dnsRequestPending = false;

  const int requestLength = dnsServer.parsePacket();
  if (requestLength <= 0 || dnsRequestPending || WiFi.status() != WL_CONNECTED) return;
  const int length = dnsServer.read(packet, min(requestLength, static_cast<int>(sizeof(packet))));
  if (length <= 0) return;
  dnsClientAddress = dnsServer.remoteIP();
  dnsClientPort = dnsServer.remotePort();
  dnsRequestPending = true;
  dnsRequestStartedAt = millis();
  logActivity("DNS " + dnsHostname(packet, length));
  upstreamDns.beginPacket(WiFi.dnsIP(), DnsPort);
  upstreamDns.write(packet, length);
  upstreamDns.endPacket();
}

void setup() {
  Serial.begin(115200);
  pinMode(BacklightPin, OUTPUT);
  digitalWrite(BacklightPin, HIGH);
  display.init();
  display.setRotation(1);
  WiFi.mode(WIFI_STA);
  networkNames[0] = UpstreamSsid;
  networkCount = 1;
  selectedNetwork = 0;
  passwordInput = UpstreamPassword;
  connectUpstream();
}

void loop() {
  if (!relayStarted) return;
  while (pendingClientConnects > 0) {
    --pendingClientConnects;
    logActivity("Client joined relay");
  }
  while (pendingClientDisconnects > 0) {
    --pendingClientDisconnects;
    logActivity("Client left relay");
  }
  handleDnsRequests();
  const bool upstreamConnected = WiFi.status() == WL_CONNECTED;
  if (upstreamConnected && !upstreamWasConnected) logActivity("Upstream " + WiFi.localIP().toString());
  if (!upstreamConnected && upstreamWasConnected) logActivity("Upstream lost");
  upstreamWasConnected = upstreamConnected;
  if (WiFi.status() != WL_CONNECTED && millis() - lastConnectionAttemptAt > 15000) {
    WiFi.reconnect();
    lastConnectionAttemptAt = millis();
    logActivity("Retrying upstream");
  }
  if (displayDirty || millis() - lastDisplayAt >= DisplayRefreshIntervalMs) drawRelayScreen();
  delay(25);
}