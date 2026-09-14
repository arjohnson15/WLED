/*
 * Compile-time proof that this library is being built the only way it is correct to build it.
 *
 * Everything here compiles against the framework package's own mbedtls headers and its
 * "mbedtls/esp_config.h" (selected by the package-wide -DMBEDTLS_CONFIG_FILE), with the
 * CONFIG_MBEDTLS_* switches in library.json turning the TLS client back on. If any of these
 * checks fire, the objects produced would not match the precompiled libmbedcrypto.a /
 * libmbedx509.a they link against, or would be missing functions the ssl layer calls.
 */
#include "mbedtls/version.h"
#include "mbedtls/ssl.h"
#include "sdkconfig.h"

/* 1. The headers must be the exact version the vendored .c files (and their private headers)
 *    were taken from: espressif/mbedtls @ 4cfd2256 == mbedtls 2.28.8, IDF v4.4.8's submodule. */
#if MBEDTLS_VERSION_NUMBER != 0x021C0800
#error "idf4_tls_from_source: package mbedtls headers are not 2.28.8; re-vendor the sources to match"
#endif

/* 2. The config must be the ESP port's esp_config.h, not mbedtls' stock config.h.
 *    Only the port defines these (hardware AES/SHA/entropy, esp_mbedtls_mem_calloc). */
#if !defined(MBEDTLS_CONFIG_FILE) || !defined(MBEDTLS_AES_ALT) || !defined(MBEDTLS_ENTROPY_HARDWARE_ALT) \
    || !defined(MBEDTLS_PLATFORM_STD_CALLOC)
#error "idf4_tls_from_source: not compiled against the package's mbedtls/esp_config.h"
#endif

/* 3. This library only makes sense where the package compiled TLS out. */
#if !defined(CONFIG_MBEDTLS_TLS_DISABLED)
#error "idf4_tls_from_source: the framework package already has TLS; do not link this library"
#endif

/* 4. The switches in library.json must have taken effect: a TLS 1.2 client with ECDHE/ECDSA. */
#if !defined(MBEDTLS_SSL_TLS_C) || !defined(MBEDTLS_SSL_CLI_C) || !defined(MBEDTLS_SSL_PROTO_TLS1_2) \
    || !defined(MBEDTLS_ECP_C) || !defined(MBEDTLS_ECDH_C) || !defined(MBEDTLS_ECDSA_C) \
    || !defined(MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED) || !defined(MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED) \
    || !defined(MBEDTLS_KEY_EXCHANGE_RSA_ENABLED) || !defined(MBEDTLS_PEM_PARSE_C) || !defined(MBEDTLS_GCM_C)
#error "idf4_tls_from_source: TLS client features not enabled; check the -DCONFIG_MBEDTLS_* flags in library.json"
#endif

/* 5. Things that must stay OFF because the precompiled crypto/x509 objects were built without
 *    them, or because turning them on would change struct layouts shared with the package. */
#if defined(MBEDTLS_SSL_SRV_C) || defined(MBEDTLS_SSL_PROTO_DTLS) || defined(MBEDTLS_SSL_RENEGOTIATION) \
    || defined(MBEDTLS_SSL_ALPN) || defined(MBEDTLS_SSL_SESSION_TICKETS) || defined(MBEDTLS_SSL_TICKET_C) \
    || defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1) || defined(MBEDTLS_SHA512_C) \
    || defined(MBEDTLS_USE_PSA_CRYPTO) || defined(MBEDTLS_THREADING_C) || defined(MBEDTLS_DEBUG_C)
#error "idf4_tls_from_source: a config switch is on that the vendored file set does not cover"
#endif

/* 6. GCM must come from the port's hardware-AES implementation (esp_aes_gcm_*, exported by the
 *    package's libmbedcrypto.a), which gcm.h maps mbedtls_gcm_* onto. gcm.c is deliberately not
 *    vendored; if MBEDTLS_GCM_ALT ever disappears this must be revisited. */
#if !defined(MBEDTLS_GCM_ALT)
#error "idf4_tls_from_source: MBEDTLS_GCM_ALT is not set; gcm.c would need to be vendored"
#endif

/* Give the object a symbol so the linker has nothing to complain about. */
const char idf4_tls_from_source_id[] = "idf4_tls_from_source mbedtls " MBEDTLS_VERSION_STRING;
