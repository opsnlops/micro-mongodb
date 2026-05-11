# API reference

The end-user API is `mongo_client_t` plus a handful of helpers. Include
`micro_mongodb.h` (the umbrella header) and link against `umongoc`.

```c
#include "micro_mongodb.h"
```

## Lifecycle

### `mongo_client_t *mongo_client_new(const mongo_client_config_t *cfg)`

Construct a client. Parses the URI eagerly so a malformed URI errors out
here rather than at first use. Does **not** open a network connection;
that happens lazily on the first CRUD call or via an explicit
`mongo_client_connect()`.

Returns `NULL` on parse / allocation failure.

```c
mongo_client_config_t cfg = {
    .mongo_uri          = "mongodb+srv://user:pass@cluster.mongodb.net/",
    .app_name           = "telemetry",      // optional, sent in hello.client.application.name
    .default_timeout_ms = 5000,             // optional, 5000 ms if 0
};
mongo_client_t *c = mongo_client_new(&cfg);
```

### `void mongo_client_free(mongo_client_t *c)`

Tear down the client and free associated resources. Safe on `NULL`. Closes
the underlying transport if one is open.

### `int mongo_client_connect(mongo_client_t *c, uint32_t timeout_ms)`

Explicitly open the connection (TLS handshake + hello + primary discovery +
SCRAM). Optional — CRUD calls auto-connect. Use this if you want to pay
the handshake cost up-front rather than on the first operation.

Returns 0 on success, negative `MONGO_WIRE_ERR_*` or `MONGO_AUTH_ERR_*`
on failure.

### `bool mongo_client_is_connected(const mongo_client_t *c)`

True once a connection has been established and has not since errored.
Informational — the client manages reconnection internally.

## CRUD

All CRUD functions:

- Lazy-connect on first call.
- Acquire the client's internal mutex (so concurrent callers serialize).
- Invalidate the connection on transport errors so the next call reconnects.
- `timeout_ms = 0` means "use the client's default timeout".

### `int mongo_client_insert(c, db, coll, doc, reply, timeout_ms)`

Insert a single document. The reply contains `{ok, n, ...}` on success or
`{writeErrors: [...]}` on failure. If the caller didn't set `_id` on the
document, the server generates one.

### `int mongo_client_find(c, db, coll, filter, batch_size, out_cursor, timeout_ms)`

Run a find and return a cursor over the matching documents. Iterate with
`mongo_cursor_next`.

```c
mongo_cursor_t *cur = NULL;
mongo_client_find(c, "db", "coll", &filter, 10, &cur, 5000);

const bson_t *doc;
while (mongo_cursor_next(cur, &doc, 5000) == 1) {
    // `doc` is borrowed; invalidated by the next next() or destroy() call
    bson_iter_t it;
    if (bson_iter_init_find(&it, doc, "field")) { ... }
}
mongo_cursor_destroy(cur);
```

`batch_size <= 0` lets the server pick.

### `int mongo_client_update(c, db, coll, filter, update, upsert, reply, timeout_ms)`

Update the first document matching `filter` with `update`. `update` is a
MongoDB update document, e.g. `{"$set": {...}}` or `{"$inc": {...}}`.

### `int mongo_client_delete(c, db, coll, filter, reply, timeout_ms)`

Delete the first document matching `filter`.

### `int mongo_client_run_command(c, db, cmd, reply, timeout_ms)`

Escape hatch for arbitrary commands: `ping`, `aggregate`, `createIndexes`,
`distinct`, etc. See the [aggregator example](examples.md#aggregator-task)
for how to build a pipeline.

## Time-series helper

### `int mongo_client_ensure_timeseries(c, db, coll, opts, timeout_ms)`

Idempotently create a time-series collection. If it already exists,
returns success without modifying it.

```c
mongo_client_ensure_timeseries(client, "db", "telemetry",
    &(mongo_timeseries_opts_t){
        .time_field  = "ts",
        .meta_field  = "board",
        .granularity = "seconds",
    }, 5000);
```

Options:

| Field | Required | Notes |
|---|---|---|
| `time_field` | yes | Name of the BSON Date field, e.g. `"ts"` |
| `meta_field` | no | Identifies the data source, e.g. `"board"` or `"sensor_id"` |
| `granularity` | no | `"seconds"` / `"minutes"` / `"hours"`. Server default if NULL. |
| `expire_after_seconds` | no | TTL in seconds. 0 = no TTL. |

The corresponding document on the wire should use `BSON_APPEND_DATE_TIME`
(BSON type 9) for the `time_field`, not an int64 — time-series collections
require a real Date.

> If the collection already exists as a *regular* (non-time-series)
> collection, the server returns `NamespaceExists` (code 48). The driver
> logs a clear warning telling you to drop and recreate. We can't convert
> in place.

## Cursor API

### `int mongo_cursor_next(cursor, out_doc, timeout_ms)`

Returns:
- `1` and populates `*out_doc` with a borrowed view of the next document.
- `0` when the cursor is exhausted.
- A negative `MONGO_WIRE_ERR_*` on transport / protocol failure.

`*out_doc` is invalidated by the next call to `mongo_cursor_next()` or
`mongo_cursor_destroy()`. If you need the data longer, deep-copy with
`bson_copy_to()`.

### `size_t mongo_cursor_total_seen(const cursor)`

Total documents the cursor has produced so far.

### `void mongo_cursor_destroy(cursor)`

`killCursors` if the server cursor is still alive, then free local state.
Safe on `NULL`.

## Wall-clock time

### `int mongo_time_sync(uint32_t timeout_ms)`

Kick off SNTP against the configured server (`-DNTP_SERVER=...`, default
`pool.ntp.org`) and block until the first fix lands. Must be called after
WiFi is up. Idempotent: if already synced, returns immediately.

### `int64_t mongo_time_now_ms(void)`

Wall-clock milliseconds since the Unix epoch. Returns 0 if time has not
been synced (also a valid pre-1970 timestamp; check `mongo_time_is_synced()`
if you need to disambiguate).

### `bool mongo_time_is_synced(void)`

### `size_t mongo_time_format_iso8601(char *out, size_t sz)`

Format the current wall clock as ISO 8601, e.g. `"2026-05-11T20:32:45Z"`.
`sz` should be at least 21 bytes.

## WiFi helper

### `int mongo_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms)`

Wraps `cyw43_arch_init` + `cyw43_arch_enable_sta_mode` +
`cyw43_arch_wifi_connect_timeout_ms`. Returns 0 on success, non-zero on
error.

## Threading model

A single `mongo_client_t` is safe to share across FreeRTOS tasks. Network
operations are serialized internally by a priority-inheriting mutex; a
slow operation in one task blocks others until it returns.

A task that triggers reconnect holds the mutex across the full TLS+SCRAM
cycle — that's typically 1-4 seconds depending on board. Plan task
priorities accordingly.

There's no support for parallel operations on a single client (no pipelining,
no overlapping requests). If you need that, create multiple clients —
each gets its own TLS connection.

## Error codes

CRUD functions return negative codes from this set:

```c
typedef enum {
    MONGO_WIRE_OK            =  0,
    MONGO_WIRE_ERR_ARGS      = -1,  // NULL pointer, missing arg, etc.
    MONGO_WIRE_ERR_ALLOC     = -2,
    MONGO_WIRE_ERR_TRANSPORT = -3,  // TCP/TLS error -- client will reconnect
    MONGO_WIRE_ERR_PROTOCOL  = -4,  // server reply malformed or rejected
    MONGO_WIRE_ERR_TOO_BIG   = -5,
    MONGO_WIRE_ERR_NO_BODY   = -6,
    MONGO_WIRE_ERR_BSON      = -7,
} mongo_wire_status_t;
```

For SCRAM-specific failures, `mongo_auth_status_str()` maps a code to a
human-readable description:

```c
int rc = mongo_client_connect(c, 5000);
if (rc != 0) {
    error("connect: %s (%d)", mongo_auth_status_str(rc), rc);
}
```
