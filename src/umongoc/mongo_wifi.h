/*
 * mongo_wifi — convenience helper to bring up the CYW43 WiFi chip and
 * associate with an AP. This is pico-sdk-specific glue; it just wraps the
 * cyw43_arch_* calls so app main() stays small.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the CYW43 driver in station mode and connect to `ssid` using
 * WPA2 PSK with `password`. Blocks up to `timeout_ms` for association +
 * DHCP. Returns 0 on success, non-zero on error (negative for our codes,
 * positive cyw43 errnos for the underlying failure). */
int mongo_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
