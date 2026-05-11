
#pragma once

#include <stddef.h>

#include <FreeRTOS.h>
#include <task.h>

#include "types.h"

/**
 * Called at the end of the logging process. Used to allow
 * for a hook to be called after logging has been completed.
 *
 * @param message the message that was logged
 * @param message_length the length of the message in bytes. Using size_t
 *        (was u8) so log lines longer than 255 bytes don't silently
 *        truncate the length passed to the hook.
 */
void acw_post_logging_hook(char *message, size_t message_length);
