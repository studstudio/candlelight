// TLS session resumption for the lamp's connections to the worker.
//
// A full TLS handshake costs the ESP32 ~1.6 s, mostly the key exchange math.
// After one, the server hands the client a session ticket (an encrypted blob
// only the server can read). Offering that ticket on the next connection lets
// both sides skip the key exchange. The ticket is not tied to the lamp's IP
// address, so a new DHCP lease or network doesn't matter; if the server no
// longer accepts it (expired, keys rotated) the handshake simply runs in full.
//
// WiFiClientSecure has no hook to offer a saved session, so connectResumable()
// does what WiFiClientSecure::connect() does with setInsecure() (the same
// socket setup as the library's start_ssl_client(), minus the certificate
// options the lamp doesn't use) plus mbedtls_ssl_set_session() before the
// handshake and mbedtls_ssl_get_session() after it. Everything else (read,
// write, available, stop) is the library's own.
#pragma once
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lwip/sockets.h>

class ResumableTLS : public WiFiClientSecure {
 public:
  // connects to host:port like connect() after setInsecure() (prototype only:
  // no certificate check), offering the session saved in sess[0..sessLen). On
  // success the session now in use is saved back into sess (sessLen updated;
  // 0 if it doesn't fit in sessCap). Returns 1 on success, 0 on failure
  int connectResumable(const char* host, uint16_t port, uint8_t* sess, size_t& sessLen, size_t sessCap) {
    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) return 0;
    if (start(ip, port, host, sess, sessLen) < 0) {
      stop();
      return 0;
    }
    _connected = true;
    save(sess, sessLen, sessCap);
    return 1;
  }

 private:
  int start(const IPAddress& ip, uint16_t port, const char* host, const uint8_t* sess, size_t sessLen) {
    sslclient_context* c = sslclient;
    int enable = 1;
    c->socket = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->socket < 0) return -1;

    // non-blocking connect with a timeout, then the same socket options as the library
    fcntl(c->socket, F_SETFL, fcntl(c->socket, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip;
    addr.sin_port = htons(port);
    int timeout = _timeout > 0 ? _timeout : 30000;
    c->socket_timeout = timeout;
    fd_set fdset;
    struct timeval tv;
    FD_ZERO(&fdset);
    FD_SET(c->socket, &fdset);
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    int res = lwip_connect(c->socket, (struct sockaddr*)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) return -1;
    res = select(c->socket + 1, nullptr, &fdset, nullptr, &tv);
    if (res <= 0) return -1;
    int sockerr;
    socklen_t len = sizeof(int);
    if (getsockopt(c->socket, SOL_SOCKET, SO_ERROR, &sockerr, &len) < 0 || sockerr != 0) return -1;
    if (lwip_setsockopt(c->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return -1;
    if (lwip_setsockopt(c->socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return -1;
    if (lwip_setsockopt(c->socket, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) < 0) return -1;
    if (lwip_setsockopt(c->socket, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable)) < 0) return -1;

    // TLS setup, as the library does it with setInsecure()
    static const char* pers = "esp32-tls";
    mbedtls_entropy_init(&c->entropy_ctx);
    if (mbedtls_ctr_drbg_seed(&c->drbg_ctx, mbedtls_entropy_func, &c->entropy_ctx, (const unsigned char*)pers,
                              strlen(pers)) != 0)
      return -1;
    if (mbedtls_ssl_config_defaults(&c->ssl_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
      return -1;
    mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);  // prototype only
    mbedtls_ssl_conf_session_tickets(&c->ssl_conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
    if (mbedtls_ssl_set_hostname(&c->ssl_ctx, host) != 0) return -1;
    mbedtls_ssl_conf_rng(&c->ssl_conf, mbedtls_ctr_drbg_random, &c->drbg_ctx);
    if (mbedtls_ssl_setup(&c->ssl_ctx, &c->ssl_conf) != 0) return -1;
    mbedtls_ssl_set_bio(&c->ssl_ctx, &c->socket, mbedtls_net_send, mbedtls_net_recv, NULL);

    // the saved session, if there is one and it still parses (an unusable one is just skipped)
    if (sessLen) {
      mbedtls_ssl_session s;
      mbedtls_ssl_session_init(&s);
      if (mbedtls_ssl_session_load(&s, sess, sessLen) == 0) mbedtls_ssl_set_session(&c->ssl_ctx, &s);
      mbedtls_ssl_session_free(&s);
    }

    uint32_t t0 = millis();
    int ret;
    while ((ret = mbedtls_ssl_handshake(&c->ssl_ctx)) != 0) {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) return -1;
      if (millis() - t0 > c->handshake_timeout) return -1;
      vTaskDelay(2);
    }
    return c->socket;
  }

  // keeps the session (with the newest ticket the server sent) for the next connection
  void save(uint8_t* sess, size_t& sessLen, size_t sessCap) {
    mbedtls_ssl_session s;
    mbedtls_ssl_session_init(&s);
    size_t olen = 0;
    if (mbedtls_ssl_get_session(&sslclient->ssl_ctx, &s) == 0 && mbedtls_ssl_session_save(&s, sess, sessCap, &olen) == 0)
      sessLen = olen;
    else
      sessLen = 0;
    mbedtls_ssl_session_free(&s);
  }
};
