/**
 * @file emlog.h
 * @brief Tiny, thread-safe logging API: one call per line, errno-transparent, injection-safe, journald-aware.
 *
 * This header exposes a compact logging API with printf-like formatting, optional ISO8601 timestamps, and a mapping layer from POSIX errno
 * values to a small set of canonical error categories. The implementation is thread-safe and allows installing a custom writer callback.
 *
 * The API aims to be minimal and stable to allow building a small static library that other projects can embed. All public symbols are
 * declared here and documented with Doxygen for easy generation of reference docs.
 *
 * License: MIT Copyright: 2025 Roman Horshkov
 */

#ifndef EMLOG_H
#define EMLOG_H

#include <errno.h> /* EML_PERR reads errno; consumers must not need to include it themselves */
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Logging levels used by the library.
 *
 * These are intentionally short (three-letter) and do not collide with syslog names. Use these values when calling emlog_log() or when
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
 * @brief Optional writer callback used to customize output destination.
 *
 * If a writer is installed with emlog_set_writer(), the logger will call this function for each formatted line. The implementation should
 * return the number of bytes written on success or a negative value on failure.
 *
 * Reentrancy rules: the callback runs with the emit lock held but NOT the configuration lock, so it MAY call emlog_set_level(),
 * emlog_set_writer(), emlog_enable_timestamps(), emlog_set_journal_mode() and emlog_set_writev_flush(). Calling emlog_log() (or the
 * EML_* macros) from inside the callback does not deadlock — the reentrant line is silently dropped. Custom-writer callbacks are
 * serialized, so a writer that blocks stalls other threads that log through it; the DEFAULT path (no custom writer) takes no lock.
 *
 * Lifetime: @p line points into the calling thread's logger-owned buffer and is valid only for the duration of the callback — copy it
 * out if needed. Control bytes (< 0x20 and 0x7f) in the message have already been replaced by '?', so @p line is exactly one line.
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
 * This must be called early if you want to set a non-default minimum level or disable timestamps. If @p min_level is negative the current
 * value of the EMLOG_LEVEL environment variable will be parsed and used (accepted values: debug, info, warn, error, crit). A value above
 * EML_LEVEL_CRIT disables all output (no level can reach it) — useful for benchmarks and silent operation.
 *
 * Calling emlog_init() multiple times is safe; each invocation replaces the previous configuration (the most recent call "wins"), which
 * allows different subsystems to reconfigure the logger without tearing down internal state.
 *
 * journald: when systemd hands the process the journal as stdout/stderr it sets JOURNAL_STREAM; emlog_init() then enables journal mode
 * (see emlog_set_journal_mode()) automatically. Timezone rules are loaded here (tzset); a /etc/localtime change while the process runs
 * is not picked up, ordinary DST transitions are. errno is preserved across the call.
 *
 * @param min_level Minimum level to emit; negative to read EMLOG_LEVEL; above EML_LEVEL_CRIT to disable all output.
 * @param timestamps Enable ISO8601 timestamps when true.
 */
void emlog_init(int min_level, bool timestamps);

/**
 * @brief journald-stream mode: emit EVERY level on ONE stream (stderr) with a "<N>" syslog-priority prefix per line.
 *
 * Two streams into journald are stamped independently on arrival, so an ERROR and the INFO emitted just before it can appear swapped in
 * journalctl; one stream keeps emission order. The "<N>" prefix (2 crit, 3 err, 4 warning, 6 info, 7 debug) is the convention journald
 * parses from stdout/stderr: it strips it and stores the real priority, so `journalctl -p err` and priority colouring work. No libsystemd
 * dependency. Auto-enabled by emlog_init() when JOURNAL_STREAM is set; call this after init to override either way. Custom writers are
 * unaffected (they receive the level as a parameter).
 */
void emlog_set_journal_mode(bool on);

/**
 * @brief Set the current runtime minimum log level.
 *
 * Messages with level lower than @p min_level will be dropped. A value above EML_LEVEL_CRIT disables all output; a negative value is
 * clamped to EML_LEVEL_DBG.
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
 * Passing NULL for @p fn restores the default behavior which writes to stdout (info and below) and stderr (errors and above).
 *
 * Lifetime guarantee: this call synchronizes with in-flight emission — it blocks until any currently running writer callback has returned,
 * and after it returns no thread will invoke the previous writer or touch the previous @p user context again. Reclaiming the old context
 * immediately after replacement (`emlog_set_writer(NULL, NULL); free(ctx);`) is therefore safe. Exception: when called from INSIDE a writer
 * callback it cannot wait for itself; the swap still takes effect for every later line, but the executing callback's own context must stay
 * alive until that callback returns.
 *
 * @param fn Writer callback or NULL to restore default.
 * @param user User data pointer passed to the writer when invoked.
 */
void emlog_set_writer(eml_writer_fn fn, void* user);

/**
 * @brief Control whether the logger flushes stdio buffers before using writev.
 *
 * Default is DISABLED (fastest path). When enabled the logger calls fflush() on the destination FILE* before issuing the writev() syscall,
 * which reduces — but cannot fully eliminate — interleaving with other code using stdio on the same stream (another thread can still write
 * between the flush and the writev). When disabled the logger writes directly via writev() and may interleave with stdio-buffered output.
 */
void emlog_set_writev_flush(bool on);

/**
 * @brief Core printf-style logger.
 *
 * The logger is thread-safe and will drop messages whose level is below the current minimum. The @p comp argument is an optional
 * component/tag string; pass NULL or "-" if not applicable.
 *
 * Guarantees: errno is preserved across the call (`EML_ERROR(...); return -errno;` is safe); every control byte in the formatted message
 * becomes '?' so an attacker-controlled argument can never forge a second line; a line is written whole with one syscall, retried on
 * EINTR and completed after a partial write; the default path holds no lock and allocates nothing.
 *
 * @note This function is declared with a printf attribute so format
 *       string mismatches are detected at compile time when supported.
 *
 * @param level Log level for this message.
 * @param comp Component/tag string (may be NULL).
 * @param fmt printf-style format string followed by arguments.
 */
void emlog_log(eml_level_t level, const char* comp, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

/**
 * @brief Log a message that includes formatted errno text.
 *
 * This composes the formatted message from @p fmt and appends the strerror() text for @p err.
 *
 * @warning NOT async-signal-safe (mutexes, vsnprintf, localtime and writer callbacks are involved) — never call any EMlog function from a
 * signal handler.
 *
 * @param level Log level.
 * @param comp Optional component/tag.
 * @param err errno value to format (e.g., errno).
 * @param fmt printf-style format string and args.
 */
void emlog_log_errno(eml_level_t level, const char* comp, int err, const char* fmt, ...) __attribute__((format(printf, 4, 5)));

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
#define EML_PERR(tag, ...)                                       \
    do                                                           \
    {                                                            \
        int __e = errno;                                         \
        emlog_log_errno(EML_LEVEL_ERROR, tag, __e, __VA_ARGS__); \
    } while(0)

#ifdef __cplusplus
}
#endif
#endif /* EMLOG_H */
