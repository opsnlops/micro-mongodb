#pragma once

/* FreeRTOS task that periodically queries the telemetry collection and
 * prints aggregate stats (avg / min / max / count) for the most recent
 * samples. Shares a mongo_client_t with the telemetry task to demonstrate
 * that the client is safe to use from multiple tasks concurrently. */
void aggregator_task(void *arg);
