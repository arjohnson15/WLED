#pragma once
// JTS: captures WLED_DEBUG output into a RAM ring buffer, served over HTTP by the NetLog usermod
// (usermods/NetLog) at GET /jts/log, instead of requiring a serial cable to read it.
// Selected as DEBUGOUT by wled.h when WLED_NET_RINGLOG is defined. Also echoes to Serial so a
// serial cable, when attached, still sees everything.

#include <Arduino.h>
#include <Print.h>

#ifndef JTS_RINGLOG_SIZE
#define JTS_RINGLOG_SIZE 8192
#endif

class RingLogClass : public Print {
  public:
    size_t write(uint8_t c) override {
      buf[head] = c;
      head = (head + 1) % JTS_RINGLOG_SIZE;
      if (count < JTS_RINGLOG_SIZE) count++;
      else wrapped = true;
      return Serial.write(c);
    }
    size_t write(const uint8_t *b, size_t len) override {
      for (size_t i = 0; i < len; i++) write(b[i]);
      return len;
    }
    // Oldest-to-newest snapshot. Safe to call from the web server task: worst case under a
    // concurrent write is a torn/duplicated byte in a *debug* log, not a crash.
    String dump() {
      String out;
      out.reserve(count + 1);
      if (wrapped) for (size_t i = head; i < JTS_RINGLOG_SIZE; i++) out += (char)buf[i];
      for (size_t i = 0; i < (wrapped ? head : count); i++) out += (char)buf[i];
      return out;
    }
  private:
    uint8_t buf[JTS_RINGLOG_SIZE];
    size_t  head    = 0;
    size_t  count   = 0;
    bool    wrapped = false;
};

extern RingLogClass RingLog;
