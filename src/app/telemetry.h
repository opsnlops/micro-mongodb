#pragma once

/* FreeRTOS task that periodically inserts a sample document. `arg` is a
 * mongo_client_t * that the task does NOT free (lifetime is the caller's). */
void telemetry_task(void *arg);
