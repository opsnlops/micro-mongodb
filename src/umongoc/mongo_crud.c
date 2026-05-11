#include "mongo_crud.h"

#include <string.h>

#include "logging.h"

/* Helper: build the command BSON. The pattern is the same for the three
 * write commands -- a top-level field naming the collection, then an array
 * of operation documents. The driver does not set ordered / writeConcern /
 * comment; the caller can pass {writeConcern: {...}} via a custom command if
 * needed (use mongo_run_command directly). */
static int build_and_run(mongo_transport_t *t, const char *db, bson_t *cmd, bson_t *reply, uint32_t timeout_ms) {
    int rc = mongo_run_command(t, db, cmd, reply, timeout_ms);
    bson_destroy(cmd);
    return rc;
}

int mongo_insert_one(mongo_transport_t *t, const char *db, const char *coll, const bson_t *doc, bson_t *reply,
                     uint32_t timeout_ms) {
    if (!t || !db || !coll || !doc || !reply) {
        return MONGO_WIRE_ERR_ARGS;
    }

    bson_t cmd;
    bson_init(&cmd);
    if (!bson_append_utf8(&cmd, "insert", 6, coll, -1)) {
        bson_destroy(&cmd);
        return MONGO_WIRE_ERR_BSON;
    }
    bson_t arr;
    if (!bson_append_array_begin(&cmd, "documents", 9, &arr)) {
        bson_destroy(&cmd);
        return MONGO_WIRE_ERR_BSON;
    }
    if (!bson_append_document(&arr, "0", 1, doc)) {
        bson_append_array_end(&cmd, &arr);
        bson_destroy(&cmd);
        return MONGO_WIRE_ERR_BSON;
    }
    if (!bson_append_array_end(&cmd, &arr)) {
        bson_destroy(&cmd);
        return MONGO_WIRE_ERR_BSON;
    }

    return build_and_run(t, db, &cmd, reply, timeout_ms);
}

int mongo_update_one(mongo_transport_t *t, const char *db, const char *coll, const bson_t *filter, const bson_t *update,
                     bool upsert, bson_t *reply, uint32_t timeout_ms) {
    if (!t || !db || !coll || !filter || !update || !reply) {
        return MONGO_WIRE_ERR_ARGS;
    }

    bson_t cmd;
    bson_init(&cmd);
    bson_append_utf8(&cmd, "update", 6, coll, -1);

    bson_t arr;
    bson_append_array_begin(&cmd, "updates", 7, &arr);

    bson_t op;
    bson_append_document_begin(&arr, "0", 1, &op);
    bson_append_document(&op, "q", 1, filter);
    bson_append_document(&op, "u", 1, update);
    bson_append_bool(&op, "upsert", 6, upsert);
    bson_append_bool(&op, "multi", 5, false);
    bson_append_document_end(&arr, &op);

    bson_append_array_end(&cmd, &arr);

    return build_and_run(t, db, &cmd, reply, timeout_ms);
}

int mongo_delete_one(mongo_transport_t *t, const char *db, const char *coll, const bson_t *filter, bson_t *reply,
                     uint32_t timeout_ms) {
    if (!t || !db || !coll || !filter || !reply) {
        return MONGO_WIRE_ERR_ARGS;
    }

    bson_t cmd;
    bson_init(&cmd);
    bson_append_utf8(&cmd, "delete", 6, coll, -1);

    bson_t arr;
    bson_append_array_begin(&cmd, "deletes", 7, &arr);

    bson_t op;
    bson_append_document_begin(&arr, "0", 1, &op);
    bson_append_document(&op, "q", 1, filter);
    /* limit=1 deletes only the first match (vs limit=0 for all matches). */
    bson_append_int32(&op, "limit", 5, 1);
    bson_append_document_end(&arr, &op);

    bson_append_array_end(&cmd, &arr);

    return build_and_run(t, db, &cmd, reply, timeout_ms);
}
