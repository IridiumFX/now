/*
 * pico_transport.c — Shared socket + TLS transport layer
 *
 * Extracted from pico_http.c. Provides PicoConn (socket + optional
 * mbedTLS), pico_connect, pico_set_timeout, pico_wsa_init.
 * Used by both pico_http.c and pico_ws.c.
 *
 * When PICO_HTTP_APENNINES is defined, the mbedTLS-dependent TLS
 * functions are excluded (apennines provides its own TLS stack).
 *
 * License: MIT
 * Copyright (c) 2026 The now contributors
 */

#ifndef PICO_HTTP_BUILDING
  #define PICO_HTTP_BUILDING
#endif

#include "pico_http.h"
#include "pico_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PICO_HTTP_APENNINES
#ifdef PICO_HTTP_TLS
  #include <mbedtls/error.h>
  #ifdef _WIN32
    #include <wincrypt.h>
  #endif
#endif
#endif /* !PICO_HTTP_APENNINES */

/* ---- Platform init ---- */

#ifdef _WIN32
  int pico_wsa_init(void) {
      static int done = 0;
      if (!done) {
          WSADATA wsa;
          if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
          done = 1;
      }
      return 0;
  }
#else
  int pico_wsa_init(void) { return 0; }
#endif

/* ---- Connection abstraction ---- */

void pico_conn_init(PicoConn *c) {
    memset(c, 0, sizeof(*c));
    c->sock = PICO_INVALID_SOCKET;
}

int pico_conn_send(PicoConn *c, const char *data, size_t len) {
#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
    if (c->use_tls) {
        while (len > 0) {
            int ret = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data, len);
            if (ret < 0) return -1;
            data += ret;
            len -= (size_t)ret;
        }
        return 0;
    }
#endif
    size_t sent = 0;
    while (sent < len) {
        int n = send(c->sock, data + sent, (int)(len - sent), 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int pico_conn_recv(PicoConn *c, char *buf, int buflen) {
#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
    if (c->use_tls) {
        int ret = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, (size_t)buflen);
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return ret;
    }
#endif
    return recv(c->sock, buf, buflen, 0);
}

void pico_conn_close(PicoConn *c) {
#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
    if (c->tls_init) {
        if (c->use_tls)
            mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->entropy);
        c->tls_init = 0;
    }
    if (c->cacert_init) {
        mbedtls_x509_crt_free(&c->cacert);
        c->cacert_init = 0;
    }
#endif
    if (c->sock != PICO_INVALID_SOCKET) {
        pico_closesocket(c->sock);
        c->sock = PICO_INVALID_SOCKET;
    }
}

/* ---- Socket timeout ---- */

void pico_set_timeout(pico_socket_t sock, int timeout_ms) {
#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

/* ---- Connect with timeout ---- */

pico_socket_t pico_connect(const char *host, int port,
                            int connect_timeout_ms, int *err_out) {
    struct addrinfo hints, *res, *rp;
    char port_str[16];
    pico_socket_t sock = PICO_INVALID_SOCKET;

    snprintf(port_str, sizeof(port_str), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        *err_out = PICO_ERR_DNS;
        return PICO_INVALID_SOCKET;
    }

    for (rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == PICO_INVALID_SOCKET) continue;

        if (connect_timeout_ms > 0) {
#ifdef _WIN32
            unsigned long nb = 1;
            ioctlsocket(sock, FIONBIO, &nb);

            if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) != 0) {
                int wsa_err = WSAGetLastError();
                if (wsa_err == WSAEWOULDBLOCK) {
                    fd_set wset, eset;
                    struct timeval tv;
                    tv.tv_sec  = connect_timeout_ms / 1000;
                    tv.tv_usec = (connect_timeout_ms % 1000) * 1000;
                    FD_ZERO(&wset);
                    FD_ZERO(&eset);
                    FD_SET(sock, &wset);
                    FD_SET(sock, &eset);
                    if (select(0, NULL, &wset, &eset, &tv) > 0) {
                        int optval = 0;
                        int optlen = sizeof(optval);
                        getsockopt(sock, SOL_SOCKET, SO_ERROR,
                                   (char *)&optval, &optlen);
                        if (optval != 0 || FD_ISSET(sock, &eset)) {
                            pico_closesocket(sock);
                            sock = PICO_INVALID_SOCKET;
                            continue;
                        }
                        nb = 0;
                        ioctlsocket(sock, FIONBIO, &nb);
                        break;
                    } else {
                        pico_closesocket(sock);
                        sock = PICO_INVALID_SOCKET;
                        continue;
                    }
                } else {
                    pico_closesocket(sock);
                    sock = PICO_INVALID_SOCKET;
                    continue;
                }
            } else {
                nb = 0;
                ioctlsocket(sock, FIONBIO, &nb);
                break;
            }
#else
            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            if (connect(sock, rp->ai_addr, rp->ai_addrlen) != 0) {
                if (errno == EINPROGRESS) {
                    struct pollfd pfd;
                    pfd.fd = sock;
                    pfd.events = POLLOUT;
                    if (poll(&pfd, 1, connect_timeout_ms) > 0) {
                        int optval = 0;
                        socklen_t optlen = sizeof(optval);
                        getsockopt(sock, SOL_SOCKET, SO_ERROR,
                                   &optval, &optlen);
                        if (optval != 0) {
                            pico_closesocket(sock);
                            sock = PICO_INVALID_SOCKET;
                            continue;
                        }
                        fcntl(sock, F_SETFL, flags);
                        break;
                    } else {
                        pico_closesocket(sock);
                        sock = PICO_INVALID_SOCKET;
                        continue;
                    }
                } else {
                    pico_closesocket(sock);
                    sock = PICO_INVALID_SOCKET;
                    continue;
                }
            } else {
                fcntl(sock, F_SETFL, flags);
                break;
            }
#endif
        } else {
            if (connect(sock, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
            pico_closesocket(sock);
            sock = PICO_INVALID_SOCKET;
        }
    }

    freeaddrinfo(res);
    if (sock == PICO_INVALID_SOCKET)
        *err_out = PICO_ERR_CONNECT;
    return sock;
}

/* ---- TLS (mbedTLS only, not compiled with apennines backend) ---- */

#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)

int pico_tls_send(void *ctx, const unsigned char *buf, size_t len) {
    pico_socket_t s = *(pico_socket_t *)ctx;
    int n = send(s, (const char *)buf, (int)len, 0);
    return n < 0 ? -1 : n;
}

int pico_tls_recv(void *ctx, unsigned char *buf, size_t len) {
    pico_socket_t s = *(pico_socket_t *)ctx;
    int n = recv(s, (char *)buf, (int)len, 0);
    if (n < 0) return -1;
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return n;
}

#ifdef _WIN32
int pico_load_system_ca(PicoConn *c) {
    if (!c->cacert_init) { mbedtls_x509_crt_init(&c->cacert); c->cacert_init = 1; }
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    if (!store) return -1;
    int loaded = 0;
    PCCERT_CONTEXT ctx = NULL;
    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != NULL) {
        if (mbedtls_x509_crt_parse_der(&c->cacert, ctx->pbCertEncoded, ctx->cbCertEncoded) == 0)
            loaded++;
    }
    CertCloseStore(store, 0);
    return loaded > 0 ? 0 : -1;
}
#else
static const char *const pico_ca_paths[] = {
    "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/cert.pem", "/etc/ssl/ca-bundle.pem",
    "/usr/local/share/certs/ca-root-nss.crt", NULL
};
int pico_load_system_ca(PicoConn *c) {
    if (!c->cacert_init) { mbedtls_x509_crt_init(&c->cacert); c->cacert_init = 1; }
    for (int i = 0; pico_ca_paths[i]; i++)
        if (mbedtls_x509_crt_parse_file(&c->cacert, pico_ca_paths[i]) == 0) return 0;
    return -1;
}
#endif

int pico_load_ca_file(PicoConn *c, const char *path) {
    if (!path) return -1;
    if (!c->cacert_init) { mbedtls_x509_crt_init(&c->cacert); c->cacert_init = 1; }
    return mbedtls_x509_crt_parse_file(&c->cacert, path) == 0 ? 0 : -1;
}

int pico_load_ca_data(PicoConn *c, const unsigned char *pem, size_t pem_len) {
    if (!pem || pem_len == 0) return -1;
    if (!c->cacert_init) { mbedtls_x509_crt_init(&c->cacert); c->cacert_init = 1; }
    return mbedtls_x509_crt_parse(&c->cacert, pem, pem_len) == 0 ? 0 : -1;
}

int pico_tls_handshake(PicoConn *c, const char *hostname) {
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_entropy_init(&c->entropy);
    c->tls_init = 1;
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy, NULL, 0) != 0)
        return PICO_ERR_TLS;
    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return PICO_ERR_TLS;
    if (c->tls_verify && c->cacert_init) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
    } else if (c->tls_verify) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    /* ALPN: prefer h2, fall back to http/1.1 */
    static const char *alpn_list[] = { "h2", "http/1.1", NULL };
    mbedtls_ssl_conf_alpn_protocols(&c->conf, alpn_list);
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) return PICO_ERR_TLS;
    mbedtls_ssl_set_hostname(&c->ssl, hostname);
    mbedtls_ssl_set_bio(&c->ssl, &c->sock, pico_tls_send, pico_tls_recv, NULL);
    int ret;
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return PICO_ERR_TLS;
    }
    c->use_tls = 1;
    /* Check ALPN result */
    const char *alpn = mbedtls_ssl_get_alpn_protocol(&c->ssl);
    c->alpn_h2 = (alpn && strcmp(alpn, "h2") == 0) ? 1 : 0;
    return PICO_OK;
}

#endif /* PICO_HTTP_TLS && !PICO_HTTP_APENNINES */
