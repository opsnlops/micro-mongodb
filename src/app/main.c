/* micro-mongodb bring-up entry point.
 *
 * Current scope:
 *   1. Init stdio (USB-CDC) and the queue-based logger.
 *   2. Bring up the CYW43 WiFi chip and join the configured network.
 *   3. Open a TCP connection to MONGO_URI, run a {ping:1}, log the reply.
 *   4. Heartbeat forever.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include <bson/bson.h>

#include "logging.h"
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

/* Minimal mongodb:// parser — proper URI handling (auth, options, +srv) lands
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

    /* Take the LAST ':' inside hostport to split host from port — robust against
     * IPv6 literals later (though those need brackets, not handled yet). */
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

static void run_ping(mongo_transport_t *t) {
    bson_t cmd;
    bson_init(&cmd);
    BSON_APPEND_INT32(&cmd, "ping", 1);

    bson_t reply;
    int rc = mongo_run_command(t, "admin", &cmd, &reply, 5000);
    bson_destroy(&cmd);

    if (rc != MONGO_WIRE_OK) {
        error("ping: mongo_run_command rc=%d", rc);
        return;
    }

    bson_iter_t it;
    if (bson_iter_init_find(&it, &reply, "ok") && BSON_ITER_HOLDS_NUMBER(&it)) {
        double ok = bson_iter_as_double(&it);
        info("ping reply ok=%.1f", ok);
    } else {
        warning("ping reply has no 'ok' field");
    }
    bson_destroy(&reply);
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
        info("connected, sending ping");
        run_ping(t);
        mongo_transport_close(t);
        info("closed; sleeping 10s");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

int main(void) {
    stdio_init_all();

    /* Give USB-CDC a moment to enumerate before any output. */
    sleep_ms(2000);

    /* Stand the logger up before anything else so all subsequent code can
     * use info()/debug()/error() without thinking about it. */
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
