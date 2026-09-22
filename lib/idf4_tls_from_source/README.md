# idf4_tls_from_source

The TLS client layer that Tasmota's IDF 4.4.8 Arduino package left out, supplied from source so
`CloudLink` (esp-tls + `esp_transport_ws` over `wss://`) links and can run on that package.

Opt-in only. Used by `[env:house_esp32_v4cloudtest]` in `platformio_override.ini` via
`lib_deps = idf4_tls_from_source`. **Never add it to an env on the official espressif32 package or
any IDF5 package** — those already have TLS and the result would be duplicate or ABI-mismatched
symbols. `jts_guard.py` refuses to build unless the selected framework package has
`CONFIG_MBEDTLS_TLS_DISABLED` and mbedtls 2.28.8 headers.

## What is actually wrong with the package (measured 2026-09-14)

Package: `https://github.com/tasmota/platform-espressif32/releases/download/2024.06.00/platform-espressif32.zip`
→ `framework-arduinoespressif32@src-d3ff28c963a2769eac63b92de1fe648a` (arduino-esp32 2.0.18,
esp-idf 4.4.8, mbedtls **2.28.8** — the official espressif32@6.13 package is the one on 2.28.7).

Tasmota did not delete files; they built IDF with `CONFIG_MBEDTLS_TLS_DISABLED=y` (their
`tools/sdk/esp32/sdkconfig`, and `qio_qspi/include/sdkconfig.h` line 462). `esp_config.h` gates
`MBEDTLS_SSL_TLS_C` on `CONFIG_MBEDTLS_TLS_ENABLED`, so:

| archive | members | exported symbols | meaning |
|---|---|---|---|
| `libmbedtls_2.a` (187 KB) | `ssl_*.c.obj`, `debug`, `mbedtls_debug`, `net_sockets` all present | only `mbedtls_ssl_cache_*`, `mbedtls_ssl_cookie_*`, `mbedtls_net_*` (22) | every `ssl_tls/msg/cli/srv` object compiled to nothing |
| `libmbedcrypto.a` | full member list incl. `ecp`, `ecdh`, `ecdsa`, `gcm`, `pem`, `sha512` | 553, **zero** `mbedtls_ecp_*`/`ecdh_*`/`ecdsa_*`/`gcm_*`/`pem_*`/`sha512_*` | `CONFIG_MBEDTLS_ECP_C`, `GCM_C`, `PEM_PARSE_C`, `SHA512_C` are also unset |
| `libesp-tls.a` | `esp_tls.c`, `esp_tls_mbedtls.c`, … | 35 | intact, but with 19 undefined `mbedtls_ssl_*` references |
| `libmbedx509.a` | intact | 202 | compiled without ECDSA/PEM support |

So "just add `ssl_*.c`" would link but could only negotiate `RSA`/`DHE_RSA` + `AES-CBC` and could
not verify an ECDSA leaf — no current nginx (NPM ships the Mozilla-intermediate list, GCM/ChaCha
only) would complete the handshake. Two more constraints come from the headers:

- **`esp_tls_t` embeds `mbedtls_ssl_context` and `mbedtls_ssl_config` by value** (`esp_tls.h`
  ~line 294) and `esp_tls_conn_read/write` are inline field accessors. Enabling any cert-based key
  exchange adds `sig_hashes` (and `dhm_min_bitlen`, `curve_list`) to `mbedtls_ssl_config`, so the
  precompiled `libesp-tls.a` and `libtcp_transport.a` would address `sockfd`/`read`/`write` at the
  wrong offsets. Both components are therefore recompiled here from IDF v4.4.8 source.
- `pk.c`, `pk_wrap.c`, `pkparse.c`, `oid.c`, `cipher.c`, `cipher_wrap.c`, `x509_crt.c` and
  `constant_time.c` change content with those macros (`mbedtls_pk_info_from_type(ECKEY)` returns
  NULL, `x509_profile_check_key` rejects EC keys, no GCM cipher infos, no `mbedtls_ct_hmac`). They
  are overridden; their symbol sets are strict supersets of the package's, so the package members
  are never pulled.

## What is vendored

Everything is byte-for-byte upstream. Do not edit these files; re-vendor instead.

**mbedtls** — `espressif/mbedtls` @ `4cfd2256ee068ba931a6e332ad7b55639812d225` (= mbedtls 2.28.8),
the exact submodule IDF v4.4.8 pins. Its `include/mbedtls/*.h` are byte-identical to the package's
headers (checked with `diff -rq`); only `bignum.c` and `ecp.c` differ from stock 2.28.8, and
`ecp.c` is one of ours, which is why the fork is the source rather than the Mbed-TLS tarball.

| purpose | files |
|---|---|
| TLS layer that compiled to nothing | `ssl_tls.c` `ssl_msg.c` `ssl_cli.c` `ssl_ciphersuites.c` |
| crypto switched off entirely | `ecp.c` `ecp_curves.c` `ecdh.c` `ecdsa.c` `pem.c` |
| files whose content depends on the macros above (override) | `oid.c` `pk.c` `pk_wrap.c` `pkparse.c` `cipher.c` `cipher_wrap.c` `constant_time.c` `x509_crt.c` |
| library-private headers those need (not shipped in the package) | `common.h` `constant_time_internal.h` `constant_time_invasive.h` `ecp_invasive.h` |
| SHA-384/512 (added 2026-09-22 -- Let's Encrypt's ECDSA chains are signed `ecdsa-with-SHA384`) | `md.c` (the package's copy lacks the SHA-384/512 `md_info` entries) `sha512.c` (only the `mbedtls_sha512_ret` wrappers survive; the body is under the port's ALT) and `port/esp_sha512.c` (IDF v4.4.8 `components/mbedtls/port/sha/parallel_engine/`, the hardware SHA-512 port for classic ESP32; the package's `esp_sha512.c.obj` is empty) |

Deliberately **not** vendored: `gcm.c` — `esp_config.h` sets `MBEDTLS_GCM_ALT` whenever hardware
AES is on and the package exports `esp_aes_gcm_*`, which `gcm_alt.h` maps `mbedtls_gcm_*` onto;
`ssl_srv.c`, `ssl_ticket.c`, `ssl_cache.c`, `ssl_cookie.c`, `debug.c`, `net_sockets.c` (server,
tickets, debug are off; cache/cookie/net_sockets are still exported by the package); `x509.c`
(only reacts to PEM through an `#include`); ~~`md.c`/`sha512.c`~~ -- **now vendored (2026-09-22)**: with SHA-512 off, `mbedtls_oid_get_sig_alg()` fails on
every `ecdsa-with-SHA384` certificate and `ssl_parse_certificate_chain()` silently drops them, which is
what `wled.cloudjohnson.com`'s whole Let's Encrypt ECDSA chain is; see "SHA-384" below.

**esp-idf v4.4.8** — `components/esp-tls/{esp_tls.c, esp_tls_mbedtls.c, esp_tls_error_capture.c,
esp-tls-crypto/esp_tls_crypto.c}` plus the two `private_include` headers, and
`components/tcp_transport/{transport.c, transport_ssl.c, transport_internal.c, transport_ws.c}`
plus `private_include/esp_transport_internal.h` (the package ships the public headers only).
`components/mbedtls/port/sha/parallel_engine/esp_sha512.c` (added 2026-09-22, see SHA-384 below).
`esp_tls.h` in the package is identical to v4.4.8's.

## How it is compiled

Against the **package's** headers and the package's own `mbedtls/esp_config.h` — selected by the
package-wide `-DMBEDTLS_CONFIG_FILE=\"mbedtls/esp_config.h\"` in `tools/sdk/esp32/flags/defines`,
with `mbedtls/port/include` ahead of `mbedtls/mbedtls/include` on the include path exactly as IDF
does it. The only thing this library adds is the set of `CONFIG_MBEDTLS_*` switches in
`library.json` (`build.flags`, so they apply to these translation units only) that `esp_config.h`
consults: TLS client, TLS 1.2, RSA/ECDHE-RSA/ECDHE-ECDSA key exchange, ECP/ECDH/ECDSA with P-256
and P-384, GCM, PEM. Everything else — memory hooks, hardware AES/SHA/MPI, entropy, content length
16384, no `MBEDTLS_HAVE_TIME` — is whatever the package's crypto was built with, which is the
point: our objects must agree with `libmbedcrypto.a`/`libmbedx509.a` on every shared struct.

`src/jts_tls_config_check.c` turns that into `#error`s: header version 0x021C0800, ESP-port config
in use (`MBEDTLS_AES_ALT`, `MBEDTLS_ENTROPY_HARDWARE_ALT`, `esp_mbedtls_mem_calloc`),
`CONFIG_MBEDTLS_TLS_DISABLED` present, the client features on, and the layout-changing features
that must stay off (server, DTLS, renegotiation, ALPN, session tickets, TLS 1.0/1.1, SHA-512,
PSA, threading, debug) actually off.

`libArchive: false` is load-bearing: as loose objects our definitions precede every `-l` archive
on the link line, so the linker never opens a package member for a symbol we define. As an archive
the package's `libmbedx509.a(x509_crt.c.obj)` would be pulled first and the link would fail with
duplicate definitions.

## Verification record (2026-09-14, `pio run -e house_esp32_v4cloudtest`)

- Links. `Flash: 83.8% (1,317,633 of 1,572,864 bytes)` on `default_partitions`, `RAM: 24.3%`.
  Previously this env failed at link with ~40 `undefined reference to mbedtls_ssl_*`.
- 26 objects compiled from this library, 0 warnings. Cost: 78,896 bytes of `.text` before
  `--gc-sections` (`xtensa-esp32-elf-size -t` over the objects, an upper bound), about 55 KB kept
  according to the link map; 96 bytes of static RAM. The TLS I/O buffers (2 x 16 KB) are heap,
  allocated per connection, same as on the official package.
- Include trace (`-H` on the real compile command from `pio run -t compiledb`): `mbedtls/version.h`
  → `mbedtls/port/include/mbedtls/esp_config.h` → `dio_qspi/include/sdkconfig.h` →
  `mbedtls/mbedtls/include/mbedtls/config.h` → `check_config.h`, every one under
  `framework-arduinoespressif32@src-d3ff28c963a2769eac63b92de1fe648a/tools/sdk/esp32/`; no header
  opened from anywhere else.
- Guard negative test: adding `idf4_tls_from_source` to `house_esp32_v4cloudtest_official`
  (espressif32@6.13, TLS present) fails in 11 s with "the selected framework package has TLS
  enabled (no CONFIG_MBEDTLS_TLS_DISABLED in tools/sdk/esp32/qio_qspi/include/sdkconfig.h)" before
  a single file is compiled.
- `libmbedx509.a(certs.c.obj)` (mbedtls test certificates) is extracted from the archive because the
  package's own `x509.c.obj` self-test code references it; `--gc-sections` then discards all of it
  (0 bytes kept). Pre-existing, not caused by this library.
- From the link map: `mbedtls_ssl_handshake/setup/config_defaults` → `ssl_tls.c.o`, `ssl_read/write`
  → `ssl_msg.c.o`, `mbedtls_ecp_mul` → `ecp.c.o`, `mbedtls_ecdh_calc_secret` → `ecdh.c.o`,
  `mbedtls_ecdsa_read_signature` → `ecdsa.c.o`, `mbedtls_x509_crt_parse/verify_restartable` →
  `x509_crt.c.o`, `mbedtls_pk_parse_subpubkey` → `pkparse.c.o`, `mbedtls_pk_info_from_type` →
  `pk.c.o`, `mbedtls_oid_get_sig_alg` → `oid.c.o`, `mbedtls_cipher_info_from_type` /
  `cipher_auth_decrypt_ext` → `cipher.c.o`, `mbedtls_ct_hmac` → `constant_time.c.o`,
  `mbedtls_pem_read_buffer` → `pem.c.o`, `esp_tls_init/conn_new_async` → `esp_tls.c.o`,
  `esp_mbedtls_handshake` → `esp_tls_mbedtls.c.o`, `esp_transport_init/ssl_init/ws_init` → our
  transport objects.
- Still from the package, as intended: `esp_aes_gcm_*` (`libmbedcrypto.a(esp_aes_gcm.c.obj)`),
  `mbedtls_rsa_*`, `mbedtls_mpi_exp_mod` (`esp_bignum.c.obj`), `mbedtls_md_*`, `mbedtls_sha256_*`,
  `mbedtls_x509_get_name` (`x509.c.obj`), `mbedtls_net_recv` (`libmbedtls_2.a(net_sockets.c.obj)`).
- The **only** member pulled from `libmbedtls_2.a`/`libesp-tls.a`/`libtcp_transport.a` is
  `net_sockets.c.obj`; no member of `libmbedcrypto.a`/`libmbedx509.a` for any overridden file;
  each checked symbol is defined exactly once in the ELF.

**Not verified: runtime.** There is no ESP32 on the build host. The first thing to test on hardware
is a pairing/relay cycle against `wled.cloudjohnson.com`, then watch heap: TLS 1.2 with 16 KB I/O
buffers costs the same as it did on the official package. Known behavioural differences from that
package, inherited from Tasmota's crypto build and not fixable here without also overriding the
package's `libmbedx509.a`: certificate validity dates are **not** checked (`MBEDTLS_HAVE_TIME` off).

**SHA-384 (2026-09-22).** The sentence that used to be here -- "there is no SHA-384/512 ... fine for
Let's Encrypt chains" -- was the bug behind six days of `connect failed`. Let's Encrypt's ECDSA
hierarchy (leaf, the `YE2` intermediate, `Root YE`, chained to `ISRG Root X2`) is signed
`ecdsa-with-SHA384`. Without `MBEDTLS_SHA512_C`, `mbedtls_oid_get_sig_alg()` returns
`MBEDTLS_ERR_OID_NOT_FOUND` for each of them and `ssl_parse_certificate_chain()` (ssl_tls.c) drops the
certificate on exactly that error (-0x262E) and carries on, so the only certificate left to verify
was the cross-signed `ISRG Root X2`: it validates against the pinned X1 and then fails the hostname
check -- `-0x2700`, flags `0x4` (`BADCERT_CN_MISMATCH`), `CN=ISRG Root X2` at depth 0. That exact
output was reproduced on the build host with stock mbedtls 2.28.8 (`scripts/config.py unset
MBEDTLS_SHA512_C`) and the server's real chain, and passes with SHA-512 on. `jts_tls_config_check.c`
now requires `MBEDTLS_SHA512_C` and the parallel-engine ALT. A DMA-SHA chip (S2/S3/C3) would need
`port/sha/dma/esp_sha512.c` instead of the parallel-engine file vendored here.

## Re-vendoring when the package moves

1. Read `MBEDTLS_VERSION_STRING` from the new package's `tools/sdk/esp32/include/mbedtls/mbedtls/include/mbedtls/version.h`.
2. Get IDF's pinned submodule: `https://api.github.com/repos/espressif/esp-idf/contents/components/mbedtls/mbedtls?ref=<idf tag>` → `sha`, then `https://github.com/espressif/mbedtls/archive/<sha>.tar.gz`.
3. `diff -rq` its `include/mbedtls` against the package's headers — must be empty.
4. Copy the same file list from `library/`; copy esp-tls and tcp_transport sources from the matching esp-idf tag.
5. Update `EXPECTED_MBEDTLS` in `jts_guard.py`, the version number in `jts_tls_config_check.c`, and this file.
6. Rebuild and repeat the map check above (`grep -oE "lib(mbedtls_2|esp-tls|tcp_transport)\.a\([a-z_0-9]+\.c\.obj\)" firmware.map | sort -u` should print only `net_sockets`).

Licences: mbedtls files are Apache-2.0 OR GPL-2.0-or-later (`src/mbedtls/LICENSE`); the esp-idf
files are Apache-2.0 (SPDX headers in each file).
