# Examples

Recipes for common things you'll want to do with `mongo_client_t`. Each
example assumes you've already got a client connected (the demo's
`main.c` shows the boilerplate).

## Insert a typed document

```c
bson_t doc;
bson_init(&doc);
BSON_APPEND_DATE_TIME(&doc, "ts", mongo_time_now_ms());
BSON_APPEND_UTF8     (&doc, "sensor", "indoor");
BSON_APPEND_DOUBLE   (&doc, "temp_c", 22.7);
BSON_APPEND_INT32    (&doc, "humidity_pct", 41);

bson_t reply;
int rc = mongo_client_insert(c, "demo", "readings", &doc, &reply, 5000);
bson_destroy(&doc);
if (rc == 0) {
    bson_destroy(&reply);
}
```

## Find with a filter, iterate

```c
bson_t filter;
bson_init(&filter);
BSON_APPEND_UTF8(&filter, "sensor", "indoor");

mongo_cursor_t *cur = NULL;
int rc = mongo_client_find(c, "demo", "readings", &filter, 50, &cur, 5000);
bson_destroy(&filter);
if (rc != 0) {
    error("find failed: %d", rc);
    return;
}

const bson_t *doc;
while (mongo_cursor_next(cur, &doc, 5000) == 1) {
    bson_iter_t it;
    if (bson_iter_init_find(&it, doc, "temp_c") && BSON_ITER_HOLDS_DOUBLE(&it)) {
        info("temp: %.1f", bson_iter_double(&it));
    }
}
mongo_cursor_destroy(cur);
```

> The `bson_t *doc` you get from `mongo_cursor_next` is **borrowed**.
> It's invalidated by the next call to `mongo_cursor_next` or
> `mongo_cursor_destroy`. If you need the data longer, `bson_copy_to`
> it into a `bson_t` you own.

## Update a field (atomic increment)

```c
bson_t filter, update;
bson_init(&filter);
BSON_APPEND_UTF8(&filter, "device_id", "pico-001");

bson_init(&update);
bson_t inc;
BSON_APPEND_DOCUMENT_BEGIN(&update, "$inc", &inc);
BSON_APPEND_INT32(&inc, "boot_count", 1);
bson_append_document_end(&update, &inc);

bson_t reply;
mongo_client_update(c, "demo", "devices", &filter, &update,
                    /* upsert = */ true, &reply, 5000);
bson_destroy(&filter);
bson_destroy(&update);
bson_destroy(&reply);
```

`upsert=true` means "insert if no matching document exists." Useful
for "increment this counter, creating the doc on first call."

## Run an aggregate

The aggregator task already does this; here's a simpler version that
just counts documents matching a filter:

```c
bson_t cmd;
bson_init(&cmd);
bson_append_utf8(&cmd, "aggregate", 9, "readings", -1);

bson_t pipeline;
bson_append_array_begin(&cmd, "pipeline", 8, &pipeline);

// {$match: {sensor: "indoor"}}
bson_t stage0;
bson_append_document_begin(&pipeline, "0", 1, &stage0);
bson_t match;
bson_append_document_begin(&stage0, "$match", 6, &match);
bson_append_utf8(&match, "sensor", 6, "indoor", -1);
bson_append_document_end(&stage0, &match);
bson_append_document_end(&pipeline, &stage0);

// {$count: "n"}
bson_t stage1;
bson_append_document_begin(&pipeline, "1", 1, &stage1);
bson_append_utf8(&stage1, "$count", 6, "n", -1);
bson_append_document_end(&pipeline, &stage1);

bson_append_array_end(&cmd, &pipeline);

bson_t cursor_doc;
bson_append_document_begin(&cmd, "cursor", 6, &cursor_doc);
bson_append_document_end(&cmd, &cursor_doc);

bson_t reply;
mongo_client_run_command(c, "demo", &cmd, &reply, 5000);
bson_destroy(&cmd);

// Parse reply.cursor.firstBatch[0].n
// See src/app/aggregator.c for the full parse-result helper.
bson_destroy(&reply);
```

## Replace random data with a real sensor

The telemetry task currently fills `value` with `get_rand_32() & 0x3ff`.
To replace with an I2C temp sensor (e.g. MCP9808 at address `0x18`):

```c
// In src/app/telemetry.c, add at the top:
#include "hardware/i2c.h"

// In the task initialization (before the for(;;) loop):
i2c_init(i2c0, 100 * 1000);
gpio_set_function(4, GPIO_FUNC_I2C);   // SDA
gpio_set_function(5, GPIO_FUNC_I2C);   // SCL
gpio_pull_up(4);
gpio_pull_up(5);

// Replace the `get_rand_32()` line in the loop with:
uint8_t reg = 0x05;                     // T_AMBIENT register
uint8_t raw[2];
i2c_write_blocking(i2c0, 0x18, &reg, 1, true);
i2c_read_blocking (i2c0, 0x18, raw, 2, false);
int32_t sample = ((int32_t)(raw[0] & 0x1f) << 4) | (raw[1] >> 4);  // 0.0625°C units
if (raw[0] & 0x10) sample -= 4096;       // sign extension for negatives
// sample is now in 0.0625°C units, multiply by 16 to get millidegrees etc.
```

The BSON shape stays the same; everything downstream (time-series
collection, aggregator, Atlas Charts) works without modification.

## Add a second task that consumes data

The demo's aggregator already shows this pattern. The key facts:

- One `mongo_client_t` can be shared across any number of FreeRTOS
  tasks. The internal mutex serializes network I/O.
- Spawn tasks with `xTaskCreate(..., (void *)client, ..., NULL)` and
  pin them to core 0 with `vTaskCoreAffinitySet(handle, 1 << 0)`.
- Keep the stacks generous (4096 words / 16 KB is reasonable). TLS
  reconnection can deepen call stacks meaningfully.

A minimal pattern:

```c
static void my_task(void *arg) {
    mongo_client_t *c = arg;
    for (;;) {
        bson_t cmd, reply;
        bson_init(&cmd);
        BSON_APPEND_INT32(&cmd, "ping", 1);
        if (mongo_client_run_command(c, "admin", &cmd, &reply, 5000) == 0) {
            bson_destroy(&reply);
        }
        bson_destroy(&cmd);
        vTaskDelay(pdMS_TO_TICKS(30000));   // every 30 s
    }
}

// In your app_task:
TaskHandle_t h;
xTaskCreate(my_task, "my", 4096, client, tskIDLE_PRIORITY + 1, &h);
vTaskCoreAffinitySet(h, 1 << 0);
```

## Use a different CA bundle (self-hosted mongod with your own CA)

If you're talking to a private mongod with TLS using your own CA
instead of a public one:

```c
extern const char my_ca_pem[];
extern const size_t my_ca_pem_len;

// Build the URI normally with tls=true:
mongo_client_config_t cfg = {
    .mongo_uri = "mongodb://user:pass@my-mongod.local:27017/?tls=true",
};

// ... but mongo_client_new doesn't currently take a custom CA, so:
// (TODO: expose this on mongo_client_config_t)
//
// For now, use mongo_transport directly:
mongo_tls_config_t tls = {
    .ca_pem = my_ca_pem,
    .ca_pem_len = my_ca_pem_len,
    .sni_hostname = "my-mongod.local",
};
mongo_transport_t *t = mongo_transport_new(&tls);
// ... and drive the lower layers manually.
```

This is a known sharp edge — the client should grow a `ca_pem` field
on its config. Patches welcome.

## Query the demo collection from your laptop

While the firmware is running, you can read from the same collection
with mongosh:

```bash
mongosh "mongodb+srv://your-user:your-pass@your-cluster.mongodb.net/" --eval '
  db = db.getSiblingDB("micro_mongodb");
  db.telemetry.find().sort({ts: -1}).limit(5).pretty()
'
```

Or with the MongoDB Python driver:

```python
from pymongo import MongoClient
client = MongoClient("mongodb+srv://...")
for doc in client.micro_mongodb.telemetry.find().sort("ts", -1).limit(10):
    print(doc["ts"], doc["board"], doc["value"])
```

The Pico's writes are visible everywhere a regular MongoDB driver can
reach. That's the whole point.
