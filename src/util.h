#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

/**
 * HDB_ASSERT: Fatal assertion with message.
 * 
 * DANGER: This will abort the process immediately without cleanup.
 * Use for conditions that indicate programming errors or unrecoverable states.
 * For recoverable errors, use explicit error handling instead.
 */
#define HDB_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FATAL] %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        fprintf(stderr, "[FATAL] Assertion failed: %s\n", #cond); \
        abort(); \
    } \
} while (0)

/**
 * HDB_CHECK: Non-fatal error check that returns error code.
 * 
 * Use for libdrm/ioctl errors that should propagate up the call stack.
 * The caller must handle the error appropriately.
 */
#define HDB_CHECK(cond, ret_val, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[ERROR] %s:%d: %s (errno=%d: %s)\n", \
                __FILE__, __LINE__, (msg), errno, strerror(errno)); \
        return (ret_val); \
    } \
} while (0)

/**
 * HDB_WARN: Non-fatal warning for suspicious but recoverable conditions.
 */
#define HDB_WARN(msg) do { \
    fprintf(stderr, "[WARN] %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
} while (0)

/**
 * HDB_INFO: Informational logging for debugging.
 */
#define HDB_INFO(msg) do { \
    fprintf(stdout, "[INFO] %s\n", (msg)); \
} while (0)

/**
 * for_range: Simple loop macro for [start, end) iteration.
 * 
 * Usage: for_range(i, 0, count) { ... }
 * Note: 'i' is declared as size_t within the loop scope.
 */
#define for_range(var, start, end) \
    for (size_t var = (start); var < (end); var++)

/**
 * ARRAY_SIZE: Get the number of elements in a static array.
 * 
 * WARNING: Do not use with pointers, only with actual arrays.
 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/**
 * MIN/MAX: Standard min/max macros with type safety.
 */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/**
 * ALIGN_UP: Align value up to the nearest multiple of alignment.
 * alignment must be a power of 2.
 */
#define ALIGN_UP(value, alignment) \
    (((value) + (alignment) - 1) & ~((alignment) - 1))

/**
 * PAGE_SIZE: Standard 4K page size for GPU operations.
 */
#define PAGE_SIZE 4096
