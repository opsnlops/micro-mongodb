/*
 * logger_hook.c — application-provided sink for the queue-based logger
 * (src/logging/logging.c). The logger reader task formats each message with
 * a timestamp and level prefix, then calls this hook to actually emit it.
 *
 * For micro-mongodb the only sink right now is stdout (USB-CDC), gated by
 * LOGGING_LOG_VIA_PRINTF in logging_config.h. As the project grows, this is
 * the place to fan out to additional sinks (remote syslog, ring buffer
 * accessible to a host tool, etc.) without touching call sites.
 */

#include <stddef.h>
#include <stdio.h>

#include "logging_api.h"
#include "logging_config.h"
#include "types.h"

void acw_post_logging_hook(char *message, size_t message_length) {
    (void)message_length;

#if LOGGING_LOG_VIA_PRINTF
    printf("%s\n", message);
#else
    (void)message;
#endif
}
