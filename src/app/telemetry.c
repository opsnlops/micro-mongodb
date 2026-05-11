/*
 * telemetry.c — sample task that periodically inserts a record into a
 * MongoDB time-series collection.
 *
 * Right now it records a pseudo-random value (placeholder for an I2C temp
 * sensor like MCP9808 / SHT3x / etc.). Swapping in the real sensor is
 * one line inside the loop; the document shape and time-series setup
 * stay the same.
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

    /* Idempotent server-side setup. If the collection already exists as
     * time-series this is a no-op; if it doesn't exist, the server creates
     * it with the right schema so future inserts land as time-series buckets
     * instead of regular documents. */
    mongo_client_ensure_timeseries(c, TELEMETRY_DB, TELEMETRY_COLL,
                                   &(mongo_timeseries_opts_t){
                                       .time_field = "ts",
                                       .meta_field = "board",
                                       .granularity = "seconds",
                                   },
                                   5000);

    for (;;) {
        if (!mongo_time_is_synced()) {
            warning("[telemetry] skipping insert -- wall clock not synced yet");
            vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
            continue;
        }

        /* TODO: replace with an I2C sensor read once the hardware is wired up. */
        int32_t sample = (int32_t)(get_rand_32() & 0x3ff); /* 0..1023 */
        int64_t ts_ms = mongo_time_now_ms();

        bson_t doc;
        bson_init(&doc);
        BSON_APPEND_DATE_TIME(&doc, "ts", ts_ms);
        BSON_APPEND_UTF8(&doc, "board", TELEMETRY_BOARD);
        BSON_APPEND_INT32(&doc, "value", sample);

        bson_t reply;
        int rc = mongo_client_insert(c, TELEMETRY_DB, TELEMETRY_COLL, &doc, &reply, 5000);
        bson_destroy(&doc);
        if (rc == 0) {
            char ts_iso[32];
            mongo_time_format_iso8601(ts_iso, sizeof ts_iso);
            info("[telemetry] sample=%d ts=%s", (int)sample, ts_iso);
            bson_destroy(&reply);
        } else {
            warning("[telemetry] insert failed: rc=%d", rc);
        }

        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));
    }
}
