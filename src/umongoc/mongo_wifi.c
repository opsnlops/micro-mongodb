#include "mongo_wifi.h"

#include <string.h>

#include "pico/cyw43_arch.h"

#include "logging.h"

int mongo_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (!ssid || !ssid[0]) {
        error("[wifi] no SSID");
        return -1;
    }
    if (cyw43_arch_init()) {
        error("[wifi] cyw43_arch_init failed");
        return -2;
    }
    cyw43_arch_enable_sta_mode();

    info("[wifi] connecting to '%s' ...", ssid);
    int rc = cyw43_arch_wifi_connect_timeout_ms(ssid, password ? password : "", CYW43_AUTH_WPA2_AES_PSK, timeout_ms);
    if (rc) {
        error("[wifi] connect failed: rc=%d", rc);
        return rc;
    }
    info("[wifi] connected");
    return 0;
}
