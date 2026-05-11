/*
 * telemetry.c — sample task that periodically inserts a record into MongoDB.
 *
 * Right now it records a pseudo-random value (intended as a placeholder for
 * the real demo, which will read a temperature off an I2C sensor like an
 * MCP9808 / SHT3x / etc.). The shape of the insert document is the same
 * either way; swapping in the sensor read is a one-line change.
 */

#include <stdint.h>

#include "pico/rand.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "FreeRTOS.h"
#include "task.h"

#include <bson/bson.h>

#include "logging.h"
#include "micro_mongodb.h"

#include "telemetry.h"

#define TELEMETRY_DB "micro_mongodb"
#define TELEMETRY_COLL "telemetry"
#define TELEMETRY_INTERVAL_MS 10000

#if defined(PICO_PLATFORM_IS_RP2350) && PICO_PLATFORM_IS_RP2350
#define TELEMETRY_BOARD "pico2_w"
#else
#define TELEMETRY_BOARD "pico_w"
#endif

void telemetry_task(void *arg) {
    mongo_client_t *c = (mongo_client_t *)arg;
    if (!c) {
        error("[telemetry] no client passed to task");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        /* TODO: replace with an I2C sensor read once the hardware is wired up. */
        int32_t sample = (int32_t)(get_rand_32() & 0x3ff); /* 0..1023 */
        int64_t ts_ms = (int64_t)to_ms_since_boot(get_absolute_time());

        bson_t doc;
        bson_init(&doc);
        BSON_APPEND_INT32(&doc, "value", sample);
        BSON_APPEND_INT64(&doc, "ts_ms", ts_ms);
        BSON_APPEND_UTF8(&doc, "board", TELEMETRY_BOARD);

        bson_t reply;
        int rc = mongo_client_insert(c, TELEMETRY_DB, TELEMETRY_COLL, &doc, &reply, 5000);
        bson_destroy(&doc);
        if (rc == 0) {
            info("[telemetry] sample=%d ts_ms=%lld", (int)sample, (long long)ts_ms);
            bson_destroy(&reply);
        } else {
            warning("[telemetry] insert failed: rc=%d", rc);
        }

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
    }
}
