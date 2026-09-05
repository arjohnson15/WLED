/*
 * CloudLink usermod
 *
 * Keeps one outbound WebSocket (wss://) connection from the controller to the
 * house-lights cloud server, so apps can reach the controller through the cloud
 * without any port being opened on the home network.
 *
 * Pairing: the cloud account issues a short pairing code. Enter it on the usermod
 * settings page (or POST it to /cloud/pair). The controller connects, sends the
 * code, receives a device id + device token, and stores them in /cloud.json on the
 * flash filesystem (never in cfg.json). From then on it authenticates with
 * `Authorization: Bearer <token>` on every connection.
 *
 * Relay: the cloud forwards JSON API requests as {"type":"req",...} frames and the
 * controller answers with {"type":"res",...}. Handlers call WLED's own
 * serializeState()/deserializeState() etc. directly, so the HTTP login layer
 * (DirectAuth) is not involved: the cloud vouches for the user.
 *
 * Networking runs in its own FreeRTOS task (TLS handshakes block for seconds and
 * must not stall the LED loop). The task owns the transport; the main loop owns
 * WLED state. They exchange whole text frames through two queues.
 *
 * Built on ESP-IDF esp-tls + esp_transport_ws, which the Tasmota Arduino core used
 * by WLED ships; the Arduino WiFiClientSecure/WebSockets libraries are NOT
 * available in that core, which is why no Arduino library is used here.
 *
 * Protocol reference: see readme.md next to this file.
 */

#include "wled.h"

#ifndef ARDUINO_ARCH_ESP32
  #error "CloudLink requires ESP32 (uses ESP-IDF esp-tls and the WebSocket transport)"
#endif

#include <esp_transport.h>
#include <esp_transport_tcp.h>
#include <esp_transport_ssl.h>
#include <esp_transport_ws.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "isrg_root_x1.h"

// json.cpp helpers that are not exported through fcn_declare.h
void serializePalettes(JsonObject root, int page);
void serializeNodes(JsonObject root);

#define CL_PROTO_VERSION       1
#define CL_IDENTITY_FILE       "/cloud.json"
#define CL_CA_FILE             "/cloud_ca.pem"
#define CL_DEFAULT_PORT        443
#define CL_DEFAULT_PATH        "/device/ws"
#define CL_TASK_STACK          12288
#define CL_TASK_PRIO           1
#define CL_RX_QUEUE_LEN        4
#define CL_TX_QUEUE_LEN        8
#define CL_MAX_MSG_LEN         JSON_BUFFER_SIZE   // frames bigger than WLED's JSON buffer are dropped
#define CL_MAX_PRESETS         32768              // presets.json larger than this answers 507
#define CL_MAX_LIVE_LEDS       256                // same as MAX_LIVE_LEDS in json.cpp (file-local there)
#define CL_READ_CHUNK          1024
#define CL_CONNECT_TIMEOUT_MS  10000
#define CL_READ_TIMEOUT_MS     200
#define CL_WRITE_TIMEOUT_MS    5000
#define CL_PING_INTERVAL_MS    20000
#define CL_PONG_TIMEOUT_MS     10000
#define CL_RECONNECT_MIN_MS    5000
#define CL_RECONNECT_MAX_MS    120000
#define CL_STABLE_SESSION_MS   60000   // a session this long resets the reconnect back-off
#define CL_STATE_DEBOUNCE_MS   300
#define CL_MAX_CODE_LEN        32
#define CL_MAX_HOST_LEN        128
// esp_transport_ws_send_raw() writes the opcode byte verbatim; the FIN bit must be set by the caller
#define WS_FIN(op)             ((ws_transport_opcodes_t)((op) | WS_TRANSPORT_OPCODES_FIN))

class CloudLinkUsermod : public Usermod {
  private:
    // ---- config (cfg.json) ----
    bool     enabled = false;
    String   host;
    uint16_t port    = CL_DEFAULT_PORT;
    bool     tls     = true;              // false = plain ws:// for local development only
    String   path    = CL_DEFAULT_PATH;

    // ---- identity (/cloud.json) ----
    String deviceId;
    String deviceToken;
    String pairCode;                      // transient, cleared once paired or rejected
    String caOverride;                    // contents of /cloud_ca.pem if present

    // ---- task plumbing ----
    TaskHandle_t      task   = nullptr;
    QueueHandle_t     rxq    = nullptr;   // char* frames from cloud, freed by main loop
    QueueHandle_t     txq    = nullptr;   // char* frames to cloud, freed by task
    SemaphoreHandle_t cfgMtx = nullptr;   // guards host/port/path/token/pairCode/caOverride
    volatile bool connected          = false;   // WebSocket is up (set by task)
    volatile bool authenticated      = false;   // cloud accepted hello (set by main loop)
    volatile bool linkUpPending      = false;   // task -> main: send hello/pair
    volatile bool reconnectRequested = false;   // main -> task: drop and rebuild connection
    // Last error as two word-sized values (safe to write from the network task and read from
    // the web/main tasks without a lock); rendered to text on demand by errorText().
    enum ErrCode : uint8_t { CLE_NONE, CLE_TRANSPORT_INIT, CLE_WS_INIT, CLE_HANDSHAKE, CLE_CONNECT, CLE_WRITE, CLE_SOCKET, CLE_READ, CLE_LOST, CLE_CLOSED, CLE_PONG, CLE_SERVER, CLE_QUEUE_FULL };
    volatile uint8_t lastErrCode   = CLE_NONE;
    volatile int     lastErrDetail = 0;         // errno, HTTP status or server error index
    static const char* const serverErrors[];    // server error codes we know, indexed for lastErrDetail
    unsigned long lastConnectMs      = 0;
    unsigned long lastDisconnectMs   = 0;
    uint32_t      reconnects         = 0;

    // ---- state push ----
    bool          statePending    = false;
    unsigned long lastStateChange = 0;
    char*         deferredMsg     = nullptr;    // inbound frame waiting for the JSON buffer lock

    static const char _name[];
    static const char _enabled[];

    // ---------- locking helpers ----------
    // Bounded wait (docs/cpp.instructions.md: never portMAX_DELAY). Callers skip their update
    // when the lock is not obtained rather than stalling the LED loop.
    bool lock()   { return !cfgMtx || xSemaphoreTake(cfgMtx, pdMS_TO_TICKS(250)) == pdTRUE; }
    void unlock() { if (cfgMtx) xSemaphoreGive(cfgMtx); }

    void setErr(ErrCode code, int detail = 0) { lastErrCode = code; lastErrDetail = detail; }
    String errorText() {
      static const char* const names[] = { "", "transport init failed", "ws init failed", "handshake rejected, HTTP ", "connect failed", "write failed",
                                           "socket error", "read failed", "connection lost", "closed by server", "pong timeout", "server error: ", "send queue full" };
      uint8_t c = lastErrCode; int d = lastErrDetail;
      if (c == CLE_NONE || c >= sizeof(names) / sizeof(names[0])) return String();
      String t = names[c];
      if (c == CLE_HANDSHAKE) t += d;
      else if (c == CLE_SERVER) t += (d >= 0 && d < 6) ? serverErrors[d] : "unknown";
      else if (d) { t += F(" (errno "); t += d; t += ')'; }
      return t;
    }

    // ---------- identity ----------
    void loadIdentity() {
      deviceId = ""; deviceToken = "";
      if (!WLED_FS.exists(CL_IDENTITY_FILE)) return;
      File f = WLED_FS.open(CL_IDENTITY_FILE, "r");
      if (!f) return;
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      if (err) { DEBUG_PRINTLN(F("CloudLink: bad cloud.json")); return; }
      deviceId    = doc["id"] | "";
      deviceToken = doc["t"]  | "";
    }

    bool saveIdentity() {
      StaticJsonDocument<512> doc;
      doc["id"] = deviceId;
      doc["t"]  = deviceToken;
      File f = WLED_FS.open(CL_IDENTITY_FILE, "w");
      if (!f) return false;
      serializeJson(doc, f);
      f.close();
      return true;
    }

    void clearIdentity() {
      if (lock()) { deviceId = ""; deviceToken = ""; unlock(); }
      WLED_FS.remove(CL_IDENTITY_FILE);
      authenticated = false;
    }

    // Single place that validates and stores the connection settings, used by the settings
    // page (readFromConfig) and by POST /cloud/pair so both accept exactly the same values.
    // Returns true when something changed (caller then asks the task to reconnect).
    bool applyConnectionSettings(String nHost, int nPort, bool nTls, String nPath) {
      nHost.trim(); nPath.trim();
      int scheme = nHost.indexOf("://"); if (scheme >= 0) nHost = nHost.substring(scheme + 3);   // people paste URLs
      int slash = nHost.indexOf('/');    if (slash >= 0) nHost = nHost.substring(0, slash);
      if (nHost.length() > CL_MAX_HOST_LEN) nHost = nHost.substring(0, CL_MAX_HOST_LEN);
      if (nPort <= 0 || nPort > 65535) nPort = CL_DEFAULT_PORT;
      if (!nPath.startsWith("/")) nPath = CL_DEFAULT_PATH;
      bool changed = nHost != host || nPort != port || nTls != tls || nPath != path;
      if (changed && lock()) { host = nHost; port = nPort; tls = nTls; path = nPath; unlock(); }
      return changed;
    }

    void loadCaOverride() {
      caOverride = "";
      if (!WLED_FS.exists(CL_CA_FILE)) return;
      File f = WLED_FS.open(CL_CA_FILE, "r");
      if (!f) return;
      if (f.size() > 8192) { f.close(); DEBUG_PRINTLN(F("CloudLink: cloud_ca.pem too large, ignored")); return; }
      caOverride = f.readString();
      f.close();
      if (caOverride.indexOf(F("-----BEGIN CERTIFICATE-----")) < 0) caOverride = "";
    }

    static bool validPairCode(const String& c) {
      if (c.length() < 4 || c.length() > CL_MAX_CODE_LEN) return false;
      for (size_t i = 0; i < c.length(); i++) {
        char ch = c[i];
        if (!(isalnum((unsigned char)ch) || ch == '-')) return false;
      }
      return true;
    }

    bool hasToken() { return deviceToken.length() > 0; }

    // ---------- outbound ----------
    // Queues a NUL-terminated frame allocated with d_malloc(); takes ownership.
    void sendOwned(char* frame) {
      if (!frame) return;
      if (!connected || !txq || xQueueSend(txq, &frame, 0) != pdTRUE) { d_free(frame); setErr(CLE_QUEUE_FULL); }
    }
    void send(const String& frame) {
      if (!connected || !txq) return;
      char* copy = (char*)d_malloc(frame.length() + 1);
      if (!copy) return;
      memcpy(copy, frame.c_str(), frame.length() + 1);
      sendOwned(copy);
    }
    // Serialises pDoc straight into one right-sized buffer and queues it.
    void sendDoc() {
      size_t len = measureJson(*pDoc);
      char* buf = (char*)d_malloc(len + 1);
      if (!buf) { setErr(CLE_QUEUE_FULL); return; }
      serializeJson(*pDoc, buf, len + 1);
      sendOwned(buf);
    }

    void addIdentityFields(JsonObject root) {
      root["proto"] = CL_PROTO_VERSION;
      root["name"]  = serverDescription;
      root["mac"]   = escapedMac;
      root["ver"]   = versionString;
      root["ip"]    = WLEDNetwork.localIP().toString();
    }

    void sendHelloOrPair() {
      JSONBufferGuard guard(JSON_LOCK_UNKNOWN);
      if (!guard) { linkUpPending = true; return; }   // retry next loop
      pDoc->clear();
      JsonObject root = pDoc->to<JsonObject>();
      if (!lock()) { linkUpPending = true; return; }
      if (hasToken()) {
        root["type"] = "hello";
        root["id"]   = deviceId;
      } else {
        root["type"] = "pair";
        root["code"] = pairCode;
      }
      unlock();
      addIdentityFields(root);
      sendDoc();
    }

    void pushState() {
      JSONBufferGuard guard(JSON_LOCK_UNKNOWN);
      if (!guard) { statePending = true; return; }
      pDoc->clear();
      JsonObject root = pDoc->to<JsonObject>();
      root["type"] = "state";
      JsonObject body = root.createNestedObject("body");
      serializeState(body);
      sendDoc();
    }

    // ---------- inbound ----------
    // Response frames that bypass ArduinoJson (large or pre-serialised bodies).
    static size_t resHeader(char* buf, size_t cap, uint32_t id, int status) {
      return snprintf_P(buf, cap, PSTR("{\"type\":\"res\",\"id\":%lu,\"status\":%d,\"body\":"), (unsigned long)id, status);
    }
    void sendRawResponse(uint32_t id, int status, const String& rawBody) {
      char* buf = (char*)d_malloc(rawBody.length() + 64);
      if (!buf) { setErr(CLE_QUEUE_FULL); return; }
      size_t n = resHeader(buf, 64, id, status);
      memcpy(buf + n, rawBody.c_str(), rawBody.length()); n += rawBody.length();
      buf[n++] = '}'; buf[n] = '\0';
      sendOwned(buf);
    }
    // /presets.json: stream the file straight into the frame buffer (one copy in RAM).
    void sendPresetsFile(uint32_t id) {
      File f = WLED_FS.open("/presets.json", "r");
      if (!f) { sendRawResponse(id, 200, F("{}")); return; }
      size_t size = f.size();
      if (size > CL_MAX_PRESETS) { f.close(); sendRawResponse(id, 507, F("{\"error\":\"presets.json too large\"}")); return; }
      if (size == 0) { f.close(); sendRawResponse(id, 200, F("{}")); return; }
      char* buf = (char*)d_malloc(size + 64);
      if (!buf) { f.close(); sendRawResponse(id, 507, F("{\"error\":\"out of memory\"}")); return; }
      size_t n = resHeader(buf, 64, id, 200);
      size_t got = f.read((uint8_t*)buf + n, size);
      f.close();
      n += got; buf[n++] = '}'; buf[n] = '\0';
      sendOwned(buf);
    }

    // Same output as WLED's /json/live (json.cpp serveLiveLeds): every n-th LED as "RRGGBB",
    // brightness applied, matrix subsampling and w/h for 2D setups.
    String buildLiveLeds() {
      unsigned used = strip.getLengthTotal();
      unsigned n = used ? (used - 1) / CL_MAX_LIVE_LEDS + 1 : 1;
      #ifndef WLED_DISABLE_2D
      if (strip.isMatrix) {
        used = Segment::maxWidth * Segment::maxHeight;
        n = 1;
        if (used > CL_MAX_LIVE_LEDS) n = 2;
        if (used > CL_MAX_LIVE_LEDS * 4) n = 4;
      }
      #endif
      String out;
      out.reserve(9 + 9 * (used / n + 1) + 32);
      out = F("{\"leds\":[");
      char hex[10];
      bool first = true;
      for (unsigned i = 0; i < used; i += n) {
        #ifndef WLED_DISABLE_2D
        if (strip.isMatrix && n > 1 && (i / Segment::maxWidth) % n) i += Segment::maxWidth * (n - 1);
        if (i >= used) break;
        #endif
        uint32_t c = strip.getPixelColor(i);
        uint8_t r = R(c), g = G(c), b = B(c), w = W(c);
        r = scale8(qadd8(w, r), strip.getBrightness());
        g = scale8(qadd8(w, g), strip.getBrightness());
        b = scale8(qadd8(w, b), strip.getBrightness());
        snprintf_P(hex, sizeof(hex), PSTR("%s\"%02X%02X%02X\""), first ? "" : ",", r, g, b);
        out += hex;
        first = false;
      }
      out += F("],\"n\":");
      out += n;
      #ifndef WLED_DISABLE_2D
      if (strip.isMatrix) { out += F(",\"w\":"); out += Segment::maxWidth / n; out += F(",\"h\":"); out += Segment::maxHeight / n; }
      #endif
      out += '}';
      return out;
    }

    // Executes one relayed API request. `root` is the parsed frame living in pDoc;
    // the response is built in pDoc afterwards, so everything needed is copied first.
    void handleRelayRequest(JsonObject root) {
      uint32_t id       = root["id"] | 0UL;
      String   method   = root["method"] | "GET";
      String   fullPath = root["path"] | "";
      int      q        = fullPath.indexOf('?');
      String   pathOnly = q >= 0 ? fullPath.substring(0, q) : fullPath;
      String   query    = q >= 0 ? fullPath.substring(q + 1) : String();
      method.toUpperCase();

      enum class Target { none, state, info, si, effects, palettes, palx, nodes, fxdata, all, live, presets };
      Target target = Target::none;
      int status = 200;
      bool isJson = pathOnly.startsWith("/json");
      String sub = isJson ? pathOnly.substring(5) : String();   // "" or "/state", "/si", ...

      if (method == "POST" && isJson && (sub == "" || sub == "/state" || sub == "/si")) {
        JsonObject body = root["body"];
        if (body.isNull()) status = 400;
        else { deserializeState(body, CALL_MODE_DIRECT_CHANGE); target = Target::state; }
      } else if (pathOnly.startsWith("/win")) {
        unloadPlaylist();   // the HTTP handler does this before handleSet(); with request == nullptr handleSet() skips it
        handleSet(nullptr, fullPath, true);
        target = Target::state;
      } else if (method == "GET" && pathOnly == "/presets.json") {
        target = Target::presets;
      } else if (method == "GET" && isJson) {
        // same loose matching as WLED's serveJson(), so the stock UI's URLs work unchanged
        if      (sub == "")                    target = Target::all;
        else if (sub.indexOf("state") > 0)     target = Target::state;
        else if (sub.indexOf("info") > 0)      target = Target::info;
        else if (sub.indexOf("si") > 0)        target = Target::si;
        else if (sub.indexOf("nodes") > 0)     target = Target::nodes;
        else if (sub.indexOf("eff") > 0)       target = Target::effects;
        else if (sub.indexOf("palx") > 0)      target = Target::palx;
        else if (sub.indexOf("fxda") > 0)      target = Target::fxdata;
        else if (sub.indexOf("live") > 0)      target = Target::live;
        else if (sub.indexOf("pal") > 0)       target = Target::palettes;
        else status = 404;
      } else if (method == "GET") {
        status = 404;
      } else {
        status = 405;
      }

      int page = 0;
      int pp = query.indexOf(F("page="));
      if (pp >= 0) page = query.substring(pp + 5).toInt();

      // bodies that are built outside ArduinoJson
      if (status == 200 && target == Target::live) { sendRawResponse(id, 200, buildLiveLeds()); return; }
      if (status == 200 && target == Target::presets) { sendPresetsFile(id); return; }

      pDoc->clear();
      JsonObject res = pDoc->to<JsonObject>();
      res["type"]   = "res";
      res["id"]     = id;
      res["status"] = status;
      if (status == 200) {
        switch (target) {
          case Target::state:    serializeState(res.createNestedObject("body")); break;
          case Target::info:     serializeInfo(res.createNestedObject("body")); break;
          case Target::si: {
            JsonObject body = res.createNestedObject("body");
            serializeState(body.createNestedObject("state"));
            serializeInfo(body.createNestedObject("info"));
            break;
          }
          case Target::effects:  serializeModeNames(res.createNestedArray("body")); break;
          case Target::palettes: res["body"] = serialized((const char*)JSON_palette_names); break;
          case Target::palx:     serializePalettes(res.createNestedObject("body"), page); break;
          case Target::nodes:    serializeNodes(res.createNestedObject("body")); break;
          case Target::fxdata: {
            JsonArray arr = res.createNestedArray("body");
            for (size_t i = 0; i < strip.getModeCount(); i++) {
              const char* md = strip.getModeData(i);
              const char* at = strchr(md, '@');
              arr.add(at ? at + 1 : "");
            }
            break;
          }
          case Target::all: {
            JsonObject body = res.createNestedObject("body");
            serializeState(body.createNestedObject("state"));
            serializeInfo(body.createNestedObject("info"));
            serializeModeNames(body.createNestedArray("effects"));
            body["palettes"] = serialized((const char*)JSON_palette_names);
            break;
          }
          default: break;
        }
      } else {
        res["error"] = status == 404 ? "not found" : status == 405 ? "method not allowed" : "bad request";
      }
      if (pDoc->overflowed()) {
        pDoc->clear();
        res = pDoc->to<JsonObject>();
        res["type"] = "res"; res["id"] = id; res["status"] = 507; res["error"] = "response too large";
      }
      sendDoc();
    }

    // returns false if the JSON buffer was busy and the frame must be retried
    bool handleMessage(const char* msg) {
      JSONBufferGuard guard(JSON_LOCK_UNKNOWN);
      if (!guard) return false;
      pDoc->clear();
      DeserializationError err = deserializeJson(*pDoc, msg);
      if (err) { DEBUG_PRINTF_P(PSTR("CloudLink: bad frame (%s)\n"), err.c_str()); return true; }
      JsonObject root = pDoc->as<JsonObject>();
      const char* type = root["type"] | "";

      if (!strcmp(type, "req")) {
        handleRelayRequest(root);
      } else if (!strcmp(type, "welcome")) {
        authenticated = true;
        setErr(CLE_NONE);
        DEBUG_PRINTLN(F("CloudLink: authenticated"));
        statePending = true; lastStateChange = 0;     // send a full state right away
      } else if (!strcmp(type, "paired")) {
        const char* id  = root["deviceId"] | "";
        const char* tok = root["token"] | "";
        if (strlen(id) && strlen(tok) && lock()) {
          deviceId = id; deviceToken = tok; pairCode = "";
          unlock();
          saveIdentity();
          setErr(CLE_NONE);
          DEBUG_PRINTLN(F("CloudLink: paired, reconnecting with device token"));
          reconnectRequested = true;
        }
      } else if (!strcmp(type, "error")) {
        const char* code = root["code"] | "unknown";
        int idx = 5;
        for (int i = 0; i < 5; i++) if (!strcmp(code, serverErrors[i])) idx = i;
        setErr(CLE_SERVER, idx);
        DEBUG_PRINTF_P(PSTR("CloudLink: server error %s\n"), code);
        if (!strcmp(code, "bad_code") || !strcmp(code, "code_expired")) { if (lock()) { pairCode = ""; unlock(); } }
        if (!strcmp(code, "bad_token") || !strcmp(code, "revoked")) clearIdentity();
      } else if (!strcmp(type, "unpair")) {
        DEBUG_PRINTLN(F("CloudLink: unpaired by server"));
        clearIdentity();
        reconnectRequested = true;
      } else if (!strcmp(type, "ping")) {
        send(F("{\"type\":\"pong\"}"));
      }
      return true;
    }

    // ---------- the network task ----------
    bool wantConnection() {
      if (!enabled || !WLED_CONNECTED) return false;
      if (!lock()) return false;
      bool want = host.length() && (hasToken() || pairCode.length());
      unlock();
      return want;
    }

    void setError(ErrCode code, int detail) {
      setErr(code, detail);
      DEBUG_PRINTF_P(PSTR("CloudLink: %s\n"), errorText().c_str());
    }

    // one connection attempt + session; returns the session length in ms (0 = never connected)
    unsigned long runSession() {
      // snapshot config so settings changes on the main thread cannot race us
      if (!lock()) return 0;
      String sHost = host, sPath = path, sToken = deviceToken, sCa = caOverride;
      uint16_t sPort = port;
      bool sTls = tls;
      unlock();

      esp_transport_handle_t base = sTls ? esp_transport_ssl_init() : esp_transport_tcp_init();
      if (!base) { setError(CLE_TRANSPORT_INIT, 0); return 0; }
      if (sTls) {
        const char* ca = sCa.length() ? sCa.c_str() : CLOUDLINK_DEFAULT_CA;
        esp_transport_ssl_set_cert_data(base, ca, strlen(ca));   // IDF adds the terminating NUL itself
      }
      esp_transport_handle_t ws = esp_transport_ws_init(base);
      if (!ws) { esp_transport_destroy(base); setError(CLE_WS_INIT, 0); return 0; }

      String headers;
      if (sToken.length()) { headers = F("Authorization: Bearer "); headers += sToken; headers += F("\r\n"); }
      esp_transport_ws_config_t cfg = {};
      cfg.ws_path = sPath.c_str();
      cfg.headers = headers.length() ? headers.c_str() : nullptr;
      cfg.user_agent = "WLED-CloudLink/1";
      cfg.propagate_control_frames = true;   // we answer PING and watch for CLOSE ourselves
      esp_transport_ws_set_config(ws, &cfg);

      DEBUG_PRINTF_P(PSTR("CloudLink: connecting %s://%s:%u%s\n"), sTls ? "wss" : "ws", sHost.c_str(), sPort, sPath.c_str());
      if (esp_transport_connect(ws, sHost.c_str(), sPort, CL_CONNECT_TIMEOUT_MS) < 0) {
        int st = esp_transport_ws_get_upgrade_request_status(ws);
        if (st > 0) setError(CLE_HANDSHAKE, st);
        else setError(CLE_CONNECT, esp_transport_get_errno(ws));
        esp_transport_close(ws);
        esp_transport_destroy(ws);
        esp_transport_destroy(base);
        return 0;
      }

      connected = true;
      linkUpPending = true;
      lastConnectMs = millis();
      DEBUG_PRINTLN(F("CloudLink: connected"));

      char* msgBuf = nullptr;         // message being assembled (may span several frames)
      size_t msgLen = 0, msgCap = 0;
      int frameRemaining = 0;         // payload bytes still to read for the current frame
      int zeroReads = 0;              // consecutive empty reads after poll said "readable"
      char chunk[CL_READ_CHUNK];
      unsigned long lastPing = millis();
      unsigned long pingSent = 0;
      bool awaitingPong = false;
      bool discardMsg = false;

      while (wantConnection() && !reconnectRequested) {
        // ---- outbound ----
        char* out;
        while (xQueueReceive(txq, &out, 0) == pdTRUE) {
          int w = esp_transport_ws_send_raw(ws, WS_FIN(WS_TRANSPORT_OPCODES_TEXT), out, strlen(out), CL_WRITE_TIMEOUT_MS);
          d_free(out);
          if (w < 0) { setError(CLE_WRITE, esp_transport_get_errno(ws)); goto drop; }
        }

        // ---- inbound ----
        {
          int r = esp_transport_poll_read(ws, CL_READ_TIMEOUT_MS);
          if (r < 0) { setError(CLE_SOCKET, esp_transport_get_errno(ws)); goto drop; }
          if (r > 0) {
            int n = esp_transport_read(ws, chunk, sizeof(chunk), CL_READ_TIMEOUT_MS);
            if (n < 0) { setError(CLE_READ, esp_transport_get_errno(ws)); goto drop; }
            bool newFrame = (frameRemaining == 0);
            if (n == 0 && !newFrame) {   // readable but nothing delivered mid-frame: timeout, or the peer vanished
              if (++zeroReads > 20) { setError(CLE_LOST, esp_transport_get_errno(ws)); goto drop; }
              continue;
            }
            int opcode = esp_transport_ws_get_read_opcode(ws) & 0x0F;
            if (n == 0 && opcode == WS_TRANSPORT_OPCODES_NONE) {   // no frame at all
              if (++zeroReads > 20) { setError(CLE_LOST, esp_transport_get_errno(ws)); goto drop; }
              continue;
            }
            zeroReads = 0;
            // Zero-length frames (the server's empty PING/PONG/CLOSE) legitimately return n == 0
            // with the opcode set; they must still be handled below.
            if (newFrame) frameRemaining = esp_transport_ws_get_read_payload_len(ws);   // reads may return a frame in pieces
            frameRemaining -= n;
            if (frameRemaining < 0) frameRemaining = 0;
            bool frameDone = (frameRemaining == 0);
            bool fin = esp_transport_ws_get_fin_flag(ws);
            switch (opcode) {
              case WS_TRANSPORT_OPCODES_CLOSE:
                setError(CLE_CLOSED, 0);
                goto drop;
              case WS_TRANSPORT_OPCODES_PING:
                esp_transport_ws_send_raw(ws, WS_FIN(WS_TRANSPORT_OPCODES_PONG), chunk, n, CL_WRITE_TIMEOUT_MS);
                break;
              case WS_TRANSPORT_OPCODES_PONG:
                awaitingPong = false;
                break;
              case WS_TRANSPORT_OPCODES_TEXT:
              case WS_TRANSPORT_OPCODES_BINARY:
              case WS_TRANSPORT_OPCODES_CONT: {
                if (newFrame && opcode != WS_TRANSPORT_OPCODES_CONT) {
                  // first frame of a new message: buffer sized from this frame's length (a stray CONT without a start is ignored)
                  if (msgBuf) { d_free(msgBuf); msgBuf = nullptr; }
                  msgLen = 0; msgCap = 0;
                  int total = esp_transport_ws_get_read_payload_len(ws);
                  discardMsg = (total < 0 || (size_t)total > CL_MAX_MSG_LEN);
                  if (!discardMsg) { msgCap = total + 1; msgBuf = (char*)d_malloc(msgCap); if (!msgBuf) discardMsg = true; }
                } else if (newFrame && !discardMsg && msgBuf) {
                  // continuation frame of a fragmented message: grow the buffer
                  int total = esp_transport_ws_get_read_payload_len(ws);
                  if (total < 0 || msgLen + total > CL_MAX_MSG_LEN) discardMsg = true;
                  else { char* g = (char*)d_realloc_malloc(msgBuf, msgLen + total + 1); if (g) { msgBuf = g; msgCap = msgLen + total + 1; } else discardMsg = true; }
                }
                if (!discardMsg && msgBuf) {
                  if (msgLen + n + 1 > msgCap) discardMsg = true;
                  else { memcpy(msgBuf + msgLen, chunk, n); msgLen += n; }
                }
                if (frameDone && fin) {
                  if (msgBuf && !discardMsg && msgLen) {
                    msgBuf[msgLen] = '\0';
                    if (xQueueSend(rxq, &msgBuf, pdMS_TO_TICKS(100)) != pdTRUE) d_free(msgBuf);
                  } else if (msgBuf) {
                    d_free(msgBuf);
                  }
                  msgBuf = nullptr; msgLen = 0; msgCap = 0; discardMsg = false;
                }
                break;
              }
              default: break;
            }
          }
        }

        // ---- heartbeat ----
        unsigned long now = millis();
        if (now - lastPing > CL_PING_INTERVAL_MS) {
          esp_transport_ws_send_raw(ws, WS_FIN(WS_TRANSPORT_OPCODES_PING), "", 0, CL_WRITE_TIMEOUT_MS);
          lastPing = now; pingSent = now; awaitingPong = true;
        }
        if (awaitingPong && now - pingSent > CL_PONG_TIMEOUT_MS) { setError(CLE_PONG, 0); goto drop; }
      }

    drop:
      if (msgBuf) d_free(msgBuf);
      connected = false;
      authenticated = false;
      lastDisconnectMs = millis();
      { char* out; while (xQueueReceive(txq, &out, 0) == pdTRUE) d_free(out); }   // drop stale outbound frames
      esp_transport_close(ws);
      esp_transport_destroy(ws);
      esp_transport_destroy(base);
      DEBUG_PRINTLN(F("CloudLink: disconnected"));
      return lastDisconnectMs - lastConnectMs;
    }

    static void taskEntry(void* arg) {
      CloudLinkUsermod* self = static_cast<CloudLinkUsermod*>(arg);
      unsigned long backoff = CL_RECONNECT_MIN_MS;
      for (;;) {
        if (!self->wantConnection()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        self->reconnectRequested = false;
        unsigned long sessionMs = self->runSession();   // 0 when the connect itself failed
        self->reconnects++;
        if (sessionMs > CL_STABLE_SESSION_MS) backoff = CL_RECONNECT_MIN_MS;
        if (self->reconnectRequested) { self->reconnectRequested = false; vTaskDelay(pdMS_TO_TICKS(500)); continue; }   // config changed: retry quickly
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = min<unsigned long>(backoff * 2, CL_RECONNECT_MAX_MS);
      }
    }

    void startTask() {
      if (task) return;
      rxq    = xQueueCreate(CL_RX_QUEUE_LEN, sizeof(char*));
      txq    = xQueueCreate(CL_TX_QUEUE_LEN, sizeof(char*));
      cfgMtx = xSemaphoreCreateMutex();
      if (!rxq || !txq || !cfgMtx) { DEBUG_PRINTLN(F("CloudLink: queue alloc failed")); return; }
      #if CONFIG_FREERTOS_UNICORE
      xTaskCreate(taskEntry, "cloudlink", CL_TASK_STACK, this, CL_TASK_PRIO, &task);
      #else
      xTaskCreatePinnedToCore(taskEntry, "cloudlink", CL_TASK_STACK, this, CL_TASK_PRIO, &task, 0);   // keep off the LED core
      #endif
    }

    // ---------- HTTP helpers ----------
    static void sendJson(AsyncWebServerRequest* request, int code, const String& body) {
      AsyncWebServerResponse* res = request->beginResponse(code, FPSTR(CONTENT_TYPE_JSON), body);
      res->addHeader(F("Cache-Control"), F("no-store"));
      request->send(res);
    }

    void handleStatus(AsyncWebServerRequest* request) {
      StaticJsonDocument<512> doc;
      doc["enabled"]   = enabled;
      doc["host"]      = host;
      doc["port"]      = port;
      doc["tls"]       = tls;
      doc["path"]      = path;
      doc["caFile"]    = caOverride.length() > 0;
      doc["paired"]    = hasToken();
      doc["deviceId"]  = deviceId;
      doc["pairing"]   = pairCode.length() > 0;
      doc["connected"] = (bool)connected;
      doc["authed"]    = (bool)authenticated;
      doc["error"]     = errorText();
      doc["reconnects"]= reconnects;
      String out;
      serializeJson(doc, out);
      sendJson(request, 200, out);
    }

    // POST /cloud/pair  form: code [, host, port, tls, path]
    void handlePair(AsyncWebServerRequest* request) {
      String code = request->arg("code");
      code.trim();
      if (!validPairCode(code)) { sendJson(request, 400, F("{\"error\":\"pairing code: 4-32 letters, digits or dashes\"}")); return; }
      applyConnectionSettings(request->hasArg("host") ? request->arg("host") : host,
                              request->hasArg("port") ? request->arg("port").toInt() : port,
                              request->hasArg("tls")  ? (request->arg("tls") != "false" && request->arg("tls") != "0") : tls,
                              request->hasArg("path") ? request->arg("path") : path);
      if (!lock()) { sendJson(request, 503, F("{\"error\":\"busy, retry\"}")); return; }
      pairCode = code;
      deviceId = ""; deviceToken = "";   // a new code replaces any previous registration
      bool haveHost = host.length() > 0;
      unlock();
      if (!haveHost) { sendJson(request, 400, F("{\"error\":\"cloud host not set\"}")); return; }
      WLED_FS.remove(CL_IDENTITY_FILE);
      enabled = true;
      configNeedsWrite = true;   // host/port/path/tls/enabled live in cfg.json; the code itself is never written
      setErr(CLE_NONE);
      reconnectRequested = true;
      sendJson(request, 200, F("{\"ok\":true,\"pairing\":true}"));
    }

    void handleUnpair(AsyncWebServerRequest* request) {
      clearIdentity();
      if (lock()) { pairCode = ""; unlock(); }
      reconnectRequested = true;
      sendJson(request, 200, F("{\"ok\":true,\"paired\":false}"));
    }

  public:
    void setup() override {
      loadIdentity();
      loadCaOverride();
      server.on("/cloud/status", HTTP_GET,  [this](AsyncWebServerRequest* r) { handleStatus(r); });
      server.on("/cloud/pair",   HTTP_POST, [this](AsyncWebServerRequest* r) { handlePair(r); });
      server.on("/cloud/unpair", HTTP_POST, [this](AsyncWebServerRequest* r) { handleUnpair(r); });
      if (enabled) startTask();
      DEBUG_PRINTF_P(PSTR("CloudLink: %s, %s\n"), enabled ? "enabled" : "disabled", hasToken() ? "paired" : "not paired");
    }

    void loop() override {
      if (!enabled) return;
      if (!task) startTask();

      // inbound frames (retry a frame if the JSON buffer was busy)
      if (deferredMsg) {
        if (!handleMessage(deferredMsg)) return;
        d_free(deferredMsg); deferredMsg = nullptr;
      }
      char* msg;
      while (rxq && xQueueReceive(rxq, &msg, 0) == pdTRUE) {
        if (!handleMessage(msg)) { deferredMsg = msg; break; }
        d_free(msg);
      }

      if (connected && linkUpPending) { linkUpPending = false; sendHelloOrPair(); }

      if (statePending && connected && authenticated && millis() - lastStateChange > CL_STATE_DEBOUNCE_MS) {
        statePending = false;
        pushState();
      }
    }

    void onStateChange(uint8_t mode) override {
      statePending = true;
      lastStateChange = millis();
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray a = user.createNestedArray(F("Cloud link"));
      if (!enabled)            a.add(F("disabled"));
      else if (authenticated)  a.add(F("connected"));
      else if (connected)      a.add(hasToken() ? F("authenticating") : F("pairing"));
      else if (!hasToken() && !pairCode.length()) a.add(F("not paired"));
      else                     { String e = errorText(); a.add(e.length() ? e : String(F("connecting"))); }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[F("host")]     = host;
      top[F("port")]     = port;
      top[F("tls")]      = tls;
      top[F("path")]     = path;
      top[F("pairCode")] = "";   // write-only: consumed in readFromConfig, never persisted
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) return false;
      bool complete = true;
      bool   nEnabled = enabled;
      String nHost    = host;
      int    nPort    = port;
      bool   nTls     = tls;
      String nPath    = path;
      complete &= getJsonValue(top[FPSTR(_enabled)], nEnabled, false);
      complete &= getJsonValue(top[F("host")], nHost, "");
      complete &= getJsonValue(top[F("port")], nPort, CL_DEFAULT_PORT);
      complete &= getJsonValue(top[F("tls")],  nTls, true);
      complete &= getJsonValue(top[F("path")], nPath, CL_DEFAULT_PATH);
      String code = top[F("pairCode")] | "";
      code.trim();
      bool changed = applyConnectionSettings(nHost, nPort, nTls, nPath);
      if (code.length()) {
        if (validPairCode(code)) {
          if (lock()) { pairCode = code; deviceId = ""; deviceToken = ""; unlock(); WLED_FS.remove(CL_IDENTITY_FILE); changed = true; }
        } else DEBUG_PRINTLN(F("CloudLink: ignored malformed pairing code"));
      }
      enabled = nEnabled;
      if (changed) reconnectRequested = true;
      return complete;
    }

    void appendConfigData(Print& s) override {
      s.print(F("addInfo('CloudLink:host',1,'cloud server hostname, no scheme');"));
      s.print(F("addInfo('CloudLink:port',1,'443 for wss');"));
      s.print(F("addInfo('CloudLink:tls',1,'untick only for a local dev server (plain ws://)');"));
      s.print(F("addInfo('CloudLink:path',1,'WebSocket path on the server');"));
      s.print(F("addInfo('CloudLink:pairCode',1,'"));
      s.print(hasToken() ? F("paired as <b>") : F("<b>not paired</b> - enter the code from your cloud account"));
      if (hasToken()) { s.print(deviceId); s.print(F("</b>; enter a new code to re-pair")); }
      s.print(F("');"));
    }
};

const char CloudLinkUsermod::_name[]    PROGMEM = "CloudLink";
const char* const CloudLinkUsermod::serverErrors[] = { "bad_code", "code_expired", "bad_token", "revoked", "unknown", "unknown" };
const char CloudLinkUsermod::_enabled[] PROGMEM = "enabled";

static CloudLinkUsermod cloudLinkUsermod;
REGISTER_USERMOD(cloudLinkUsermod);
