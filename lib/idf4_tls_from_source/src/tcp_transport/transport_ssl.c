/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>

#include "esp_tls.h"
#include "esp_log.h"

#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_internal.h"

#define INVALID_SOCKET (-1)

#define GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t)         \
    transport_esp_tls_t *ssl = ssl_get_context_data(t);  \
    if (!ssl) { return; }

static const char *TAG = "TRANSPORT_BASE";

typedef enum {
    TRANS_SSL_INIT = 0,
    TRANS_SSL_CONNECTING,
} transport_ssl_conn_state_t;

/**
 *  mbedtls specific transport data
 */
typedef struct transport_esp_tls {
    esp_tls_t                *tls;
    esp_tls_cfg_t            cfg;
    bool                     ssl_initialized;
    transport_ssl_conn_state_t conn_state;
    int                      sockfd;
} transport_esp_tls_t;

/**
 * @brief      Destroys esp-tls transport used in the foundation transport
 *
 * @param[in]  transport esp-tls handle
 */
void esp_transport_esp_tls_destroy(struct transport_esp_tls* transport_esp_tls);

static inline transport_esp_tls_t *ssl_get_context_data(esp_transport_handle_t t)
{
    if (!t) {
        return NULL;
    }
    return (transport_esp_tls_t *)t->data;
}

// JTS-CLOUDLINK-TLS-FIX: releases 64/65 tried to read the peer certificate back out of the
// mbedtls_ssl_context AFTER esp_tls_conn_new_sync() had already failed (release 64 read it too
// late, after esp_tls_conn_destroy(); release 65 moved the read earlier, right before destroy).
// Neither could ever have worked: mbedtls_ssl_get_peer_cert() (ssl_tls.c) unconditionally
// returns NULL whenever ssl->session == NULL, and ssl->session is only populated by copying
// ssl->session_negotiate into it once a handshake completes SUCCESSFULLY. On exactly the
// failure this is meant to diagnose -- mbedtls_ssl_parse_certificate() calling
// ssl_parse_certificate_verify() and getting a non-zero result back (which is what
// MBEDTLS_ERR_X509_CERT_VERIFY_FAILED / flag 0x4 CN_MISMATCH is) -- the function takes the
// `goto exit;` branch, which frees the just-parsed chain and returns without ever assigning it
// to ssl->session_negotiate->peer_cert (that assignment only happens on the success path).
// So there was never a certificate available to read back this way, at any point, on this
// failure path, independent of the release 65 ordering fix or of MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
// -- it will always have printed "(no peer certificate received)".
// (Compare mbedtls_ssl_get_verify_result(), which explicitly falls back to
// ssl->session_negotiate->verify_result when ssl->session is NULL -- ssl_tls.c ~4957 -- which is
// why the verify-flags diagnostic from release 60/62, the -0x2700 / flag 0x4 this investigation
// started from, has been reporting real data all along even though the cert dump could not.)
//
// Fix: capture the certificate from INSIDE verification instead of after it. mbedtls calls the
// verify callback registered with mbedtls_ssl_conf_verify() once per certificate in the chain,
// with a live, not-yet-freed mbedtls_x509_crt* and that certificate's own flags, regardless of
// whether the overall chain ends up trusted (x509_crt.c's x509_crt_merge_flags_with_cb() calls it
// unconditionally while walking the chain). That is the one place this data is guaranteed to
// still exist. jts_tls_capture_verify_cb() below is registered from esp_create_mbedtls_handle()
// in esp_tls_mbedtls.c and always returns 0 -- it only observes, it must never change what flags
// mbedtls decides on, or this "diagnostic" would itself become a verification bypass.
//
// Declared extern "C" and forward-declared directly in usermods/CloudLink/CloudLink.cpp (the
// jts_tls_dump_peer_cert() reader), not a shared header -- cross-library include paths are not
// reliable in PlatformIO's LDF.
static char s_lastPeerCertInfo[160] = {0};

static void format_peer_cert_info(const mbedtls_x509_crt *crt)
{
    char *buf = s_lastPeerCertInfo;
    size_t len = sizeof(s_lastPeerCertInfo);
    size_t off = 0;
    int n = mbedtls_x509_dn_gets(buf, len, &crt->subject);
    if (n > 0) off = (size_t)n;
    if (off < len) off += snprintf(buf + off, len - off, " | SAN: ");
    const mbedtls_x509_sequence *san = &crt->subject_alt_names;
    bool any = false;
    for (; san != NULL && off < len; san = san->next) {
        if (any && off < len) off += snprintf(buf + off, len - off, ", ");
        off += snprintf(buf + off, len - off, "%.*s", (int)san->buf.len, (const char *)san->buf.p);
        any = true;
    }
    if (!any && off < len) snprintf(buf + off, len - off, "(none)");
}

// Registered via mbedtls_ssl_conf_verify() (esp_tls_mbedtls.c). Not static: that TU declares it
// extern and passes it as a function pointer, so it needs external linkage.
int jts_tls_capture_verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx; (void)flags;
    if (depth == 0 && crt) {   // depth 0 = the leaf/server certificate, the one with our SAN
        format_peer_cert_info(crt);
    }
    return 0;   // observe only -- never override mbedtls's own verification result
}

void jts_tls_dump_peer_cert(esp_transport_handle_t t, char *buf, size_t len)
{
    (void)t;   // kept in the signature for call-site compatibility
    snprintf(buf, len, "%s", s_lastPeerCertInfo[0] ? s_lastPeerCertInfo : "(no certificate seen this attempt)");
}
// JTS-CLOUDLINK-TLS-FIX-END

static int esp_tls_connect_async(esp_transport_handle_t t, const char *host, int port, int timeout_ms, bool is_plain_tcp)
{
    transport_esp_tls_t *ssl = ssl_get_context_data(t);
    if (ssl->conn_state == TRANS_SSL_INIT) {
        ssl->cfg.timeout_ms = timeout_ms;
        ssl->cfg.is_plain_tcp = is_plain_tcp;
        ssl->cfg.non_block = true;
        ssl->ssl_initialized = true;
        ssl->tls = esp_tls_init();
        if (!ssl->tls) {
            return -1;
        }
        ssl->conn_state = TRANS_SSL_CONNECTING;
        ssl->sockfd = INVALID_SOCKET;
    }
    if (ssl->conn_state == TRANS_SSL_CONNECTING) {
        int progress = esp_tls_conn_new_async(host, strlen(host), port, &ssl->cfg, ssl->tls);
        if (progress >= 0) {
            ssl->sockfd = ssl->tls->sockfd;
        }
        return progress;

    }
    return 0;
}

static inline int ssl_connect_async(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    return esp_tls_connect_async(t, host, port, timeout_ms, false);
}

static inline int tcp_connect_async(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    return esp_tls_connect_async(t, host, port, timeout_ms, true);
}

static int ssl_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    transport_esp_tls_t *ssl = ssl_get_context_data(t);

    ssl->cfg.timeout_ms = timeout_ms;

    // JTS-CLOUDLINK-TLS-FIX: clear any capture left over from a previous attempt so a failure
    // that never reaches certificate verification (e.g. a bare TCP/DNS failure) can't be
    // misread as "the same certificate as last time" -- jts_tls_capture_verify_cb() below is
    // the only thing that fills this in, and only once verification actually runs.
    s_lastPeerCertInfo[0] = '\0';

    ssl->ssl_initialized = true;
    ssl->tls = esp_tls_init();
    if (ssl->tls == NULL) {
        ESP_LOGE(TAG, "Failed to initialize new connection object");
        capture_tcp_transport_error(t, ERR_TCP_TRANSPORT_NO_MEM);
        return -1;
    }
    if (esp_tls_conn_new_sync(host, strlen(host), port, &ssl->cfg, ssl->tls) <= 0) {
        ESP_LOGE(TAG, "Failed to open a new connection");
        esp_transport_set_errors(t, ssl->tls->error_handle);
        // No cert capture here: by now, on a verify failure, mbedtls has already freed the
        // parsed chain (see the JTS-CLOUDLINK-TLS-FIX comment above) -- s_lastPeerCertInfo was
        // already filled during the handshake itself, via jts_tls_capture_verify_cb(), if
        // verification ran at all.
        esp_tls_conn_destroy(ssl->tls);
        ssl->tls = NULL;
        ssl->sockfd = INVALID_SOCKET;
        return -1;
    }
    ssl->sockfd = ssl->tls->sockfd;
    return 0;
}

static int tcp_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    transport_esp_tls_t *ssl = ssl_get_context_data(t);
    esp_tls_last_error_t *err_handle = esp_transport_get_error_handle(t);

    ssl->cfg.timeout_ms = timeout_ms;
    esp_err_t err = esp_tls_plain_tcp_connect(host, strlen(host), port, &ssl->cfg, err_handle, &ssl->sockfd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open a new connection: %d", err);
        err_handle->last_error = err;
        ssl->sockfd = INVALID_SOCKET;
        return -1;
    }
    return 0;
}

static int base_poll_read(esp_transport_handle_t t, int timeout_ms)
{
    transport_esp_tls_t *ssl = ssl_get_context_data(t);
    int ret = -1;
    int remain = 0;
    struct timeval timeout;
    fd_set readset;
    fd_set errset;
    FD_ZERO(&readset);
    FD_ZERO(&errset);
    FD_SET(ssl->sockfd, &readset);
    FD_SET(ssl->sockfd, &errset);

    if (ssl->tls && (remain = esp_tls_get_bytes_avail(ssl->tls)) > 0) {
        ESP_LOGD(TAG, "remain data in cache, need to read again");
        return remain;
    }
    ret = select(ssl->sockfd + 1, &readset, NULL, &errset, esp_transport_utils_ms_to_timeval(timeout_ms, &timeout));
    if (ret > 0 && FD_ISSET(ssl->sockfd, &errset)) {
        int sock_errno = 0;
        uint32_t optlen = sizeof(sock_errno);
        getsockopt(ssl->sockfd, SOL_SOCKET, SO_ERROR, &sock_errno, &optlen);
        esp_transport_capture_errno(t, sock_errno);
        ESP_LOGE(TAG, "poll_read select error %d, errno = %s, fd = %d", sock_errno, strerror(sock_errno), ssl->sockfd);
        ret = -1;
    }
    return ret;
}

static int base_poll_write(esp_transport_handle_t t, int timeout_ms)
{
    transport_esp_tls_t *ssl = ssl_get_context_data(t);
    int ret = -1;
    struct timeval timeout;
    fd_set writeset;
    fd_set errset;
    FD_ZERO(&writeset);
    FD_ZERO(&errset);
    FD_SET(ssl->sockfd, &writeset);
    FD_SET(ssl->sockfd, &errset);
    ret = select(ssl->sockfd + 1, NULL, &writeset, &errset, esp_transport_utils_ms_to_timeval(timeout_ms, &timeout));
    if (ret > 0 && FD_ISSET(ssl->sockfd, &errset)) {
        int sock_errno = 0;
        uint32_t optlen = sizeof(sock_errno);
        getsockopt(ssl->sockfd, SOL_SOCKET, SO_ERROR, &sock_errno, &optlen);
        esp_transport_capture_errno(t, sock_errno);
        ESP_LOGE(TAG, "poll_write select error %d, errno = %s, fd = %d", sock_errno, strerror(sock_errno), ssl->sockfd);
        ret = -1;
    }
    return ret;
}

static int ssl_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    int poll;
    transport_esp_tls_t *ssl = ssl_get_context_data(t);

    if ((poll = esp_transport_poll_write(t, timeout_ms)) <= 0) {
        ESP_LOGW(TAG, "Poll timeout or error, errno=%s, fd=%d, timeout_ms=%d", strerror(errno), ssl->sockfd, timeout_ms);
        return poll;
    }
    int ret = esp_tls_conn_write(ssl->tls, (const unsigned char *) buffer, len);
    if (ret < 0) {
        ESP_LOGE(TAG, "esp_tls_conn_write error, errno=%s", strerror(errno));
        esp_transport_set_errors(t, ssl->tls->error_handle);
    }
    return ret;
}

static int tcp_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    int poll;
    transport_esp_tls_t *ssl = ssl_get_context_data(t);

    if ((poll = esp_transport_poll_write(t, timeout_ms)) <= 0) {
        ESP_LOGW(TAG, "Poll timeout or error, errno=%s, fd=%d, timeout_ms=%d", strerror(errno), ssl->sockfd, timeout_ms);
        return poll;
    }
    int ret = send(ssl->sockfd, (const unsigned char *) buffer, len, 0);
    if (ret < 0) {
        ESP_LOGE(TAG, "tcp_write error, errno=%s", strerror(errno));
        esp_transport_capture_errno(t, errno);
    }
    return ret;
}

static int ssl_read(esp_transport_handle_t t, char *buffer, int len, int timeout_ms)
{
    int poll;
    transport_esp_tls_t *ssl = ssl_get_context_data(t);

    if ((poll = esp_transport_poll_read(t, timeout_ms)) <= 0) {
        return poll;
    }
    int ret = esp_tls_conn_read(ssl->tls, (unsigned char *)buffer, len);
    if (ret < 0) {
        ESP_LOGE(TAG, "esp_tls_conn_read error, errno=%s", strerror(errno));
        esp_transport_set_errors(t, ssl->tls->error_handle);
    }
    if (ret == 0) {
        if (poll > 0) {
            // no error, socket reads 0 while previously detected as readable -> connection has been closed cleanly
            capture_tcp_transport_error(t, ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN);
        }
        ret = -1;
    }
    return ret;
}

static int tcp_read(esp_transport_handle_t t, char *buffer, int len, int timeout_ms)
{
    int poll;
    transport_esp_tls_t *ssl = ssl_get_context_data(t);

    if ((poll = esp_transport_poll_read(t, timeout_ms)) <= 0) {
        return poll;
    }
    int ret = recv(ssl->sockfd, (unsigned char *)buffer, len, 0);
    if (ret < 0) {
        ESP_LOGE(TAG, "tcp_read error, errno=%s", strerror(errno));
        esp_transport_capture_errno(t, errno);
    }
    if (ret == 0) {
        if (poll > 0) {
            // no error, socket reads 0 while previously detected as readable -> connection has been closed cleanly
            capture_tcp_transport_error(t, ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN);
        }
        ret = -1;
    }
    return ret;
}

static int base_close(esp_transport_handle_t t)
{
    int ret = -1;
    transport_esp_tls_t *ssl = ssl_get_context_data(t);
    if (ssl && ssl->ssl_initialized) {
        ret = esp_tls_conn_destroy(ssl->tls);
        ssl->tls = NULL;
        ssl->conn_state = TRANS_SSL_INIT;
        ssl->ssl_initialized = false;
        ssl->sockfd = INVALID_SOCKET;
    } else if (ssl && ssl->sockfd >= 0) {
        ret = close(ssl->sockfd);
        ssl->sockfd = INVALID_SOCKET;
    }
    return ret;
}

static int base_destroy(esp_transport_handle_t transport)
{
    transport_esp_tls_t *ssl = ssl_get_context_data(transport);
    if (ssl) {
        esp_transport_close(transport);
        esp_transport_destroy_foundation_transport(transport->foundation);

        esp_transport_esp_tls_destroy(transport->data); // okay to pass NULL
    }
    return 0;
}

void esp_transport_ssl_enable_global_ca_store(esp_transport_handle_t t)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.use_global_ca_store = true;
}

#ifdef CONFIG_ESP_TLS_PSK_VERIFICATION
void esp_transport_ssl_set_psk_key_hint(esp_transport_handle_t t, const psk_hint_key_t *psk_hint_key)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.psk_hint_key =  psk_hint_key;
}
#endif

void esp_transport_ssl_set_cert_data(esp_transport_handle_t t, const char *data, int len)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.cacert_pem_buf = (void *)data;
    ssl->cfg.cacert_pem_bytes = len + 1;
}

void esp_transport_ssl_set_cert_data_der(esp_transport_handle_t t, const char *data, int len)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.cacert_buf = (void *)data;
    ssl->cfg.cacert_bytes = len;
}

void esp_transport_ssl_set_client_cert_data(esp_transport_handle_t t, const char *data, int len)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.clientcert_pem_buf = (void *)data;
    ssl->cfg.clientcert_pem_bytes = len + 1;
}

void esp_transport_ssl_set_client_cert_data_der(esp_transport_handle_t t, const char *data, int len)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.clientcert_buf = (void *)data;
    ssl->cfg.clientcert_bytes = len;
}

void esp_transport_ssl_set_client_key_data(esp_transport_handle_t t, const char *data, int len)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.clientkey_pem_buf = (void *)data;
    ssl->cfg.clientkey_pem_bytes = len + 1;
}

void esp_transport_ssl_set_client_key_password(esp_transport_handle_t t, const char *password, int password_len)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.clientkey_password = (void *)password;
    ssl->cfg.clientkey_password_len = password_len;
}

void esp_transport_ssl_set_client_key_data_der(esp_transport_handle_t t, const char *data, int len)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.clientkey_buf = (void *)data;
    ssl->cfg.clientkey_bytes = len;
}

#if defined(CONFIG_MBEDTLS_SSL_ALPN) || defined(CONFIG_WOLFSSL_HAVE_ALPN)
void esp_transport_ssl_set_alpn_protocol(esp_transport_handle_t t, const char **alpn_protos)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.alpn_protos = alpn_protos;
}
#endif

void esp_transport_ssl_skip_common_name_check(esp_transport_handle_t t)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.skip_common_name = true;
}

#ifdef CONFIG_ESP_TLS_USE_SECURE_ELEMENT
void esp_transport_ssl_use_secure_element(esp_transport_handle_t t)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.use_secure_element = true;
}
#endif

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t t, esp_err_t ((*crt_bundle_attach)(void *conf)))
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.crt_bundle_attach = crt_bundle_attach;
}
#endif

static int base_get_socket(esp_transport_handle_t t)
{
    transport_esp_tls_t *ctx = ssl_get_context_data(t);
    if (ctx) {
        return ctx->sockfd;
    }
    return INVALID_SOCKET;
}

#ifdef CONFIG_ESP_TLS_USE_DS_PERIPHERAL
void esp_transport_ssl_set_ds_data(esp_transport_handle_t t, void *ds_data)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.ds_data = ds_data;
}
#endif

void esp_transport_ssl_set_keep_alive(esp_transport_handle_t t, esp_transport_keep_alive_t *keep_alive_cfg)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.keep_alive_cfg = (tls_keep_alive_cfg_t *) keep_alive_cfg;
}

void esp_transport_ssl_set_interface_name(esp_transport_handle_t t, struct ifreq *if_name)
{
    GET_SSL_FROM_TRANSPORT_OR_RETURN(ssl, t);
    ssl->cfg.if_name = if_name;
}

static transport_esp_tls_t *esp_transport_esp_tls_create(void)
{
    transport_esp_tls_t *transport_esp_tls = calloc(1, sizeof(transport_esp_tls_t));
    if (transport_esp_tls == NULL) {
        return NULL;
    }
    transport_esp_tls->sockfd = INVALID_SOCKET;
    return transport_esp_tls;
}

static esp_transport_handle_t esp_transport_base_init(void)
{
    esp_transport_handle_t transport = esp_transport_init();
    if (transport == NULL) {
        return NULL;
    }
    transport->foundation = esp_transport_init_foundation_transport();
    ESP_TRANSPORT_MEM_CHECK(TAG, transport->foundation,
                            free(transport);
                            return NULL);

    transport->data = esp_transport_esp_tls_create();
    ESP_TRANSPORT_MEM_CHECK(TAG, transport->data,
                            free(transport->foundation);
                            free(transport);
                            return NULL)
    return transport;
}

esp_transport_handle_t esp_transport_ssl_init(void)
{
    esp_transport_handle_t ssl_transport = esp_transport_base_init();
    if (ssl_transport == NULL) {
        return NULL;
    }
    ((transport_esp_tls_t *)ssl_transport->data)->cfg.is_plain_tcp = false;
    esp_transport_set_func(ssl_transport, ssl_connect, ssl_read, ssl_write, base_close, base_poll_read, base_poll_write, base_destroy);
    esp_transport_set_async_connect_func(ssl_transport, ssl_connect_async);
    ssl_transport->_get_socket = base_get_socket;
    return ssl_transport;
}


void esp_transport_esp_tls_destroy(struct transport_esp_tls *transport_esp_tls)
{
    free(transport_esp_tls);
}

esp_transport_handle_t esp_transport_tcp_init(void)
{
    esp_transport_handle_t tcp_transport = esp_transport_base_init();
    if (tcp_transport == NULL) {
        return NULL;
    }
    ((transport_esp_tls_t *)tcp_transport->data)->cfg.is_plain_tcp = true;
    esp_transport_set_func(tcp_transport, tcp_connect, tcp_read, tcp_write, base_close, base_poll_read, base_poll_write, base_destroy);
    esp_transport_set_async_connect_func(tcp_transport, tcp_connect_async);
    tcp_transport->_get_socket = base_get_socket;
    return tcp_transport;
}

void esp_transport_tcp_set_keep_alive(esp_transport_handle_t t, esp_transport_keep_alive_t *keep_alive_cfg)
{
    return esp_transport_ssl_set_keep_alive(t, keep_alive_cfg);
}

void esp_transport_tcp_set_interface_name(esp_transport_handle_t t, struct ifreq *if_name)
{
    return esp_transport_ssl_set_interface_name(t, if_name);
}
