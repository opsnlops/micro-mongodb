/*
 * mongo_time — SNTP-synced wall-clock time.
 *
 * lwIP's built-in SNTP module hands new times to us via the
 * `SNTP_SET_SYSTEM_TIME` macro (defined in lwipopts.h to call
 * `mongo_time_set_unix_seconds`). We translate that into a boot-tick offset
 * so the Pico's monotonic clock keeps running and adding the offset gives
 * Unix-epoch milliseconds.
 *
 * The NTP server is configured at compile time via -DNTP_SERVER="...";
 * defaults to "pool.ntp.org".
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kick off SNTP against the configured server and block until the first
 * time fix lands (or `timeout_ms` elapses). Must be called after WiFi is
 * up and DHCP has assigned a nameserver. Idempotent: if time has already
 * been synced, returns immediately. */
int mongo_time_sync(uint32_t timeout_ms);

/* Wall-clock milliseconds since the Unix epoch. Returns 0 if time has not
 * yet been synced (which is also a valid pre-1970 timestamp; check
 * mongo_time_is_synced() if you need to disambiguate). */
int64_t mongo_time_now_ms(void);

bool mongo_time_is_synced(void);

/* Wall-clock seconds since the Unix epoch, in time_t form. Used as the
 * MBEDTLS_PLATFORM_TIME_MACRO so X.509 certificate notBefore / notAfter
 * checks run against real wall time. Returns 0 (and sets *t to 0 when
 * non-NULL) before SNTP has synced. */
time_t mongo_time_seconds(time_t *t);

/* Format the current wall clock as ISO 8601 (e.g. "2026-05-11T23:33:35Z").
 * Writes up to `sz - 1` chars + NUL. Returns the number of bytes written,
 * or 0 if time hasn't been synced (in which case `out` is set to an empty
 * string). `sz` should be at least 21 bytes. */
size_t mongo_time_format_iso8601(char *out, size_t sz);

/* Called by lwIP SNTP via SNTP_SET_SYSTEM_TIME -- not meant for app code. */
void mongo_time_set_unix_seconds(unsigned int sec);

#ifdef __cplusplus
}
#endif
