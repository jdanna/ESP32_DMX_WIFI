#include "web.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include "config.h"
#include "engine.h"
#include "net.h"
#include "web_index.h"

namespace {

WebServer server(80);
uint32_t rebootAt;

const char *SOURCE_NAMES[] = {nullptr, "dmx", "artnet", "manual", "hold"};

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

void handleStatus() {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["fw"] = FW_VERSION;
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
  if (!configFromJson(g_cfg, in.as<JsonObjectConst>(), err, networkChanged)) {
    sendError(400, err);
    return;
  }
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
      Serial.printf("[ota] Receiving %s\n", up.filename.c_str());
      Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
      break;
    case UPLOAD_FILE_WRITE:
      if (!Update.hasError() && Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
      break;
    case UPLOAD_FILE_END:
      if (Update.end(true)) Serial.printf("[ota] Update OK, %u bytes\n", up.totalSize);
      else Update.printError(Serial);
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
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/relays", HTTP_POST, handleRelays);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  server.begin();
}

void webLoop() {
  server.handleClient();
  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) {
    Serial.println("[sys] Rebooting");
    delay(100);
    ESP.restart();
  }
}
