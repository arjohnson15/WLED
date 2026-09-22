#include "net_ringlog.h"
#include <esp_attr.h>

// RTC "no-init" storage: survives a software reset / crash-reboot (cleared only by a power cycle).
#define JTS_RTCLOG_SIZE 6144
#define JTS_RTCLOG_MAGIC 0x4A54534Cu   // 'JTSL'

RTC_NOINIT_ATTR static char     s_buf[JTS_RTCLOG_SIZE];
RTC_NOINIT_ATTR static uint32_t s_head;
RTC_NOINIT_ATTR static uint32_t s_count;
RTC_NOINIT_ATTR static uint32_t s_magic;

RingLogClass RingLog;

static inline void ensureInit() {
  // Guard every entry point: RTC RAM is random after a power-on, and static-init order across
  // translation units is undefined, so a DEBUG_PRINT could reach us before any constructor ran.
  if (s_magic != JTS_RTCLOG_MAGIC) { s_head = 0; s_count = 0; s_magic = JTS_RTCLOG_MAGIC; }
}

RingLogClass::RingLogClass() { ensureInit(); }

size_t RingLogClass::write(uint8_t c) {
  ensureInit();
  s_buf[s_head] = (char)c;
  s_head = (s_head + 1) % JTS_RTCLOG_SIZE;
  if (s_count < JTS_RTCLOG_SIZE) s_count++;
  return 1;   // no Serial echo: UART0 TX is GPIO1 = LED3 data on the Dig-Quad
}

size_t RingLogClass::write(const uint8_t *b, size_t len) {
  for (size_t i = 0; i < len; i++) write(b[i]);
  return len;
}

String RingLogClass::dump() {
  ensureInit();
  String out;
  out.reserve(s_count + 1);
  bool full = (s_count == JTS_RTCLOG_SIZE);
  if (full) for (uint32_t i = s_head; i < JTS_RTCLOG_SIZE; i++) out += s_buf[i];
  for (uint32_t i = 0; i < (full ? s_head : s_count); i++) out += s_buf[i];
  return out;
}
