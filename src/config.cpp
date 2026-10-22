#include "config.h"

#include <Preferences.h>

Config g_cfg;

static const char *NVS_NS = "dmxrelay";
static const char *NVS_KEY = "cfg";

static const char *MERGE_NAMES[] = {"htp", "ltp", "dmx", "artnet"};
static const char *LOSS_NAMES[] = {"hold", "off"};

const char *mergeName(MergeMode m) { return MERGE_NAMES[m]; }
const char *lossName(LossAction l) { return LOSS_NAMES[l]; }

void configDefaults(Config &cfg) {
  cfg = Config();
  strlcpy(cfg.hostname, "dmx-relay", sizeof(cfg.hostname));
  cfg.wifiSsid[0] = 0;
  cfg.wifiPass[0] = 0;
  for (int i = 0; i < RELAY_COUNT; i++) {
    cfg.io.map[i] = i + 1;
    snprintf(cfg.relayNames[i], sizeof(cfg.relayNames[i]), "Relay %d", i + 1);
  }
}

// ─── JSON helpers ──────────────────────────────────────────────────────

static String ipStr(const IPAddress &ip) { return ip == IPAddress() ? String() : ip.toString(); }

static void ipToJson(const IpSettings &s, JsonObject o) {
  o["dhcp"] = s.dhcp;
  o["ip"] = ipStr(s.ip);
  o["mask"] = ipStr(s.mask);
  o["gw"] = ipStr(s.gw);
  o["dns"] = ipStr(s.dns);
}

static bool parseIp(JsonVariantConst v, IPAddress &out, bool required, const char *field, String &err) {
  if (v.isNull()) return true;
  const char *s = v.as<const char *>();
  if (!s || !*s) {
    if (required) {
      err = String(field) + " is required";
      return false;
    }
    out = IPAddress();
    return true;
  }
  IPAddress ip;
  if (!ip.fromString(s)) {
    err = String(field) + " is not a valid IPv4 address";
    return false;
  }
  out = ip;
  return true;
}

static bool ipFromJson(IpSettings &s, JsonObjectConst o, const char *label, String &err) {
  if (o.isNull()) return true;
  if (o["dhcp"].is<bool>()) s.dhcp = o["dhcp"];
  String l(label);
  if (!parseIp(o["ip"], s.ip, !s.dhcp, (l + " IP address").c_str(), err)) return false;
  if (!parseIp(o["mask"], s.mask, !s.dhcp, (l + " subnet mask").c_str(), err)) return false;
  if (!parseIp(o["gw"], s.gw, false, (l + " gateway").c_str(), err)) return false;
  if (!parseIp(o["dns"], s.dns, false, (l + " DNS").c_str(), err)) return false;
  if (!s.dhcp && (s.ip == IPAddress() || s.mask == IPAddress())) {
    err = l + " static IP needs an address and subnet mask";
    return false;
  }
  return true;
}

static bool ipEqual(const IpSettings &a, const IpSettings &b) {
  return a.dhcp == b.dhcp && a.ip == b.ip && a.mask == b.mask && a.gw == b.gw && a.dns == b.dns;
}

static bool readRanged(JsonVariantConst v, long lo, long hi, long &out, const char *field, String &err) {
  if (v.isNull()) return false;
  if (!v.is<long>()) {
    err = String(field) + " must be a number";
    return false;
  }
  long n = v.as<long>();
  if (n < lo || n > hi) {
    err = String(field) + " must be between " + lo + " and " + hi;
    return false;
  }
  out = n;
  return true;
}

static int indexOf(const char *const *names, int count, const char *s) {
  if (!s) return -1;
  for (int i = 0; i < count; i++)
    if (strcmp(names[i], s) == 0) return i;
  return -1;
}

static bool validHostname(const char *h) {
  size_t n = strlen(h);
  if (n == 0 || n > 31 || h[0] == '-' || h[n - 1] == '-') return false;
  for (size_t i = 0; i < n; i++) {
    char c = h[i];
    if (!(isdigit(c) || (c >= 'a' && c <= 'z') || c == '-')) return false;
  }
  return true;
}

void configToJson(const Config &cfg, JsonObject out, bool includeSecrets) {
  out["hostname"] = cfg.hostname;
  ipToJson(cfg.eth, out["eth"].to<JsonObject>());

  JsonObject w = out["wifi"].to<JsonObject>();
  w["enabled"] = cfg.wifiEnabled;
  w["ssid"] = cfg.wifiSsid;
  if (includeSecrets) w["pass"] = cfg.wifiPass;
  else w["hasPass"] = cfg.wifiPass[0] != 0;
  ipToJson(cfg.wifi, w);

  JsonObject d = out["dmx"].to<JsonObject>();
  d["enabled"] = cfg.io.dmxEnabled;
  d["start"] = cfg.io.dmxStart;

  JsonObject a = out["artnet"].to<JsonObject>();
  a["enabled"] = cfg.io.artEnabled;
  a["universe"] = cfg.io.artUniverse;
  a["start"] = cfg.io.artStart;

  JsonObject r = out["relays"].to<JsonObject>();
  r["threshold"] = cfg.io.threshold;
  r["merge"] = mergeName(cfg.io.merge);
  r["loss"] = lossName(cfg.io.loss);
  r["lossTimeout"] = cfg.io.lossTimeoutMs / 1000.0f;
  JsonArray map = r["map"].to<JsonArray>();
  JsonArray names = r["names"].to<JsonArray>();
  for (int i = 0; i < RELAY_COUNT; i++) {
    map.add(cfg.io.map[i]);
    names.add(cfg.relayNames[i]);
  }
}

bool configFromJson(Config &cfg, JsonObjectConst in, String &err, bool &networkChanged) {
  Config c = cfg;  // validate into a copy so a bad request changes nothing
  long n;

  if (in["hostname"].is<const char *>()) {
    String h = in["hostname"].as<String>();
    h.trim();
    h.toLowerCase();
    if (!validHostname(h.c_str())) {
      err = "Hostname may only use a-z, 0-9 and '-' (max 31 characters)";
      return false;
    }
    strlcpy(c.hostname, h.c_str(), sizeof(c.hostname));
  }

  if (!ipFromJson(c.eth, in["eth"], "Ethernet", err)) return false;

  JsonObjectConst w = in["wifi"];
  if (!w.isNull()) {
    if (w["enabled"].is<bool>()) c.wifiEnabled = w["enabled"];
    if (w["ssid"].is<const char *>()) strlcpy(c.wifiSsid, w["ssid"], sizeof(c.wifiSsid));
    if (w["pass"].is<const char *>()) {
      const char *p = w["pass"];
      size_t len = strlen(p);
      if (len > 0 && (len < 8 || len > 63)) {
        err = "Wi-Fi password must be 8-63 characters (or empty for an open network)";
        return false;
      }
      strlcpy(c.wifiPass, p, sizeof(c.wifiPass));
    }
    if (!ipFromJson(c.wifi, w, "Wi-Fi", err)) return false;
    if (c.wifiEnabled && !c.wifiSsid[0]) {
      err = "Enter a Wi-Fi network name or disable Wi-Fi";
      return false;
    }
  }

  JsonObjectConst d = in["dmx"];
  if (!d.isNull()) {
    if (d["enabled"].is<bool>()) c.io.dmxEnabled = d["enabled"];
    if (readRanged(d["start"], 1, RANGE_MAX_START, n, "DMX start address", err)) c.io.dmxStart = n;
    if (err.length()) return false;
  }

  JsonObjectConst a = in["artnet"];
  if (!a.isNull()) {
    if (a["enabled"].is<bool>()) c.io.artEnabled = a["enabled"];
    if (readRanged(a["universe"], 0, 32767, n, "Art-Net universe", err)) c.io.artUniverse = n;
    if (err.length()) return false;
    if (readRanged(a["start"], 1, RANGE_MAX_START, n, "Art-Net start channel", err)) c.io.artStart = n;
    if (err.length()) return false;
  }

  JsonObjectConst r = in["relays"];
  if (!r.isNull()) {
    if (readRanged(r["threshold"], 1, 255, n, "Threshold", err)) c.io.threshold = n;
    if (err.length()) return false;
    if (!r["merge"].isNull()) {
      int m = indexOf(MERGE_NAMES, 4, r["merge"]);
      if (m < 0) {
        err = "Unknown merge mode";
        return false;
      }
      c.io.merge = (MergeMode)m;
    }
    if (!r["loss"].isNull()) {
      int l = indexOf(LOSS_NAMES, 2, r["loss"]);
      if (l < 0) {
        err = "Unknown signal-loss action";
        return false;
      }
      c.io.loss = (LossAction)l;
    }
    if (!r["lossTimeout"].isNull()) {
      float s = r["lossTimeout"].as<float>();
      if (!(s >= 1 && s <= 60)) {
        err = "Signal-loss timeout must be 1-60 seconds";
        return false;
      }
      c.io.lossTimeoutMs = (uint16_t)(s * 1000);
    }
    JsonArrayConst map = r["map"];
    if (!map.isNull()) {
      if (map.size() != RELAY_COUNT) {
        err = "Relay map must have 8 entries";
        return false;
      }
      for (int i = 0; i < RELAY_COUNT; i++) {
        if (!readRanged(map[i], 0, RELAY_COUNT, n, "Relay channel", err)) {
          if (!err.length()) err = "Relay channel missing";
          return false;
        }
        c.io.map[i] = n;
      }
    }
    JsonArrayConst names = r["names"];
    if (!names.isNull()) {
      for (int i = 0; i < RELAY_COUNT && i < (int)names.size(); i++) {
        if (names[i].is<const char *>()) strlcpy(c.relayNames[i], names[i], sizeof(c.relayNames[i]));
      }
    }
  }

  networkChanged = strcmp(c.hostname, cfg.hostname) != 0 || !ipEqual(c.eth, cfg.eth) ||
                   c.wifiEnabled != cfg.wifiEnabled || strcmp(c.wifiSsid, cfg.wifiSsid) != 0 ||
                   strcmp(c.wifiPass, cfg.wifiPass) != 0 || !ipEqual(c.wifi, cfg.wifi);
  cfg = c;
  return true;
}

// ─── NVS ───────────────────────────────────────────────────────────────

void configLoad(Config &cfg) {
  configDefaults(cfg);
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return;  // namespace doesn't exist yet: first boot
  String raw = prefs.getString(NVS_KEY, "");
  prefs.end();
  if (!raw.length()) return;

  JsonDocument doc;
  if (deserializeJson(doc, raw)) {
    log_w("Stored config is corrupt, using defaults");
    return;
  }
  String err;
  bool changed;
  if (!configFromJson(cfg, doc.as<JsonObjectConst>(), err, changed)) {
    log_w("Stored config rejected (%s), using defaults", err.c_str());
    configDefaults(cfg);
  }
}

void configSave(const Config &cfg) {
  JsonDocument doc;
  configToJson(cfg, doc.to<JsonObject>(), true);
  String raw;
  serializeJson(doc, raw);
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putString(NVS_KEY, raw);
  prefs.end();
}

void configErase() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.clear();
  prefs.end();
}
