#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include <FreeRTOS.h>
#include <queue.h>

#include "logging.h"
#include "logging_api.h"

#include "logging_config.h"
#include "types.h"

TaskHandle_t log_queue_reader_task_handle;
QueueHandle_t creature_log_message_queue_handle;
bool volatile logging_queue_exists = false;

// What level of logging we want (this is overridden from the EEPROM if it exists)
u8 configured_logging_level = DEFAULT_LOGGING_LEVEL;

void logger_init() {
    creature_log_message_queue_handle = xQueueCreate(LOGGING_QUEUE_LENGTH, sizeof(struct LogMessage));
    vQueueAddToRegistry(creature_log_message_queue_handle, "log_message_queue");
    logging_queue_exists = true;
    start_log_reader();
}

bool inline is_safe_to_log() {
    return (logging_queue_exists && !xQueueIsQueueFullFromISR(creature_log_message_queue_handle));
}

/**
 * @brief Internal logging function that handles the common logic for all log levels
 *
 * @param level The logging level
 * @param message The format string
 * @param args Variable arguments for the format string
 */
static void log_internal(uint8_t level, const char *message, va_list args) {
    // If not at the right logging level or queue is full, stop now
    if (configured_logging_level < level || !is_safe_to_log())
        return;

    struct LogMessage lm = createMessageObject(level, message, args);
    xQueueSendToBackFromISR(creature_log_message_queue_handle, &lm, NULL);
}

void __unused verbose(const char *message, ...) {
    va_list args;
    va_start(args, message);
    log_internal(LOG_LEVEL_VERBOSE, message, args);
    va_end(args);
}

void debug(const char *message, ...) {
    va_list args;
    va_start(args, message);
    log_internal(LOG_LEVEL_DEBUG, message, args);
    va_end(args);
}

void info(const char *message, ...) {
    va_list args;
    va_start(args, message);
    log_internal(LOG_LEVEL_INFO, message, args);
    va_end(args);
}

void warning(const char *message, ...) {
    va_list args;
    va_start(args, message);
    log_internal(LOG_LEVEL_WARNING, message, args);
    va_end(args);
}

void error(const char *message, ...) {
    va_list args;
    va_start(args, message);
    log_internal(LOG_LEVEL_ERROR, message, args);
    va_end(args);
}

void __unused fatal(const char *message, ...) {
    va_list args;
    va_start(args, message);
    log_internal(LOG_LEVEL_FATAL, message, args);
    va_end(args);
}

struct LogMessage createMessageObject(uint8_t level, const char *message, va_list args) {
    char buffer[LOGGING_MESSAGE_MAX_LENGTH + 1];
    memset(buffer, '\0', LOGGING_MESSAGE_MAX_LENGTH + 1);

    vsnprintf(buffer, LOGGING_MESSAGE_MAX_LENGTH, message, args);

    struct LogMessage lm;
    lm.level = level;
    memcpy(lm.message, buffer, LOGGING_MESSAGE_MAX_LENGTH);
    return lm;
}

void start_log_reader() {
    xTaskCreate(log_queue_reader_task, "log_queue_reader_task", 1512, NULL, 1, &log_queue_reader_task_handle);
}

char *log_level_to_string(u8 level) {
    switch (level) {
    case LOG_LEVEL_VERBOSE:
        return "Verbose";
    case LOG_LEVEL_DEBUG:
        return "Debug";
    case LOG_LEVEL_INFO:
        return "Info";
    case LOG_LEVEL_WARNING:
        return "Warning";
    case LOG_LEVEL_ERROR:
        return "Error";
    case LOG_LEVEL_FATAL:
        return "Fatal";
    default:
        return "Unknown";
    }
}

/**
 * @brief Creates a task that polls the logging queue
 *
 * It then spits things to the Serial port, and optionally to syslog so that a
 * Linux host can handle the heavy lifting.
 */
#pragma clang diagnostic push
#pragma ide diagnostic ignored "EndlessLoop"

portTASK_FUNCTION(log_queue_reader_task, pvParameters) {

    struct LogMessage lm;
    char levelBuffer[4];
    memset(&levelBuffer, '\0', 4);

    for (EVER) {
        if (xQueueReceive(creature_log_message_queue_handle, &lm, (TickType_t)portMAX_DELAY) == pdPASS) {
            switch (lm.level) {
            case LOG_LEVEL_VERBOSE:
                strncpy(levelBuffer, "[V] ", 3);
                break;
            case LOG_LEVEL_DEBUG:
                strncpy(levelBuffer, "[D] ", 3);
                break;
            case LOG_LEVEL_INFO:
                strncpy(levelBuffer, "[I] ", 3);
                break;
            case LOG_LEVEL_WARNING:
                strncpy(levelBuffer, "[W] ", 3);
                break;
            case LOG_LEVEL_ERROR:
                strncpy(levelBuffer, "[E] ", 3);
                break;
            case LOG_LEVEL_FATAL:
                strncpy(levelBuffer, "[F] ", 3);
                break;
            default:
                strncpy(levelBuffer, "[?] ", 3);
            }

            // Format our messaging
            u32 time = to_ms_since_boot(get_absolute_time());

            // Create space on the heap for the message
            char *message = (char *)pvPortMalloc(strlen(lm.message) + 33);
            memset(message, '\0', strlen(lm.message) + 33);
            snprintf(message, strlen(lm.message) + 32, "LOG\t%lu\t%s\t%s", time, levelBuffer, lm.message);

            // Allow the running application to hook in
            acw_post_logging_hook(message, strlen(message));

            vPortFree(message);

            // Wipe the buffer for next time
            memset(&levelBuffer, '\0', 4);
        }
    }
}
#pragma clang diagnostic pop