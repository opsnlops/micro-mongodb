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

/* Minimal mongodb:// parser -- proper URI handling (auth, options, +srv) lands
 * in task #6. Accepts "mongodb://host[:port][/...]"; ignores everything after
 * the first '/'. */
static bool parse_mongo_uri(const char *uri, char *host_out, size_t host_sz, uint16_t *port_out) {
    static const char prefix[] = "mongodb://";
    size_t prefix_len = sizeof prefix - 1;
    if (strncmp(uri, prefix, prefix_len) != 0) {
        return false;
    }
    const char *rest = uri + prefix_len;
    const char *path = strchr(rest, '/');
    size_t hostport_len = path ? (size_t)(path - rest) : strlen(rest);

    const char *colon = NULL;
    for (size_t i = 0; i < hostport_len; i++) {
        if (rest[i] == ':') {
            colon = rest + i;
        }
    }

    size_t host_len = colon ? (size_t)(colon - rest) : hostport_len;
    if (host_len == 0 || host_len >= host_sz) {
        return false;
    }
    memcpy(host_out, rest, host_len);
    host_out[host_len] = '\0';

    if (colon) {
        unsigned long port = strtoul(colon + 1, NULL, 10);
        if (port == 0 || port > 65535) {
            return false;
        }
        *port_out = (uint16_t)port;
    } else {
        *port_out = 27017;
    }
    return true;
}

/* Pull a numeric `ok` from a reply doc. Server returns 1.0 or 1, depending. */
static double reply_ok(const bson_t *reply) {
    bson_iter_t it;
    if (bson_iter_init_find(&it, reply, "ok") && BSON_ITER_HOLDS_NUMBER(&it)) {
        return bson_iter_as_double(&it);
    }
    return 0.0;
}

static int run_ping(mongo_transport_t *t) {
    bson_t cmd;
    bson_init(&cmd);
    BSON_APPEND_INT32(&cmd, "ping", 1);

    bson_t reply;
    int rc = mongo_run_command(t, "admin", &cmd, &reply, CMD_TIMEOUT_MS);
    bson_destroy(&cmd);
    if (rc != MONGO_WIRE_OK) {
        error("ping: rc=%d", rc);
        return rc;
    }
    info("ping ok=%.0f", reply_ok(&reply));
    bson_destroy(&reply);
    return MONGO_WIRE_OK;
}

static int run_smoke_test(mongo_transport_t *t) {
    int rc = run_ping(t);
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
    BSON_APPEND_UTF8(&doc, "board", "pico2w");

    bson_t reply;
    rc = mongo_insert_one(t, TEST_DB, TEST_COLL, &doc, &reply, CMD_TIMEOUT_MS);
    bson_destroy(&doc);
    if (rc != MONGO_WIRE_OK || reply_ok(&reply) < 1.0) {
        error("insert: rc=%d ok=%.0f", rc, reply_ok(&reply));
        bson_destroy(&reply);
        return rc != MONGO_WIRE_OK ? rc : MONGO_WIRE_ERR_PROTOCOL;
    }
    bson_destroy(&reply);
    info("insert ok tag=%lld", (long long)tag);

    /* --- find with cursor --- */
    bson_t filter;
    bson_init(&filter);
    BSON_APPEND_UTF8(&filter, "board", "pico2w");

    mongo_cursor_t *cur = NULL;
    rc = mongo_find(t, TEST_DB, TEST_COLL, &filter, 5, &cur, CMD_TIMEOUT_MS);
    bson_destroy(&filter);
    if (rc != MONGO_WIRE_OK) {
        error("find: rc=%d", rc);
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
    info("find iterated %u docs", (unsigned)mongo_cursor_total_seen(cur));
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

    rc = mongo_update_one(t, TEST_DB, TEST_COLL, &up_filter, &up_spec, false, &reply, CMD_TIMEOUT_MS);
    bson_destroy(&up_filter);
    bson_destroy(&up_spec);
    if (rc != MONGO_WIRE_OK || reply_ok(&reply) < 1.0) {
        error("update: rc=%d ok=%.0f", rc, reply_ok(&reply));
        bson_destroy(&reply);
        return rc != MONGO_WIRE_OK ? rc : MONGO_WIRE_ERR_PROTOCOL;
    }
    bson_destroy(&reply);
    info("update ok");

    /* --- delete (cleanup) --- */
    bson_t del_filter;
    bson_init(&del_filter);
    BSON_APPEND_INT64(&del_filter, "pico_tag", tag);

    rc = mongo_delete_one(t, TEST_DB, TEST_COLL, &del_filter, &reply, CMD_TIMEOUT_MS);
    bson_destroy(&del_filter);
    if (rc != MONGO_WIRE_OK || reply_ok(&reply) < 1.0) {
        error("delete: rc=%d ok=%.0f", rc, reply_ok(&reply));
        bson_destroy(&reply);
        return rc != MONGO_WIRE_OK ? rc : MONGO_WIRE_ERR_PROTOCOL;
    }
    bson_destroy(&reply);
    info("delete ok");

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

    char host[128];
    uint16_t port = 0;
    if (!parse_mongo_uri(MONGO_URI, host, sizeof host, &port)) {
        error("could not parse MONGO_URI: %s", MONGO_URI);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    info("mongo target host=%s port=%u", host, port);

    mongo_transport_t *t = mongo_transport_new();
    if (!t) {
        error("mongo_transport_new failed");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    for (;;) {
        info("opening tcp connection ...");
        rc = mongo_transport_connect(t, host, port, 5000);
        if (rc != MONGO_TRANSPORT_OK) {
            error("connect failed: rc=%d (lwip err=%d)", rc, mongo_transport_last_err(t));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        info("connected, running smoke test");
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
    info("=== micro-mongodb boot ===");

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
