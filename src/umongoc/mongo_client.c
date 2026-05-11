#include "mongo_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "logging.h"
#include "mongo_auth.h"
#include "mongo_crud.h"
#include "mongo_cursor.h"
#include "mongo_transport.h"
#include "mongo_uri.h"

#define MAX_REDIRECTS 5
#define DEFAULT_TIMEOUT_MS 5000

#if defined(PICO_PLATFORM_IS_RP2350) && PICO_PLATFORM_IS_RP2350
#define CLIENT_BOARD_NAME "pico2_w"
#else
#define CLIENT_BOARD_NAME "pico_w"
#endif

struct mongo_client {
    /* config (copied so caller doesn't need to keep their buffers alive) */
    char mongo_uri_str[512];
    char app_name[64];
    uint32_t default_timeout_ms;

    /* parsed URI -- populated in mongo_client_new */
    mongo_uri_t uri;

    /* current connection target. Starts at uri.hosts[0]; updated when we
     * follow a primary redirect from hello. */
    char target_host[MONGO_URI_HOST_MAX];
    uint16_t target_port;

    mongo_transport_t *t;
    bool connected;
};

mongo_client_t *mongo_client_new(const mongo_client_config_t *cfg) {
    if (!cfg || !cfg->mongo_uri) {
        return NULL;
    }

    mongo_client_t *c = pvPortMalloc(sizeof *c);
    if (!c) {
        return NULL;
    }
    memset(c, 0, sizeof *c);

    strncpy(c->mongo_uri_str, cfg->mongo_uri, sizeof c->mongo_uri_str - 1);
    if (cfg->app_name) {
        strncpy(c->app_name, cfg->app_name, sizeof c->app_name - 1);
    } else {
        strncpy(c->app_name, "micro-mongodb", sizeof c->app_name - 1);
    }
    c->default_timeout_ms = cfg->default_timeout_ms ? cfg->default_timeout_ms : DEFAULT_TIMEOUT_MS;

    int rc = mongo_uri_parse(c->mongo_uri_str, &c->uri, c->default_timeout_ms);
    if (rc != MONGO_URI_OK) {
        error("[client] uri parse failed: rc=%d for '%s'", rc, c->mongo_uri_str);
        vPortFree(c);
        return NULL;
    }
    info("[client] parsed URI: %zu host(s), tls=%d, srv=%d, replicaSet='%s', user='%s', authSource='%s'",
         c->uri.n_hosts, (int)c->uri.tls, (int)c->uri.is_srv, c->uri.replica_set, c->uri.username, c->uri.auth_source);

    /* Initial target: first host the SRV record returned. */
    strncpy(c->target_host, c->uri.hosts[0].host, sizeof c->target_host - 1);
    c->target_port = c->uri.hosts[0].port;

    return c;
}

void mongo_client_free(mongo_client_t *c) {
    if (!c) {
        return;
    }
    if (c->t) {
        mongo_transport_free(c->t);
    }
    vPortFree(c);
}

bool mongo_client_is_connected(const mongo_client_t *c) { return c && c->connected; }

/* Inspect a hello reply and decide whether we landed on the writable primary.
 * If not, the `primary` field tells us where the real primary is; we update
 * c->target_host/port for the next attempt. Returns true if we're on the
 * primary already (proceed to SCRAM), false if a redirect is needed. */
static bool hello_is_primary_else_redirect(mongo_client_t *c, const bson_t *hello_reply) {
    bson_iter_t it;
    bool is_primary = false;
    if (bson_iter_init_find(&it, hello_reply, "isWritablePrimary") && BSON_ITER_HOLDS_BOOL(&it)) {
        is_primary = bson_iter_bool(&it);
    } else if (bson_iter_init_find(&it, hello_reply, "ismaster") && BSON_ITER_HOLDS_BOOL(&it)) {
        is_primary = bson_iter_bool(&it);
    }
    if (is_primary) {
        return true;
    }

    const char *primary_str = NULL;
    if (bson_iter_init_find(&it, hello_reply, "primary") && BSON_ITER_HOLDS_UTF8(&it)) {
        primary_str = bson_iter_utf8(&it, NULL);
    }
    if (!primary_str || !primary_str[0]) {
        warning("[client] on secondary but no primary known");
        return false;
    }

    const char *colon = strchr(primary_str, ':');
    size_t host_len = colon ? (size_t)(colon - primary_str) : strlen(primary_str);
    if (host_len == 0 || host_len >= sizeof c->target_host) {
        warning("[client] malformed primary hint: %s", primary_str);
        return false;
    }
    memcpy(c->target_host, primary_str, host_len);
    c->target_host[host_len] = '\0';
    c->target_port = colon ? (uint16_t)strtoul(colon + 1, NULL, 10) : 27017;
    info("[client] redirecting to primary %s:%u", c->target_host, c->target_port);
    return false;
}

/* Tear down the existing transport (if any) and create a fresh one configured
 * for the current target_host -- SNI must match for Atlas's load balancer. */
static int rebuild_transport(mongo_client_t *c) {
    if (c->t) {
        mongo_transport_free(c->t);
        c->t = NULL;
    }
    if (c->uri.tls) {
        mongo_tls_config_t tls = {0};
        tls.sni_hostname = c->target_host;
        c->t = mongo_transport_new(&tls);
    } else {
        c->t = mongo_transport_new(NULL);
    }
    return c->t ? 0 : MONGO_WIRE_ERR_ALLOC;
}

int mongo_client_connect(mongo_client_t *c, uint32_t timeout_ms) {
    if (!c) {
        return MONGO_WIRE_ERR_ARGS;
    }
    if (c->connected) {
        return 0;
    }

    /* Reset target to the first SRV host on every fresh connect attempt --
     * the previous primary may have stepped down. */
    strncpy(c->target_host, c->uri.hosts[0].host, sizeof c->target_host - 1);
    c->target_host[sizeof c->target_host - 1] = '\0';
    c->target_port = c->uri.hosts[0].port;

    int last_rc = MONGO_WIRE_ERR_TRANSPORT;
    for (int attempt = 0; attempt < MAX_REDIRECTS; attempt++) {
        int rc = rebuild_transport(c);
        if (rc != 0) {
            return rc;
        }

        info("[client] connecting to %s:%u (%s)", c->target_host, c->target_port, c->uri.tls ? "TLS" : "plain");
        rc = mongo_transport_connect(c->t, c->target_host, c->target_port, timeout_ms);
        if (rc != MONGO_TRANSPORT_OK) {
            error("[client] connect failed: rc=%d", rc);
            last_rc = rc;
            continue;
        }

        /* hello with saslSupportedMechs query so the server tells us which
         * SCRAM mechanism is enabled for our user. */
        char sasl_user_db[160] = {0};
        const char *sasl_user_db_arg = NULL;
        if (c->uri.username[0]) {
            snprintf(sasl_user_db, sizeof sasl_user_db, "%s.%s", c->uri.auth_source, c->uri.username);
            sasl_user_db_arg = sasl_user_db;
        }
        bson_t hello_reply;
        rc = mongo_handshake(c->t, c->app_name, CLIENT_BOARD_NAME, sasl_user_db_arg, &hello_reply, timeout_ms);
        if (rc != MONGO_AUTH_OK) {
            mongo_transport_close(c->t);
            last_rc = rc;
            continue;
        }

        /* If we landed on a secondary, redirect to the primary and try again
         * with a fresh transport (SNI changes). */
        if (!hello_is_primary_else_redirect(c, &hello_reply)) {
            bson_destroy(&hello_reply);
            mongo_transport_close(c->t);
            continue;
        }

        /* SCRAM if we have credentials. Auth failure is fatal -- no point
         * retrying with the same password. */
        if (c->uri.username[0] && c->uri.password[0]) {
            if (!c->uri.tls) {
                warning(
                    "[client] SCRAM over plain TCP -- saslSupportedMechs unauthenticated, proofs offline-attackable");
            }
            rc = mongo_authenticate(c->t, &hello_reply, c->uri.auth_source, c->uri.username, c->uri.password,
                                    timeout_ms);
            if (rc != MONGO_AUTH_OK) {
                error("[client] auth failed: %s (rc=%d)", mongo_auth_status_str(rc), rc);
                bson_destroy(&hello_reply);
                mongo_transport_close(c->t);
                return rc;
            }
        }

        bson_destroy(&hello_reply);
        c->connected = true;
        info("[client] ready (host=%s:%u)", c->target_host, c->target_port);
        return 0;
    }

    error("[client] exhausted %d connect attempts; last rc=%d", MAX_REDIRECTS, last_rc);
    return last_rc;
}

/* Invalidate the client connection on any error that means the transport is
 * no longer usable. Next CRUD call will trigger reconnect. */
static void mark_disconnected(mongo_client_t *c) {
    c->connected = false;
    if (c->t) {
        mongo_transport_close(c->t);
    }
}

/* Used in CRUD wrappers: ensure we're connected, run `op`, on transport
 * failure invalidate so the next call rebuilds. */
#define CLIENT_DO(c, timeout_ms, op_call)                                                                              \
    do {                                                                                                               \
        int _ec = (c)->connected ? 0 : mongo_client_connect((c), (timeout_ms));                                        \
        if (_ec != 0) {                                                                                                \
            return _ec;                                                                                                \
        }                                                                                                              \
        int _rc = (op_call);                                                                                           \
        if (_rc == MONGO_WIRE_ERR_TRANSPORT || _rc == MONGO_WIRE_ERR_ALLOC) {                                          \
            mark_disconnected(c);                                                                                      \
        }                                                                                                              \
        return _rc;                                                                                                    \
    } while (0)

int mongo_client_insert(mongo_client_t *c, const char *db, const char *coll, const bson_t *doc, bson_t *reply,
                        uint32_t timeout_ms) {
    if (!c) {
        return MONGO_WIRE_ERR_ARGS;
    }
    if (timeout_ms == 0) {
        timeout_ms = c->default_timeout_ms;
    }
    CLIENT_DO(c, timeout_ms, mongo_insert_one(c->t, db, coll, doc, reply, timeout_ms));
}

int mongo_client_find(mongo_client_t *c, const char *db, const char *coll, const bson_t *filter, int32_t batch_size,
                      mongo_cursor_t **out_cursor, uint32_t timeout_ms) {
    if (!c) {
        return MONGO_WIRE_ERR_ARGS;
    }
    if (timeout_ms == 0) {
        timeout_ms = c->default_timeout_ms;
    }
    CLIENT_DO(c, timeout_ms, mongo_find(c->t, db, coll, filter, batch_size, out_cursor, timeout_ms));
}

int mongo_client_update(mongo_client_t *c, const char *db, const char *coll, const bson_t *filter, const bson_t *update,
                        bool upsert, bson_t *reply, uint32_t timeout_ms) {
    if (!c) {
        return MONGO_WIRE_ERR_ARGS;
    }
    if (timeout_ms == 0) {
        timeout_ms = c->default_timeout_ms;
    }
    CLIENT_DO(c, timeout_ms, mongo_update_one(c->t, db, coll, filter, update, upsert, reply, timeout_ms));
}

int mongo_client_delete(mongo_client_t *c, const char *db, const char *coll, const bson_t *filter, bson_t *reply,
                        uint32_t timeout_ms) {
    if (!c) {
        return MONGO_WIRE_ERR_ARGS;
    }
    if (timeout_ms == 0) {
        timeout_ms = c->default_timeout_ms;
    }
    CLIENT_DO(c, timeout_ms, mongo_delete_one(c->t, db, coll, filter, reply, timeout_ms));
}

int mongo_client_run_command(mongo_client_t *c, const char *db, const bson_t *cmd, bson_t *reply, uint32_t timeout_ms) {
    if (!c) {
        return MONGO_WIRE_ERR_ARGS;
    }
    if (timeout_ms == 0) {
        timeout_ms = c->default_timeout_ms;
    }
    CLIENT_DO(c, timeout_ms, mongo_run_command(c->t, db, cmd, reply, timeout_ms));
}

int mongo_client_ensure_timeseries(mongo_client_t *c, const char *db, const char *coll,
                                   const mongo_timeseries_opts_t *opts, uint32_t timeout_ms) {
    if (!c || !db || !coll || !opts || !opts->time_field) {
        return MONGO_WIRE_ERR_ARGS;
    }
    if (timeout_ms == 0) {
        timeout_ms = c->default_timeout_ms;
    }

    bson_t cmd;
    bson_init(&cmd);
    bson_append_utf8(&cmd, "create", 6, coll, -1);

    bson_t ts;
    bson_append_document_begin(&cmd, "timeseries", 10, &ts);
    bson_append_utf8(&ts, "timeField", 9, opts->time_field, -1);
    if (opts->meta_field && opts->meta_field[0]) {
        bson_append_utf8(&ts, "metaField", 9, opts->meta_field, -1);
    }
    if (opts->granularity && opts->granularity[0]) {
        bson_append_utf8(&ts, "granularity", 11, opts->granularity, -1);
    }
    bson_append_document_end(&cmd, &ts);

    if (opts->expire_after_seconds > 0) {
        bson_append_int64(&cmd, "expireAfterSeconds", 18, opts->expire_after_seconds);
    }

    bson_t reply;
    int rc = mongo_client_run_command(c, db, &cmd, &reply, timeout_ms);
    bson_destroy(&cmd);
    if (rc != MONGO_WIRE_OK) {
        return rc;
    }

    if (mongo_reply_ok(&reply) >= 1.0) {
        info("[client] created time-series collection %s.%s (timeField=%s, metaField=%s)", db, coll, opts->time_field,
             opts->meta_field ? opts->meta_field : "(none)");
        bson_destroy(&reply);
        return 0;
    }

    /* Server rejected. If it's NamespaceExists (48) we treat that as success
     * -- some other boot created it. If it's anything else, surface the
     * server's errmsg so the caller can see why. */
    bson_iter_t it;
    int code = 0;
    if (bson_iter_init_find(&it, &reply, "code") && BSON_ITER_HOLDS_INT32(&it)) {
        code = bson_iter_int32(&it);
    }
    if (code == 48 /* NamespaceExists */) {
        debug("[client] time-series collection %s.%s already exists", db, coll);
        bson_destroy(&reply);
        return 0;
    }
    warning("[client] could not create time-series collection %s.%s -- if it exists as a regular collection, drop it "
            "in Atlas and reboot",
            db, coll);
    mongo_log_reply_error("create timeseries", &reply);
    bson_destroy(&reply);
    return MONGO_WIRE_ERR_PROTOCOL;
}
