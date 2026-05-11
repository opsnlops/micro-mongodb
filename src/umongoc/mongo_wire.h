/*
 * mongo_wire — minimal OP_MSG framing.
 *
 * MongoDB's modern wire protocol is just OP_MSG (opcode 2013). Each request
 * is a 16-byte header + 4-byte flagBits + sections; for our purposes we send
 * one section of type 0 (body) and read back one of the same. Section type 1
 * (document sequences for bulk inserts) is handled by the higher-level CRUD
 * layer when it ships.
 */

#pragma once

#include <stdint.h>

#include <bson/bson.h>

#include "mongo_transport.h"

#define MONGO_OP_MSG_OPCODE 2013

typedef enum {
    MONGO_WIRE_OK = 0,
    MONGO_WIRE_ERR_ARGS = -1,
    MONGO_WIRE_ERR_ALLOC = -2,
    MONGO_WIRE_ERR_TRANSPORT = -3,
    MONGO_WIRE_ERR_PROTOCOL = -4,
    MONGO_WIRE_ERR_TOO_BIG = -5,
    MONGO_WIRE_ERR_NO_BODY = -6,
    MONGO_WIRE_ERR_BSON = -7,
} mongo_wire_status_t;

/* Largest reply we will accept off the wire (mirrors MongoDB's default
 * maxMessageSizeBytes; the server's hello response can lower this). */
#define MONGO_WIRE_MAX_REPLY (16u * 1024u * 1024u)

/* Run a single command over `t`, with `db` placed in the standard $db field.
 *
 * On success, `reply_out` is initialised (caller must `bson_destroy()` it) and
 * 0 is returned. On failure the caller must NOT call bson_destroy(reply_out).
 *
 * The function blocks for up to `timeout_ms` covering both send and receive.
 */
int mongo_run_command(mongo_transport_t *t, const char *db, const bson_t *cmd, bson_t *reply_out, uint32_t timeout_ms);
