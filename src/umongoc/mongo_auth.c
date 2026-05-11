#include "mongo_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/rand.h"

#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/md5.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"

#include "logging.h"
#include "mongo_wire.h"

/* Detailed SCRAM diagnostic logging. When on, this prints salted-password-
 * derived material (client_key, salted_password, client_proof, the literal
 * auth_message, etc.) to the log -- which is sufficient material to
 * impersonate the user. Default is OFF; flip with -DMONGO_SCRAM_DEBUG=1 at
 * the cmake level while debugging an auth failure. */
#ifndef MONGO_SCRAM_DEBUG
#define MONGO_SCRAM_DEBUG 0
#endif

#if MONGO_SCRAM_DEBUG
static void hexify(const uint8_t *bytes, size_t n, char *out, size_t out_sz) {
    static const char hex[] = "0123456789abcdef";
    if (out_sz < 1) {
        return;
    }
    if (n * 2 + 1 > out_sz) {
        n = (out_sz - 1) / 2;
    }
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[n * 2] = '\0';
}
#endif

#define SCRAM_NONCE_RAW 24 /* spec: 24 random bytes per server-side */
#define SCRAM_NONCE_B64 ((SCRAM_NONCE_RAW + 2) / 3 * 4 + 1)
#define SCRAM_MAX_KEY_LEN 32 /* SHA-256 output; SHA-1 only needs 20 */
#define SCRAM_MIN_ITERATIONS 4096
#define SCRAM_AUTH_MSG_MAX 1024
#define SCRAM_FIELD_MAX 256
#define SCRAM_GS2_HEADER "n,,"
#define SCRAM_GS2_HEADER_B64 "biws" /* base64("n,,") */

/* Per-mechanism configuration. Parameterizes the otherwise-identical SCRAM
 * exchange across SCRAM-SHA-1 and SCRAM-SHA-256. */
typedef struct {
    mbedtls_md_type_t md_type;
    size_t key_len; /* hash output size: 20 for SHA-1, 32 for SHA-256 */
    const char *mechanism;
    /* Prepare the password input to PBKDF2. For SHA-256 this is identity;
     * for SHA-1 it's MongoDB's legacy hex(md5(user+":mongo:"+pw)) derivation.
     * Returns the length written to `out` (excl. NUL), or -1 on error. */
    int (*prep_password)(const char *username, const char *password, char *out, size_t out_sz);
} scram_mech_t;

static int prep_pw_identity(const char *username, const char *password, char *out, size_t out_sz) {
    (void)username;
    size_t n = strlen(password);
    if (n + 1 > out_sz) {
        return -1;
    }
    memcpy(out, password, n);
    out[n] = '\0';
    return (int)n;
}

/* MongoDB SCRAM-SHA-1 derives the PBKDF2 input via the same MONGODB-CR step:
 *     md5(username + ":mongo:" + password)
 * encoded as 32 lowercase hex characters. */
static int prep_pw_mongodb_cr(const char *username, const char *password, char *out, size_t out_sz) {
    if (out_sz < 33) {
        return -1;
    }
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    if (mbedtls_md5_starts(&ctx) != 0 ||
        mbedtls_md5_update(&ctx, (const unsigned char *)username, strlen(username)) != 0 ||
        mbedtls_md5_update(&ctx, (const unsigned char *)":mongo:", 7) != 0 ||
        mbedtls_md5_update(&ctx, (const unsigned char *)password, strlen(password)) != 0) {
        mbedtls_md5_free(&ctx);
        return -1;
    }
    uint8_t digest[16];
    int rc = mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);
    if (rc != 0) {
        return -1;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    out[32] = '\0';
    return 32;
}

static const scram_mech_t SCRAM_SHA1 = {
    .md_type = MBEDTLS_MD_SHA1,
    .key_len = 20,
    .mechanism = "SCRAM-SHA-1",
    .prep_password = prep_pw_mongodb_cr,
};

static const scram_mech_t SCRAM_SHA256 = {
    .md_type = MBEDTLS_MD_SHA256,
    .key_len = SCRAM_MAX_KEY_LEN,
    .mechanism = "SCRAM-SHA-256",
    .prep_password = prep_pw_identity,
};

/* Alias to keep diff small. Public version lives in mongo_wire.c. */
#define reply_ok(reply) mongo_reply_ok(reply)

const char *mongo_auth_status_str(int status) {
    switch (status) {
    case MONGO_AUTH_OK:
        return "OK";
    case MONGO_AUTH_ERR_ARGS:
        return "ARGS";
    case MONGO_AUTH_ERR_ALLOC:
        return "ALLOC";
    case MONGO_AUTH_ERR_PROTOCOL:
        return "PROTOCOL";
    case MONGO_AUTH_ERR_TRANSPORT:
        return "TRANSPORT";
    case MONGO_AUTH_ERR_ASCII_ONLY:
        return "ASCII_ONLY (non-ASCII password rejected by ASCII-only build)";
    case MONGO_AUTH_ERR_NONCE_MISMATCH:
        return "NONCE_MISMATCH (server-side replay / tampering)";
    case MONGO_AUTH_ERR_SERVER_REJECTED:
        return "SERVER_REJECTED (auth failed; check username/password/authSource)";
    case MONGO_AUTH_ERR_SERVER_SIG:
        return "SERVER_SIG (server signature did not verify -- MITM or impl bug)";
    case MONGO_AUTH_ERR_CRYPTO:
        return "CRYPTO (mbedTLS primitive failed)";
    default:
        return "?";
    }
}

/* Defer to the public reply-error logger so auth and CRUD ops share one
 * format. */
#define log_server_rejection(phase, reply) mongo_log_reply_error(phase, reply)

int mongo_handshake(mongo_transport_t *t, const char *app_name, const char *board_name, const char *sasl_user_db,
                    bson_t *reply_out, uint32_t timeout_ms) {
    if (!t || !reply_out) {
        return MONGO_AUTH_ERR_ARGS;
    }

    bson_t cmd;
    bson_init(&cmd);
    BSON_APPEND_INT32(&cmd, "hello", 1);

    bson_t client;
    BSON_APPEND_DOCUMENT_BEGIN(&cmd, "client", &client);

    bson_t driver;
    BSON_APPEND_DOCUMENT_BEGIN(&client, "driver", &driver);
    BSON_APPEND_UTF8(&driver, "name", "micro-mongodb");
    BSON_APPEND_UTF8(&driver, "version", "0.1.0");
    bson_append_document_end(&client, &driver);

    bson_t os;
    BSON_APPEND_DOCUMENT_BEGIN(&client, "os", &os);
    BSON_APPEND_UTF8(&os, "type", "embedded");
    if (board_name) {
        BSON_APPEND_UTF8(&os, "name", board_name);
    }
    BSON_APPEND_UTF8(&os, "architecture", "arm");
    bson_append_document_end(&client, &os);

    if (app_name) {
        bson_t app;
        BSON_APPEND_DOCUMENT_BEGIN(&client, "application", &app);
        BSON_APPEND_UTF8(&app, "name", app_name);
        bson_append_document_end(&client, &app);
    }

    bson_append_document_end(&cmd, &client);

    if (sasl_user_db) {
        BSON_APPEND_UTF8(&cmd, "saslSupportedMechs", sasl_user_db);
    }

    int rc = mongo_run_command(t, "admin", &cmd, reply_out, timeout_ms);
    bson_destroy(&cmd);
    if (rc != MONGO_WIRE_OK) {
        error("[auth] hello: rc=%d", rc);
        return MONGO_AUTH_ERR_TRANSPORT;
    }
    if (reply_ok(reply_out) < 1.0) {
        log_server_rejection("hello", reply_out);
        bson_destroy(reply_out);
        return MONGO_AUTH_ERR_SERVER_REJECTED;
    }

    /* Surface the saslSupportedMechs the server reported. */
    bson_iter_t it;
    if (sasl_user_db && bson_iter_init_find(&it, reply_out, "saslSupportedMechs") && BSON_ITER_HOLDS_ARRAY(&it)) {
        bson_iter_t arr;
        if (bson_iter_recurse(&it, &arr)) {
            char buf[128] = {0};
            size_t pos = 0;
            while (bson_iter_next(&arr) && BSON_ITER_HOLDS_UTF8(&arr)) {
                const char *m = bson_iter_utf8(&arr, NULL);
                int n = snprintf(buf + pos, sizeof buf - pos, "%s%s", pos ? "," : "", m);
                if (n < 0 || (size_t)n >= sizeof buf - pos) {
                    break;
                }
                pos += (size_t)n;
            }
            info("[auth] saslSupportedMechs for %s: [%s]", sasl_user_db, buf);
        }
    }
    return MONGO_AUTH_OK;
}

/* Pull a string field out of "k=v,k=v,..." style payloads. Returns the
 * value length and copies into `out` (NUL-terminated). */
static int extract_field(const char *payload, char key, char *out, size_t out_sz) {
    const char *p = payload;
    while (*p) {
        if (p[0] == key && p[1] == '=') {
            const char *value = p + 2;
            const char *end = strchr(value, ',');
            size_t len = end ? (size_t)(end - value) : strlen(value);
            if (len >= out_sz) {
                return -1;
            }
            memcpy(out, value, len);
            out[len] = '\0';
            return (int)len;
        }
        const char *next = strchr(p, ',');
        if (!next) {
            break;
        }
        p = next + 1;
    }
    return -1;
}

/* One-shot HMAC using the mechanism's hash. */
static int hmac_digest(mbedtls_md_type_t md_type, const uint8_t *key, size_t key_len, const uint8_t *msg,
                       size_t msg_len, uint8_t *out) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md_type);
    if (!info) {
        return -1;
    }
    return mbedtls_md_hmac(info, key, key_len, msg, msg_len, out);
}

/* One-shot hash using the mechanism's hash. */
static int md_digest(mbedtls_md_type_t md_type, const uint8_t *msg, size_t msg_len, uint8_t *out) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md_type);
    if (!info) {
        return -1;
    }
    return mbedtls_md(info, msg, msg_len, out);
}

/* Fill `buf` with `n` cryptographically random bytes using pico_rand. */
static void rand_bytes(uint8_t *buf, size_t n) {
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t r = get_rand_64();
        memcpy(buf + i, &r, 8);
        i += 8;
    }
    if (i < n) {
        uint64_t r = get_rand_64();
        memcpy(buf + i, &r, n - i);
    }
}

static int scram_auth_run(mongo_transport_t *t, const scram_mech_t *mech, const char *auth_source, const char *username,
                          const char *password, uint32_t timeout_ms) {
    if (!t || !mech || !auth_source || !username || !password) {
        return MONGO_AUTH_ERR_ARGS;
    }

    /* ASCII-only password (SASLprep punt -- see header doc comment). */
    for (const unsigned char *p = (const unsigned char *)password; *p; p++) {
        if (*p >= 0x80) {
            error("[auth] SCRAM: non-ASCII password not supported on embedded build");
            return MONGO_AUTH_ERR_ASCII_ONLY;
        }
    }

    debug("[auth] %s user='%s' authSource='%s' pw_len=%u", mech->mechanism, username, auth_source,
          (unsigned)strlen(password));

    /* Mechanism-specific password preprocessing. */
    char prepped_pw[64];
    int prepped_len = mech->prep_password(username, password, prepped_pw, sizeof prepped_pw);
    if (prepped_len < 0) {
        return MONGO_AUTH_ERR_CRYPTO;
    }

    /* ---- client-first ---- */
    uint8_t nonce_raw[SCRAM_NONCE_RAW];
    rand_bytes(nonce_raw, sizeof nonce_raw);

    char client_nonce[SCRAM_NONCE_B64];
    size_t client_nonce_len = 0;
    if (mbedtls_base64_encode((unsigned char *)client_nonce, sizeof client_nonce, &client_nonce_len, nonce_raw,
                              sizeof nonce_raw) != 0) {
        return MONGO_AUTH_ERR_CRYPTO;
    }
    client_nonce[client_nonce_len] = '\0';

    char client_first_bare[SCRAM_FIELD_MAX];
    int cfb_len = snprintf(client_first_bare, sizeof client_first_bare, "n=%s,r=%s", username, client_nonce);
    if (cfb_len < 0 || cfb_len >= (int)sizeof client_first_bare) {
        return MONGO_AUTH_ERR_ARGS;
    }

    char client_first_msg[SCRAM_FIELD_MAX + sizeof SCRAM_GS2_HEADER];
    int cfm_len = snprintf(client_first_msg, sizeof client_first_msg, "%s%s", SCRAM_GS2_HEADER, client_first_bare);
    if (cfm_len < 0 || cfm_len >= (int)sizeof client_first_msg) {
        return MONGO_AUTH_ERR_ARGS;
    }

    bson_t cmd;
    bson_init(&cmd);
    BSON_APPEND_INT32(&cmd, "saslStart", 1);
    BSON_APPEND_UTF8(&cmd, "mechanism", mech->mechanism);
    bson_append_binary(&cmd, "payload", 7, BSON_SUBTYPE_BINARY, (const uint8_t *)client_first_msg, (uint32_t)cfm_len);
    /* skipEmptyExchange asks the server to set done=true on the saslContinue
     * reply rather than expecting a final empty round-trip. */
    bson_t options;
    BSON_APPEND_DOCUMENT_BEGIN(&cmd, "options", &options);
    BSON_APPEND_BOOL(&options, "skipEmptyExchange", true);
    bson_append_document_end(&cmd, &options);

    bson_t reply;
    int rc = mongo_run_command(t, auth_source, &cmd, &reply, timeout_ms);
    bson_destroy(&cmd);
    if (rc != MONGO_WIRE_OK) {
        return MONGO_AUTH_ERR_TRANSPORT;
    }
    if (reply_ok(&reply) < 1.0) {
        log_server_rejection("saslStart/Continue", &reply);
        bson_destroy(&reply);
        return MONGO_AUTH_ERR_SERVER_REJECTED;
    }

    bson_iter_t it;
    int32_t conversation_id = 0;
    if (bson_iter_init_find(&it, &reply, "conversationId") && BSON_ITER_HOLDS_INT32(&it)) {
        conversation_id = bson_iter_int32(&it);
    }

    char server_first[SCRAM_FIELD_MAX * 2];
    if (!bson_iter_init_find(&it, &reply, "payload") || !BSON_ITER_HOLDS_BINARY(&it)) {
        bson_destroy(&reply);
        return MONGO_AUTH_ERR_PROTOCOL;
    }
    bson_subtype_t subtype;
    uint32_t bin_len = 0;
    const uint8_t *bin_data = NULL;
    bson_iter_binary(&it, &subtype, &bin_len, &bin_data);
    if (bin_len >= sizeof server_first) {
        bson_destroy(&reply);
        return MONGO_AUTH_ERR_PROTOCOL;
    }
    memcpy(server_first, bin_data, bin_len);
    server_first[bin_len] = '\0';
    bson_destroy(&reply);

    /* ---- server-first parse: r=<nonce>, s=<base64 salt>, i=<iterations> ---- */
    char server_nonce[SCRAM_FIELD_MAX];
    char salt_b64[SCRAM_FIELD_MAX];
    char iter_str[16];
    if (extract_field(server_first, 'r', server_nonce, sizeof server_nonce) < 0 ||
        extract_field(server_first, 's', salt_b64, sizeof salt_b64) < 0 ||
        extract_field(server_first, 'i', iter_str, sizeof iter_str) < 0) {
        error("[auth] server-first missing required fields");
        return MONGO_AUTH_ERR_PROTOCOL;
    }
    if (strncmp(server_nonce, client_nonce, client_nonce_len) != 0) {
        error("[auth] server nonce does not begin with our client nonce");
        return MONGO_AUTH_ERR_NONCE_MISMATCH;
    }
    long iterations = strtol(iter_str, NULL, 10);
    if (iterations < SCRAM_MIN_ITERATIONS) {
        error("[auth] server iteration count %ld below SCRAM floor", iterations);
        return MONGO_AUTH_ERR_PROTOCOL;
    }

    uint8_t salt[64];
    size_t salt_len = 0;
    if (mbedtls_base64_decode(salt, sizeof salt, &salt_len, (const unsigned char *)salt_b64, strlen(salt_b64)) != 0) {
        return MONGO_AUTH_ERR_PROTOCOL;
    }

#if MONGO_SCRAM_DEBUG
    debug("[auth] server-first parsed: iterations=%ld salt_b64='%s' salt_len=%u", iterations, salt_b64,
          (unsigned)salt_len);
    {
        char hex[2 * sizeof salt + 1];
        hexify(salt, salt_len, hex, sizeof hex);
        debug("[auth] salt hex=%s", hex);
    }
    debug("[auth] client_first_bare='%s'", client_first_bare);
    debug("[auth] server_first='%s'", server_first);
#endif

    /* ---- key derivation ---- */
    uint8_t salted_password[SCRAM_MAX_KEY_LEN];
    {
        mbedtls_md_context_t md_ctx;
        mbedtls_md_init(&md_ctx);
        if (mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(mech->md_type), 1) != 0) {
            mbedtls_md_free(&md_ctx);
            return MONGO_AUTH_ERR_CRYPTO;
        }
        int pkr =
            mbedtls_pkcs5_pbkdf2_hmac(&md_ctx, (const unsigned char *)prepped_pw, (size_t)prepped_len, salt, salt_len,
                                      (unsigned int)iterations, (uint32_t)mech->key_len, salted_password);
        mbedtls_md_free(&md_ctx);
        if (pkr != 0) {
            error("[auth] PBKDF2 failed: %d", pkr);
            return MONGO_AUTH_ERR_CRYPTO;
        }
    }

    uint8_t client_key[SCRAM_MAX_KEY_LEN];
    uint8_t stored_key[SCRAM_MAX_KEY_LEN];
    uint8_t server_key[SCRAM_MAX_KEY_LEN];
    if (hmac_digest(mech->md_type, salted_password, mech->key_len, (const uint8_t *)"Client Key", 10, client_key) !=
            0 ||
        hmac_digest(mech->md_type, salted_password, mech->key_len, (const uint8_t *)"Server Key", 10, server_key) !=
            0 ||
        md_digest(mech->md_type, client_key, mech->key_len, stored_key) != 0) {
        return MONGO_AUTH_ERR_CRYPTO;
    }

#if MONGO_SCRAM_DEBUG
    {
        char hex[2 * SCRAM_MAX_KEY_LEN + 1];
        hexify(salted_password, mech->key_len, hex, sizeof hex);
        debug("[auth] salted_password=%s", hex);
        hexify(client_key, mech->key_len, hex, sizeof hex);
        debug("[auth] client_key=%s", hex);
        hexify(stored_key, mech->key_len, hex, sizeof hex);
        debug("[auth] stored_key=%s", hex);
    }
#endif

    /* ---- client-final ---- */
    char client_final_without_proof[SCRAM_FIELD_MAX];
    int cfwp_len = snprintf(client_final_without_proof, sizeof client_final_without_proof, "c=%s,r=%s",
                            SCRAM_GS2_HEADER_B64, server_nonce);
    if (cfwp_len < 0 || cfwp_len >= (int)sizeof client_final_without_proof) {
        return MONGO_AUTH_ERR_PROTOCOL;
    }

    char auth_message[SCRAM_AUTH_MSG_MAX];
    int am_len = snprintf(auth_message, sizeof auth_message, "%s,%s,%s", client_first_bare, server_first,
                          client_final_without_proof);
    if (am_len < 0 || am_len >= (int)sizeof auth_message) {
        error("[auth] auth_message buffer too small");
        return MONGO_AUTH_ERR_PROTOCOL;
    }

#if MONGO_SCRAM_DEBUG
    debug("[auth] client_final_without_proof='%s'", client_final_without_proof);
    debug("[auth] auth_message='%s'", auth_message);
#endif

    uint8_t client_signature[SCRAM_MAX_KEY_LEN];
    if (hmac_digest(mech->md_type, stored_key, mech->key_len, (const uint8_t *)auth_message, (size_t)am_len,
                    client_signature) != 0) {
        return MONGO_AUTH_ERR_CRYPTO;
    }

    uint8_t client_proof[SCRAM_MAX_KEY_LEN];
    for (size_t i = 0; i < mech->key_len; i++) {
        client_proof[i] = client_key[i] ^ client_signature[i];
    }

#if MONGO_SCRAM_DEBUG
    {
        char hex[2 * SCRAM_MAX_KEY_LEN + 1];
        hexify(client_signature, mech->key_len, hex, sizeof hex);
        debug("[auth] client_signature=%s", hex);
        hexify(client_proof, mech->key_len, hex, sizeof hex);
        debug("[auth] client_proof=%s", hex);
    }
#endif

    char proof_b64[64];
    size_t proof_b64_len = 0;
    if (mbedtls_base64_encode((unsigned char *)proof_b64, sizeof proof_b64, &proof_b64_len, client_proof,
                              mech->key_len) != 0) {
        return MONGO_AUTH_ERR_CRYPTO;
    }
    proof_b64[proof_b64_len] = '\0';

    char client_final[SCRAM_FIELD_MAX * 2];
    int cf_len = snprintf(client_final, sizeof client_final, "%s,p=%s", client_final_without_proof, proof_b64);
    if (cf_len < 0 || cf_len >= (int)sizeof client_final) {
        return MONGO_AUTH_ERR_PROTOCOL;
    }

    bson_init(&cmd);
    BSON_APPEND_INT32(&cmd, "saslContinue", 1);
    BSON_APPEND_INT32(&cmd, "conversationId", conversation_id);
    bson_append_binary(&cmd, "payload", 7, BSON_SUBTYPE_BINARY, (const uint8_t *)client_final, (uint32_t)cf_len);

    rc = mongo_run_command(t, auth_source, &cmd, &reply, timeout_ms);
    bson_destroy(&cmd);
    if (rc != MONGO_WIRE_OK) {
        return MONGO_AUTH_ERR_TRANSPORT;
    }
    if (reply_ok(&reply) < 1.0) {
        log_server_rejection("saslStart/Continue", &reply);
        bson_destroy(&reply);
        return MONGO_AUTH_ERR_SERVER_REJECTED;
    }

    bool done = false;
    if (bson_iter_init_find(&it, &reply, "done") && BSON_ITER_HOLDS_BOOL(&it)) {
        done = bson_iter_bool(&it);
    }
    if (!bson_iter_init_find(&it, &reply, "payload") || !BSON_ITER_HOLDS_BINARY(&it)) {
        bson_destroy(&reply);
        return MONGO_AUTH_ERR_PROTOCOL;
    }
    bson_iter_binary(&it, &subtype, &bin_len, &bin_data);
    char server_final[SCRAM_FIELD_MAX];
    if (bin_len >= sizeof server_final) {
        bson_destroy(&reply);
        return MONGO_AUTH_ERR_PROTOCOL;
    }
    memcpy(server_final, bin_data, bin_len);
    server_final[bin_len] = '\0';
    bson_destroy(&reply);

    /* ---- server signature verification ---- */
    if (strncmp(server_final, "v=", 2) != 0) {
        error("[auth] server-final missing v=");
        return MONGO_AUTH_ERR_PROTOCOL;
    }

    uint8_t expected_sig[SCRAM_MAX_KEY_LEN];
    if (hmac_digest(mech->md_type, server_key, mech->key_len, (const uint8_t *)auth_message, (size_t)am_len,
                    expected_sig) != 0) {
        return MONGO_AUTH_ERR_CRYPTO;
    }

    uint8_t actual_sig[64];
    size_t actual_sig_len = 0;
    if (mbedtls_base64_decode(actual_sig, sizeof actual_sig, &actual_sig_len, (const unsigned char *)(server_final + 2),
                              strlen(server_final + 2)) != 0) {
        return MONGO_AUTH_ERR_PROTOCOL;
    }
    if (actual_sig_len != mech->key_len || memcmp(actual_sig, expected_sig, mech->key_len) != 0) {
        error("[auth] server signature mismatch");
        return MONGO_AUTH_ERR_SERVER_SIG;
    }

    /* ---- optional empty saslContinue ---- */
    if (!done) {
        bson_init(&cmd);
        BSON_APPEND_INT32(&cmd, "saslContinue", 1);
        BSON_APPEND_INT32(&cmd, "conversationId", conversation_id);
        uint8_t empty = 0;
        bson_append_binary(&cmd, "payload", 7, BSON_SUBTYPE_BINARY, &empty, 0);
        rc = mongo_run_command(t, auth_source, &cmd, &reply, timeout_ms);
        bson_destroy(&cmd);
        if (rc == MONGO_WIRE_OK) {
            bson_destroy(&reply);
        }
    }

    return MONGO_AUTH_OK;
}

int mongo_authenticate_scram_sha256(mongo_transport_t *t, const char *auth_source, const char *username,
                                    const char *password, uint32_t timeout_ms) {
    return scram_auth_run(t, &SCRAM_SHA256, auth_source, username, password, timeout_ms);
}

int mongo_authenticate_scram_sha1(mongo_transport_t *t, const char *auth_source, const char *username,
                                  const char *password, uint32_t timeout_ms) {
    return scram_auth_run(t, &SCRAM_SHA1, auth_source, username, password, timeout_ms);
}

int mongo_authenticate(mongo_transport_t *t, const bson_t *hello_reply, const char *auth_source, const char *username,
                       const char *password, uint32_t timeout_ms) {
    if (!t || !auth_source || !username || !password) {
        return MONGO_AUTH_ERR_ARGS;
    }

    bool has_sha256 = false;
    bool has_sha1 = false;
    bool have_mechs_list = false;

    bson_iter_t it;
    if (hello_reply && bson_iter_init_find(&it, hello_reply, "saslSupportedMechs") && BSON_ITER_HOLDS_ARRAY(&it)) {
        bson_iter_t arr;
        if (bson_iter_recurse(&it, &arr)) {
            have_mechs_list = true;
            while (bson_iter_next(&arr) && BSON_ITER_HOLDS_UTF8(&arr)) {
                const char *m = bson_iter_utf8(&arr, NULL);
                if (strcmp(m, "SCRAM-SHA-256") == 0) {
                    has_sha256 = true;
                } else if (strcmp(m, "SCRAM-SHA-1") == 0) {
                    has_sha1 = true;
                }
            }
        }
    }

    /* If the hello reply didn't include saslSupportedMechs (caller didn't ask
     * for it, or the server is old), prefer SHA-256 -- a SHA-256-only server
     * will accept it, and a SHA-1-only server will tell us so via the
     * SERVER_REJECTED that comes back. */
    if (!have_mechs_list) {
        has_sha256 = true;
    }

    if (has_sha256) {
        return scram_auth_run(t, &SCRAM_SHA256, auth_source, username, password, timeout_ms);
    }
    if (has_sha1) {
        warning("[auth] server only stores SCRAM-SHA-1 for this user; using legacy mechanism");
        return scram_auth_run(t, &SCRAM_SHA1, auth_source, username, password, timeout_ms);
    }
    error("[auth] no SCRAM mechanism available for %s.%s", auth_source, username);
    return MONGO_AUTH_ERR_PROTOCOL;
}
