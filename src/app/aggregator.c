/*
 * aggregator.c — periodically asks Atlas for stats over the last N
 * telemetry samples and logs them. Demonstrates two things:
 *
 *   1. The mongo_client_t is shared with the telemetry task. Both can call
 *      into it concurrently; the client's internal mutex serializes.
 *   2. The Pico runs real aggregation pipelines on Atlas, not just CRUD.
 *      The $sort/$limit/$group stages here are evaluated server-side --
 *      we just print the result.
 */

#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include <bson/bson.h>

#include "logging.h"
#include "micro_mongodb.h"

#include "aggregator.h"

#define AGG_DB "micro_mongodb"
#define AGG_COLL "telemetry"
#define AGG_WINDOW 10         /* most-recent N samples to average */
#define AGG_INTERVAL_MS 19000 /* run every 19 s -- different from telemetry's 10 s */

/* Build:
 *   {aggregate: "telemetry",
 *    pipeline: [
 *      {$sort: {ts: -1}},
 *      {$limit: N},
 *      {$group: {_id: null, avg: {$avg: "$value"},
 *                n:   {$sum: 1},
 *                min: {$min: "$value"},
 *                max: {$max: "$value"}}}
 *    ],
 *    cursor: {}} */
static void build_aggregate_cmd(bson_t *cmd, int32_t window) {
    bson_init(cmd);
    bson_append_utf8(cmd, "aggregate", 9, AGG_COLL, -1);

    bson_t pipeline;
    bson_append_array_begin(cmd, "pipeline", 8, &pipeline);

    /* stage 0: $sort */
    bson_t stage0;
    bson_append_document_begin(&pipeline, "0", 1, &stage0);
    bson_t sort_doc;
    bson_append_document_begin(&stage0, "$sort", 5, &sort_doc);
    bson_append_int32(&sort_doc, "ts", 2, -1);
    bson_append_document_end(&stage0, &sort_doc);
    bson_append_document_end(&pipeline, &stage0);

    /* stage 1: $limit */
    bson_t stage1;
    bson_append_document_begin(&pipeline, "1", 1, &stage1);
    bson_append_int32(&stage1, "$limit", 6, window);
    bson_append_document_end(&pipeline, &stage1);

    /* stage 2: $group */
    bson_t stage2;
    bson_append_document_begin(&pipeline, "2", 1, &stage2);
    bson_t group;
    bson_append_document_begin(&stage2, "$group", 6, &group);
    bson_append_null(&group, "_id", 3);

    bson_t avg_doc;
    bson_append_document_begin(&group, "avg", 3, &avg_doc);
    bson_append_utf8(&avg_doc, "$avg", 4, "$value", -1);
    bson_append_document_end(&group, &avg_doc);

    bson_t n_doc;
    bson_append_document_begin(&group, "n", 1, &n_doc);
    bson_append_int32(&n_doc, "$sum", 4, 1);
    bson_append_document_end(&group, &n_doc);

    bson_t min_doc;
    bson_append_document_begin(&group, "min", 3, &min_doc);
    bson_append_utf8(&min_doc, "$min", 4, "$value", -1);
    bson_append_document_end(&group, &min_doc);

    bson_t max_doc;
    bson_append_document_begin(&group, "max", 3, &max_doc);
    bson_append_utf8(&max_doc, "$max", 4, "$value", -1);
    bson_append_document_end(&group, &max_doc);

    bson_append_document_end(&stage2, &group);
    bson_append_document_end(&pipeline, &stage2);

    bson_append_array_end(cmd, &pipeline);

    /* cursor: {} -- required for aggregate even when batchSize is omitted. */
    bson_t cursor_doc;
    bson_append_document_begin(cmd, "cursor", 6, &cursor_doc);
    bson_append_document_end(cmd, &cursor_doc);
}

/* Pull cursor.firstBatch[0] from an aggregate reply and read out the
 * stats fields. Returns true only when at least the `n` field was present
 * -- without n the pipeline didn't produce a row, and the other stats
 * (avg/min/max) would be left at their input defaults, which would look
 * like "everything is 0" rather than "no data." */
static bool parse_aggregate_result(const bson_t *reply, double *avg, int32_t *n, int32_t *min_v, int32_t *max_v) {
    bson_iter_t it;
    if (!bson_iter_init_find(&it, reply, "cursor") || !BSON_ITER_HOLDS_DOCUMENT(&it)) {
        return false;
    }
    bson_iter_t cursor_it;
    if (!bson_iter_recurse(&it, &cursor_it)) {
        return false;
    }
    /* bson_iter_init_find restarts at the document's first key, which is
     * what we want -- the previous hand-rolled loop happened to be O(1)
     * because firstBatch is the second key, but a server reply with extra
     * fields would have left it pointing at the wrong key. */
    bson_iter_t batch_it;
    if (!bson_iter_find(&cursor_it, "firstBatch") || !BSON_ITER_HOLDS_ARRAY(&cursor_it)) {
        return false;
    }
    batch_it = cursor_it;
    bson_iter_t arr_it;
    if (!bson_iter_recurse(&batch_it, &arr_it) || !bson_iter_next(&arr_it) || !BSON_ITER_HOLDS_DOCUMENT(&arr_it)) {
        return false;
    }

    uint32_t doc_len = 0;
    const uint8_t *doc_data = NULL;
    bson_iter_document(&arr_it, &doc_len, &doc_data);
    bson_t doc;
    if (!bson_init_static(&doc, doc_data, doc_len)) {
        return false;
    }

    bson_iter_t r_it;
    /* n is mandatory; if it's missing the result row is unusable. */
    if (!bson_iter_init_find(&r_it, &doc, "n") || !BSON_ITER_HOLDS_NUMBER(&r_it)) {
        return false;
    }
    *n = (int32_t)bson_iter_as_int64(&r_it);

    if (bson_iter_init_find(&r_it, &doc, "avg") && BSON_ITER_HOLDS_NUMBER(&r_it)) {
        *avg = bson_iter_as_double(&r_it);
    }
    if (bson_iter_init_find(&r_it, &doc, "min") && BSON_ITER_HOLDS_NUMBER(&r_it)) {
        *min_v = (int32_t)bson_iter_as_int64(&r_it);
    }
    if (bson_iter_init_find(&r_it, &doc, "max") && BSON_ITER_HOLDS_NUMBER(&r_it)) {
        *max_v = (int32_t)bson_iter_as_int64(&r_it);
    }
    return true;
}

void aggregator_task(void *arg) {
    mongo_client_t *c = (mongo_client_t *)arg;
    if (!c) {
        error("[agg] no client");
        vTaskDelete(NULL);
        return;
    }

    /* Stagger the first run so we don't race the telemetry task's first
     * insert when the device just booted. */
    vTaskDelay(pdMS_TO_TICKS(AGG_INTERVAL_MS));

    for (;;) {
        bson_t cmd;
        build_aggregate_cmd(&cmd, AGG_WINDOW);

        bson_t reply;
        int rc = mongo_client_run_command(c, AGG_DB, &cmd, &reply, 5000);
        bson_destroy(&cmd);

        if (rc != 0) {
            warning("[agg] aggregate failed: rc=%d", rc);
            vTaskDelay(pdMS_TO_TICKS(AGG_INTERVAL_MS));
            continue;
        }

        double avg = 0.0;
        int32_t n = 0, min_v = 0, max_v = 0;
        if (parse_aggregate_result(&reply, &avg, &n, &min_v, &max_v) && n > 0) {
            info("[agg] last %d samples: avg=%.1f min=%d max=%d", (int)n, avg, (int)min_v, (int)max_v);
        } else {
            info("[agg] no samples yet");
        }
        bson_destroy(&reply);

        vTaskDelay(pdMS_TO_TICKS(AGG_INTERVAL_MS));
    }
}
