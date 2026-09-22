#pragma once
// JTS: captures WLED_DEBUG output into a ring buffer served over HTTP by the NetLog usermod
// (usermods/NetLog) at GET /jts/log, instead of requiring a serial cable to read it.
// Selected as DEBUGOUT by wled.h when WLED_NET_RINGLOG is defined. Also echoes to Serial.
//
// The buffer lives in RTC "no-init" memory (2026-09-22): it is NOT cleared on a software reset,
// so after the board crashes and reboots, GET /jts/log still shows the lines from just before
// the crash -- the only way to see what happened when the crash takes the board off WiFi and
// back onto WLED-AP, with no serial cable and no LAN access. A power cycle DOES clear it (RTC
// RAM loses state without power), which is why a magic word guards against reading garbage.

#include <Arduino.h>
#include <Print.h>

class RingLogClass : public Print {
  public:
    RingLogClass();
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *b, size_t len) override;
    String dump();          // oldest-to-newest snapshot
};

extern RingLogClass RingLog;
