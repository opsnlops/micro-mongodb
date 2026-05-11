#include "mongo_cursor.h"

#include <string.h>

#include "FreeRTOS.h"

#include "logging.h"

#define MAX_NS_LEN 128 /* db.coll string -- generous */

struct mongo_cursor {
    mongo_transport_t *t;
    char db[64];
    char coll[64];
    int32_t batch_size;
    int64_t cursor_id;     /* 0 means server has nothing more */
    bson_t batch_reply;    /* current run_command / getMore reply */
    bool batch_reply_init; /* whether batch_reply has been bson_init'd */
    bson_iter_t batch_iter;
    bson_iter_t doc_iter;  /* iterator into current batch array */
    bson_t current_doc;    /* init_static into the latest array element */
    bool current_doc_init; /* whether current_doc is valid this turn */
    size_t total_seen;
};

/* Walk the reply for the standard "cursor.{id, firstBatch|nextBatch}" shape.
 * On success, leaves cursor->doc_iter ready to be bson_iter_next()'d into
 * each array element. */
static int load_batch_from_reply(mongo_cursor_t *c, const char *batch_field) {
    bson_iter_t it;
    if (!bson_iter_init_find(&it, &c->batch_reply, "cursor")) {
        error("[cursor] reply has no 'cursor' field");
        return MONGO_WIRE_ERR_PROTOCOL;
    }
    if (!BSON_ITER_HOLDS_DOCUMENT(&it)) {
        error("[cursor] 'cursor' is not a document");
        return MONGO_WIRE_ERR_PROTOCOL;
    }

    bson_iter_t cursor_doc_iter;
    if (!bson_iter_recurse(&it, &cursor_doc_iter)) {
        return MONGO_WIRE_ERR_PROTOCOL;
    }

    bson_iter_t batch_field_iter;
    bson_iter_t id_iter;
    bool have_batch = false;
    bool have_id = false;

    /* Take a copy of the recursion start so we can scan twice (id + batch). */
    while (bson_iter_next(&cursor_doc_iter)) {
        const char *key = bson_iter_key(&cursor_doc_iter);
        if (strcmp(key, batch_field) == 0) {
            batch_field_iter = cursor_doc_iter;
            have_batch = true;
        } else if (strcmp(key, "id") == 0) {
            id_iter = cursor_doc_iter;
            have_id = true;
        }
    }

    if (!have_batch) {
        error("[cursor] no '%s' in cursor reply", batch_field);
        return MONGO_WIRE_ERR_PROTOCOL;
    }
    if (!BSON_ITER_HOLDS_ARRAY(&batch_field_iter)) {
        error("[cursor] '%s' is not an array", batch_field);
        return MONGO_WIRE_ERR_PROTOCOL;
    }

    if (have_id && BSON_ITER_HOLDS_INT64(&id_iter)) {
        c->cursor_id = bson_iter_int64(&id_iter);
    } else if (have_id && BSON_ITER_HOLDS_INT32(&id_iter)) {
        c->cursor_id = (int64_t)bson_iter_int32(&id_iter);
    } else {
        /* Per the spec, if absent, treat as exhausted. */
        c->cursor_id = 0;
    }

    if (!bson_iter_recurse(&batch_field_iter, &c->doc_iter)) {
        return MONGO_WIRE_ERR_PROTOCOL;
    }
    return MONGO_WIRE_OK;
}

static void clear_batch_reply(mongo_cursor_t *c) {
    if (c->batch_reply_init) {
        bson_destroy(&c->batch_reply);
        c->batch_reply_init = false;
    }
}

int mongo_find(mongo_transport_t *t, const char *db, const char *coll, const bson_t *filter, int32_t batch_size,
               mongo_cursor_t **out_cursor, uint32_t timeout_ms) {
    if (!t || !db || !coll || !out_cursor) {
        return MONGO_WIRE_ERR_ARGS;
    }
    if (strlen(db) >= sizeof((mongo_cursor_t *)0)->db || strlen(coll) >= sizeof((mongo_cursor_t *)0)->coll) {
        return MONGO_WIRE_ERR_ARGS;
    }

    mongo_cursor_t *c = pvPortMalloc(sizeof *c);
    if (!c) {
        return MONGO_WIRE_ERR_ALLOC;
    }
    memset(c, 0, sizeof *c);
    c->t = t;
    strncpy(c->db, db, sizeof c->db - 1);
    strncpy(c->coll, coll, sizeof c->coll - 1);
    c->batch_size = batch_size;

    bson_t cmd;
    bson_init(&cmd);
    bson_append_utf8(&cmd, "find", 4, coll, -1);
    if (filter) {
        bson_append_document(&cmd, "filter", 6, filter);
    }
    if (batch_size > 0) {
        bson_append_int32(&cmd, "batchSize", 9, batch_size);
    }

    int rc = mongo_run_command(t, db, &cmd, &c->batch_reply, timeout_ms);
    bson_destroy(&cmd);
    if (rc != MONGO_WIRE_OK) {
        vPortFree(c);
        return rc;
    }
    c->batch_reply_init = true;

    rc = load_batch_from_reply(c, "firstBatch");
    if (rc != MONGO_WIRE_OK) {
        clear_batch_reply(c);
        vPortFree(c);
        return rc;
    }

    *out_cursor = c;
    return MONGO_WIRE_OK;
}

static int send_get_more(mongo_cursor_t *c, uint32_t timeout_ms) {
    bson_t cmd;
    bson_init(&cmd);
    bson_append_int64(&cmd, "getMore", 7, c->cursor_id);
    bson_append_utf8(&cmd, "collection", 10, c->coll, -1);
    if (c->batch_size > 0) {
        bson_append_int32(&cmd, "batchSize", 9, c->batch_size);
    }

    clear_batch_reply(c);

    int rc = mongo_run_command(c->t, c->db, &cmd, &c->batch_reply, timeout_ms);
    bson_destroy(&cmd);
    if (rc != MONGO_WIRE_OK) {
        return rc;
    }
    c->batch_reply_init = true;

    return load_batch_from_reply(c, "nextBatch");
}

int mongo_cursor_next(mongo_cursor_t *c, const bson_t **out_doc, uint32_t timeout_ms) {
    if (!c || !out_doc) {
        return MONGO_WIRE_ERR_ARGS;
    }

    for (;;) {
        if (bson_iter_next(&c->doc_iter)) {
            if (!BSON_ITER_HOLDS_DOCUMENT(&c->doc_iter)) {
                error("[cursor] batch element is not a document");
                return MONGO_WIRE_ERR_PROTOCOL;
            }
            uint32_t doc_len = 0;
            const uint8_t *doc_data = NULL;
            bson_iter_document(&c->doc_iter, &doc_len, &doc_data);
            if (!bson_init_static(&c->current_doc, doc_data, doc_len)) {
                return MONGO_WIRE_ERR_PROTOCOL;
            }
            /* Each batch element is server-controlled; validate its structure
             * before handing the borrowed bson_t off to the caller. */
            size_t bad_off = 0;
            if (!bson_validate(&c->current_doc, BSON_VALIDATE_NONE, &bad_off)) {
                error("[cursor] malformed BSON in batch element at offset %u", (unsigned)bad_off);
                return MONGO_WIRE_ERR_PROTOCOL;
            }
            c->current_doc_init = true;
            c->total_seen++;
            *out_doc = &c->current_doc;
            return 1;
        }
        /* batch exhausted */
        if (c->cursor_id == 0) {
            *out_doc = NULL;
            return 0;
        }
        int rc = send_get_more(c, timeout_ms);
        if (rc != MONGO_WIRE_OK) {
            return rc;
        }
        /* loop and try again with the new batch */
    }
}

size_t mongo_cursor_total_seen(const mongo_cursor_t *c) { return c ? c->total_seen : 0; }

void mongo_cursor_destroy(mongo_cursor_t *c) {
    if (!c) {
        return;
    }
    if (c->cursor_id != 0 && c->t) {
        /* Best-effort killCursors so the server can free its state. We don't
         * care about errors here -- the connection might be torn down already. */
        bson_t cmd;
        bson_init(&cmd);
        bson_append_utf8(&cmd, "killCursors", 11, c->coll, -1);
        bson_t arr;
        bson_append_array_begin(&cmd, "cursors", 7, &arr);
        bson_append_int64(&arr, "0", 1, c->cursor_id);
        bson_append_array_end(&cmd, &arr);

        bson_t reply;
        if (mongo_run_command(c->t, c->db, &cmd, &reply, 2000) == MONGO_WIRE_OK) {
            bson_destroy(&reply);
        }
        bson_destroy(&cmd);
    }
    clear_batch_reply(c);
    vPortFree(c);
}
