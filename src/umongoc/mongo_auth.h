/*
 * mongo_auth — handshake (hello) and SCRAM-SHA-256 authentication.
 *
 * Sequence:
 *   1. mongo_handshake() runs {hello: 1, client: {...}} so the server knows
 *      who's connecting. Reply contains saslSupportedMechs, maxBsonObjectSize,
 *      maxMessageSizeBytes, etc.
 *   2. If credentials exist, mongo_authenticate_scram_sha256() runs the
 *      three-message SCRAM-SHA-256 exchange.
 *
 * ASCII-only passwords. Full RFC 3454 SASLprep is ~100 KB of Unicode
 * normalization tables; passwords containing non-ASCII codepoints are
 * rejected outright with a clear error rather than silently corrupted.
 *
 * Out of scope: speculativeAuthenticate (would combine hello + saslStart),
 * X.509 client cert auth, AWS IAM, OIDC.
 */

#pragma once

#include <stdint.h>

#include <bson/bson.h>

#include "mongo_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MONGO_AUTH_OK = 0,
    MONGO_AUTH_ERR_ARGS = -1,
    MONGO_AUTH_ERR_ALLOC = -2,
    MONGO_AUTH_ERR_PROTOCOL = -3,
    MONGO_AUTH_ERR_TRANSPORT = -4,
    MONGO_AUTH_ERR_ASCII_ONLY = -5,
    MONGO_AUTH_ERR_NONCE_MISMATCH = -6,
    MONGO_AUTH_ERR_SERVER_REJECTED = -7,
    MONGO_AUTH_ERR_SERVER_SIG = -8,
    MONGO_AUTH_ERR_CRYPTO = -9,
} mongo_auth_status_t;

/* Return a human-readable name for a mongo_auth_status_t code, e.g.
 * "SERVER_REJECTED" for -7. */
const char *mongo_auth_status_str(int status);

/* Run the {hello: 1, client: {...}} handshake. The reply (which contains
 * saslSupportedMechs, maxBsonObjectSize, etc.) is copied into `reply_out`;
 * caller must bson_destroy() it on success. */
int mongo_handshake(mongo_transport_t *t, const char *app_name, const char *board_name, bson_t *reply_out,
                    uint32_t timeout_ms);

/* Run SCRAM-SHA-256. `auth_source` is the database to authenticate against
 * (typically "admin" for Atlas users). Caller must have already completed
 * the hello handshake on `t`. */
int mongo_authenticate_scram_sha256(mongo_transport_t *t, const char *auth_source, const char *username,
                                    const char *password, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
