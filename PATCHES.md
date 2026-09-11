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

Both are web UI sources, and both changes are wrapped in `JTS-…-START` / `JTS-…-END`
comments so they are easy to find and re-apply.

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
| `wled00/wled.cpp` | `JTS_FREE_UART0_LED_PINS` (off by default) skips `Serial.begin()`/the RX pulldown in `WLED::setup()` so UART0 never claims GPIO1/GPIO3 before the LED buses do |

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

## The one C++ core file we do edit

`wled00/wled.cpp`, wrapped in `JTS-FREE-UART0-PINS-START`/`-END` comments, off by default
(`house_esp32` does not define `JTS_FREE_UART0_LED_PINS`; only the diagnostic
`house_esp32_freepins` env does — see `platformio_override.ini`). GPIO1/GPIO3 double as UART0
TX/RX and, on the Dig-Quad pinout, as WS2812 LED outputs 3/2. `Serial.begin()` in
`WLED::setup()` claims both pins for UART0 before the LED buses exist; a usermod cannot get in
ahead of that, because `beginStrip()` (which creates the RMT LED driver) runs *before*
`UsermodManager::setup()`, not after — an earlier attempt released the pins from a usermod's
`setup()` and it corrupted a live RMT channel and hung the main loop on real hardware
(2026-09-11). Skipping `Serial.begin()` outright, ahead of `beginStrip()`, is the only point
early enough to leave the pins unclaimed instead of un-claiming them, and needs a change to
`wled.cpp` itself. `DEBUG_PRINT*` macros before that point are confirmed to compile to nothing
without `WLED_DEBUG`, so nothing else in this window depends on `Serial` already being open.

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
