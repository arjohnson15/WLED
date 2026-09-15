/*
 * NetLog usermod
 *
 * Serves the WLED_DEBUG output ring buffer (wled00/net_ringlog.h) over HTTP, for a diagnostic
 * host with no serial cable attached and no network route straight to the device -- fetch the
 * log through the LAN, or through the cloud's existing HTTP relay once CloudLink is connected.
 *
 * Only built into WLED_NET_RINGLOG environments (the ring buffer does not exist otherwise).
 * Goes through DirectAuth's normal login gate like any other page; no separate auth of its own.
 *
 *   GET /jts/log            the buffered log, oldest to newest, as plain text
 */

#include "wled.h"

#ifndef WLED_NET_RINGLOG
  #error "NetLog requires WLED_NET_RINGLOG (see platformio_override.ini)"
#endif

#include "net_ringlog.h"

class NetLogUsermod : public Usermod {
  public:
    void setup() override {
      server.on("/jts/log", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, F("text/plain"), RingLog.dump());
      });
    }
    void loop() override {}
    static const char _name[];
};

const char NetLogUsermod::_name[] PROGMEM = "NetLog";

static NetLogUsermod netLogUsermod;
REGISTER_USERMOD(netLogUsermod);
