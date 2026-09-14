# Refuse to build this library against a framework package that already ships a TLS layer.
#
# The whole library exists because Tasmota's platform-espressif32 2024.06.00 package
# (arduino-esp32 2.0.18 / esp-idf 4.4.8) was compiled with CONFIG_MBEDTLS_TLS_DISABLED=y,
# so its libmbedtls_2.a / libesp-tls.a / libtcp_transport.a either define no mbedtls_ssl_*
# symbols or were compiled against struct layouts that assume there are none. Linking these
# sources into a package that DOES have TLS (the official espressif32 package, or any IDF5
# package) would produce duplicate or ABI-mismatched definitions, so fail early and loudly.
#
# Two things are checked, both against the package actually selected for this env:
#   1. its sdkconfig.h says CONFIG_MBEDTLS_TLS_DISABLED (the condition this library fixes);
#   2. its mbedtls headers are the version the vendored sources were taken from (2.28.8),
#      because these .c files include library-private headers that must match.
Import("env")

import glob
import os
import re

from click import secho
from SCons.Script import Exit

EXPECTED_MBEDTLS = "2.28.8"

fw_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
mcu = env.BoardConfig().get("build.mcu", "esp32")
sdk_dir = os.path.join(fw_dir or "", "tools", "sdk", mcu)


def fail(msg):
    secho("idf4_tls_from_source: " + msg, fg="red", err=True)
    secho("idf4_tls_from_source: this library is only for Tasmota's IDF 4.4.x package "
          "(CONFIG_MBEDTLS_TLS_DISABLED). Remove it from lib_deps for this env.", fg="red", err=True)
    Exit(1)


if not fw_dir or not os.path.isdir(sdk_dir):
    fail("cannot locate the framework SDK dir (%s)" % sdk_dir)

sdkconfigs = glob.glob(os.path.join(sdk_dir, "**", "sdkconfig.h"), recursive=True)
if not sdkconfigs:
    fail("no sdkconfig.h under %s" % sdk_dir)

tls_disabled = False
for path in sdkconfigs:
    with open(path, encoding="utf-8", errors="replace") as f:
        if re.search(r"^#define CONFIG_MBEDTLS_TLS_DISABLED\b", f.read(), re.M):
            tls_disabled = True
            break
if not tls_disabled:
    fail("the selected framework package has TLS enabled (no CONFIG_MBEDTLS_TLS_DISABLED in %s)"
         % os.path.relpath(sdkconfigs[0], fw_dir))

version_h = os.path.join(sdk_dir, "include", "mbedtls", "mbedtls", "include", "mbedtls", "version.h")
if not os.path.isfile(version_h):
    fail("no mbedtls version.h at %s" % version_h)
with open(version_h, encoding="utf-8", errors="replace") as f:
    m = re.search(r'#define MBEDTLS_VERSION_STRING\s+"([^"]+)"', f.read())
if not m or m.group(1) != EXPECTED_MBEDTLS:
    fail("package mbedtls headers are %s, vendored sources are %s -- re-vendor from the matching "
         "espressif/mbedtls tag before building" % (m.group(1) if m else "unknown", EXPECTED_MBEDTLS))

secho("idf4_tls_from_source: package has CONFIG_MBEDTLS_TLS_DISABLED and mbedtls %s headers; "
      "supplying the TLS layer from source" % EXPECTED_MBEDTLS, fg="yellow")
