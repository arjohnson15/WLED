/*
 * DirectAuth usermod
 *
 * Puts a login page in front of the WLED web UI and HTTP/WebSocket APIs.
 *
 * - First boot (no /auth.json): every page redirects to /login, which shows a
 *   one-time "create admin login" form. Nothing is reachable until it is done.
 * - Afterwards /login accepts username + password and issues a session token,
 *   delivered as an HttpOnly cookie (browsers) and in the JSON reply (apps).
 *   Apps send it back in the `X-Auth-Token` request header.
 * - Passwords are stored as PBKDF2-HMAC-SHA256 (10k rounds, random salt) in
 *   /auth.json on the flash filesystem, never in cfg.json. Session tokens are
 *   stored hashed in /auth_sess.json so they survive a reboot.
 * - Implemented as an AsyncWebHandler registered before all core routes, so no
 *   core WLED file is modified. Handlers are attached after the request headers
 *   are parsed, which is why cookies can be inspected in canHandle().
 *
 * Scope: HTTP(S) on port 80 only. UDP realtime, E1.31, MQTT, Alexa/Hue emulation
 * and other network protocols are untouched; disable the ones you do not use.
 *
 * Recovery if locked out: hold button 0 for >10 s (WLED factory reset, wipes the
 * filesystem) or flash a build without this usermod and delete /auth.json.
 *
 * HTTP surface (all paths below are public unless noted):
 *   GET  /login                 login / setup page
 *   GET  /auth/status           {"setup":bool,"auth":bool,"enabled":bool,"name","mac","ver"[,"user"]}
 *   POST /auth/setup            form: user, pass          (only while no credentials exist)
 *   POST /auth/login            form: user, pass[, to]    -> cookie + {"ok":true,"token":...}
 *   POST /auth/logout[?all]     (authenticated) revoke this or every session
 *   GET  /auth/sessions         (authenticated) list active sessions
 */

#include "wled.h"

#ifndef ARDUINO_ARCH_ESP32
  #error "DirectAuth requires ESP32 (uses mbedtls PBKDF2/SHA-256 and the hardware RNG)"
#endif

#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>
#include <esp_random.h>
#include "login_page.h"

#define DA_MAX_SESSIONS       8
#define DA_TOKEN_BYTES        32
#define DA_TOKEN_HEX_LEN      (DA_TOKEN_BYTES * 2)
#define DA_SALT_BYTES         16
#define DA_HASH_BYTES         32
#define DA_PBKDF2_ITERATIONS  4096   // ~60-100 ms on an ESP32; runs on the async_tcp task, so keep it short. Stored per credential file, so changing it does not break existing logins.
#define DA_MAX_FAILS          5
#define DA_LOCKOUT_MS         30000
#define DA_MIN_VALID_TIME     1700000000UL  // toki below this means NTP has not synced yet
#define DA_MAX_USER_LEN       32
#define DA_MIN_PASS_LEN       8
#define DA_MAX_PASS_LEN       64
#define DA_DEFAULT_DAYS       30
#define DA_CREDS_FILE         "/auth.json"
#define DA_SESS_FILE          "/auth_sess.json"
#define DA_COOKIE             "wled_sid"
#define DA_TOKEN_HEADER       "X-Auth-Token"

// ---------- small helpers ----------

static void daToHex(const uint8_t* in, size_t len, char* out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2*i]   = digits[in[i] >> 4];
    out[2*i+1] = digits[in[i] & 0x0F];
  }
  out[2*len] = '\0';
}

static int daHexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool daFromHex(const char* in, uint8_t* out, size_t len) {
  if (!in || strlen(in) != 2*len) return false;
  for (size_t i = 0; i < len; i++) {
    int hi = daHexNibble(in[2*i]), lo = daHexNibble(in[2*i+1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// constant-time comparison so a token/hash mismatch does not leak where it differs
static bool daConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

static bool daPbkdf2(const String& password, const uint8_t* salt, uint32_t iterations, uint8_t out[DA_HASH_BYTES]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                       (const unsigned char*)password.c_str(), password.length(),
                                       salt, DA_SALT_BYTES, iterations, DA_HASH_BYTES, out) == 0;
}

static void daSha256(const char* data, size_t len, uint8_t out[DA_HASH_BYTES]) {
  mbedtls_sha256((const unsigned char*)data, len, out, 0);
}

// returns the value of cookie `name` from a Cookie header, or an empty String
static String daCookieValue(const String& cookies, const char* name) {
  size_t nameLen = strlen(name);
  int pos = 0;
  while (pos < (int)cookies.length()) {
    int end = cookies.indexOf(';', pos);
    if (end < 0) end = cookies.length();
    String pair = cookies.substring(pos, end);
    pair.trim();
    if (pair.length() > nameLen && pair[nameLen] == '=' && pair.startsWith(name)) return pair.substring(nameLen + 1);
    pos = end + 1;
  }
  return String();
}

// same test WLED's captivePortal() uses, so the gate agrees with it on what a "host" is
static bool daIsIp(const String& str) {
  for (size_t i = 0; i < str.length(); i++) {
    char c = str[i];
    if (c != '.' && (c < '0' || c > '9')) return false;
  }
  return true;
}

// usernames are restricted so they can be embedded safely in JSON/JS without escaping
static bool daValidUser(const String& u) {
  if (u.length() == 0 || u.length() > DA_MAX_USER_LEN) return false;
  for (size_t i = 0; i < u.length(); i++) {
    char c = u[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '@')) return false;
  }
  return true;
}

// only allow same-origin absolute paths as post-login redirect targets
static String daSafeRedirect(const String& to) {
  if (to.length() < 2 || to[0] != '/' || to[1] == '/' || to[1] == '\\') return String("/");
  for (size_t i = 0; i < to.length(); i++) {
    char c = to[i];
    if (c <= ' ' || c == '"' || c == '<' || c == '>' || c == '\r' || c == '\n') return String("/");
  }
  return to;
}

struct DaSession {
  uint8_t  hash[DA_HASH_BYTES];
  uint32_t created;   // unix seconds, 0 if time was unknown
  uint32_t expires;   // unix seconds, 0 = never (time unknown when created)
  bool     valid;
};

class DirectAuthUsermod;

// Sits first in the handler list. Claims (and denies) any request that is not
// public and not authenticated; returns false otherwise so the real handler runs.
class DirectAuthGate : public AsyncWebHandler {
  private:
    DirectAuthUsermod* um;
  public:
    explicit DirectAuthGate(DirectAuthUsermod* owner) : um(owner) {}
    bool canHandle(AsyncWebServerRequest* request) override;
    void handleRequest(AsyncWebServerRequest* request) override;
    bool isRequestHandlerTrivial() override { return true; }
};

class DirectAuthUsermod : public Usermod {
  private:
    // ---- config (cfg.json) ----
    bool     enabled     = true;
    uint16_t sessionDays = DA_DEFAULT_DAYS;
    bool     allowAlexa  = false;

    // ---- credentials (/auth.json) ----
    bool     credsLoaded = false;
    bool     hasCreds    = false;
    String   userName;
    uint8_t  salt[DA_SALT_BYTES];
    uint8_t  pwHash[DA_HASH_BYTES];
    uint32_t iterations  = DA_PBKDF2_ITERATIONS;

    // ---- sessions (/auth_sess.json) ----
    bool      sessionsLoaded = false;
    DaSession sessions[DA_MAX_SESSIONS];

    // ---- brute-force throttle ----
    uint8_t       failCount = 0;
    unsigned long lockUntil = 0;
    bool          locked    = false;

    DirectAuthGate* gate = nullptr;

    // One-time loopback self-test: an anonymous GET /json/state must be refused. Guards the
    // assumption that this handler was registered before WLED's own routes (see setup()).
    int8_t        gateSelfTest   = -1;    // -1 not run, 0 failed, 1 passed, 2 could not run
    unsigned long selfTestDueAt  = 0;

    static const char _name[];
    static const char _enabled[];

    // ---------- time ----------
    static uint32_t nowUnix() {
      uint32_t t = toki.second();
      return t >= DA_MIN_VALID_TIME ? t : 0;
    }

    // ---------- credentials ----------
    void loadCredentials() {
      if (credsLoaded) return;
      credsLoaded = true;
      hasCreds = false;
      if (!WLED_FS.exists(DA_CREDS_FILE)) return;
      File f = WLED_FS.open(DA_CREDS_FILE, "r");
      if (!f) return;
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      if (err) { DEBUG_PRINTLN(F("DirectAuth: bad auth.json")); return; }
      const char* u = doc["u"];
      const char* s = doc["s"];
      const char* h = doc["h"];
      if (!u || !daFromHex(s, salt, DA_SALT_BYTES) || !daFromHex(h, pwHash, DA_HASH_BYTES)) return;
      userName   = u;
      iterations = doc["i"] | (uint32_t)DA_PBKDF2_ITERATIONS;
      hasCreds   = daValidUser(userName);
    }

    bool writeCredentials() {
      char saltHex[2*DA_SALT_BYTES+1], hashHex[2*DA_HASH_BYTES+1];
      daToHex(salt, DA_SALT_BYTES, saltHex);
      daToHex(pwHash, DA_HASH_BYTES, hashHex);
      StaticJsonDocument<512> doc;
      doc["u"] = userName;
      doc["s"] = String(saltHex);
      doc["h"] = String(hashHex);
      doc["i"] = iterations;
      File f = WLED_FS.open(DA_CREDS_FILE, "w");
      if (!f) return false;
      serializeJson(doc, f);
      f.close();
      return true;
    }

    bool saveCredentials(const String& user, const String& password) {
      uint8_t newSalt[DA_SALT_BYTES], newHash[DA_HASH_BYTES];
      esp_fill_random(newSalt, sizeof(newSalt));
      if (!daPbkdf2(password, newSalt, DA_PBKDF2_ITERATIONS, newHash)) return false;
      memcpy(salt, newSalt, sizeof(salt));
      memcpy(pwHash, newHash, sizeof(pwHash));
      iterations  = DA_PBKDF2_ITERATIONS;
      userName    = user;
      credsLoaded = true;
      hasCreds    = true;
      DEBUG_PRINTLN(F("DirectAuth: credentials updated"));
      return writeCredentials();
    }

    bool verifyLogin(const String& user, const String& password) {
      loadCredentials();
      if (!hasCreds) return false;
      uint8_t h[DA_HASH_BYTES];
      if (!daPbkdf2(password, salt, iterations, h)) return false;   // always run the KDF so timing does not reveal the username
      bool userOk = (user == userName);
      bool passOk = daConstantTimeEqual(h, pwHash, DA_HASH_BYTES);
      return userOk && passOk;
    }

    // ---------- sessions ----------
    void loadSessions() {
      if (sessionsLoaded) return;
      sessionsLoaded = true;
      for (auto& s : sessions) s.valid = false;
      if (!WLED_FS.exists(DA_SESS_FILE)) return;
      File f = WLED_FS.open(DA_SESS_FILE, "r");
      if (!f) return;
      DynamicJsonDocument doc(2048);
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      if (err) return;
      JsonArray arr = doc["s"];
      size_t i = 0;
      for (JsonObject o : arr) {
        if (i >= DA_MAX_SESSIONS) break;
        const char* h = o["h"];
        if (!daFromHex(h, sessions[i].hash, DA_HASH_BYTES)) continue;
        sessions[i].created = o["c"] | 0UL;
        sessions[i].expires = o["e"] | 0UL;
        sessions[i].valid   = true;
        i++;
      }
    }

    bool saveSessions() {
      DynamicJsonDocument doc(2048);
      JsonArray arr = doc.createNestedArray("s");
      for (const auto& s : sessions) {
        if (!s.valid) continue;
        char hex[2*DA_HASH_BYTES+1];
        daToHex(s.hash, DA_HASH_BYTES, hex);
        JsonObject o = arr.createNestedObject();
        o["h"] = String(hex);
        o["c"] = s.created;
        o["e"] = s.expires;
      }
      File f = WLED_FS.open(DA_SESS_FILE, "w");
      if (!f) return false;
      serializeJson(doc, f);
      f.close();
      return true;
    }

    void purgeExpired() {
      uint32_t now = nowUnix();
      if (!now) return;
      bool changed = false;
      for (auto& s : sessions) {
        if (s.valid && s.expires && now > s.expires) { s.valid = false; changed = true; }
      }
      if (changed) saveSessions();
    }

    // creates a session and returns the bearer token (64 hex chars). Only the SHA-256 of it is stored.
    String createSession() {
      loadSessions();
      purgeExpired();
      int slot = -1;
      for (int i = 0; i < DA_MAX_SESSIONS; i++) if (!sessions[i].valid) { slot = i; break; }
      if (slot < 0) {   // all in use: evict the oldest
        slot = 0;
        for (int i = 1; i < DA_MAX_SESSIONS; i++) if (sessions[i].created < sessions[slot].created) slot = i;
      }
      uint8_t token[DA_TOKEN_BYTES];
      esp_fill_random(token, sizeof(token));
      char hex[DA_TOKEN_HEX_LEN+1];
      daToHex(token, DA_TOKEN_BYTES, hex);
      uint32_t now = nowUnix();
      daSha256(hex, DA_TOKEN_HEX_LEN, sessions[slot].hash);
      sessions[slot].created = now;
      sessions[slot].expires = now ? now + (uint32_t)sessionDays * 86400UL : 0;
      sessions[slot].valid   = true;
      saveSessions();
      return String(hex);
    }

    int findSession(const String& token) {
      if (token.length() != DA_TOKEN_HEX_LEN) return -1;
      loadSessions();
      uint8_t h[DA_HASH_BYTES];
      daSha256(token.c_str(), DA_TOKEN_HEX_LEN, h);
      uint32_t now = nowUnix();
      for (int i = 0; i < DA_MAX_SESSIONS; i++) {
        if (!sessions[i].valid || !daConstantTimeEqual(h, sessions[i].hash, DA_HASH_BYTES)) continue;
        if (now && sessions[i].expires && now > sessions[i].expires) {
          sessions[i].valid = false;
          saveSessions();
          return -1;
        }
        return i;
      }
      return -1;
    }

    void revokeSession(int idx) {
      if (idx < 0 || idx >= DA_MAX_SESSIONS) return;
      sessions[idx].valid = false;
      saveSessions();
    }

    void revokeAll() {
      for (auto& s : sessions) s.valid = false;
      saveSessions();
    }

    int sessionCount() {
      loadSessions();
      int n = 0;
      for (const auto& s : sessions) if (s.valid) n++;
      return n;
    }

    // ---------- request helpers ----------
    String tokenFromRequest(AsyncWebServerRequest* request) {
      const String& hdr = request->header(F(DA_TOKEN_HEADER));
      if (hdr.length()) return hdr;
      const String& cookies = request->header(F("Cookie"));
      if (cookies.length()) return daCookieValue(cookies, DA_COOKIE);
      return String();
    }

    bool isAuthenticated(AsyncWebServerRequest* request) {
      String token = tokenFromRequest(request);
      return token.length() && findSession(token) >= 0;
    }

    static bool wantsJson(AsyncWebServerRequest* request) {
      if (request->hasArg("json")) return true;
      return request->header(F("Accept")).indexOf(F("application/json")) >= 0;
    }

    String cookieHeader(const String& token) {
      String c = F(DA_COOKIE "=");
      c += token;
      c += F("; Path=/; HttpOnly; SameSite=Lax; Max-Age=");
      c += String((uint32_t)sessionDays * 86400UL);
      return c;
    }

    static String clearCookieHeader() {
      return String(F(DA_COOKIE "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0"));
    }

    static void sendJson(AsyncWebServerRequest* request, int code, const String& body, const String* cookie = nullptr) {
      AsyncWebServerResponse* res = request->beginResponse(code, FPSTR(CONTENT_TYPE_JSON), body);
      res->addHeader(F("Cache-Control"), F("no-store"));
      if (cookie) res->addHeader(F("Set-Cookie"), *cookie);
      request->send(res);
    }

    static void sendError(AsyncWebServerRequest* request, int code, const __FlashStringHelper* msg) {
      String body = F("{\"error\":\"");
      body += msg;
      body += F("\"}");
      sendJson(request, code, body);
    }

    static void sendRedirect(AsyncWebServerRequest* request, const String& location, const String* cookie = nullptr) {
      AsyncWebServerResponse* res = request->beginResponse(302);
      res->addHeader(F("Location"), location);
      res->addHeader(F("Cache-Control"), F("no-store"));
      if (cookie) res->addHeader(F("Set-Cookie"), *cookie);
      request->send(res);
    }

    // shared tail of /auth/login and /auth/setup once the caller is trusted
    void finishLogin(AsyncWebServerRequest* request) {
      String token  = createSession();
      String cookie = cookieHeader(token);
      if (wantsJson(request)) {
        StaticJsonDocument<256> doc;
        doc["ok"]    = true;
        doc["token"] = token;
        doc["user"]  = userName;
        doc["days"]  = sessionDays;
        String out;
        serializeJson(doc, out);
        sendJson(request, 200, out, &cookie);
      } else {
        sendRedirect(request, daSafeRedirect(request->arg("to")), &cookie);
      }
    }

    bool isLocked() {
      if (!locked) return false;
      if ((long)(millis() - lockUntil) >= 0) { locked = false; failCount = 0; }
      return locked;
    }

    // ---------- HTTP handlers ----------
    void handleStatus(AsyncWebServerRequest* request) {
      loadCredentials();
      bool auth = hasCreds && isAuthenticated(request);
      StaticJsonDocument<384> doc;
      doc["setup"]   = !hasCreds;
      doc["auth"]    = auth;
      doc["enabled"] = enabled;
      doc["name"]    = serverDescription;
      doc["mac"]     = escapedMac;
      doc["ver"]     = versionString;
      if (auth) doc["user"] = userName;
      String out;
      serializeJson(doc, out);
      sendJson(request, 200, out);
    }

    void handleLogin(AsyncWebServerRequest* request) {
      loadCredentials();
      if (!hasCreds) { sendError(request, 409, F("setup required")); return; }
      if (isLocked()) { sendError(request, 429, F("too many attempts")); return; }
      String user = request->arg("user");
      String pass = request->arg("pass");
      user.trim();
      if (!verifyLogin(user, pass)) {
        if (++failCount >= DA_MAX_FAILS) {
          locked    = true;
          lockUntil = millis() + DA_LOCKOUT_MS;
          failCount = 0;
        }
        DEBUG_PRINTF_P(PSTR("DirectAuth: failed login from %s\n"), request->client()->remoteIP().toString().c_str());
        sendError(request, 401, F("invalid credentials"));
        return;
      }
      failCount = 0;
      finishLogin(request);
    }

    void handleSetup(AsyncWebServerRequest* request) {
      loadCredentials();
      if (hasCreds) { sendError(request, 409, F("already configured")); return; }
      String user = request->arg("user");
      String pass = request->arg("pass");
      user.trim();
      if (!daValidUser(user)) { sendError(request, 400, F("username: 1-32 chars, letters digits . _ - @")); return; }
      if (pass.length() < DA_MIN_PASS_LEN || pass.length() > DA_MAX_PASS_LEN) { sendError(request, 400, F("password must be 8-64 characters")); return; }
      if (!saveCredentials(user, pass)) { sendError(request, 500, F("could not save credentials")); return; }
      finishLogin(request);
    }

    void handleLogout(AsyncWebServerRequest* request) {
      int idx = findSession(tokenFromRequest(request));
      if (idx < 0) { sendError(request, 401, F("unauthorized")); return; }
      if (request->hasArg("all")) revokeAll(); else revokeSession(idx);
      String clear = clearCookieHeader();
      if (wantsJson(request)) sendJson(request, 200, F("{\"ok\":true}"), &clear);
      else sendRedirect(request, F("/login"), &clear);
    }

    void handleSessions(AsyncWebServerRequest* request) {
      int current = findSession(tokenFromRequest(request));
      loadSessions();
      DynamicJsonDocument doc(1024);
      doc["now"] = nowUnix();
      JsonArray arr = doc.createNestedArray("sessions");
      for (int i = 0; i < DA_MAX_SESSIONS; i++) {
        if (!sessions[i].valid) continue;
        JsonObject o = arr.createNestedObject();
        o["created"] = sessions[i].created;
        o["expires"] = sessions[i].expires;
        o["current"] = (i == current);
      }
      String out;
      serializeJson(doc, out);
      sendJson(request, 200, out);
    }

  public:
    // ---- used by the gate ----
    bool isPublicPath(const String& url) {
      if (url == "/login" || url == "/favicon.ico" || url == "/auth/status" || url == "/auth/login" || url == "/auth/setup") return true;
      #ifndef WLED_DISABLE_ALEXA
      if (allowAlexa && alexaEnabled && (url == "/description.xml" || url.startsWith("/api"))) return true;
      #endif
      return false;
    }

    // true = deny this request (not public, and no valid session / setup not done)
    bool shouldBlock(AsyncWebServerRequest* request) {
      if (!enabled) return false;
      if (request->method() == HTTP_OPTIONS) return false;   // CORS preflight carries no credentials by design; WLED answers it
      // In AP mode let anything addressed to a name (phone captive-portal probes, "http://wled/")
      // reach WLED's own captivePortal(), which 302s to http://4.3.2.1; that request arrives with
      // an IP host and is gated normally. Without this the probes get our redirect/401 and the
      // phone never shows the "sign in to network" popup.
      if (apActive) {
        const String& h = request->host();
        if (h.length() && !daIsIp(h) && h.indexOf(F("wled.me")) < 0 && h.indexOf(cmDNS) < 0 && h.indexOf(':') < 0) return false;
      }
      if (isPublicPath(request->url())) return false;
      loadCredentials();
      if (!hasCreds) return true;
      return !isAuthenticated(request);
    }

    void deny(AsyncWebServerRequest* request) {
      bool browser = request->method() == HTTP_GET && request->header(F("Accept")).indexOf(F("text/html")) >= 0;
      if (browser) {
        String loc = F("/login");
        const String& url = request->url();
        if (url.length() > 1 && url != "/" && daSafeRedirect(url) == url && url.indexOf('?') < 0) { loc += F("?to="); loc += url; }
        sendRedirect(request, loc);
      } else {
        AsyncWebServerResponse* res = request->beginResponse(401, FPSTR(CONTENT_TYPE_JSON), F("{\"error\":\"unauthorized\"}"));
        res->addHeader(F("Cache-Control"), F("no-store"));
        res->addHeader(F("WWW-Authenticate"), F("Token realm=\"WLED\", header=\"" DA_TOKEN_HEADER "\""));
        request->send(res);
      }
    }

    // ---- Usermod API ----
    void setup() override {
      loadCredentials();
      loadSessions();
      // Registered from setup(), which WLED runs before initServer(): this handler
      // is therefore first in the list and sees every request before core routes.
      gate = new DirectAuthGate(this);
      server.addHandler(gate);

      server.on("/login", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* res = request->beginResponse_P(200, FPSTR(CONTENT_TYPE_HTML), PAGE_login);
        res->addHeader(F("Cache-Control"), F("no-store"));
        request->send(res);
      });
      server.on("/auth/status",   HTTP_GET,  [this](AsyncWebServerRequest* r) { handleStatus(r); });
      server.on("/auth/login",    HTTP_POST, [this](AsyncWebServerRequest* r) { handleLogin(r); });
      server.on("/auth/setup",    HTTP_POST, [this](AsyncWebServerRequest* r) { handleSetup(r); });
      server.on("/auth/logout",   HTTP_POST, [this](AsyncWebServerRequest* r) { handleLogout(r); });
      server.on("/auth/sessions", HTTP_GET,  [this](AsyncWebServerRequest* r) { handleSessions(r); });
      DEBUG_PRINTF_P(PSTR("DirectAuth: %s, %s\n"), enabled ? "enabled" : "disabled", hasCreds ? "credentials present" : "SETUP REQUIRED");
    }

    void loop() override {
      if (gateSelfTest != -1 || !enabled) return;
      if (!WLED_CONNECTED) { selfTestDueAt = 0; return; }
      if (!selfTestDueAt) { selfTestDueAt = millis() + 10000; return; }   // give the server time to come up
      if ((long)(millis() - selfTestDueAt) < 0) return;
      runGateSelfTest();
    }

    void runGateSelfTest() {
      WiFiClient c;
      c.setTimeout(1500);
      if (!c.connect(IPAddress(127, 0, 0, 1), 80)) { gateSelfTest = 2; DEBUG_PRINTLN(F("DirectAuth: self-test skipped (no loopback)")); return; }
      c.print(F("GET /json/state HTTP/1.0\r\nHost: localhost\r\nAccept: application/json\r\nConnection: close\r\n\r\n"));
      unsigned long t0 = millis();
      while (!c.available() && millis() - t0 < 1500) delay(1);
      String status = c.readStringUntil('\n');
      c.stop();
      gateSelfTest = status.indexOf(F(" 401 ")) > 0 ? 1 : 0;
      if (gateSelfTest == 1) DEBUG_PRINTLN(F("DirectAuth: self-test passed, gate is active"));
      else DEBUG_PRINTF_P(PSTR("DirectAuth: SELF-TEST FAILED, anonymous request got '%s' - gate is not first in the handler chain\n"), status.c_str());
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray a = user.createNestedArray(F("Direct login"));
      if (!enabled)       a.add(F("disabled"));
      else if (!hasCreds) a.add(F("setup required"));
      else if (gateSelfTest == 0) a.add(F("GATE INACTIVE (self-test failed)"));
      else { a.add(sessionCount()); a.add(F(" active sessions")); }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)]   = enabled;
      top[F("sessionDays")]  = sessionDays;
      top[F("allowAlexa")]   = allowAlexa;
      top[F("newUser")]      = "";   // write-only fields: consumed in readFromConfig, never persisted
      top[F("newPassword")]  = "";
    }

    bool readFromConfig(JsonObject& root) override {
      JsonObject top = root[FPSTR(_name)];
      if (top.isNull()) return false;
      bool complete = true;
      complete &= getJsonValue(top[FPSTR(_enabled)], enabled, true);
      complete &= getJsonValue(top[F("sessionDays")], sessionDays, (uint16_t)DA_DEFAULT_DAYS);
      complete &= getJsonValue(top[F("allowAlexa")], allowAlexa, false);
      if (sessionDays < 1) sessionDays = 1;
      if (sessionDays > 365) sessionDays = 365;

      String newUser = top[F("newUser")] | "";
      String newPass = top[F("newPassword")] | "";
      newUser.trim();
      if (newPass.length()) {
        loadCredentials();
        String u = newUser.length() ? newUser : (hasCreds ? userName : String(F("admin")));
        if (daValidUser(u) && newPass.length() >= DA_MIN_PASS_LEN && newPass.length() <= DA_MAX_PASS_LEN) saveCredentials(u, newPass);
        else DEBUG_PRINTLN(F("DirectAuth: rejected new credentials from settings (length/charset)"));
      } else if (newUser.length()) {
        loadCredentials();
        if (hasCreds && daValidUser(newUser)) { userName = newUser; writeCredentials(); }
      }
      return complete;
    }

    void appendConfigData(Print& s) override {
      s.print(F("addInfo('DirectAuth:enabled',1,'off = open like stock WLED');"));
      s.print(F("addInfo('DirectAuth:sessionDays',1,'days a login stays valid (1-365)');"));
      s.print(F("addInfo('DirectAuth:allowAlexa',1,'let Alexa / Hue emulation bypass the login');"));
      s.print(F("addInfo('DirectAuth:newUser',1,'blank = keep <b>"));
      s.print(hasCreds ? userName.c_str() : "(not set up yet)");
      s.print(F("</b>');"));
      s.print(F("addInfo('DirectAuth:newPassword',1,'blank = keep; 8-64 characters');"));
      s.print(F("d.getElementsByName('DirectAuth:newPassword')[0].type='password';"));
    }
};

const char DirectAuthUsermod::_name[]    PROGMEM = "DirectAuth";
const char DirectAuthUsermod::_enabled[] PROGMEM = "enabled";

bool DirectAuthGate::canHandle(AsyncWebServerRequest* request) {
  if (!um->shouldBlock(request)) return false;
  request->addInterestingHeader(F("Accept"));   // needed in handleRequest; other headers are dropped after attach
  return true;
}

void DirectAuthGate::handleRequest(AsyncWebServerRequest* request) {
  um->deny(request);
}

static DirectAuthUsermod directAuthUsermod;
REGISTER_USERMOD(directAuthUsermod);
