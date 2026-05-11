/* micro-mongodb bring-up entry point.
 *
 * Current scope:
 *   1. Init stdio (USB-CDC) and the queue-based logger.
 *   2. Bring up the CYW43 WiFi chip and join the configured network.
 *   3. Heartbeat the assigned IP forever.
 *
 * Once umongoc's wire protocol lands (task 4), this task will also connect
 * to MONGO_URI and run a ping.
 */

#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "logging.h"

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
    info("target mongo uri: %s", MONGO_URI);

    /* TODO(task 4): connect to MONGO_URI and run {ping:1}. */
    for (;;) {
        const ip4_addr_t *ip = netif_ip4_addr(netif_default);
        if (ip && ip->addr != 0) {
            debug("heartbeat ip=%s", ip4addr_ntoa(ip));
        } else {
            debug("heartbeat (no ip yet)");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
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
