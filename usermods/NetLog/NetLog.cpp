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
#include <esp_system.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

class NetLogUsermod : public Usermod {
  public:
    void setup() override {
      // Print WHY the last boot ended, into the RTC-persisted log. This is the datum that has
      // been missing: after a crash takes the board to WLED-AP, GET /jts/log shows this line and
      // the crash decides the whole direction -- PANIC = a code bug (TLS/CloudLink), TASK_WDT =
      // something blocked too long, BROWNOUT = the USB supply sagged during the crypto (not code
      // at all), SW = an intentional restart, POWERON = a clean cold boot.
      const char* r;
      switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  r = "POWERON (clean cold boot)"; break;
        case ESP_RST_SW:       r = "SW (esp_restart called)";   break;
        case ESP_RST_PANIC:    r = "PANIC (crash/exception)";   break;
        case ESP_RST_INT_WDT:  r = "INT_WDT (interrupt watchdog)"; break;
        case ESP_RST_TASK_WDT: r = "TASK_WDT (task watchdog)";  break;
        case ESP_RST_WDT:      r = "WDT (other watchdog)";      break;
        case ESP_RST_BROWNOUT: r = "BROWNOUT (supply voltage sagged)"; break;
        case ESP_RST_DEEPSLEEP:r = "DEEPSLEEP";                 break;
        case ESP_RST_EXT:      r = "EXT (external reset pin)";  break;
        default:               r = "UNKNOWN";                   break;
      }
      DEBUG_PRINTF_P(PSTR("\n=== JTS BOOT === reset_reason: %s ===\n"), r);
      // JTS-BROWNOUT-OFF (2026-09-22): see the commit for why. Standard ESP32 mitigation for a
      // supply that sags under WiFi TX + crypto; the handshake is the one moment this board
      // draws its peak current, and the reset lands exactly there.
      WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
      DEBUG_PRINTLN(F("=== brownout detector disabled ==="));

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
