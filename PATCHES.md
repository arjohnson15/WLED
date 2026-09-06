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

If an upstream release rewrites either file, git may merge without a conflict yet drop our
block. That is exactly what the check script catches.

## Deliberately not changed

No C++ core file is touched. DirectAuth works because usermod `setup()` runs before
`initServer()`, so its handler is first in the chain; CloudLink calls WLED's own
serializers. That is why the merge surface stays this small — keep it that way.

## After a merge

```bash
git -C firmware merge upstream/main
./tools/jts-check-patches.sh          # must pass
npm ci && npm run build               # regenerate the web UI headers
pio run -e house_esp32                # must fit the 1.8 MB slot (see platformio_override.ini)
```

Pushing to `main` makes CI build and publish the image to the cloud panel, where it shows up
as an available update for every paired controller.
