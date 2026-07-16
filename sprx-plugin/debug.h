/**
 * debug.h
 * Debug logging utilities for the PS3 .sprx plugin
 *
 * Provides debug output macros that can be compiled out for release builds.
 * On the PS3, debug output goes to:
 *   1. /dev_hdd0/plugins/ldtoypad_debug.log (HDD via sysFs calls)
 *   2. Remote UDP log stream (to bridge server port 28473)
 *
 * Debug levels:
 *   DEBUG_LEVEL_NONE     - No debug output
 *   DEBUG_LEVEL_ERROR    - Only errors
 *   DEBUG_LEVEL_INFO     - Normal info + errors
 *   DEBUG_LEVEL_VERBOSE  - High verbosity
 *   DEBUG_LEVEL_ALL      - Everything (packet dumps included)
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <ppu-types.h>

// =========================================
// Debug level configuration
// =========================================

// Change this for release builds: #define DEBUG_LEVEL DEBUG_LEVEL_ERROR
#define DEBUG_LEVEL DEBUG_LEVEL_INFO

#define DEBUG_LEVEL_NONE    0
#define DEBUG_LEVEL_ERROR   1
#define DEBUG_LEVEL_INFO    2
#define DEBUG_LEVEL_VERBOSE 3
#define DEBUG_LEVEL_ALL     4

// =========================================
// Debug output macros
// =========================================

#if DEBUG_LEVEL >= DEBUG_LEVEL_ERROR
#define DEBUG_ERROR(fmt, ...) \
    debug_printf("[LDTP_ERROR] " fmt, ##__VA_ARGS__)
#else
#define DEBUG_ERROR(fmt, ...)
#endif

#if DEBUG_LEVEL >= DEBUG_LEVEL_INFO
#define DEBUG_PRINT(fmt, ...) \
    debug_printf("[LDTP] " fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

#if DEBUG_LEVEL >= DEBUG_LEVEL_VERBOSE
#define DEBUG_VERBOSE(fmt, ...) \
    debug_printf("[LDTP_VERB] " fmt, ##__VA_ARGS__)
#else
#define DEBUG_VERBOSE(fmt, ...)
#endif

#if DEBUG_LEVEL >= DEBUG_LEVEL_VERBOSE
#define DEBUG_VERY_VERBOSE(fmt, ...) \
    debug_printf("[LDTP_TRACE] " fmt, ##__VA_ARGS__)
#else
#define DEBUG_VERY_VERBOSE(fmt, ...)
#endif

#if DEBUG_LEVEL >= DEBUG_LEVEL_ALL
#define DEBUG_PACKET(fmt, ...) \
    debug_printf("[LDTP_PACKET] " fmt, ##__VA_ARGS__)
#define DEBUG_HEX_DUMP(label, data, len) \
    debug_hex_dump(label, data, len)
#else
#define DEBUG_PACKET(fmt, ...)
#define DEBUG_HEX_DUMP(label, data, len)
#endif

// =========================================
// Debug functions
// =========================================

/**
 * Initialize the debug system
 * Opens log file and sets up output buffering
 */
void debug_init(void);

/**
 * Shutdown the debug system
 * Flushes and closes log file
 */
void debug_shutdown(void);

/**
 * Print a formatted debug string
 * Output goes to log file and/or TTY
 *
 * @param fmt printf-style format string
 * @param ... Variable arguments
 */
void debug_printf(const char* fmt, ...);

/**
 * Configure remote UDP log target.
 * Passing ip=0 or port=0 disables remote log streaming.
 *
 * @param ip IPv4 address in network byte order
 * @param port UDP port in host byte order
 */
void debug_set_remote(uint32_t ip, uint16_t port);

/**
 * Dump a buffer as hex for debugging
 *
 * @param label Text label for the dump
 * @param data Data buffer to dump
 * @param len Length of data
 */
void debug_hex_dump(const char* label, const uint8_t* data, int len);

#endif // DEBUG_H
