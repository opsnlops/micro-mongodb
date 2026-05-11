#include "mongo_time.h"

#include <time.h>

#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "lwip/apps/sntp.h"

#include "logging.h"

#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif

static volatile bool g_time_synced = false;
/* wall_clock_ms = to_ms_since_boot() + boot_offset_ms */
static volatile int64_t g_boot_offset_ms = 0;
static SemaphoreHandle_t g_sync_sem = NULL;

void mongo_time_set_unix_seconds(unsigned int sec) {
    int64_t now_boot_ms = (int64_t)to_ms_since_boot(get_absolute_time());
    g_boot_offset_ms = (int64_t)sec * 1000 - now_boot_ms;
    g_time_synced = true;
    info("[time] SNTP synced: unix=%u s, boot=%lld ms, offset=%lld ms", sec, (long long)now_boot_ms,
         (long long)g_boot_offset_ms);
    if (g_sync_sem) {
        xSemaphoreGive(g_sync_sem);
    }
}

int mongo_time_sync(uint32_t timeout_ms) {
    if (g_time_synced) {
        return 0;
    }
    if (!g_sync_sem) {
        g_sync_sem = xSemaphoreCreateBinary();
        if (!g_sync_sem) {
            return -1;
        }
    }

    info("[time] starting SNTP against %s ...", NTP_SERVER);
    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, NTP_SERVER);
    sntp_init();
    cyw43_arch_lwip_end();

    if (xSemaphoreTake(g_sync_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        error("[time] SNTP sync timeout after %lu ms (no reply from %s)", (unsigned long)timeout_ms, NTP_SERVER);
        return -2;
    }
    return 0;
}

int64_t mongo_time_now_ms(void) {
    if (!g_time_synced) {
        return 0;
    }
    return (int64_t)to_ms_since_boot(get_absolute_time()) + g_boot_offset_ms;
}

bool mongo_time_is_synced(void) { return g_time_synced; }

size_t mongo_time_format_iso8601(char *out, size_t sz) {
    if (!out || sz == 0) {
        return 0;
    }
    if (!g_time_synced) {
        out[0] = '\0';
        return 0;
    }
    time_t now_sec = (time_t)(mongo_time_now_ms() / 1000);
    struct tm tm;
    gmtime_r(&now_sec, &tm);
    return strftime(out, sz, "%Y-%m-%dT%H:%M:%SZ", &tm);
}
