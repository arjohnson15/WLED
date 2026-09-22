// JTS: this library's mbedtls config = the package's own esp_config.h, with one change.
// Selected for the library's sources only, via -DMBEDTLS_CONFIG_FILE in library.json (a later -D
// on the command line overrides the framework's); jts_tls_config_check.c fails the build if it
// did not take effect.
#pragma once
#include "mbedtls/esp_config.h"

// SHA-512/384 in software (2026-09-22). esp_config.h selects the hardware port (MBEDTLS_SHA512_ALT)
// whenever CONFIG_MBEDTLS_HARDWARE_SHA is on, but the package's esp_sha512.c object is empty, so
// the port would have to be vendored -- and its first-ever run on this hardware would be a live
// TLS handshake against the cloud, on a controller that reboots a few seconds after joining WiFi
// (release 75). The software implementation in sha512.c is the exact code the host reproduction
// ran the real certificate chain through. Certificates are small; speed is irrelevant here.
#undef MBEDTLS_SHA512_ALT
