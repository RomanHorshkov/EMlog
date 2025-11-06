/**
 * @file emlog.h
 * @brief Tiny, thread-safe logging and canonical error categorization API.
 *
 * This header exposes a compact logging API with printf-like formatting,
 * optional ISO8601 timestamps, and a mapping layer from POSIX errno values
 * to a small set of canonical error categories. The implementation is
 * thread-safe and allows installing a custom writer callback.
 *
 * The API aims to be minimal and stable to allow building a small static
 * library that other projects can embed. All public symbols are declared
 * here and documented with Doxygen for easy generation of reference docs.
 *
 * License: MIT
 * Copyright: 2025 Roman Horshkov
 */

#ifndef EMLOG_H
#define EMLOG_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Logging levels used by the library.
 *
 * These are intentionally short (three-letter) and do not collide with
 * syslog names. Use these values when calling emlog_log() or when
 * adjusting the runtime minimum log level via emlog_set_level().
 */
typedef enum
{
    EML_LEVEL_DBG = 0, /**< Debug-level, very verbose. */
    EML_LEVEL_INFO,    /**< Informational messages. */
    EML_LEVEL_WARN,    /**< Warnings: non-fatal, degrade behavior. */
    EML_LEVEL_ERROR,   /**< Errors that should be investigated. */
    EML_LEVEL_CRIT     /**< Critical conditions, usually followed by exit. */
} eml_level_t;

/**
 * @brief Canonical error categories used to map errno values to a small set
 * of high-level outcomes.
 *
 * These categories are portable across platforms and can be converted to
 * exit codes with eml_err_to_exit() or mapped back to their string name
 * with eml_err_name().
 */
typedef enum
{
    EML_OK = 0,           /**< No error. */
    EML_TRY_AGAIN,        /**< Try again / interrupted. */
    EML_TEMP_RESOURCE,    /**< Temporarily out of resources (memory/files). */
    EML_TEMP_UNAVAILABLE, /**< Temporary service or network unavailability. */
    EML_BAD_INPUT,        /**< Invalid input or protocol error. */
    EML_NOT_FOUND,        /**< Item not found. */
    EML_PERM,             /**< Permission denied. */
    EML_CONFLICT,         /**< Conflicting resource / already exists. */
    EML_FATAL_CONF,       /**< Fatal configuration error. */
    EML_FATAL_IO,         /**< Fatal I/O error. */
    EML_FATAL_CRYPTO,     /**< Fatal cryptographic error. */
    EML_FATAL_BUG,        /**< Internal bug / unexpected state. */
    EML__COUNT            /**< Internal sentinel (do not use). */
} eml_err_t;

/**
 * @name Exit codes
 * These map a subset of canonical errors to common exit codes used by
 * programs (helps CLI utilities). They are simple integers and may be
 * returned by eml_err_to_exit().
 */
/*@{*/
enum
{
    EML_EXIT_OK   = 0, /**< Success */
    EML_EXIT_CONF = 2, /**< Configuration error */
    EML_EXIT_IO   = 3, /**< I/O error */
    EML_EXIT_MEM  = 4, /**< Out of memory / resource */
    EML_EXIT_BUG  = 5, /**< Internal bug */
};
/*@}*/

/**
 * @brief Optional writer callback used to customize output destination.
 *
 * If a writer is installed with emlog_set_writer(), the logger will call
 * this function for each formatted line. The implementation should return
 * the number of bytes written on success or a negative value on failure.
 *
 * @param lvl Log level for the line.
 * @param line Pointer to a NUL-terminated string (not including trailing \n).
 * @param n Number of bytes in @p line (excluding trailing \0).
 * @param user User-provided context pointer passed to emlog_set_writer().
 * @return ssize_t Number of bytes written or negative on error.
 */
typedef ssize_t (*eml_writer_fn)(eml_level_t lvl, const char* line, size_t n, void* user);

/**
 * @brief Initialize the global logger state.
 *
 * This must be called early if you want to set a non-default minimum
 * level or disable timestamps. If @p min_level is negative the current
 * value of the EMLOG_LEVEL environment variable will be parsed and used
 * (accepted values: debug, info, warn, error, crit).
 *
 * @param min_level Minimum level to emit (or negative to read EMLOG_LEVEL).
 * @param timestamps Enable ISO8601 timestamps when true.
 */
void emlog_init(int min_level, bool timestamps);

/**
 * @brief Set the current runtime minimum log level.
 *
 * Messages with level lower than @p min_level will be dropped.
 *
 * @param min_level New minimum level.
 */
void emlog_set_level(eml_level_t min_level);

/**
 * @brief Enable or disable ISO8601 timestamps in emitted lines.
 *
 * @param on true to enable timestamps, false to disable.
 */
void emlog_enable_timestamps(bool on);

/**
 * @brief Install a custom writer callback.
 *
 * Passing NULL for @p fn restores the default behavior which writes to
 * stdout (info and below) and stderr (errors and above).
 *
 * @param fn Writer callback or NULL to restore default.
 * @param user User data pointer passed to the writer when invoked.
 */
void emlog_set_writer(eml_writer_fn fn, void* user);

/**
 * @brief Control whether the logger flushes stdio buffers before using writev.
 *
 * When true (default) the logger will call fflush() on the destination
 * FILE* before issuing a writev() syscall. This avoids interleaving when
 * other code may be using stdio on the same stream (safe but slower).
 * When false the logger will write directly via writev() (faster but
 * may interleave with stdio-buffered output).
 */
void emlog_set_writev_flush(bool on);

/**
 * @brief Core printf-style logger.
 *
 * The logger is thread-safe and will drop messages whose level is below
 * the current minimum. The @p comp argument is an optional component/tag
 * string; pass NULL or "-" if not applicable.
 *
 * @note This function is declared with a printf attribute so format
 *       string mismatches are detected at compile time when supported.
 *
 * @param level Log level for this message.
 * @param comp Component/tag string (may be NULL).
 * @param fmt printf-style format string followed by arguments.
 */
void emlog_log(eml_level_t level, const char* comp, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

/**
 * @brief Log a message that includes formatted errno text.
 *
 * This composes the formatted message from @p fmt and appends the
 * strerror() text for @p err. It is safe to call from signal handlers as
 * long as the C library implementations used are async-signal-safe for
 * the invoked routines (most are not); prefer using it from normal code.
 *
 * @param level Log level.
 * @param comp Optional component/tag.
 * @param err errno value to format (e.g., errno).
 * @param fmt printf-style format string and args.
 */
void emlog_log_errno(eml_level_t level, const char* comp, int err, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Short logging macros for easy call-sites. These forward to emlog_log().
 * Example: EML_INFO("main", "listening on %d", port);
 */
#define EML_DBG(tag, ...)   emlog_log(EML_LEVEL_DBG, tag, __VA_ARGS__)
#define EML_INFO(tag, ...)  emlog_log(EML_LEVEL_INFO, tag, __VA_ARGS__)
#define EML_WARN(tag, ...)  emlog_log(EML_LEVEL_WARN, tag, __VA_ARGS__)
#define EML_ERROR(tag, ...) emlog_log(EML_LEVEL_ERROR, tag, __VA_ARGS__)
#define EML_CRIT(tag, ...)  emlog_log(EML_LEVEL_CRIT, tag, __VA_ARGS__)

/**
 * @brief Helper that logs errno using the current global errno value.
 *
 * Usage: EML_PERR("mod", "failed to open %s", path);
 */
#define EML_PERR(tag, fmt, ...)                                         \
    do                                                                  \
    {                                                                   \
        int __e = errno;                                                \
        emlog_log_errno(EML_LEVEL_ERROR, tag, __e, fmt, ##__VA_ARGS__); \
    } while(0)

/**
 * @brief Map a POSIX errno value to a canonical eml_err_t category.
 *
 * @param err POSIX errno value (e.g., errno)
 * @return eml_err_t Canonical error category.
 */
eml_err_t eml_from_errno(int err);

/**
 * @brief Return a string name for a canonical error category.
 *
 * The returned pointer is always valid and points to a static string.
 *
 * @param e Error category value.
 * @return const char* Static string describing the category.
 */
const char* eml_err_name(eml_err_t e);

/**
 * @brief Map a canonical error category to a suggested program exit code.
 *
 * This is useful for CLI programs that want to return a meaningful exit
 * status derived from a library error.
 *
 * @param e Canonical error category.
 * @return int Suggested exit code.
 */
int eml_err_to_exit(eml_err_t e);

/**
 * @brief Return a numeric thread identifier suitable for logging.
 *
 * On Linux this returns the kernel thread id via syscall(SYS_gettid).
 * On other platforms it converts the pthread_t value to a 64-bit value.
 * The value is intended for human-readable logs, not for strict
 * comparisons across processes.
 *
 * @return uint64_t Numeric thread identifier.
 */
uint64_t eml_tid(void);

#ifdef __cplusplus
}
#endif
#endif /* EMLOG_H */
