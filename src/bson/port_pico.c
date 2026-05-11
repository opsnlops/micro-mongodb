/*
 * port_pico.c — micro-mongodb port shims for vendored libbson on the RP2350.
 *
 * Provides the few POSIX-shaped symbols libbson reaches for that newlib-nano
 * does not supply. Keep this file small and obvious -- if it grows, that's
 * usually a sign upstream is dragging in something we should be stubbing
 * inside the patched libbson source instead.
 */

#include "FreeRTOS.h"
#include "task.h"

/* libbson calls sched_yield() from the atomic-emulation fallback path
 * (bson-atomic.c::bson_thrd_yield, only used when the compiler lacks atomic
 * intrinsics for the integer width). On GCC + Cortex-M33 we should never
 * actually hit it, but the linker still resolves the symbol. */
int sched_yield(void) {
    taskYIELD();
    return 0;
}

/* libbson seeds the bson_context_t (which generates ObjectIDs) with a hash
 * of the hostname. We don't have a real hostname; "pico" is fine because the
 * context already mixes in a per-process counter, time, and PID. If you want
 * OIDs to differ across multiple boards on the same network, override this
 * to bake in pico_get_unique_board_id() bytes. */
#include <string.h>
int gethostname(char *name, size_t len) {
    static const char hostname[] = "pico";
    if (!name || len == 0) {
        return -1;
    }
    size_t n = sizeof hostname - 1;
    if (n >= len) {
        n = len - 1;
    }
    memcpy(name, hostname, n);
    name[n] = '\0';
    return 0;
}
