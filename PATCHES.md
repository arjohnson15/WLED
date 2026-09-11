# What this fork changes on top of upstream WLED

Keeping this list short is what makes `git merge upstream/main` cheap. Run
`tools/jts-check-patches.sh` after every merge; CI runs it on every build and on every
automated upstream sync, and refuses to push a sync that loses any of it.

## Files upstream does not have (a merge cannot touch these)

| Path | What |
|---|---|
| `usermods/DirectAuth/` | login page and session tokens in front of the whole HTTP/WS surface |
| `usermods/CloudLink/` | outbound link to the JTS Lights cloud: pairing, API relay, live LEDs, firmware updates |
| `platformio_override.ini` | the `house_esp32` build environment (gitignored upstream, force-added here) |
| `.github/workflows/jts-firmware.yml` | build, weekly upstream sync, publish to the cloud |
| `tools/jts-check-patches.sh` | this integrity check |
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
| `wled00/data/index.htm` | upstream's "DEVELOPMENT BUILD — NOT FOR PRODUCTION USE" banner div removed |
| `wled00/data/welcome.htm` | (same file as above) the banner div and its inline style block removed too |
| `wled00/data/common.js` | the banner's dynamic injector (runs on every settings page) disabled with an early `return` |
| `wled00/data/index.css` | `--dbh` (banner height) set to `0px` so nothing that positions off it leaves a gap |
| `wled00/json.cpp` | `serializeNetworks()` abandons a stuck WiFi connect attempt (`WiFi.disconnect(false)`) before retrying a failed scan, instead of retrying forever against a radio that is still mid-connect |

If an upstream release rewrites either file, git may merge without a conflict yet drop our
block. That is exactly what the check script catches.

## Why the development-build banner is gone

Upstream ships an unconditional "⚠ DEVELOPMENT BUILD — WORK IN PROGRESS — NOT FOR PRODUCTION
USE" banner, injected both as a static div (`index.htm`, `welcome.htm`) and dynamically by
`common.js` on every settings page. It is not gated by version or build flag — it always shows.
This fork runs as a real product in Andrew's house and is shared with family members
(`cloud/src/household.js`), so upstream's banner is simply wrong here, on every page. Disabled
rather than deleted where it was easy to (an early `return` in `common.js`'s IIFE, `--dbh` set
to `0px`) so future upstream edits to the banner's own logic still apply textually instead of
conflicting; the two static divs are actually removed, with a one-line HTML comment left in
their place that `tools/jts-check-patches.sh` checks for, so a future upstream rewrite of either
file that silently reintroduces the banner is caught rather than shipped.

## The C++ core file we do edit

**The fix for the LED bug (zone 3/GPIO1 stuck white on some outputs) was never a code change:
it is building on an ESP-IDF 4.x platform instead of 5.x.** `house_esp32` itself now uses that
platform (`espressif32@~6.13.0`, arduino-esp32 2.0.17 / esp-idf 4.4.7) — confirmed on real
hardware that all 4 zones work correctly there. Two attempts to fix the same symptom in code
while staying on IDF5 (releasing GPIO1/GPIO3 from UART0, at two different points in boot) were
both tried and **both confirmed on real hardware not to fix it** (2026-09-11); that code has
been reverted rather than kept as a harmless-but-useless flag, since an unused `#ifdef` in a
core file is still merge risk for no benefit.

`wled00/json.cpp`, `JTS-WIFI-SCAN-FIX-START`/`-END`. The IDF4 platform has a real, separate bug:
`WiFi.scanNetworks()` fails permanently after any interrupted connection attempt
(arduino-esp32 #8916 — confirmed on both the Tasmota and official espressif32 IDF4 packages;
no later 2.0.x point release fixes it, and arduino-esp32 3.x dropped IDF4 entirely, so there is
no version bump that makes this go away). `serializeNetworks()` retried the scan forever
without ever addressing why it failed: the STA radio sits in ESP-IDF's "still connecting" state
for most of the 18s (or 300s with an AP client attached) between `WLED::handleConnection()`'s
own reconnect attempts, and a scan issued in that window collides with it. The fix abandons a
stuck connect attempt (`WiFi.disconnect(false)`) before retrying the scan, guarded on
`WiFi.status() != WL_CONNECTED` so a genuinely good link is never dropped just because a scan
failed — `handleConnection()` re-fires the connection on its normal schedule regardless, so a
delayed connect is retried, not lost. Applied unconditionally (not behind a build flag): the
same driver behavior exists in arduino-esp32 3.x too, so this is a safe improvement on IDF5
builds as well, not just the IDF4 path it was written for.

DirectAuth and CloudLink otherwise need no core edits: DirectAuth works because usermod
`setup()` runs before `initServer()`, so its handler is first in the chain; CloudLink calls
WLED's own serializers. That is why the merge surface otherwise stays this small — keep it
that way.

## After a merge

```bash
git -C firmware merge upstream/main
./tools/jts-check-patches.sh          # must pass
npm ci && npm run build               # regenerate the web UI headers
pio run -e house_esp32                # must fit the 1.8 MB slot (see platformio_override.ini)
```

Pushing to `main` makes CI build and publish the image to the cloud panel, where it shows up
as an available update for every paired controller.
