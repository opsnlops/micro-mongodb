/*
 * mongo_client — high-level client API for micro-mongodb.
 *
 * Wraps the transport / hello / SCRAM / primary-discovery dance into a single
 * object so end-user code looks like:
 *
 *     mongo_client_config_t cfg = {.mongo_uri = "mongodb+srv://...", .app_name = "telemetry"};
 *     mongo_client_t *c = mongo_client_new(&cfg);
 *     mongo_client_insert(c, "db", "coll", &doc, &reply, 5000);
 *
 * Lazy-connect: the first CRUD call brings the connection up. On transport
 * errors the client invalidates itself; the next call reconnects automatically.
 *
 * Threading: a single mongo_client_t is safe to share across multiple
 * FreeRTOS tasks. Network operations are serialized internally by a
 * priority-inheriting mutex; a slow operation in one task blocks others
 * until it returns. Reconnection is also serialized -- a task that
 * triggers reconnect holds the mutex across the full TLS+SCRAM cycle.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <bson/bson.h>

#include "mongo_cursor.h"
#include "mongo_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mongo_client mongo_client_t;

typedef struct {
    /* mongodb:// or mongodb+srv:// URI. Copied internally; the buffer the
     * caller passes does not need to outlive the client. */
    const char *mongo_uri;

    /* Optional. Reported in the hello's client.application.name field. */
    const char *app_name;

    /* Optional. Override the per-op default timeout (5000 ms if 0). */
    uint32_t default_timeout_ms;
} mongo_client_config_t;

/* Construct a client. Parses the URI eagerly (so a malformed URI errors out
 * here rather than at first use). Does NOT open a network connection; that
 * happens on the first CRUD call or on an explicit mongo_client_connect().
 * Returns NULL on parse / allocation failure. */
mongo_client_t *mongo_client_new(const mongo_client_config_t *cfg);

/* Tear down the client and free associated resources. Safe on NULL. */
void mongo_client_free(mongo_client_t *c);

/* Explicitly connect (TLS handshake + hello + primary discovery + SCRAM).
 * Optional -- CRUD calls auto-connect. Use this when you want to pay the
 * handshake cost up-front rather than on the first operation. Returns 0 on
 * success, negative MONGO_WIRE_ERR_* / MONGO_AUTH_ERR_* on failure. */
int mongo_client_connect(mongo_client_t *c, uint32_t timeout_ms);

/* True once a connection has been established (and has not since errored). */
bool mongo_client_is_connected(const mongo_client_t *c);

/* CRUD wrappers. Same semantics as the lower-level mongo_*_one / mongo_find
 * functions, but they auto-connect on demand and mark the client for
 * reconnect on transport errors. */
int mongo_client_insert(mongo_client_t *c, const char *db, const char *coll, const bson_t *doc, bson_t *reply,
                        uint32_t timeout_ms);
int mongo_client_find(mongo_client_t *c, const char *db, const char *coll, const bson_t *filter, int32_t batch_size,
                      mongo_cursor_t **out_cursor, uint32_t timeout_ms);
int mongo_client_update(mongo_client_t *c, const char *db, const char *coll, const bson_t *filter, const bson_t *update,
                        bool upsert, bson_t *reply, uint32_t timeout_ms);
int mongo_client_delete(mongo_client_t *c, const char *db, const char *coll, const bson_t *filter, bson_t *reply,
                        uint32_t timeout_ms);

/* Escape hatch for any command we don't have a wrapper for (ping, aggregate,
 * createIndexes, etc.). */
int mongo_client_run_command(mongo_client_t *c, const char *db, const bson_t *cmd, bson_t *reply, uint32_t timeout_ms);

/* Options for mongo_client_ensure_timeseries(). */
typedef struct {
    /* Required. Name of the field that holds the BSON Date timestamp. */
    const char *time_field;

    /* Optional. Field that identifies the data source (e.g. "board" or
     * "sensor_id"). Time-series collections cluster docs by this. */
    const char *meta_field;

    /* Optional. One of "seconds" / "minutes" / "hours". Defaults to
     * server's default ("seconds") if NULL. */
    const char *granularity;

    /* Optional. If non-zero, documents auto-delete after this many seconds.
     * Useful for capped telemetry. */
    int64_t expire_after_seconds;
} mongo_timeseries_opts_t;

/* Idempotently create a time-series collection on the server. If the
 * collection already exists, returns success without modifying it. If it
 * exists as a regular (non-time-series) collection, the server still
 * reports NamespaceExists; in that case you'll need to drop the existing
 * collection in Atlas first.
 *
 * Suggested call at boot:
 *
 *     mongo_client_ensure_timeseries(c, "db", "telemetry",
 *         &(mongo_timeseries_opts_t){
 *             .time_field = "ts", .meta_field = "board",
 *             .granularity = "seconds",
 *         }, 5000);
 */
int mongo_client_ensure_timeseries(mongo_client_t *c, const char *db, const char *coll,
                                   const mongo_timeseries_opts_t *opts, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
