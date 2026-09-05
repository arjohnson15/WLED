# DirectAuth — login page for the WLED web UI and API

Puts a username/password login in front of everything WLED serves on port 80:
the web UI, `/json`, `/win`, the `/ws` WebSocket, `/edit`, `/update`, `/settings`.
Meant for controllers that are reachable by hostname/FQDN from outside a trusted LAN.

## Behaviour

| State | What a visitor sees |
|---|---|
| Fresh flash, no credentials | Every page redirects to `/login`, which shows a one-time **Create admin login** form. Nothing else is reachable, including WiFi settings. |
| Credentials set | `/login` asks for username and password. Success sets an `HttpOnly` session cookie and redirects back. |
| Usermod disabled in settings | Device behaves like stock WLED (open). |

Sessions live 30 days by default (configurable 1–365), up to 8 at once, and survive
reboots. Five failed logins lock the login endpoint for 30 seconds.

WLED's own settings PIN and OTA lock still work on top of this.

## For apps (the iOS controller uses this)

```
GET  /auth/status                    -> {"setup":false,"auth":false,"enabled":true,"name":"Kitchen","mac":"...","ver":"0.16.0"}
POST /auth/login   user=..&pass=..   -> 200 {"ok":true,"token":"<64 hex>","user":"admin","days":30}   (+ Set-Cookie)
     header Accept: application/json  or  add ?json to get JSON instead of a redirect
GET  /json/state   X-Auth-Token: <token>
POST /auth/logout  X-Auth-Token: <token>   [?all=1 revokes every session]
GET  /auth/sessions                  -> {"now":..,"sessions":[{"created":..,"expires":..,"current":true}]}
```

Requests are form-encoded (`application/x-www-form-urlencoded`). Send the token in the
**`X-Auth-Token`** header. `Authorization: Bearer` cannot be used: the ESPAsyncWebServer
fork WLED uses swallows any non-Basic/Digest `Authorization` header before handlers see it.

Unauthenticated requests get `302 /login` when the request is a browser navigation
(`GET` with `Accept: text/html`) and `401 {"error":"unauthorized"}` otherwise.

## Storage

| File | Contents |
|---|---|
| `/auth.json` | username, 16-byte random salt, PBKDF2-HMAC-SHA256 hash (10 000 rounds), iteration count |
| `/auth_sess.json` | SHA-256 of each session token with created/expiry unix times |
| `cfg.json` → `um.DirectAuth` | `enabled`, `sessionDays`, `allowAlexa` only. `newUser` / `newPassword` are write-only fields consumed when settings are saved and always stored back empty. |

Secrets never enter `cfg.json`, so config backups stay shareable.

Expiry uses NTP time. Until the clock is synced, sessions created are marked "never expires";
sessions with an expiry are only enforced once time is known.

## What it does not cover

Only HTTP on port 80. UDP realtime/DDP/E1.31, MQTT, Hue/Alexa emulation, ESP-NOW and
serial are untouched. Turn off the ones you do not use in WLED's Sync settings.
The `allowAlexa` option punches `/description.xml` and `/api/*` through the login so
Alexa/Hue emulation keeps working; that is by definition an unauthenticated control path.

## Locked out?

* Hold button 0 for more than 10 s: WLED factory reset (wipes the filesystem, WiFi too).
* Or flash a build without this usermod, open `/edit`, delete `/auth.json`, flash back.

## Why a usermod and not a core change

`AsyncWebServer` attaches a handler only after all request headers are parsed, so a
handler registered first can read the `Cookie` header inside `canHandle()` and either
claim the request (deny) or return `false` so the normal WLED handler runs. Usermod
`setup()` runs before `initServer()`, which makes this handler first in the list without
touching any core file — `git merge upstream/main` stays trivial.

Requires ESP32 (mbedtls PBKDF2/SHA-256, hardware RNG). ESP8266 builds fail on purpose.
