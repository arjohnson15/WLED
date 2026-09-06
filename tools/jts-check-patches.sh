#!/usr/bin/env bash
# Verifies that everything this fork adds on top of upstream WLED is still in place.
#
# Almost all of our work lives in files upstream does not have (usermods/DirectAuth,
# usermods/CloudLink, platformio_override.ini), which a merge cannot touch. Two core files
# ARE edited for branding and the cloud pairing UI; if an upstream change ever rewrites them,
# git may merge cleanly while dropping our block. This script catches that.
#
# Run after every `git merge upstream/main`. CI runs it on every build.
set -uo pipefail
cd "$(dirname "$0")/.."
fail=0
check() {  # check <description> <file> <marker>
  if grep -q -- "$3" "$2" 2>/dev/null; then
    printf '  ok    %s\n' "$1"
  else
    printf '  LOST  %s  (%s: %s)\n' "$1" "$2" "$3"; fail=1
  fi
}

echo "JTS Lights fork integrity:"
check "DirectAuth usermod"        usermods/DirectAuth/DirectAuth.cpp   "REGISTER_USERMOD(directAuthUsermod)"
check "CloudLink usermod"         usermods/CloudLink/CloudLink.cpp     "REGISTER_USERMOD(cloudLinkUsermod)"
check "CloudLink OTA handler"     usermods/CloudLink/CloudLink.cpp     "ota_begin"
check "trusted CA bundle"         usermods/CloudLink/isrg_root_x1.h    "BEGIN CERTIFICATE"
check "house build env"           platformio_override.ini              "\[env:house_esp32\]"
check "usermods in the build"     platformio_override.ini              "DirectAuth CloudLink"
check "build id flag"             platformio_override.ini              "JTS_BUILD"
check "welcome page branding"     wled00/data/welcome.htm              "JTS-BRANDING-START"
check "welcome page branding end" wled00/data/welcome.htm              "JTS-BRANDING-END"
check "cloud section on WiFi page" wled00/data/settings_wifi.htm       "JTS-CLOUD-SECTION-START"
check "cloud section markup end"  wled00/data/settings_wifi.htm        "JTS-CLOUD-SECTION-END"
check "cloud section script"      wled00/data/settings_wifi.htm        "JTS-CLOUD-JS-START"
check "cloud section script end"  wled00/data/settings_wifi.htm        "JTS-CLOUD-JS-END"
check "submit hook wired"         wled00/data/settings_wifi.htm        "clOnSubmit"
check "cloud update on update page" wled00/data/update.htm             "JTS-CLOUD-UPDATE-START"
check "cloud update markup end"   wled00/data/update.htm               "JTS-CLOUD-UPDATE-END"

if [ $fail -ne 0 ]; then
  echo
  echo "Some of this fork's changes are missing. If this follows an upstream merge, the"
  echo "upstream version of that file replaced ours - re-apply the block by hand and commit."
  exit 1
fi
echo "All fork changes present."
