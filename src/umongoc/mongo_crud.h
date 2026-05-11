/*
 * mongo_crud — convenience wrappers around mongo_run_command for the
 * standard CRUD operations. Each function builds the appropriate command
 * BSON (insert / update / delete), runs it as OP_MSG, and returns the
 * server's reply for the caller to inspect (n, writeErrors, etc).
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

/* Insert a single document. Reply contains { ok, n, ... } on success or
 * { writeErrors: [...] } on failure. `_id` is server-side generated if the
 * caller hasn't set it on `doc`. */
int mongo_insert_one(mongo_transport_t *t, const char *db, const char *coll, const bson_t *doc, bson_t *reply,
                     uint32_t timeout_ms);

/* Update the first document matching `filter` with `update`. `update` is a
 * MongoDB update document, e.g. `{"$set": {...}}` or `{"$inc": {...}}`. If
 * `upsert` is true, insert a new document when nothing matches. */
int mongo_update_one(mongo_transport_t *t, const char *db, const char *coll, const bson_t *filter, const bson_t *update,
                     bool upsert, bson_t *reply, uint32_t timeout_ms);

/* Delete the first document matching `filter`. */
int mongo_delete_one(mongo_transport_t *t, const char *db, const char *coll, const bson_t *filter, bson_t *reply,
                     uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
