/*
 * pico_internal.h — Shared transport layer for pico_http and pico_ws
 *
 * Internal header — NOT part of the public API. Provides the PicoConn
 * abstraction (raw socket + optional TLS via mbedTLS) used by both
 * the HTTP client and WebSocket client.
 *
 * When PICO_HTTP_APENNINES is defined, mbedTLS is not used — the
 * apennines stack provides its own TLS. PicoConn remains for pico_ws.
 *
 * License: MIT
 * Copyright (c) 2026 The now contributors
 */
#ifndef PICO_INTERNAL_H
#define PICO_INTERNAL_H

#include <stddef.h>
#include <string.h>

#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
  #include <mbedtls/ssl.h>
  #include <mbedtls/entropy.h>
  #include <mbedtls/ctr_drbg.h>
  #include <mbedtls/x509_crt.h>
#endif

/* ---- Platform sockets ---- */

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")

  typedef SOCKET pico_socket_t;
  #define PICO_INVALID_SOCKET INVALID_SOCKET
  #define pico_closesocket closesocket
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/time.h>     /* struct timeval (SO_RCVTIMEO / SO_SNDTIMEO) */
  #include <netinet/in.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <poll.h>

  typedef int pico_socket_t;
  #define PICO_INVALID_SOCKET (-1)
  #define pico_closesocket close
#endif

/* ---- Connection abstraction ---- */

typedef struct {
    pico_socket_t sock;
    int           use_tls;
#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
    mbedtls_x509_crt         cacert;
    int                      tls_init;
    int                      tls_verify;   /* 1=REQUIRED (default), 0=NONE */
    int                      cacert_init;  /* 1 if cacert has been initialized */
    int                      alpn_h2;     /* 1 if ALPN negotiated h2 */
#endif
} PicoConn;

/* Declared in pico_transport.c — shared across pico_*.c files */
int  pico_wsa_init(void);
void pico_conn_init(PicoConn *c);
int  pico_conn_send(PicoConn *c, const char *data, size_t len);
int  pico_conn_recv(PicoConn *c, char *buf, int buflen);
void pico_conn_close(PicoConn *c);
void pico_set_timeout(pico_socket_t sock, int timeout_ms);
pico_socket_t pico_connect(const char *host, int port,
                            int connect_timeout_ms, int *err_out);
#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)
int  pico_tls_handshake(PicoConn *c, const char *hostname);
int  pico_load_system_ca(PicoConn *c);
int  pico_load_ca_file(PicoConn *c, const char *path);
int  pico_load_ca_data(PicoConn *c, const unsigned char *pem,
                       size_t pem_len);
#endif

#endif /* PICO_INTERNAL_H */
