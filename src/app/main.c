/*
 * main.c — entry point for the micro-mongodb demo firmware.
 *
 * Brings up logging + WiFi, constructs a mongo_client, hands it to the
 * telemetry task. All connection / auth / reconnect logic lives in
 * mongo_client; this file stays small.
 */

#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "logging.h"
#include "micro_mongodb.h"

#include "telemetry.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef MONGO_URI
#define MONGO_URI "mongodb://192.168.1.1:27017"
#endif

#if defined(PICO_PLATFORM_IS_RP2350) && PICO_PLATFORM_IS_RP2350
#define BOARD_NAME "pico2_w"
#else
#define BOARD_NAME "pico_w"
#endif

#define APP_TASK_STACK_WORDS 4096
#define APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

static void app_task(void *arg) {
    (void)arg;

    if (mongo_wifi_connect(WIFI_SSID, WIFI_PASSWORD, 30000) != 0) {
        error("[app] wifi bring-up failed");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    /* SNTP -- gets us real Unix-epoch time. Non-fatal if it fails (we still
     * have monotonic time for sequencing), but most telemetry use cases want
     * wall-clock timestamps so try hard. */
    if (mongo_time_sync(15000) != 0) {
        warning("[app] continuing without wall-clock time");
    } else {
        info("[app] wall clock: %lld ms", (long long)mongo_time_now_ms());
    }

    mongo_client_config_t cfg = {
        .mongo_uri = MONGO_URI,
        .app_name = "micro-mongodb-demo",
    };
    mongo_client_t *client = mongo_client_new(&cfg);
    if (!client) {
        error("[app] mongo_client_new failed");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    /* Hand the client to the telemetry task and let it own the demo loop.
     * Other sample tasks (a temp sensor, etc.) could go alongside it. */
    TaskHandle_t tel_handle = NULL;
    BaseType_t ok = xTaskCreate(telemetry_task, "tel", APP_TASK_STACK_WORDS, client, APP_TASK_PRIORITY, &tel_handle);
    if (ok != pdPASS) {
        panic("[app] failed to create telemetry task");
    }
    vTaskCoreAffinitySet(tel_handle, 1 << 0);

    /* Nothing else to do; this task can sleep forever. */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    logger_init();
    info("=== micro-mongodb boot (board=%s) ===", BOARD_NAME);

    TaskHandle_t app_handle = NULL;
    BaseType_t ok = xTaskCreate(app_task, "app", APP_TASK_STACK_WORDS, NULL, APP_TASK_PRIORITY, &app_handle);
    if (ok != pdPASS) {
        panic("[main] failed to create app task");
    }
    vTaskCoreAffinitySet(app_handle, 1 << 0);

    vTaskStartScheduler();

    for (;;) {
    }
}
