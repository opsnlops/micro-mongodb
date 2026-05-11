#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "logging_config.h"
#include "types.h"

/**
 * @file logging.h
 * @brief Logging system for embedded applications
 *
 * This module provides a thread-safe, queue-based logging system with
 * multiple severity levels. Log messages are placed in a queue and processed
 * by a dedicated task to avoid blocking the calling code.
 */

/**
 * @brief Logging level definitions
 *
 * Higher values indicate more verbose logging.
 * The system will only log messages with a level less than or equal to
 * the configured_logging_level setting.
 */
#define LOG_LEVEL_VERBOSE 5 /**< Verbose debug information */
#define LOG_LEVEL_DEBUG 4   /**< Debug information */
#define LOG_LEVEL_INFO 3    /**< Informational messages */
#define LOG_LEVEL_WARNING 2 /**< Warning conditions */
#define LOG_LEVEL_ERROR 1   /**< Error conditions */
#define LOG_LEVEL_FATAL 0   /**< Critical failures */

/**
 * @brief Structure to hold a log message in the queue
 */
struct LogMessage {
    u8 level;                                 /**< Logging level of the message */
    char message[LOGGING_MESSAGE_MAX_LENGTH]; /**< Message content */
} __attribute__((packed));

/**
 * @brief Initialize the logging system
 *
 * Creates the message queue and starts the log reader task.
 * Must be called before any logging functions.
 */
void logger_init();

/**
 * @brief Log a message at VERBOSE level
 *
 * Used for detailed diagnostic information. These messages are typically
 * disabled in production builds.
 *
 * @param message Format string (printf style)
 * @param ... Variable arguments for format string
 */
void __unused verbose(const char *message, ...);

/**
 * @brief Log a message at DEBUG level
 *
 * Used for development and debugging information.
 *
 * @param message Format string (printf style)
 * @param ... Variable arguments for format string
 */
void debug(const char *message, ...);

/**
 * @brief Log a message at INFO level
 *
 * Used for general informational messages about system operation.
 *
 * @param message Format string (printf style)
 * @param ... Variable arguments for format string
 */
void info(const char *message, ...);

/**
 * @brief Log a message at WARNING level
 *
 * Used for potentially problematic conditions that don't affect functionality.
 *
 * @param message Format string (printf style)
 * @param ... Variable arguments for format string
 */
void warning(const char *message, ...);

/**
 * @brief Log a message at ERROR level
 *
 * Used for error conditions that affect functionality but don't cause system failure.
 *
 * @param message Format string (printf style)
 * @param ... Variable arguments for format string
 */
void error(const char *message, ...);

/**
 * @brief Log a message at FATAL level
 *
 * Used for critical errors that may cause system failure.
 * These are always logged regardless of configured log level.
 *
 * @param message Format string (printf style)
 * @param ... Variable arguments for format string
 */
void __unused fatal(const char *message, ...);

/**
 * @brief Create a log message object from format string and arguments
 *
 * Internal function used by logging macros.
 *
 * @param level Log level of the message
 * @param message Format string
 * @param args Variable argument list
 * @return LogMessage structure containing the formatted message
 */
struct LogMessage createMessageObject(uint8_t level, const char *message, va_list args);

/**
 * @brief Check if it's safe to log a message
 *
 * Verifies that the logging queue exists and is not full.
 *
 * @return true if logging is safe, false otherwise
 */
bool is_safe_to_log(void);

/**
 * @brief Start the log reader task
 *
 * Creates the FreeRTOS task that processes log messages from the queue.
 * Called automatically by logger_init().
 */
void start_log_reader();

/**
 * @brief FreeRTOS task that processes log messages
 *
 * Continuously reads from the log queue and processes messages.
 *
 * @param pvParameters Task parameters (unused)
 */
portTASK_FUNCTION_PROTO(log_queue_reader_task, pvParameters);

char *log_level_to_string(u8 level);