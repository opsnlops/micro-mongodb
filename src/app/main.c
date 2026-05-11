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

/* Pull a numeric `ok` from a reply doc. Server returns 1.0 or 1, depending. */
static double reply_ok(const bson_t *reply) {
    bson_iter_t it;
    if (bson_iter_init_find(&it, reply, "ok") && BSON_ITER_HOLDS_NUMBER(&it)) {
        return bson_iter_as_double(&it);
    }
    return 0.0;
}

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
    if (rc != MONGO_WIRE_OK || reply_ok(&reply) < 1.0) {
        error("insert: rc=%d ok=%.0f", rc, reply_ok(&reply));
        bson_destroy(&reply);
        return rc != MONGO_WIRE_OK ? rc : MONGO_WIRE_ERR_PROTOCOL;
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
    if (rc != MONGO_WIRE_OK || reply_ok(&reply) < 1.0) {
        error("update: rc=%d ok=%.0f", rc, reply_ok(&reply));
        bson_destroy(&reply);
        return rc != MONGO_WIRE_OK ? rc : MONGO_WIRE_ERR_PROTOCOL;
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
    if (rc != MONGO_WIRE_OK || reply_ok(&reply) < 1.0) {
        error("delete: rc=%d ok=%.0f", rc, reply_ok(&reply));
        bson_destroy(&reply);
        return rc != MONGO_WIRE_OK ? rc : MONGO_WIRE_ERR_PROTOCOL;
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
    for (size_t i = 0; i < uri.n_hosts; i++) {
        info("  host[%zu] = %s:%u", i, uri.hosts[i].host, uri.hosts[i].port);
    }

    /* Pick the first host. Multi-host failover is task 9 (Atlas RS quirks). */
    const char *target_host = uri.hosts[0].host;
    uint16_t target_port = uri.hosts[0].port;

    mongo_transport_t *t = NULL;
    if (uri.tls) {
        mongo_tls_config_t tls = {0};
        tls.sni_hostname = target_host;
        t = mongo_transport_new(&tls);
        info("transport: TLS enabled, sni=%s", target_host);
    } else {
        t = mongo_transport_new(NULL);
        info("transport: plain TCP");
    }
    if (!t) {
        error("mongo_transport_new failed");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    for (;;) {
        info("opening tcp connection to %s:%u ...", target_host, target_port);
        TIMED_BEGIN(connect);
        rc = mongo_transport_connect(t, target_host, target_port, 5000);
        int t_connect = TIMED_US(connect);
        if (rc != MONGO_TRANSPORT_OK) {
            error("connect failed: rc=%d (lwip err=%d) after %d us", rc, mongo_transport_last_err(t), t_connect);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        info("connected (%d us), running smoke test", t_connect);
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
