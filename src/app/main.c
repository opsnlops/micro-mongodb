/* micro-mongodb bring-up entry point.
 *
 * Each cycle of the network task:
 *   1. Connect to MONGO_URI's host:port.
 *   2. Run a small CRUD smoke test (ping, insert, find+cursor, update, delete).
 *   3. Disconnect and sleep before the next cycle.
 *
 * The smoke test is idempotent across runs -- each iteration tags its insert
 * with the current ms-since-boot so concurrent boards don't collide on the
 * same key, and the test ends by deleting its own row.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "FreeRTOS.h"
#include "task.h"

#include <bson/bson.h>

#include "logging.h"
#include "mongo_auth.h"
#include "mongo_crud.h"
#include "mongo_cursor.h"
#include "mongo_transport.h"
#include "mongo_uri.h"
#include "mongo_wire.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef MONGO_URI
#define MONGO_URI "mongodb://192.168.1.1:27017"
#endif

#define NETWORK_TASK_STACK_WORDS 4096
#define NETWORK_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

#define TEST_DB "micro_mongodb"
#define TEST_COLL "pico"
#define CMD_TIMEOUT_MS 5000

#if PICO_PLATFORM_IS_RP2350
#define BOARD_NAME "pico2_w"
#else
#define BOARD_NAME "pico_w"
#endif

/* Microsecond timer: useful for the Pico W vs Pico 2 W perf comparison.
 * absolute_time_diff_us returns int64 but for our sub-second ops it fits an
 * int comfortably. */
#define TIMED_BEGIN(name) absolute_time_t _t_##name = get_absolute_time()
#define TIMED_US(name) ((int)absolute_time_diff_us(_t_##name, get_absolute_time()))

/* reply_ok lives in umongoc now (mongo_wire.c). */
#define reply_ok(reply) mongo_reply_ok(reply)

static int run_ping(mongo_transport_t *t, int *us_out) {
    bson_t cmd;
    bson_init(&cmd);
    BSON_APPEND_INT32(&cmd, "ping", 1);

    TIMED_BEGIN(ping);
    bson_t reply;
    int rc = mongo_run_command(t, "admin", &cmd, &reply, CMD_TIMEOUT_MS);
    *us_out = TIMED_US(ping);
    bson_destroy(&cmd);
    if (rc != MONGO_WIRE_OK) {
        error("ping: rc=%d", rc);
        return rc;
    }
    info("ping ok=%.0f (%d us)", reply_ok(&reply), *us_out);
    bson_destroy(&reply);
    return MONGO_WIRE_OK;
}

static int run_smoke_test(mongo_transport_t *t) {
    TIMED_BEGIN(total);
    int t_ping = 0, t_insert = 0, t_find = 0, t_update = 0, t_delete = 0;

    int rc = run_ping(t, &t_ping);
    if (rc != MONGO_WIRE_OK) {
        return rc;
    }

    /* Use boot-time milliseconds as a per-iteration tag so back-to-back runs
     * don't see each other's rows. */
    int64_t tag = (int64_t)to_ms_since_boot(get_absolute_time());

    /* --- insert --- */
    bson_t doc;
    bson_init(&doc);
    BSON_APPEND_INT64(&doc, "pico_tag", tag);
    BSON_APPEND_UTF8(&doc, "board", BOARD_NAME);

    bson_t reply;
    TIMED_BEGIN(insert);
    rc = mongo_insert_one(t, TEST_DB, TEST_COLL, &doc, &reply, CMD_TIMEOUT_MS);
    t_insert = TIMED_US(insert);
    bson_destroy(&doc);
    if (rc != MONGO_WIRE_OK) {
        error("insert: transport rc=%d", rc);
        bson_destroy(&reply);
        return rc;
    }
    if (reply_ok(&reply) < 1.0) {
        mongo_log_reply_error("insert", &reply);
        bson_destroy(&reply);
        return MONGO_WIRE_ERR_PROTOCOL;
    }
    bson_destroy(&reply);
    info("insert ok tag=%lld (%d us)", (long long)tag, t_insert);

    /* --- find with cursor --- */
    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "board", BOARD_NAME);

    mongo_cursor_t *cur = NULL;
    TIMED_BEGIN(find);
    rc = mongo_find(t, TEST_DB, TEST_COLL, &filter, 5, &cur, CMD_TIMEOUT_MS);
    bson_destroy(&filter);
    if (rc != MONGO_WIRE_OK) {
        t_find = TIMED_US(find);
        error("find: rc=%d (%d us)", rc, t_find);
        return rc;
    }

    const bson_t *fd = NULL;
    int n;
    while ((n = mongo_cursor_next(cur, &fd, CMD_TIMEOUT_MS)) == 1) {
        bson_iter_t it;
        if (bson_iter_init_find(&it, fd, "pico_tag") && BSON_ITER_HOLDS_INT64(&it)) {
            debug("  doc pico_tag=%lld", (long long)bson_iter_int64(&it));
        }
    }
    t_find = TIMED_US(find);
    info("find iterated %u docs (%d us)", (unsigned)mongo_cursor_total_seen(cur), t_find);
    int cursor_status = n; /* 0 = exhausted, negative = error */
    mongo_cursor_destroy(cur);
    if (cursor_status < 0) {
        error("cursor: rc=%d", cursor_status);
        return cursor_status;
    }

    /* --- update --- */
    bson_t up_filter;
    bson_init(&up_filter);
    BSON_APPEND_INT64(&up_filter, "pico_tag", tag);

    bson_t up_spec;
    bson_init(&up_spec);
    bson_t set;
    BSON_APPEND_DOCUMENT_BEGIN(&up_spec, "$set", &set);
    BSON_APPEND_BOOL(&set, "seen", true);
    bson_append_document_end(&up_spec, &set);

    TIMED_BEGIN(update);
    rc = mongo_update_one(t, TEST_DB, TEST_COLL, &up_filter, &up_spec, false, &reply, CMD_TIMEOUT_MS);
    t_update = TIMED_US(update);
    bson_destroy(&up_filter);
    bson_destroy(&up_spec);
    if (rc != MONGO_WIRE_OK) {
        error("update: transport rc=%d", rc);
        bson_destroy(&reply);
        return rc;
    }
    if (reply_ok(&reply) < 1.0) {
        mongo_log_reply_error("update", &reply);
        bson_destroy(&reply);
        return MONGO_WIRE_ERR_PROTOCOL;
    }
    bson_destroy(&reply);
    info("update ok (%d us)", t_update);

    /* --- delete (cleanup) --- */
    bson_t del_filter;
    bson_init(&del_filter);
    BSON_APPEND_INT64(&del_filter, "pico_tag", tag);

    TIMED_BEGIN(del);
    rc = mongo_delete_one(t, TEST_DB, TEST_COLL, &del_filter, &reply, CMD_TIMEOUT_MS);
    t_delete = TIMED_US(del);
    bson_destroy(&del_filter);
    if (rc != MONGO_WIRE_OK) {
        error("delete: transport rc=%d", rc);
        bson_destroy(&reply);
        return rc;
    }
    if (reply_ok(&reply) < 1.0) {
        mongo_log_reply_error("delete", &reply);
        bson_destroy(&reply);
        return MONGO_WIRE_ERR_PROTOCOL;
    }
    bson_destroy(&reply);
    info("delete ok (%d us)", t_delete);

    info("perf [%s]: ping=%d insert=%d find=%d update=%d delete=%d total=%d us", BOARD_NAME, t_ping, t_insert, t_find,
         t_update, t_delete, TIMED_US(total));
    return MONGO_WIRE_OK;
}

static void network_task(void *arg) {
    (void)arg;

    if (cyw43_arch_init()) {
        error("cyw43_arch_init failed");
        vTaskDelete(NULL);
        return;
    }
    cyw43_arch_enable_sta_mode();

    if (strlen(WIFI_SSID) == 0) {
        error("WIFI_SSID not configured -- pass -DWIFI_SSID=... to cmake");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    info("connecting to wifi: %s", WIFI_SSID);
    int rc = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (rc) {
        error("wifi connect failed: rc=%d", rc);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    info("wifi connected");

    mongo_uri_t uri;
    int uri_rc = mongo_uri_parse(MONGO_URI, &uri, 5000);
    if (uri_rc != MONGO_URI_OK) {
        error("mongo_uri_parse: rc=%d for %s", uri_rc, MONGO_URI);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    info("mongo target: %zu host(s), tls=%d, srv=%d, replicaSet='%s'", uri.n_hosts, (int)uri.tls, (int)uri.is_srv,
         uri.replica_set);
    info("  user='%s' authSource='%s' database='%s'", uri.username, uri.auth_source, uri.database);
    for (size_t i = 0; i < uri.n_hosts; i++) {
        info("  host[%zu] = %s:%u", i, uri.hosts[i].host, uri.hosts[i].port);
    }

    /* Start with the first host the SRV record returned. If we land on a
     * secondary, the hello reply tells us who the actual primary is and
     * we swap targets below -- minimal SDAM-lite for replica sets. */
    char target_host[MONGO_URI_HOST_MAX];
    strncpy(target_host, uri.hosts[0].host, sizeof target_host - 1);
    target_host[sizeof target_host - 1] = '\0';
    uint16_t target_port = uri.hosts[0].port;

    mongo_transport_t *t = NULL;

    for (;;) {
        /* (Re)create the transport so SNI matches our current target. The
         * cost of free+new is small; the TLS handshake is what's expensive,
         * and that's bound to the connect path either way. */
        if (t) {
            mongo_transport_free(t);
            t = NULL;
        }
        if (uri.tls) {
            mongo_tls_config_t tls = {0};
            tls.sni_hostname = target_host;
            t = mongo_transport_new(&tls);
        } else {
            t = mongo_transport_new(NULL);
        }
        if (!t) {
            error("mongo_transport_new failed");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        info("opening tcp connection to %s:%u ... (%s)", target_host, target_port, uri.tls ? "TLS" : "plain");
        TIMED_BEGIN(connect);
        rc = mongo_transport_connect(t, target_host, target_port, 5000);
        int t_connect = TIMED_US(connect);
        if (rc != MONGO_TRANSPORT_OK) {
            error("connect failed: rc=%d (lwip err=%d) after %d us", rc, mongo_transport_last_err(t), t_connect);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        info("connected (%d us)", t_connect);

        /* hello handshake. If we have credentials, ask the server which
         * mechanisms are actually enabled for this specific user. */
        char sasl_user_db[160] = {0};
        const char *sasl_user_db_arg = NULL;
        if (uri.username[0]) {
            snprintf(sasl_user_db, sizeof sasl_user_db, "%s.%s", uri.auth_source, uri.username);
            sasl_user_db_arg = sasl_user_db;
        }
        TIMED_BEGIN(hello);
        bson_t hello_reply;
        int hrc = mongo_handshake(t, "micro-mongodb-demo", BOARD_NAME, sasl_user_db_arg, &hello_reply, CMD_TIMEOUT_MS);
        int t_hello = TIMED_US(hello);
        if (hrc != MONGO_AUTH_OK) {
            error("hello failed: %s (rc=%d, %d us)", mongo_auth_status_str(hrc), hrc, t_hello);
            mongo_transport_close(t);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        info("hello ok (%d us)", t_hello);

        /* Replica-set primary discovery (mini-SDAM). If hello tells us we
         * landed on a secondary, swap to the primary it advertises and
         * restart the cycle. Writes only work against the primary; without
         * this, a NotWritablePrimary failure loop is the result. */
        {
            bson_iter_t mit;
            bool is_writable_primary = false;
            if (bson_iter_init_find(&mit, &hello_reply, "isWritablePrimary") && BSON_ITER_HOLDS_BOOL(&mit)) {
                is_writable_primary = bson_iter_bool(&mit);
            } else if (bson_iter_init_find(&mit, &hello_reply, "ismaster") && BSON_ITER_HOLDS_BOOL(&mit)) {
                is_writable_primary = bson_iter_bool(&mit);
            }
            if (!is_writable_primary) {
                const char *primary_str = NULL;
                if (bson_iter_init_find(&mit, &hello_reply, "primary") && BSON_ITER_HOLDS_UTF8(&mit)) {
                    primary_str = bson_iter_utf8(&mit, NULL);
                }
                if (primary_str && primary_str[0]) {
                    const char *colon = strchr(primary_str, ':');
                    size_t host_len = colon ? (size_t)(colon - primary_str) : strlen(primary_str);
                    if (host_len > 0 && host_len < sizeof target_host) {
                        memcpy(target_host, primary_str, host_len);
                        target_host[host_len] = '\0';
                        target_port = colon ? (uint16_t)strtoul(colon + 1, NULL, 10) : 27017;
                        info("[net] connected to secondary; redirecting to primary %s:%u", target_host, target_port);
                        bson_destroy(&hello_reply);
                        mongo_transport_close(t);
                        continue; /* loop will recreate transport with new SNI */
                    }
                }
                warning("[net] connected to secondary but primary unknown; retrying in 2s");
                bson_destroy(&hello_reply);
                mongo_transport_close(t);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        /* SCRAM if credentials are configured. The driver picks SHA-256 or
         * SHA-1 based on the hello reply's saslSupportedMechs. */
        int t_scram = 0;
        if (uri.username[0] && uri.password[0]) {
            TIMED_BEGIN(scram);
            int arc = mongo_authenticate(t, &hello_reply, uri.auth_source, uri.username, uri.password, CMD_TIMEOUT_MS);
            t_scram = TIMED_US(scram);
            if (arc != MONGO_AUTH_OK) {
                error("scram failed: %s (rc=%d, %d us)", mongo_auth_status_str(arc), arc, t_scram);
                bson_destroy(&hello_reply);
                mongo_transport_close(t);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            info("scram ok (%d us)", t_scram);
        }
        bson_destroy(&hello_reply);

        info("running smoke test");
        rc = run_smoke_test(t);
        if (rc == MONGO_WIRE_OK) {
            info("smoke test passed");
        } else {
            warning("smoke test failed rc=%d", rc);
        }
        mongo_transport_close(t);
        info("closed; sleeping 10s");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

int main(void) {
    stdio_init_all();

    /* Give USB-CDC a moment to enumerate before any output. */
    sleep_ms(2000);

    logger_init();
    info("=== micro-mongodb boot (board=%s) ===", BOARD_NAME);

    TaskHandle_t net_handle = NULL;
    BaseType_t ok =
        xTaskCreate(network_task, "net", NETWORK_TASK_STACK_WORDS, NULL, NETWORK_TASK_PRIORITY, &net_handle);
    if (ok != pdPASS) {
        panic("failed to create network task");
    }
    vTaskCoreAffinitySet(net_handle, 1 << 0);

    vTaskStartScheduler();

    for (;;) {
    }
}
