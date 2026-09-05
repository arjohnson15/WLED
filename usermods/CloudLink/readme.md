# CloudLink — outbound connection to the house-lights cloud

Keeps a single `wss://` WebSocket from the controller to the cloud server so the phone
app can control the lights from anywhere without opening a port at home. The cloud
relays JSON API calls; the controller answers using WLED's internal serializers.

## Pairing flow

1. In the cloud account, add a controller. The server shows a **pairing code**.
2. Enter the code on the controller — either on **Settings → Usermods → CloudLink →
   pairCode**, or from the app over the LAN: `POST /cloud/pair` with `code=...`
   (this endpoint sits behind the DirectAuth login when that usermod is present).
3. The controller connects, sends `{"type":"pair"}`, receives `{"type":"paired"}` with a
   device id and device token, stores them in `/cloud.json`, and reconnects with
   `Authorization: Bearer <token>`.

`POST /cloud/unpair` deletes the token. `GET /cloud/status` reports everything the app
needs to show connection state.

Settings (`cfg.json` → `um.CloudLink`): `enabled`, `host`, `port` (443), `tls` (true),
`path` (`/device/ws`). `pairCode` is write-only and never persisted. The device token
lives only in `/cloud.json`.

## TLS

The server certificate is verified against **ISRG Root X1** (Let's Encrypt) compiled in
(`isrg_root_x1.h`). To use another CA, upload a PEM to `/cloud_ca.pem` via `/edit`; it
is picked up on the next boot. `tls=false` gives plain `ws://` for a dev server on the LAN
only. There is deliberately no "skip verification" option.

## Wire protocol (v1)

Text frames, one JSON object each. The controller sends the first frame after connect.

Controller → cloud, first frame:

```json
{"type":"hello","proto":1,"id":"<deviceId>","name":"Kitchen","mac":"aabbccddeeff","ver":"0.16.0","ip":"192.168.1.40"}
{"type":"pair", "proto":1,"code":"ABCD-1234","name":"Kitchen","mac":"aabbccddeeff","ver":"0.16.0","ip":"192.168.1.40"}
```

Cloud → controller:

| Frame | Meaning |
|---|---|
| `{"type":"welcome"}` | hello accepted; controller then pushes a full state |
| `{"type":"paired","deviceId":"...","token":"..."}` | pairing succeeded; controller stores and reconnects |
| `{"type":"error","code":"bad_code" \| "code_expired" \| "bad_token" \| "revoked" \| ...}` | `bad_code`/`code_expired` clear the pending code; `bad_token`/`revoked` delete the stored token |
| `{"type":"unpair"}` | server-side removal; controller forgets its token |
| `{"type":"req","id":123,"method":"GET","path":"/json/state"}` | relayed API call |
| `{"type":"req","id":124,"method":"POST","path":"/json/state","body":{"on":true,"bri":128}}` | relayed API call with body |
| `{"type":"ping"}` | app-level ping (WebSocket PING/PONG is also handled) |

Controller → cloud:

| Frame | Meaning |
|---|---|
| `{"type":"res","id":123,"status":200,"body":{...}}` | reply; `status` 400/404/405/507 carry `"error"` instead of `body` |
| `{"type":"state","body":{...}}` | pushed after every state change (debounced 300 ms) and once after `welcome` |
| `{"type":"pong"}` | reply to app-level ping |

Relayed paths (matched loosely, like WLED's own `/json` handler, so the stock UI's
URLs work unchanged): `GET /json`, `/json/state`, `/json/info`, `/json/si`,
`/json/eff[ects]`, `/json/pal[ettes]`, `/json/palx?page=N`, `/json/nodes`,
`/json/fxda[ta]`, `/json/live` (every n-th LED as `"RRGGBB"`, max 512, brightness
applied, same shape as WLED's), `/presets.json` (raw file); `POST /json`,
`/json/state`, `/json/si` (WLED state JSON, returns state); `GET /win&...` (WLED HTTP
API, returns state). Responses larger than WLED's JSON buffer (24 KB on ESP32) return
status 507; `presets.json` is capped at 32 KB.

The controller sends a WebSocket PING every 20 s and drops the link if no PONG arrives
within 10 s. Reconnects back off from 5 s to 2 min; a link that lasted over a minute
resets the back-off.

## Implementation notes

* Networking runs in its own FreeRTOS task pinned to core 0 (LEDs run on core 1). TLS
  handshakes block for seconds and must not stall effects. Frames cross to the main loop
  through queues; all WLED state access happens on the main loop.
* Uses ESP-IDF `esp-tls` + `esp_transport_ws` directly. No Arduino TLS library.
* **Needs the pioarduino platform**, not WLED's default Tasmota core: the Tasmota core
  strips `mbedtls_ssl_*` entirely (it has no `WiFiClientSecure`). See
  `platformio_override.ini` → `[env:house_esp32]`.
* A TLS session needs roughly 40–50 KB of free heap while connected.

Requires ESP32. ESP8266 builds fail on purpose.
