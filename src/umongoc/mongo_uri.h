/*
 * mongo_uri — connection-string parser for mongodb:// and mongodb+srv:// URIs.
 *
 * For mongodb+srv:// the parser resolves the SRV record under
 * `_mongodb._tcp.<host>` to expand into a host list and reads the TXT record
 * at `<host>` for additional options (replicaSet, authSource). The resulting
 * mongo_uri_t is self-contained: no malloc'd pointers, just fixed-size buffers
 * with caps chosen to fit comfortably on the FreeRTOS stack.
 *
 * Out of scope (for now): URL-decoding of percent-escaped credentials, IPv6
 * literal hosts (need bracket syntax), full options parsing. Patches welcome.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MONGO_URI_MAX_HOSTS 8
#define MONGO_URI_HOST_MAX 256
#define MONGO_URI_FIELD_MAX 64

typedef struct {
    char username[MONGO_URI_FIELD_MAX]; /* empty string if absent */
    char password[MONGO_URI_FIELD_MAX];
    struct {
        char host[MONGO_URI_HOST_MAX];
        uint16_t port;
    } hosts[MONGO_URI_MAX_HOSTS];
    size_t n_hosts;
    char database[MONGO_URI_FIELD_MAX];
    bool tls;
    char replica_set[MONGO_URI_FIELD_MAX];
    char auth_source[MONGO_URI_FIELD_MAX]; /* default "admin" for SCRAM */
    bool is_srv;                           /* true if parsed from mongodb+srv:// */
} mongo_uri_t;

typedef enum {
    MONGO_URI_OK = 0,
    MONGO_URI_ERR_ARGS = -1,
    MONGO_URI_ERR_FORMAT = -2,
    MONGO_URI_ERR_TOO_MANY_HOSTS = -3,
    MONGO_URI_ERR_DNS = -4,
} mongo_uri_status_t;

/* Parse `uri_str` into `out`. For mongodb+srv:// URIs, DNS lookups are
 * performed inline -- caller must already have networking up (DHCP completed,
 * nameserver known to lwIP). `dns_timeout_ms` bounds each lookup. */
int mongo_uri_parse(const char *uri_str, mongo_uri_t *out, uint32_t dns_timeout_ms);

#ifdef __cplusplus
}
#endif
