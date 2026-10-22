// Ethernet (W5500, PoE), Wi-Fi station, fallback setup access point and mDNS.
#pragma once

#include <ArduinoJson.h>
#include <IPAddress.h>
#include "config.h"

void netBegin(const Config &cfg);
void netLoop();
void netStatusJson(JsonObject out);

bool netHasIp();
bool netApActive();
// The local address/MAC on the interface that reaches `peer`, for Art-Net replies.
bool netLocalFor(const IPAddress &peer, IPAddress &ip, uint8_t mac[6], bool &dhcp);
// Makes sure the Wi-Fi radio is in a mode that can scan.
void netPrepareScan();
