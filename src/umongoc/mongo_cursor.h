/*
 * mongo_cursor — `find` plus iteration via cursor.firstBatch / cursor.id.
 *
 * Lifecycle:
 *   mongo_cursor_t *c = NULL;
 *   int rc = mongo_find(t, "db", "coll", filter, 10, &c, 5000);
 *   if (rc == MONGO_WIRE_OK) {
 *       const bson_t *doc;
 *       while (mongo_cursor_next(c, &doc, 5000) > 0) {
 *           // doc borrowed from cursor -- valid until next call
 *       }
 *       mongo_cursor_destroy(c);
 *   }
 *
 * The cursor handles the getMore loop internally: when the local batch is
 * drained and the server still has more (cursor_id != 0), it issues a
 * getMore. On destroy, if the server cursor is still open, killCursors is
 * sent so we don't leak resources on the server.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <bson/bson.h>

#include "mongo_transport.h"
#include "mongo_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mongo_cursor mongo_cursor_t;

/* Forward declaration -- defined in mongo_client.h. The cursor stores an
 * optional client pointer so multi-task callers can rely on the client's
 * internal mutex to serialize getMore / killCursors traffic against
 * concurrent CRUD calls on the same client. NULL when the cursor was
 * created via the lower-level transport-only `mongo_find()` API. */
struct mongo_client;

/* Tag the cursor with the client whose mutex it should hold while
 * doing network I/O. Called by mongo_client_find() right after the
 * underlying mongo_find() returns; not part of the public end-user API
 * for cursors obtained from mongo_find() directly. */
void mongo_cursor_set_client(mongo_cursor_t *c, struct mongo_client *client);

/* Run `find` and stash the initial batch in a fresh cursor. On success the
 * cursor is owned by the caller and must be released with
 * mongo_cursor_destroy().
 *
 * `batch_size <= 0` lets the server pick. Typical small batches (10-100)
 * keep per-roundtrip memory bounded on the Pico. */
int mongo_find(mongo_transport_t *t, const char *db, const char *coll, const bson_t *filter, int32_t batch_size,
               mongo_cursor_t **out_cursor, uint32_t timeout_ms);

/* Returns 1 and populates `*out_doc` with a borrowed view of the next
 * document. Returns 0 when the cursor is exhausted, or a negative
 * MONGO_WIRE_ERR_* on transport / protocol failure. `*out_doc` is invalidated
 * by the next call to mongo_cursor_next() or mongo_cursor_destroy(). */
int mongo_cursor_next(mongo_cursor_t *c, const bson_t **out_doc, uint32_t timeout_ms);

/* Optional: total documents the cursor has produced so far. Useful for log
 * lines or sanity checks. */
size_t mongo_cursor_total_seen(const mongo_cursor_t *c);

/* killCursors if still alive on the server, then free local state. Safe to
 * call on NULL. */
void mongo_cursor_destroy(mongo_cursor_t *c);

#ifdef __cplusplus
}
#endif
