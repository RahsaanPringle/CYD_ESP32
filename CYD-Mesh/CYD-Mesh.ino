#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <TFT_eSPI.h>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#else
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

constexpr uint8_t BacklightPin = 21;
constexpr uint8_t BootButtonPin = 0;
constexpr uint8_t MeshChannel = 6;
constexpr uint32_t NetworkId = 0x43594431; // Change this for each separate mesh.
constexpr uint16_t ProtocolVersion = 1;
constexpr uint32_t DiscoveryWindowMs = 2000;
constexpr uint32_t HeartbeatIntervalMs = 1000;
constexpr uint32_t LeaderTimeoutMs = 3500;
constexpr uint32_t ElectionWindowMs = 750;
constexpr char AccessPointPassword[] = "cydmesh1";

enum class Role : uint8_t { Discovering, Follower, Candidate, Leader };
enum class MessageType : uint8_t {
  Hello, Heartbeat, Election, Leader, Counter, CounterProposal, ChatProposal, ChatMessage
};

struct __attribute__((packed)) MeshMessage {
  uint32_t networkId;
  uint16_t protocolVersion;
  MessageType type;
  uint8_t priority;
  uint32_t senderId;
  uint32_t bootId;
  uint32_t term;
  uint32_t sequence;
  uint32_t counter;
  uint32_t originNodeId;
  char text[96];
};

static_assert(sizeof(MeshMessage) < 250, "ESP-NOW message must remain small");

const uint8_t BroadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
TFT_eSPI display;
WebServer server(80);

uint32_t nodeId;
uint32_t bootId;
String accessPointName;
IPAddress accessPointAddress;
uint32_t term = 0;
uint32_t sequence = 0;
uint32_t sharedCounter = 0;
uint32_t leaderId = 0;
uint32_t leaderLastSeenAt = 0;
uint32_t nextElectionAt = 0;
uint32_t electionEndsAt = 0;
uint32_t bestCandidateId = 0;
uint8_t bestCandidatePriority = 0;
Role role = Role::Discovering;
bool wasButtonPressed = false;

uint32_t lastHelloAt = 0;
uint32_t lastHeartbeatAt = 0;
constexpr uint8_t ChatHistorySize = 6;
String chatHistory[ChatHistorySize];
volatile bool hasPendingChat = false;
uint32_t pendingChatOriginId = 0;
char pendingChatText[sizeof(MeshMessage::text)] = {};

const char* roleName(Role value) {
  switch (value) {
    case Role::Discovering: return "DISCOVERING";
    case Role::Follower: return "FOLLOWER";
    case Role::Candidate: return "CANDIDATE";
    case Role::Leader: return "LEADER";
  }
  return "UNKNOWN";
}

uint8_t nodePriority() {
  // A deterministic, stable tie breaker. Override this for named nodes if desired.
  return 100;
}

bool isBetterCandidate(uint8_t candidatePriority, uint32_t candidateId,
                       uint8_t currentPriority, uint32_t currentId) {
  return candidatePriority > currentPriority ||
         (candidatePriority == currentPriority && candidateId < currentId);
}

void sendMessage(MessageType type, uint32_t originNodeId = 0, const char* text = "") {
  MeshMessage message = {
      NetworkId, ProtocolVersion, type, nodePriority(), nodeId, bootId,
      term, sequence, sharedCounter, originNodeId, {}};
  snprintf(message.text, sizeof(message.text), "%s", text);

  esp_err_t result = esp_now_send(BroadcastAddress,
                                  reinterpret_cast<const uint8_t*>(&message),
                                  sizeof(message));
  if (result != ESP_OK) Serial.printf("ESP-NOW send failed: %d\n", result);
}

void requestCounterIncrement() {
  if (role == Role::Leader) {
    ++sharedCounter;
    ++sequence;
    sendMessage(MessageType::Counter);
  } else if (leaderId != 0) {
    sendMessage(MessageType::CounterProposal);
  }
}

void queueChat(uint32_t originNodeId, const char* text);

void requestChat(const String& text) {
  if (leaderId == 0 || text.isEmpty()) return;
  if (role == Role::Leader) {
    // ESP-NOW receive handling ignores packets sent by this node, so a leader
    // must commit and record its own web-submitted chat directly.
    ++sequence;
    sendMessage(MessageType::ChatMessage, nodeId, text.c_str());
    queueChat(nodeId, text.c_str());
  } else {
    sendMessage(MessageType::ChatProposal, nodeId, text.c_str());
  }
}

void queueChat(uint32_t originNodeId, const char* text) {
  pendingChatOriginId = originNodeId;
  snprintf(pendingChatText, sizeof(pendingChatText), "%s", text);
  hasPendingChat = true;
}

void savePendingChat() {
  if (!hasPendingChat) return;
  hasPendingChat = false;
  for (uint8_t index = 1; index < ChatHistorySize; ++index) {
    chatHistory[index - 1] = chatHistory[index];
  }
  chatHistory[ChatHistorySize - 1] = String(pendingChatOriginId, HEX) + ": " + pendingChatText;
}

void beginElection() {
  ++term;
  role = Role::Candidate;
  leaderId = 0;
  bestCandidateId = nodeId;
  bestCandidatePriority = nodePriority();
  electionEndsAt = millis() + ElectionWindowMs;
  sendMessage(MessageType::Election);
  Serial.printf("Election started for term %lu\n", static_cast<unsigned long>(term));
}

void becomeLeader() {
  role = Role::Leader;
  leaderId = nodeId;
  leaderLastSeenAt = millis();
  sendMessage(MessageType::Leader);
  Serial.printf("Leader for term %lu\n", static_cast<unsigned long>(term));
}

void acceptLeader(const MeshMessage& message) {
  if (message.term < term) return;
  if (message.term == term && role == Role::Leader &&
      !isBetterCandidate(message.priority, message.senderId, nodePriority(), nodeId)) {
    return;
  }

  term = message.term;
  role = message.senderId == nodeId ? Role::Leader : Role::Follower;
  leaderId = message.senderId;
  leaderLastSeenAt = millis();
}

void handleMessage(const uint8_t* data, int length) {
  if (length != sizeof(MeshMessage)) return;

  MeshMessage message;
  memcpy(&message, data, sizeof(message));
  if (message.networkId != NetworkId || message.protocolVersion != ProtocolVersion ||
      message.senderId == nodeId) return;

  if (message.term > term) {
    term = message.term;
    role = Role::Follower;
  }

  switch (message.type) {
    case MessageType::Hello:
      if (role == Role::Leader) sendMessage(MessageType::Leader);
      break;

    case MessageType::Heartbeat:
    case MessageType::Leader:
      acceptLeader(message);
      break;

    case MessageType::Election:
      if (message.term < term) break;
      if (message.term > term) {
        term = message.term;
        role = Role::Follower;
      }
      if (role != Role::Leader &&
          isBetterCandidate(message.priority, message.senderId,
                            bestCandidatePriority, bestCandidateId)) {
        bestCandidatePriority = message.priority;
        bestCandidateId = message.senderId;
      }
      break;

    case MessageType::Counter:
      if (message.term == term && message.sequence > sequence) {
        sequence = message.sequence;
        sharedCounter = message.counter;
      }
      break;

    case MessageType::CounterProposal:
      if (role == Role::Leader && message.term == term) {
        ++sharedCounter;
        ++sequence;
        sendMessage(MessageType::Counter);
      }
      break;

    case MessageType::ChatProposal:
      if (role == Role::Leader && message.term == term && message.text[0] != '\0') {
        ++sequence;
        const uint32_t originNodeId = message.originNodeId == 0 ? message.senderId : message.originNodeId;
        sendMessage(MessageType::ChatMessage, originNodeId, message.text);
        queueChat(originNodeId, message.text);
      }
      break;

    case MessageType::ChatMessage:
      if (message.term == term && message.sequence > sequence) {
        sequence = message.sequence;
        queueChat(message.originNodeId, message.text);
      }
      break;
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataReceived(const esp_now_recv_info_t*, const uint8_t* data, int length) {
  handleMessage(data, length);
}
#else
void onDataReceived(const uint8_t*, const uint8_t* data, int length) {
  handleMessage(data, length);
}
#endif

void drawStatus() {
  display.fillScreen(TFT_BLACK);
  display.setTextSize(2);
  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setCursor(10, 10);
  display.print("CYD MESH");

  display.setTextColor(role == Role::Leader ? TFT_GREEN : TFT_CYAN, TFT_BLACK);
  display.setCursor(10, 46);
  display.print(roleName(role));

  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(1);
  display.setCursor(10, 82);
  display.printf("Node: %08lX", static_cast<unsigned long>(nodeId));
  display.setCursor(10, 100);
  display.printf("Leader: %08lX", static_cast<unsigned long>(leaderId));
  display.setCursor(10, 118);
  display.printf("Term: %lu  Seq: %lu", static_cast<unsigned long>(term),
                 static_cast<unsigned long>(sequence));

  display.setTextSize(3);
  display.setTextColor(TFT_YELLOW, TFT_BLACK);
  display.setCursor(10, 158);
  display.printf("COUNT %lu", static_cast<unsigned long>(sharedCounter));

  display.setTextSize(1);
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display.setCursor(10, 196);
  display.print(accessPointName);
  display.setCursor(10, 208);
  display.printf("Pass: %s", AccessPointPassword);

  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setCursor(10, 222);
  display.print("IP ");
  display.print(accessPointAddress);
}

String stateJson() {
  String state = "{\"node\":\"";
  state += String(nodeId, HEX);
  state += "\",\"role\":\"";
  state += roleName(role);
  state += "\",\"leader\":\"";
  state += String(leaderId, HEX);
  state += "\",\"term\":";
  state += term;
  state += ",\"sequence\":";
  state += sequence;
  state += ",\"counter\":";
  state += sharedCounter;
  state += "}";
  return state;
}

String chatJson() {
  String messages = "[";
  for (uint8_t index = 0; index < ChatHistorySize; ++index) {
    if (chatHistory[index].isEmpty()) continue;
    if (messages.length() > 1) messages += ',';
    messages += '"';
    for (size_t characterIndex = 0; characterIndex < chatHistory[index].length(); ++characterIndex) {
      const char character = chatHistory[index][characterIndex];
      if (character == '"' || character == '\\') messages += '\\';
      if (character >= 32) messages += character;
    }
    messages += '"';
  }
  return messages + ']';
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", R"html(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>CYD Mesh</title><style>body{margin:0;background:#101716;color:#e7f0ed;font:18px sans-serif}main{max-width:520px;margin:auto;padding:28px}h1{color:#ff9d28}dl,#messages{background:#192523;padding:18px}dl{display:grid;grid-template-columns:1fr 1fr;gap:12px}dt{color:#aabbb6}dd{margin:0;text-align:right}button,input{box-sizing:border-box;width:100%;padding:14px;border:0;font-size:18px}button{margin-top:12px;background:#f5b942;color:#17130b;font-weight:bold}input{margin-top:12px;background:#fff;color:#111}#messages{min-height:90px;margin-top:18px;white-space:pre-wrap}</style></head><body><main><h1>CYD MESH</h1><dl><dt>Role</dt><dd id="role">...</dd><dt>Leader</dt><dd id="leader">...</dd><dt>Term</dt><dd id="term">...</dd><dt>Sequence</dt><dd id="sequence">...</dd><dt>Counter</dt><dd id="counter">...</dd></dl><button onclick="fetch('/api/increment',{method:'POST'}).then(refresh)">Increment shared counter</button><section id="messages">No messages yet.</section><form onsubmit="sendChat(event)"><input id="chat" maxlength="95" placeholder="Message the mesh"><button>Send message</button></form></main><script>async function refresh(){let s=await (await fetch('/api/state')).json();for(let k in s){let e=document.getElementById(k);if(e)e.textContent=s[k]}let m=await (await fetch('/api/messages')).json();document.getElementById('messages').textContent=m.length?m.join('\n'):'No messages yet.'}async function sendChat(e){e.preventDefault();let i=document.getElementById('chat');let b=new URLSearchParams({message:i.value});await fetch('/api/messages',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});i.value='';refresh()}refresh();setInterval(refresh,1000)</script></body></html>)html");
  });
  server.on("/api/state", HTTP_GET, []() {
    server.send(200, "application/json", stateJson());
  });
  server.on("/api/increment", HTTP_POST, []() {
    requestCounterIncrement();
    server.send(202, "application/json", "{\"accepted\":true}");
  });
  server.on("/api/messages", HTTP_GET, []() {
    server.send(200, "application/json", chatJson());
  });
  server.on("/api/messages", HTTP_POST, []() {
    String message = server.arg("message");
    message.trim();
    if (message.isEmpty()) {
      server.send(400, "application/json", "{\"error\":\"Message is empty\"}");
      return;
    }
    requestChat(message.substring(0, sizeof(MeshMessage::text) - 1));
    server.send(202, "application/json", "{\"accepted\":true}");
  });
  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void setupWireless() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  const uint8_t subnet = 4 + (nodeId % 200);
  accessPointAddress = IPAddress(192, 168, subnet, 1);
  const IPAddress gateway = accessPointAddress;
  const IPAddress subnetMask(255, 255, 255, 0);
  accessPointName = "CYD-Mesh-" + String(nodeId & 0xFFFF, HEX);
  WiFi.softAPConfig(accessPointAddress, gateway, subnetMask);
  WiFi.softAP(accessPointName.c_str(), AccessPointPassword, MeshChannel);
  Serial.printf("Connect to %s at http://%s/\n", accessPointName.c_str(),
                accessPointAddress.toString().c_str());
  esp_wifi_set_channel(MeshChannel, WIFI_SECOND_CHAN_NONE);
}

void setupEspNow() {

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed");
    while (true) delay(1000);
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, BroadcastAddress, sizeof(BroadcastAddress));
  peerInfo.channel = MeshChannel;
  peerInfo.encrypt = false;
  if (!esp_now_is_peer_exist(BroadcastAddress) &&
      esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Could not add broadcast peer");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataReceived);
}

void setup() {
  Serial.begin(115200);
  pinMode(BacklightPin, OUTPUT);
  digitalWrite(BacklightPin, HIGH);
  pinMode(BootButtonPin, INPUT_PULLUP);

  display.init();
  display.setRotation(1);
  nodeId = static_cast<uint32_t>(ESP.getEfuseMac());
  bootId = esp_random();
  setupWireless();
  setupEspNow();
  setupWebServer();
  nextElectionAt = millis() + DiscoveryWindowMs + random(250, 751);
  sendMessage(MessageType::Hello);
  drawStatus();
}

void loop() {
  const uint32_t now = millis();
  const bool buttonPressed = digitalRead(BootButtonPin) == LOW;

  if (now - lastHelloAt >= 1000) {
    lastHelloAt = now;
    sendMessage(MessageType::Hello);
  }

  if (role == Role::Leader && now - lastHeartbeatAt >= HeartbeatIntervalMs) {
    lastHeartbeatAt = now;
    sendMessage(MessageType::Heartbeat);
  }

  if (role != Role::Leader && now >= nextElectionAt &&
      (leaderId == 0 || now - leaderLastSeenAt > LeaderTimeoutMs)) {
    beginElection();
    nextElectionAt = now + LeaderTimeoutMs;
  }

  if (role == Role::Candidate && now >= electionEndsAt) {
    if (bestCandidateId == nodeId) becomeLeader();
    else role = Role::Follower;
  }

  if (buttonPressed && !wasButtonPressed) {
    requestCounterIncrement();
  }
  wasButtonPressed = buttonPressed;

  // ESP-NOW callbacks only copy received chat into the pending buffer. Move it
  // into Arduino String history here, safely on the main loop's context.
  savePendingChat();
  server.handleClient();
  drawStatus();
  delay(50);
}
