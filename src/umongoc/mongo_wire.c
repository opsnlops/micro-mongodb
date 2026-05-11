#include "mongo_wire.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "logging.h"

#define HDR_SIZE 16
#define FLAGS_SIZE 4
#define SECTION_BODY 0x00
#define SECTION_DOC_SEQUENCE 0x01
#define EMPTY_BSON_SIZE 5 /* {} encoded is "\x05\x00\x00\x00\x00" */

/* Per-process monotonically increasing request id. The wire protocol uses it
 * to pair replies with requests; with a single connection and serialized
 * round trips we mostly only check that responseTo matches what we sent.
 * Atomic so a future second task running mongo_run_command can't tear the
 * counter and have responseTo correlate to the wrong request. */
static uint32_t g_request_id_counter = 1;

static uint32_t next_request_id(void) { return __atomic_fetch_add(&g_request_id_counter, 1, __ATOMIC_RELAXED); }

static int32_t read_le_int32(const uint8_t *b) {
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

static void write_le_int32(uint8_t *b, int32_t v) {
    uint32_t u = (uint32_t)v;
    b[0] = (uint8_t)(u & 0xff);
    b[1] = (uint8_t)((u >> 8) & 0xff);
    b[2] = (uint8_t)((u >> 16) & 0xff);
    b[3] = (uint8_t)((u >> 24) & 0xff);
}

static int send_op_msg(mongo_transport_t *t, const bson_t *cmd, const char *db, uint32_t request_id,
                       uint32_t timeout_ms) {
    /* Copy the caller's doc and append $db. We can't mutate the caller's
     * const bson_t. */
    bson_t with_db;
    bson_init(&with_db);
    if (!bson_concat(&with_db, cmd)) {
        bson_destroy(&with_db);
        return MONGO_WIRE_ERR_BSON;
    }
    if (!bson_append_utf8(&with_db, "$db", 3, db, -1)) {
        bson_destroy(&with_db);
        return MONGO_WIRE_ERR_BSON;
    }

    uint32_t bson_size = with_db.len;
    const uint8_t *bson_data = bson_get_data(&with_db);
    uint32_t msg_length = HDR_SIZE + FLAGS_SIZE + 1 + bson_size;

    /* Preamble (21 bytes): header + flagBits + body section marker. */
    uint8_t preamble[HDR_SIZE + FLAGS_SIZE + 1];
    write_le_int32(preamble + 0, (int32_t)msg_length);
    write_le_int32(preamble + 4, (int32_t)request_id);
    write_le_int32(preamble + 8, 0); /* responseTo */
    write_le_int32(preamble + 12, MONGO_OP_MSG_OPCODE);
    write_le_int32(preamble + 16, 0); /* flagBits */
    preamble[20] = SECTION_BODY;

    int rc = mongo_transport_send(t, preamble, sizeof preamble, timeout_ms);
    if (rc != MONGO_TRANSPORT_OK) {
        bson_destroy(&with_db);
        return MONGO_WIRE_ERR_TRANSPORT;
    }
    rc = mongo_transport_send(t, bson_data, bson_size, timeout_ms);
    bson_destroy(&with_db);
    if (rc != MONGO_TRANSPORT_OK) {
        return MONGO_WIRE_ERR_TRANSPORT;
    }
    return MONGO_WIRE_OK;
}

static int recv_op_msg(mongo_transport_t *t, uint32_t expected_response_to, bson_t *reply_out, uint32_t timeout_ms) {
    uint8_t header[HDR_SIZE];
    int rc = mongo_transport_recv_exact(t, header, HDR_SIZE, timeout_ms);
    if (rc != MONGO_TRANSPORT_OK) {
        return MONGO_WIRE_ERR_TRANSPORT;
    }

    int32_t msg_length = read_le_int32(header + 0);
    int32_t response_to = read_le_int32(header + 8);
    int32_t op_code = read_le_int32(header + 12);

    if (op_code != MONGO_OP_MSG_OPCODE) {
        error("[wire] unexpected opcode %d", (int)op_code);
        return MONGO_WIRE_ERR_PROTOCOL;
    }
    if (msg_length < (int32_t)(HDR_SIZE + FLAGS_SIZE + 1 + EMPTY_BSON_SIZE)) {
        error("[wire] reply too short: %d bytes", (int)msg_length);
        return MONGO_WIRE_ERR_PROTOCOL;
    }
    if ((uint32_t)msg_length > MONGO_WIRE_MAX_REPLY) {
        error("[wire] reply too big: %d bytes (max %u)", (int)msg_length, MONGO_WIRE_MAX_REPLY);
        return MONGO_WIRE_ERR_TOO_BIG;
    }
    if ((uint32_t)response_to != expected_response_to) {
        error("[wire] responseTo mismatch: got %u want %u", (unsigned)response_to, (unsigned)expected_response_to);
        return MONGO_WIRE_ERR_PROTOCOL;
    }

    size_t payload_size = (size_t)msg_length - HDR_SIZE;
    uint8_t *payload = pvPortMalloc(payload_size);
    if (!payload) {
        error("[wire] malloc(%u) for reply payload failed", (unsigned)payload_size);
        return MONGO_WIRE_ERR_ALLOC;
    }

    rc = mongo_transport_recv_exact(t, payload, payload_size, timeout_ms);
    if (rc != MONGO_TRANSPORT_OK) {
        vPortFree(payload);
        return MONGO_WIRE_ERR_TRANSPORT;
    }

    /* flagBits is the first 4 bytes of payload; we don't enforce any of them
     * for a basic client (no exhaust cursor, no checksum verification). */
    size_t offset = FLAGS_SIZE;

    while (offset < payload_size) {
        uint8_t section_type = payload[offset++];
        if (section_type == SECTION_BODY) {
            if (offset + 4 > payload_size) {
                vPortFree(payload);
                return MONGO_WIRE_ERR_PROTOCOL;
            }
            int32_t bson_size = read_le_int32(payload + offset);
            if (bson_size <= 0 || (size_t)(offset + (size_t)bson_size) > payload_size) {
                vPortFree(payload);
                return MONGO_WIRE_ERR_PROTOCOL;
            }
            bson_t src;
            if (!bson_init_static(&src, payload + offset, (size_t)bson_size)) {
                vPortFree(payload);
                return MONGO_WIRE_ERR_PROTOCOL;
            }
            /* The outer size check above only validates the OUTER length;
             * libbson's iterator will happily walk past nested malformed
             * length fields. Validate the full structure before letting
             * libbson (or callers) traverse it. */
            size_t bad_off = 0;
            if (!bson_validate(&src, BSON_VALIDATE_NONE, &bad_off)) {
                error("[wire] malformed BSON in reply at offset %u", (unsigned)bad_off);
                vPortFree(payload);
                return MONGO_WIRE_ERR_PROTOCOL;
            }
            bson_copy_to(&src, reply_out);
            vPortFree(payload);
            return MONGO_WIRE_OK;
        }
        if (section_type == SECTION_DOC_SEQUENCE) {
            /* MongoDB servers do not currently send section-1 in replies for
             * commands, so we treat encountering one before the body as a
             * protocol error rather than skip it. Revisit if exhaust cursors
             * or document-sequence replies enter scope. */
            error("[wire] unexpected section-1 in reply at offset %u", (unsigned)offset);
            vPortFree(payload);
            return MONGO_WIRE_ERR_PROTOCOL;
        }
        error("[wire] unknown section type 0x%02x", section_type);
        vPortFree(payload);
        return MONGO_WIRE_ERR_PROTOCOL;
    }

    vPortFree(payload);
    return MONGO_WIRE_ERR_NO_BODY;
}

int mongo_run_command(mongo_transport_t *t, const char *db, const bson_t *cmd, bson_t *reply_out, uint32_t timeout_ms) {
    if (!t || !db || !cmd || !reply_out) {
        return MONGO_WIRE_ERR_ARGS;
    }

    uint32_t request_id = next_request_id();
    int rc = send_op_msg(t, cmd, db, request_id, timeout_ms);
    if (rc != MONGO_WIRE_OK) {
        return rc;
    }
    return recv_op_msg(t, request_id, reply_out, timeout_ms);
}

double mongo_reply_ok(const bson_t *reply) {
    if (!reply) {
        return 0.0;
    }
    bson_iter_t it;
    if (bson_iter_init_find(&it, reply, "ok") && BSON_ITER_HOLDS_NUMBER(&it)) {
        return bson_iter_as_double(&it);
    }
    return 0.0;
}

/* Format the first write-error from a reply's `writeErrors` array, if present,
 * into `buf`. Atlas returns these for per-document insert/update/delete
 * failures (e.g. a permissions issue, schema validation, duplicate key). */
static void format_first_write_error(const bson_t *reply, char *buf, size_t buf_sz) {
    buf[0] = '\0';
    bson_iter_t it;
    if (!bson_iter_init_find(&it, reply, "writeErrors") || !BSON_ITER_HOLDS_ARRAY(&it)) {
        return;
    }
    bson_iter_t arr;
    if (!bson_iter_recurse(&it, &arr) || !bson_iter_next(&arr) || !BSON_ITER_HOLDS_DOCUMENT(&arr)) {
        return;
    }
    bson_iter_t doc;
    if (!bson_iter_recurse(&arr, &doc)) {
        return;
    }
    int we_code = 0;
    const char *we_msg = NULL;
    while (bson_iter_next(&doc)) {
        const char *key = bson_iter_key(&doc);
        if (strcmp(key, "code") == 0 && BSON_ITER_HOLDS_INT32(&doc)) {
            we_code = bson_iter_int32(&doc);
        } else if (strcmp(key, "errmsg") == 0 && BSON_ITER_HOLDS_UTF8(&doc)) {
            we_msg = bson_iter_utf8(&doc, NULL);
        }
    }
    snprintf(buf, buf_sz, " writeError[0]={code=%d, errmsg=%s}", we_code, we_msg ? we_msg : "(none)");
}

void mongo_log_reply_error(const char *phase, const bson_t *reply) {
    if (!reply) {
        error("[mongo] %s rejected (no reply)", phase ? phase : "?");
        return;
    }
    bson_iter_t it;
    const char *errmsg = NULL;
    int code = 0;
    const char *codename = NULL;

    if (bson_iter_init_find(&it, reply, "errmsg") && BSON_ITER_HOLDS_UTF8(&it)) {
        errmsg = bson_iter_utf8(&it, NULL);
    }
    if (bson_iter_init_find(&it, reply, "code") && BSON_ITER_HOLDS_INT32(&it)) {
        code = bson_iter_int32(&it);
    }
    if (bson_iter_init_find(&it, reply, "codeName") && BSON_ITER_HOLDS_UTF8(&it)) {
        codename = bson_iter_utf8(&it, NULL);
    }

    char we_buf[256];
    format_first_write_error(reply, we_buf, sizeof we_buf);

    if (errmsg || code != 0 || codename || we_buf[0]) {
        error("[mongo] %s rejected: code=%d codeName=%s errmsg=%s%s", phase ? phase : "?", code,
              codename ? codename : "(none)", errmsg ? errmsg : "(none)", we_buf);
    } else {
        error("[mongo] %s rejected (no errmsg/code/writeErrors in reply)", phase ? phase : "?");
    }
}
