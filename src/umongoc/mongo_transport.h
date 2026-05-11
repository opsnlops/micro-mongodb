/*
 * mongo_transport — blocking TCP socket facade over lwIP altcp, for FreeRTOS
 * tasks. Plain TCP only at this stage; the same surface will be reused for
 * TLS later by swapping the altcp_tcp_new() backing for altcp_tls_new().
 *
 * Threading: a single mongo_transport_t is owned by exactly one caller task.
 * lwIP callbacks run on the tcpip thread; we signal the owner via FreeRTOS
 * binary semaphores and synchronize state with the lwIP core lock.
 */

#ifndef MICRO_MONGODB_TRANSPORT_H
#define MICRO_MONGODB_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mongo_transport mongo_transport_t;

typedef enum {
    MONGO_TRANSPORT_OK = 0,
    MONGO_TRANSPORT_ERR_ALLOC = -1,
    MONGO_TRANSPORT_ERR_DNS = -2,
    MONGO_TRANSPORT_ERR_CONNECT = -3,
    MONGO_TRANSPORT_ERR_TIMEOUT = -4,
    MONGO_TRANSPORT_ERR_CLOSED = -5,
    MONGO_TRANSPORT_ERR_LWIP = -6,
    MONGO_TRANSPORT_ERR_STATE = -7,
    MONGO_TRANSPORT_ERR_ARGS = -8,
    MONGO_TRANSPORT_ERR_TLS = -9,
} mongo_transport_status_t;

/* TLS configuration. Pass NULL to mongo_transport_new() for plain TCP. */
typedef struct {
    /* PEM-encoded CA cert(s). If `ca_pem` is NULL, the built-in ISRG Root X1
     * bundle (mongo_ca_default_pem) is used. */
    const char *ca_pem;
    size_t ca_pem_len;

    /* Hostname for SNI. Required for Atlas. The string is copied; the buffer
     * does not need to outlive this call. */
    const char *sni_hostname;
} mongo_tls_config_t;

/* Create a transport. Pass NULL for plain TCP, or a populated mongo_tls_config_t
 * for TLS. The CA / SNI must be set at creation time and apply to every
 * subsequent mongo_transport_connect() call on this transport. */
mongo_transport_t *mongo_transport_new(const mongo_tls_config_t *tls);
void mongo_transport_free(mongo_transport_t *t);

/* Connect to host:port. `host` may be a dotted-quad IPv4 ("192.168.1.42") or
 * a hostname resolved via lwIP DNS. Blocks up to `timeout_ms` waiting for
 * DNS + TCP handshake. Returns 0 on success, negative status on failure. */
int mongo_transport_connect(mongo_transport_t *t, const char *host, uint16_t port, uint32_t timeout_ms);

/* Queue `len` bytes for transmission. Blocks until lwIP has accepted all of
 * them into its send buffer (with backpressure when the buffer is full),
 * or until `timeout_ms` elapses. Returns 0 on success. */
int mongo_transport_send(mongo_transport_t *t, const uint8_t *data, size_t len, uint32_t timeout_ms);

/* Read exactly `want` bytes into `buf`. Blocks until satisfied, the peer
 * closes, or `timeout_ms` elapses. Returns 0 on success; a partial read is
 * always treated as an error. */
int mongo_transport_recv_exact(mongo_transport_t *t, uint8_t *buf, size_t want, uint32_t timeout_ms);

/* Close the connection. Safe to call multiple times; safe on a never-connected
 * transport. */
void mongo_transport_close(mongo_transport_t *t);

bool mongo_transport_is_connected(const mongo_transport_t *t);

/* Last lwIP-level error code (`err_t`). Useful for logging when an operation
 * returns MONGO_TRANSPORT_ERR_LWIP or MONGO_TRANSPORT_ERR_CONNECT. */
int mongo_transport_last_err(const mongo_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* MICRO_MONGODB_TRANSPORT_H */
