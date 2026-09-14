# What this fork changes on top of upstream WLED

**Base: upstream tag `v16.0.1`** (branch `v16-house`, created 2026-09-14), built on WLED's default
ESP32 platform (`esp32_idf_V4` = Tasmota platform-espressif32 2024.06.00, arduino-esp32 2.0.18 /
esp-idf 4.4.8). That is the exact combination QuinLED's own released Dig-Quad firmware is built
on, and the one confirmed on Andrew's hardware to drive all four outputs correctly. The fork
previously tracked upstream `main` (17.0.0-devV5), which had a WS2812/RMT bug on IDF5 and WiFi
problems on IDF4; two days were lost to that, so the base is now a **release tag**, not `main`.

Keeping this list short is what makes moving to the next release tag cheap. Run
`tools/jts-check-patches.sh` after every upstream merge; CI runs it on every build.

## Files upstream does not have (a merge cannot touch these)

| Path | What |
|---|---|
| `usermods/DirectAuth/` | login page and session tokens in front of the whole HTTP/WS surface |
| `usermods/CloudLink/` | outbound link to the JTS Lights cloud: pairing, API relay, live LEDs, firmware updates |
| `platformio_override.ini` | the `house_esp32` build environment (gitignored upstream, force-added here) |
| `.github/workflows/jts-firmware.yml` | build, weekly upstream sync, publish to the cloud |
| `tools/jts-check-patches.sh` | this integrity check |
| `tools/jts-merge-factory.py` | builds `firmware.factory.bin` (whole-flash USB image) after every build |
| `PATCHES.md` | this file |

## Core files we do edit (the only merge risk)

Each change is wrapped in `JTS-…-START` / `JTS-…-END` comments so it is easy to find and
re-apply if an upstream merge drops it.

| File | Change |
|---|---|
| `wled00/data/welcome.htm` | "Welcome to JTS Lights!", tagline, and the link to wled.cloudjohnson.com |
| `wled00/data/settings_wifi.htm` | the "JTS Lights cloud" section (server, pairing code, TLS) plus its script, and `onsubmit="return clOnSubmit(event)"` on the form |
| `wled00/data/update.htm` | an "Update from JTS Lights cloud" button above Manual upload, which asks the cloud to push the newest build |
| `wled00/data/settings_leds.htm` | a "Controller board" picker that fills in the data, button and relay pins for known boards |
| `wled00/network.cpp` | `WiFiEvent()` `AP_STADISCONNECTED`: `apClients` is decremented only when above zero, so an unmatched disconnect can no longer underflow it to 255 and stop the board ever re-attempting its STA connection |

If an upstream release rewrites any of these, git may merge without a conflict yet drop our
block. That is exactly what the check script catches.

## What was re-applied for the v16.0.1 base (2026-09-14), and what was dropped

Everything above was carried over from the fork's previous `main` (17-dev based) by applying the
fork-point→main diff for each file onto v16.0.1's version:

- `settings_wifi.htm`, `update.htm`, `settings_leds.htm`: applied cleanly, no changes needed.
- `welcome.htm`: the branding block applied cleanly; the dev-banner removal in the same file was
  dropped (see below).
- `json.cpp` (`JTS-WIFI-SCAN-FIX`): v16.0.1's `serializeNetworks()` has the identical
  retry-on-`WIFI_SCAN_FAILED` shape, so the block applied as written. The only conflict was
  upstream 17-dev's `WiFi.setBandMode()` lines immediately above it, which do not exist in
  v16.0.1 and were not brought over.
- `network.cpp` (`JTS-APCLIENTS-FIX`): **new on this branch.** v16.0.1 still has
  `if (--apClients == 0 && isWiFiConfigured()) forceReconnect = true;`. `apClients` is a `byte`,
  so one unmatched AP-disconnect event wraps it to 255 and "no clients left" is never true again:
  the controller sits on its setup AP and never joins WiFi. Upstream fixed this on `main` after
  v16.0.1 as `if (apClients > 0) apClients--;` followed by the zero check; that exact form is
  cherry-picked here. Directly implicated in the "falls back to WLED-AP and never joins" symptom.
- **Dropped: the `JTS-NO-DEVBANNER` patches** to `index.htm`, `welcome.htm`, `common.js` and
  `index.css`. Those removed upstream's unconditional "DEVELOPMENT BUILD — NOT FOR PRODUCTION
  USE" banner, which was added on 17-dev and **does not exist in v16.0.1** (no `devBanner`, no
  `--dbh` anywhere in `wled00/data`). There is nothing to remove, so the checks for those markers
  were deleted from `tools/jts-check-patches.sh` rather than faking markers in untouched files.
  If the fork ever moves to a release that carries the banner, the four blocks are in the old
  `main` history (`PATCHES.md` there describes them).
- The usermods needed **one** API change for v16.0.1: `WLEDNetwork.localIP()` in
  `usermods/CloudLink/CloudLink.cpp` (`addIdentityFields()`) is 17-dev's name for the network
  object; v16.0.1 calls it `Network` (`wled00/src/dependencies/network/Network.h`, the same object
  `WLED_CONNECTED` expands to). DirectAuth compiled and linked with no changes. Both usermods
  already carried `ESP_IDF_VERSION` / `MBEDTLS_VERSION_NUMBER` guards for the IDF4 platform from
  the 2026-09-12 work on the old `main`, so the platform itself caused no drift.
- DirectAuth is the current `main` version including `0a57ee87` ("let first-time WiFi setup
  through the gate on the setup AP"): while no credentials are set and the setup AP is up,
  `/`, `/welcome`, `/settings`, `/settings/wifi`, `/settings/s.js`, `/json/net` and `/skin.css`
  pass the gate, so the WiFi-settings Save POST is no longer answered 401 during first-time
  setup. Those are the paths v16.0.1's own `settings_wifi.htm` fetches (`/settings/s.js?p=1`,
  `/json/net`), so the list is right for this base too.

## TLS on this platform

The Tasmota platform build's precompiled libs ship **no `mbedtls_ssl_*`**, so `house_esp32`
(with CloudLink) compiles but does not link until a TLS library is supplied — that is being
produced separately. `house_esp32_noauth_test` (DirectAuth only) is the link-testable variant
in the meantime. Crypto primitives (SHA-256, PBKDF2, base64) are present, which is all
DirectAuth needs.

## Things to fix before `v16-house` replaces `main`

- `.github/workflows/jts-firmware.yml` was copied unchanged. It triggers on pushes to `main`
  only, and its weekly `sync` job merges **`upstream/main`** — on this branch that would pull
  17-dev straight back in. Before this branch becomes the shipping one, point the sync at the
  latest upstream release **tag** (or turn it off) and add the branch to `on.push.branches`.
- `WLED_DISABLE_*` flags used by `house_esp32` all exist in v16.0.1 (checked with grep); none
  were dropped.

## Why no other core edits

DirectAuth and CloudLink need no core edits: DirectAuth works because usermod `setup()` runs
before `initServer()`, so its handler is first in the chain; CloudLink calls WLED's own
serializers. That is why the merge surface stays this small — keep it that way.

## After a merge

```bash
git -C firmware fetch upstream --tags
git -C firmware merge v16.x.y            # the next release tag, not upstream/main
./tools/jts-check-patches.sh             # must pass
npm ci && npm run build                  # regenerate the web UI headers
pio run -e house_esp32                   # must fit the 1.8 MB slot (see platformio_override.ini)
```

## Removed on purpose: the `json.cpp` WiFi-scan patch (2026-09-14)

`serializeNetworks()` briefly carried a `WiFi.disconnect(false)` before retrying a failed scan.
It was written for "scan stays broken after a failed first-time WiFi setup" — whose real cause
was DirectAuth's gate 401-ing the setup Save (fixed in the usermod). On an unconfigured board the
WiFi settings page scans as soon as it opens, and that disconnect() can raise a station-
disconnected event that 16.0.1's `WiFiEvent()` answers with a full driver teardown
(`WiFi.mode(WIFI_MODE_NULL)` in `initConnection()`), dropping the setup AP and the phone with it.
The WiFi path of this fork is now byte-identical to stock 16.0.1 plus upstream's `apClients`
fix; keep it that way.
