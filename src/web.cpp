#include "web.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <errno.h>
#include <lwip/sockets.h>
#include "config.h"
#include "engine.h"
#include "log.h"
#include "net.h"
#include "web_index.h"
#ifdef ANIMATRONIC
#include <LittleFS.h>
#endif

namespace {

WebServer server(80);
uint32_t rebootAt;

const char *SOURCE_NAMES[] = {nullptr, "dmx", "artnet", "manual", "hold", "anim"};

// Live updates are pushed as Server-Sent Events. The relay engine runs in a
// higher-priority task and DMX in an ISR, so nothing here can delay them.
constexpr int MAX_EVENT_CLIENTS = 4;
constexpr uint32_t LIVE_INTERVAL_MS = 33;   // level/fps updates: at most ~30/s, only on change
constexpr uint32_t FULL_INTERVAL_MS = 1000; // full status (network, counters)
constexpr uint32_t STALL_DROP_MS = 5000;    // drop a client that can't take data this long
constexpr int LOG_EVENT_MAX = 40;           // log lines per event; a client that falls behind refetches

struct EventClient {
  NetworkClient client;
  bool used;
  uint32_t since, stalledSince;
};
EventClient eventClients[MAX_EVENT_CLIENTS];
String lastLive;
uint8_t lastMask;
bool lastManual, forceEvents;
uint32_t lastLogSeq;
uint32_t lastLiveMs, lastFullMs;

void sendJson(int code, const JsonDocument &doc) {
  String body;
  serializeJson(doc, body);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void sendError(int code, const String &msg) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = msg;
  sendJson(code, doc);
}

bool readBody(JsonDocument &doc) {
  if (!server.hasArg("plain")) {
    sendError(400, "Missing JSON body");
    return false;
  }
  if (deserializeJson(doc, server.arg("plain")) || !doc.is<JsonObject>()) {
    sendError(400, "Invalid JSON");
    return false;
  }
  return true;
}

void scheduleReboot() { rebootAt = millis() + 1000; }

void handleIndex() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", (const char *)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

void buildStatus(JsonObject root) {
  root["fw"] = FW_VERSION;
  root["variant"] = FW_VARIANT;
  root["hostname"] = g_cfg.hostname;
  root["uptime"] = millis() / 1000;
  root["heap"] = ESP.getFreeHeap();
  netStatusJson(root);

  const EngineStatus st = engineStatus();
  JsonObject d = root["dmx"].to<JsonObject>();
  d["live"] = st.dmx.live;
  d["fps"] = st.dmx.fps;
  d["frames"] = st.dmx.count;
  d["errors"] = st.dmx.errors;
  d["slots"] = st.dmx.slots;

  JsonObject a = root["artnet"].to<JsonObject>();
  a["live"] = st.art.live;
  a["fps"] = st.art.fps;
  a["packets"] = st.art.count;
  a["slots"] = st.art.slots;
  if (st.art.count) a["from"] = st.artFrom.toString();

  root["manual"] = st.manual;
  root["expander"] = st.expanderOk;

  JsonArray relays = root["relays"].to<JsonArray>();
  for (int i = 0; i < RELAY_COUNT; i++) {
    JsonObject r = relays.add<JsonObject>();
    r["on"] = (bool)(st.relayMask & (1 << i));
    if (st.dmxValue[i] >= 0) r["dmx"] = st.dmxValue[i];
    if (st.artValue[i] >= 0) r["art"] = st.artValue[i];
    if (SOURCE_NAMES[st.source[i]]) r["src"] = SOURCE_NAMES[st.source[i]];
  }
  JsonObject inputs = root["inputs"].to<JsonObject>();
  JsonArray inDmx = inputs["dmx"].to<JsonArray>(), inArt = inputs["art"].to<JsonArray>();
  for (int i = 0; i < RELAY_COUNT; i++) {
    inDmx.add(st.inDmx[i]);
    inArt.add(st.inArt[i]);
  }

#ifdef ANIMATRONIC
  JsonObject an = root["anim"].to<JsonObject>();
  an["dir"] = motorDirName(st.anim.dir);
  an["target"] = motorDirName(st.anim.target);
  an["atLimit"] = motorDirName(st.anim.atLimit);
  if (stopReasonName(st.anim.reason)) an["reason"] = stopReasonName(st.anim.reason);
  an["center"] = st.anim.center;
  an["closed"] = st.anim.closed;
  an["override"] = st.anim.override;
  an["eyes"] = st.anim.eyes;
  an["home"] = homeStateName(st.anim.home);
  an["pos"] = st.anim.posMs;

  JsonObject sx = root["sfx"].to<JsonObject>();
  sfxStatusJson(sx);
  JsonArray slots = sx["slots"];
  for (int i = 0; i < SFX_COUNT; i++) slots[i]["name"] = g_cfg.sfxNames[i];
#endif
}

void handleStatus() {
  JsonDocument doc;
  buildStatus(doc.to<JsonObject>());
  sendJson(200, doc);
}

// Compact form of the fast-changing fields: relay mask, per-relay source and
// input values (-1 = none), input liveness and rates.
String liveJson(const EngineStatus &st) {
  JsonDocument doc;
  doc["m"] = st.relayMask;
  doc["man"] = st.manual;
  doc["dl"] = st.dmx.live;
  doc["df"] = st.dmx.fps;
  doc["al"] = st.art.live;
  doc["af"] = st.art.fps;
  JsonArray d = doc["d"].to<JsonArray>(), a = doc["a"].to<JsonArray>(), src = doc["s"].to<JsonArray>();
  JsonArray id = doc["id"].to<JsonArray>(), ia = doc["ia"].to<JsonArray>();
  for (int i = 0; i < RELAY_COUNT; i++) {
    d.add(st.dmxValue[i]);
    a.add(st.artValue[i]);
    src.add((int)st.source[i]);
    id.add(st.inDmx[i]);
    ia.add(st.inArt[i]);
  }
#ifdef ANIMATRONIC
  // [dir, target, atLimit, reason, center, closed, override, eyes, home, position ms]
  JsonArray an = doc["an"].to<JsonArray>();
  an.add((int)st.anim.dir);
  an.add((int)st.anim.target);
  an.add((int)st.anim.atLimit);
  an.add((int)st.anim.reason);
  an.add(st.anim.center);
  an.add(st.anim.closed);
  an.add(st.anim.override);
  an.add(st.anim.eyes);
  an.add((int)st.anim.home);
  an.add(st.anim.posMs);
  // [playing mask, volume %, volume set by an input]
  JsonArray sx = doc["sx"].to<JsonArray>();
  sx.add(sfxPlayingMask());
  sx.add(sfxVolume());
  sx.add(sfxVolumeFromInput());
#endif
  String out;
  serializeJson(doc, out);
  return out;
}

// Never blocks: a browser that can't keep up skips updates rather than holding
// up the loop. A partial write would corrupt the stream, so that client is dropped
// and EventSource reconnects on its own.
bool sendNonBlocking(EventClient &ec, const String &msg, uint32_t now) {
  int fd = ec.client.fd();
  if (fd < 0) return false;
  ssize_t n = send(fd, msg.c_str(), msg.length(), MSG_DONTWAIT);
  if (n == (ssize_t)msg.length()) {
    ec.stalledSince = 0;
    return true;
  }
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    if (!ec.stalledSince) ec.stalledSince = now;
    return now - ec.stalledSince < STALL_DROP_MS;
  }
  return false;
}

void broadcast(const char *event, const String &json) {
  String msg = String("event: ") + event + "\ndata: " + json + "\n\n";
  const uint32_t now = millis();
  for (EventClient &ec : eventClients) {
    if (!ec.used || sendNonBlocking(ec, msg, now)) continue;
    ec.client.stop();
    ec.client = NetworkClient();
    ec.used = false;
  }
}

void pushEvents() {
  bool any = false;
  for (const EventClient &ec : eventClients) any |= ec.used;
  if (!any) return;

  const uint32_t now = millis();
  const bool force = forceEvents;
  forceEvents = false;

  if (force || now - lastFullMs >= FULL_INTERVAL_MS) {
    JsonDocument doc;
    buildStatus(doc.to<JsonObject>());
    String json;
    serializeJson(doc, json);
    broadcast("status", json);
    lastFullMs = now;
  }

  const uint32_t seq = logSeq();
  if (seq != lastLogSeq) {
    JsonDocument doc;
    logJson(doc["entries"].to<JsonArray>(), lastLogSeq, LOG_EVENT_MAX);
    doc["seq"] = seq;
    doc["now"] = now;
    String json;
    serializeJson(doc, json);
    broadcast("log", json);
    lastLogSeq = seq;
  }

  const EngineStatus st = engineStatus();
  const bool relaysChanged = st.relayMask != lastMask || st.manual != lastManual;
  if (!force && !relaysChanged && now - lastLiveMs < LIVE_INTERVAL_MS) return;
  String live = liveJson(st);
  if (!force && live == lastLive) return;
  broadcast("live", live);
  lastLive = live;
  lastLiveMs = now;
  lastMask = st.relayMask;
  lastManual = st.manual;
}

void handleEvents() {
  // Take a slot, evicting the oldest stream if all are in use (e.g. forgotten tabs).
  int slot = 0;
  for (int i = 0; i < MAX_EVENT_CLIENTS; i++) {
    if (!eventClients[i].used) {
      slot = i;
      break;
    }
    if (eventClients[i].since < eventClients[slot].since) slot = i;
  }
  EventClient &ec = eventClients[slot];
  if (ec.used) ec.client.stop();

  // Keep our own handle on the socket; WebServer just drops its reference after
  // this returns, so the connection stays open without tying up the server.
  ec.client = server.client();
  ec.client.setNoDelay(true);
  ec.client.print(
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
      "Connection: keep-alive\r\n\r\nretry: 1000\n\n");
  ec.used = true;
  ec.since = millis();
  ec.stalledSince = 0;
  forceEvents = true;
}

// ?after=N returns only entries newer than sequence number N.
void handleGetLog() {
  JsonDocument doc;
  doc["seq"] = logSeq();
  doc["now"] = millis();
  logJson(doc["entries"].to<JsonArray>(), server.arg("after").toInt(), 1000);
  sendJson(200, doc);
}

void handlePostLog() {
  JsonDocument in;
  if (!readBody(in)) return;
  if (in["clear"] == true) logClear();
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

void handleGetConfig() {
  JsonDocument doc;
  configToJson(g_cfg, doc.to<JsonObject>(), false);
  sendJson(200, doc);
}

void handlePostConfig() {
  JsonDocument in;
  if (!readBody(in)) return;
  String err;
  bool networkChanged = false;
  Config c = g_cfg;
  if (!configFromJson(c, in.as<JsonObjectConst>(), err, networkChanged)) {
    sendError(400, err);
    return;
  }
#ifdef ANIMATRONIC
  // Only requests that change the mapping are checked, so a volume change still
  // saves on a device whose older settings share channels.
  const bool mapping = !in["relays"]["map"].isNull() || !in["anim"]["map"].isNull() ||
                       !in["anim"]["dirChannel"].isNull() || !in["anim"]["motorMode"].isNull() ||
                       !in["sfx"]["map"].isNull() || !in["sfx"]["volChannel"].isNull();
  if (mapping && !configMappingExclusive(c, err)) {
    sendError(400, err);
    return;
  }
#endif
  g_cfg = c;
  configSave(g_cfg);
  engineApply(g_cfg.io);

  JsonDocument doc;
  doc["ok"] = true;
  doc["reboot"] = networkChanged;
  configToJson(g_cfg, doc["config"].to<JsonObject>(), false);
  sendJson(200, doc);
  if (networkChanged) scheduleReboot();
}

void handleRelays() {
  JsonDocument in;
  if (!readBody(in)) return;
  if (in["manual"].is<bool>()) engineSetManual(in["manual"]);
  if (in["all"].is<bool>()) engineSetManualAll(in["all"]);
  if (in["index"].is<int>() && in["on"].is<bool>()) engineSetManualRelay(in["index"], in["on"]);
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

#ifdef ANIMATRONIC
void handleAnim() {
  JsonDocument in;
  if (!readBody(in)) return;
  if (in["override"].is<bool>()) animOverride(in["override"]);
  if (in["eyes"].is<bool>()) animEyes(in["eyes"]);
  if (in["home"] == true) animHome();
  if (!in["motor"].isNull()) {
    const char *m = in["motor"];
    MotorDir dir;
    if (m && !strcmp(m, "left")) dir = MOTOR_LEFT;
    else if (m && !strcmp(m, "right")) dir = MOTOR_RIGHT;
    else if (m && !strcmp(m, "stop")) dir = MOTOR_STOP;
    else {
      sendError(400, "motor must be left, right or stop");
      return;
    }
    String err;
    if (!animMotor(dir, err)) {
      sendError(409, err);
      return;
    }
  }
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

// Sound uploads land in a scratch file; the slot's old sound is dropped first
// so a replacement doesn't need room for both.
File sfxFile;
String sfxUploadErr;

int sfxSlotArg() {
  const int slot = server.arg("slot").toInt() - 1;
  return slot >= 0 && slot < SFX_COUNT ? slot : -1;
}

void handleSfxUploadData() {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      sfxUploadErr = "";
      if (sfxSlotArg() < 0) {
        sfxUploadErr = "slot must be 1-4";
        break;
      }
      sfxDelete(sfxSlotArg());
      LittleFS.remove(sfxUploadPath());
      sfxFile = LittleFS.open(sfxUploadPath(), "w");
      if (!sfxFile) sfxUploadErr = "Can't create the file";
      break;
    case UPLOAD_FILE_WRITE:
      if (sfxFile && sfxFile.write(up.buf, up.currentSize) != up.currentSize) {
        sfxFile.close();
        LittleFS.remove(sfxUploadPath());
        sfxUploadErr = "Not enough storage space for this sound";
      }
      break;
    case UPLOAD_FILE_END:
      if (sfxFile) sfxFile.close();
      break;
    case UPLOAD_FILE_ABORTED:
      if (sfxFile) sfxFile.close();
      LittleFS.remove(sfxUploadPath());
      sfxUploadErr = "Upload interrupted";
      break;
  }
}

void handleSfxUploadDone() {
  const int slot = sfxSlotArg();
  String err = sfxUploadErr;
  if (!err.length() && slot < 0) err = "slot must be 1-4";
  if (!err.length()) sfxInstall(slot, err);
  if (slot >= 0) {
    String name = err.length() ? String() : server.arg("name");
    name.trim();
    if (!err.length() && !name.length()) name = String("Sound ") + (slot + 1);
    strlcpy(g_cfg.sfxNames[slot], name.c_str(), sizeof(g_cfg.sfxNames[slot]));
    configSave(g_cfg);
  }
  if (err.length()) {
    sendError(400, err);
    return;
  }
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

void handleSfx() {
  JsonDocument in;
  if (!readBody(in)) return;
  if (in["play"].is<int>()) sfxPlay(in["play"].as<int>());
  if (in["stop"].is<int>()) sfxStop(in["stop"].as<int>());
  if (in["delete"].is<int>()) {
    const int slot = in["delete"];
    if (slot < 0 || slot >= SFX_COUNT) {
      sendError(400, "delete must be 0-3");
      return;
    }
    sfxDelete(slot);
    g_cfg.sfxNames[slot][0] = 0;
    configSave(g_cfg);
  }
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}
#endif

void handleScan() {
  JsonDocument doc;
  if (server.hasArg("start")) WiFi.scanDelete();
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    doc["running"] = true;
  } else if (n >= 0) {
    doc["running"] = false;
    JsonArray nets = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      bool dup = false;  // results arrive strongest-first, so keep the first of each name
      for (JsonObject o : nets)
        if (ssid == o["ssid"].as<const char *>()) dup = true;
      if (dup) continue;
      JsonObject o = nets.add<JsonObject>();
      o["ssid"] = ssid;
      o["rssi"] = WiFi.RSSI(i);
      o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
  } else {
    netPrepareScan();
    WiFi.scanNetworks(true);
    doc["running"] = true;
  }
  sendJson(200, doc);
}

void handleReboot() {
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
  scheduleReboot();
}

void handleFactoryReset() {
  configErase();
  handleReboot();
}

void handleUpdateDone() {
  JsonDocument doc;
  doc["ok"] = !Update.hasError();
  if (Update.hasError()) doc["error"] = Update.errorString();
  sendJson(Update.hasError() ? 500 : 200, doc);
  if (!Update.hasError()) scheduleReboot();
}

void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      logInfo("ota", "Receiving %s", up.filename.c_str());
      Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
      break;
    case UPLOAD_FILE_WRITE:
      if (!Update.hasError() && Update.write(up.buf, up.currentSize) != up.currentSize)
        logError("ota", "Write failed: %s", Update.errorString());
      break;
    case UPLOAD_FILE_END:
      if (Update.end(true)) logInfo("ota", "Update OK, %u bytes", up.totalSize);
      else logError("ota", "Update failed: %s", Update.errorString());
      break;
    case UPLOAD_FILE_ABORTED:
      Update.abort();
      break;
  }
}

void handleNotFound() {
  // In setup-AP mode every DNS name resolves to us; send phones' captive-portal
  // probes to the UI so the setup page pops up on its own.
  if (netApActive() && !server.uri().startsWith("/api/")) {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
    return;
  }
  sendError(404, "Not found");
}

}  // namespace

void webBegin() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/events", HTTP_GET, handleEvents);
  server.on("/api/log", HTTP_GET, handleGetLog);
  server.on("/api/log", HTTP_POST, handlePostLog);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/relays", HTTP_POST, handleRelays);
#ifdef ANIMATRONIC
  server.on("/api/anim", HTTP_POST, handleAnim);
  server.on("/api/sfx", HTTP_POST, handleSfx);
  server.on("/api/sfx/upload", HTTP_POST, handleSfxUploadDone, handleSfxUploadData);
#endif
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  server.begin();
}

void webLoop() {
  server.handleClient();
  pushEvents();
  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) {
    logInfo("sys", "Rebooting");
    delay(100);
    ESP.restart();
  }
}
