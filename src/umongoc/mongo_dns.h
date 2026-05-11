/*
 * mongo_dns — hand-rolled DNS SRV + TXT lookups over lwIP raw UDP.
 *
 * lwIP's bundled resolver does A/AAAA only. mongodb+srv:// connection strings
 * need SRV (host list) and TXT (option string), so we send our own RFC 1035
 * packets to the nameserver lwIP learned from DHCP.
 *
 * Both functions block the caller for up to `timeout_ms`, signalling via a
 * FreeRTOS semaphore when the udp recv callback fires on the tcpip thread.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t priority;
    uint16_t weight;
    uint16_t port;
    char target[256]; /* fully-qualified target host */
} mongo_srv_record_t;

typedef enum {
    MONGO_DNS_OK = 0,
    MONGO_DNS_ERR_ARGS = -1,
    MONGO_DNS_ERR_ALLOC = -2,
    MONGO_DNS_ERR_NO_NAMESERVER = -3,
    MONGO_DNS_ERR_SEND = -4,
    MONGO_DNS_ERR_TIMEOUT = -5,
    MONGO_DNS_ERR_FORMAT = -6,
    MONGO_DNS_ERR_TRUNCATED = -7,
    MONGO_DNS_ERR_RCODE = -8,
    MONGO_DNS_ERR_NO_RECORDS = -9,
} mongo_dns_status_t;

/* Query for SRV records under `name`. Fills `out` with up to `out_cap`
 * records and stores the actual count in `*n_out`. Returns 0 on success.
 *
 * For MongoDB Atlas: name is "_mongodb._tcp.<cluster-host>". The returned
 * targets are the actual mongod hostnames the client should connect to. */
int mongo_dns_query_srv(const char *name, mongo_srv_record_t *out, size_t out_cap, size_t *n_out, uint32_t timeout_ms);

/* Query for the TXT record at `name`. Concatenates all <len><bytes> segments
 * of the first answering record into `out` (NUL-terminated). Returns 0 on
 * success. For Atlas this is the same hostname as the user-facing one (no
 * _mongodb._tcp prefix) and contains a URL-encoded option string like
 * "authSource=admin&replicaSet=atlas-xyz". */
int mongo_dns_query_txt(const char *name, char *out, size_t out_cap, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
