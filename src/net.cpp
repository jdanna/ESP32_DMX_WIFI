#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ETH.h>
#include <WiFi.h>
#include "log.h"

namespace {

const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_MASK(255, 255, 255, 0);
const char *AP_PASS = "dmxrelay";
constexpr uint32_t AP_START_AFTER_MS = 20000;  // no network for this long -> open the setup AP
constexpr uint32_t AP_STOP_AFTER_MS = 120000;  // network back for this long and no AP clients -> close it
constexpr uint32_t STA_RETRY_MS = 30000;

DNSServer dns;
char apSsid[40];
bool staConfigured;
volatile bool apActive;
uint32_t downSince, upSince, lastStaTry;

void onEvent(arduino_event_id_t event, arduino_event_info_t) {
  switch (event) {
    case ARDUINO_EVENT_ETH_CONNECTED:
      logInfo("net", "Ethernet link up");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      logWarn("net", "Ethernet link down");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      logInfo("net", "Ethernet IP %s", ETH.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      logInfo("net", "Wi-Fi IP %s", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      logWarn("net", "Wi-Fi disconnected");
      break;
    default:
      break;
  }
}

bool sameSubnet(const IPAddress &a, const IPAddress &b, const IPAddress &mask) {
  return (uint32_t(a) & uint32_t(mask)) == (uint32_t(b) & uint32_t(mask));
}

String macStr(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

void startAp() {
  logWarn("net", "No network - starting setup AP \"%s\" (password %s) at %s", apSsid, AP_PASS,
          AP_IP.toString().c_str());
  // A station that keeps scanning for a missing network hops channels and makes the
  // AP unusable, so retries are paced manually while the AP is up.
  WiFi.setAutoReconnect(false);
  WiFi.mode(staConfigured ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
  WiFi.softAP(apSsid, AP_PASS);
  dns.start(53, "*", AP_IP);
  apActive = true;
  lastStaTry = millis();
}

void stopAp() {
  logInfo("net", "Network is up - closing setup AP");
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(staConfigured ? WIFI_STA : WIFI_OFF);
  WiFi.setAutoReconnect(true);
  apActive = false;
}

void ipJson(JsonObject o, NetworkInterface &nif) {
  o["ip"] = nif.localIP().toString();
  o["mask"] = nif.subnetMask().toString();
  o["gw"] = nif.gatewayIP().toString();
  o["dns"] = nif.dnsIP().toString();
}

}  // namespace

void netBegin(const Config &cfg) {
  uint64_t chip = ESP.getEfuseMac();
  snprintf(apSsid, sizeof(apSsid), "%s-%02X%02X", cfg.hostname, (uint8_t)(chip >> 32), (uint8_t)(chip >> 40));

  Network.setHostname(cfg.hostname);
  Network.onEvent(onEvent);

  if (!ETH.begin(ETH_PHY_W5500, 1, PIN_ETH_CS, PIN_ETH_INT, PIN_ETH_RST, SPI3_HOST, PIN_ETH_SCK, PIN_ETH_MISO,
                 PIN_ETH_MOSI)) {
    logError("net", "W5500 Ethernet failed to start");
  } else if (!cfg.eth.dhcp) {
    ETH.config(cfg.eth.ip, cfg.eth.gw, cfg.eth.mask, cfg.eth.dns);
  }

  WiFi.persistent(false);
  staConfigured = cfg.wifiEnabled && cfg.wifiSsid[0];
  if (staConfigured) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // modem sleep delays and drops broadcast Art-Net
    if (!cfg.wifi.dhcp) WiFi.config(cfg.wifi.ip, cfg.wifi.gw, cfg.wifi.mask, cfg.wifi.dns);
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.wifiSsid, cfg.wifiPass[0] ? cfg.wifiPass : nullptr);
  } else {
    WiFi.mode(WIFI_OFF);
  }

  if (MDNS.begin(cfg.hostname)) MDNS.addService("http", "tcp", 80);
  downSince = millis();
}

bool netHasIp() { return ETH.hasIP() || WiFi.isConnected(); }
bool netApActive() { return apActive; }

void netLoop() {
  const uint32_t now = millis();
  const bool up = netHasIp();
  if (up) {
    downSince = 0;
    if (!upSince) upSince = now;
  } else {
    upSince = 0;
    if (!downSince) downSince = now;
  }

  if (!apActive) {
    if (!up && now - downSince >= AP_START_AFTER_MS) startAp();
    return;
  }

  dns.processNextRequest();
  const bool apClients = WiFi.softAPgetStationNum() > 0;
  if (up && now - upSince >= AP_STOP_AFTER_MS && !apClients) {
    stopAp();
  } else if (staConfigured && !WiFi.isConnected() && !apClients && now - lastStaTry >= STA_RETRY_MS) {
    lastStaTry = now;
    WiFi.begin(g_cfg.wifiSsid, g_cfg.wifiPass[0] ? g_cfg.wifiPass : nullptr);
  }
}

bool netLocalFor(const IPAddress &peer, IPAddress &ip, uint8_t mac[6], bool &dhcp) {
  const bool eth = ETH.hasIP(), sta = WiFi.isConnected();
  if (eth && sameSubnet(peer, ETH.localIP(), ETH.subnetMask())) {
    ip = ETH.localIP();
    ETH.macAddress(mac);
    dhcp = g_cfg.eth.dhcp;
  } else if (sta && sameSubnet(peer, WiFi.localIP(), WiFi.subnetMask())) {
    ip = WiFi.localIP();
    WiFi.macAddress(mac);
    dhcp = g_cfg.wifi.dhcp;
  } else if (apActive && sameSubnet(peer, AP_IP, AP_MASK)) {
    ip = AP_IP;
    WiFi.softAPmacAddress(mac);
    dhcp = false;
  } else if (eth) {  // routed peer: answer from the wired side first
    ip = ETH.localIP();
    ETH.macAddress(mac);
    dhcp = g_cfg.eth.dhcp;
  } else if (sta) {
    ip = WiFi.localIP();
    WiFi.macAddress(mac);
    dhcp = g_cfg.wifi.dhcp;
  } else {
    return false;
  }
  return true;
}

void netPrepareScan() {
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_OFF) WiFi.mode(WIFI_STA);
  else if (mode == WIFI_AP) WiFi.mode(WIFI_AP_STA);
}

void netStatusJson(JsonObject out) {
  uint8_t mac[6];

  JsonObject e = out["eth"].to<JsonObject>();
  e["link"] = ETH.linkUp();
  e["connected"] = ETH.hasIP();
  e["speed"] = ETH.linkUp() ? ETH.linkSpeed() : 0;
  e["fullDuplex"] = ETH.linkUp() && ETH.fullDuplex();
  ETH.macAddress(mac);
  e["mac"] = macStr(mac);
  if (ETH.hasIP()) ipJson(e, ETH);

  JsonObject w = out["wifi"].to<JsonObject>();
  w["enabled"] = staConfigured;
  w["connected"] = WiFi.isConnected();
  w["ssid"] = g_cfg.wifiSsid;
  WiFi.macAddress(mac);
  w["mac"] = macStr(mac);
  if (WiFi.isConnected()) {
    w["rssi"] = WiFi.RSSI();
    ipJson(w, WiFi.STA);
  }

  JsonObject a = out["ap"].to<JsonObject>();
  a["active"] = (bool)apActive;
  a["ssid"] = apSsid;
  if (apActive) {
    a["ip"] = AP_IP.toString();
    a["clients"] = WiFi.softAPgetStationNum();
  }
}
