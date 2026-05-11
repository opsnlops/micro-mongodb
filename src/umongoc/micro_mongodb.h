/*
 * micro_mongodb.h — umbrella header for end-user applications.
 *
 * Include this one file to use micro-mongodb. It pulls in libbson, the
 * mongo_client_t API, the cursor type, status codes, and the WiFi bring-up
 * helper. Lower-level layers (mongo_transport, mongo_wire, mongo_auth) are
 * still available individually for advanced uses but most apps only need
 * what's here.
 *
 * Typical app:
 *
 *     #include "micro_mongodb.h"
 *
 *     mongo_client_t *c = mongo_client_new(&(mongo_client_config_t){
 *         .mongo_uri = "mongodb+srv://...",
 *         .app_name  = "my-pico-thing",
 *     });
 *     mongo_client_insert(c, "db", "coll", &doc, &reply, 5000);
 */

#pragma once

#include <bson/bson.h>

#include "mongo_client.h"
#include "mongo_cursor.h"
#include "mongo_time.h"
#include "mongo_wifi.h"
#include "mongo_wire.h" /* for MONGO_WIRE_OK / MONGO_WIRE_ERR_* status codes */
