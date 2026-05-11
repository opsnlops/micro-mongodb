#pragma once

/* Logger sizing + defaults for micro-mongodb. Included from logging.h, so we
 * can't reach back into the LOG_LEVEL_* macros (they're defined a few lines
 * later in logging.h). LOG_LEVEL_DEBUG happens to be 4 -- keep them in sync
 * if either is renumbered. */

#ifndef DEFAULT_LOGGING_LEVEL
#define DEFAULT_LOGGING_LEVEL 4 /* LOG_LEVEL_DEBUG */
#endif

#ifndef LOGGING_QUEUE_LENGTH
#define LOGGING_QUEUE_LENGTH 100
#endif

#ifndef LOGGING_MESSAGE_MAX_LENGTH
#define LOGGING_MESSAGE_MAX_LENGTH 256
#endif

#ifndef LOGGING_LOG_VIA_PRINTF
#define LOGGING_LOG_VIA_PRINTF 1 /* mirror to stdout for USB-CDC console */
#endif
