/* emlog.h — tiny, thread-safe logging & error categorization
 * MIT © 2025 Roman Horshkov
 */
#ifndef EMLOG_H
#define EMLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* -------- Levels (namespaced to avoid syslog collisions) -------- */
typedef enum
{
    EML_LEVEL_DBG = 0,
    EML_LEVEL_INFO,
    EML_LEVEL_WARN,
    EML_LEVEL_ERROR,
    EML_LEVEL_CRIT
} eml_level_t;

/* -------- Canonical error categories -------- */
typedef enum
{
    EML_OK = 0,
    EML_TRY_AGAIN,
    EML_TEMP_RESOURCE,
    EML_TEMP_UNAVAILABLE,
    EML_BAD_INPUT,
    EML_NOT_FOUND,
    EML_PERM,
    EML_CONFLICT,
    EML_FATAL_CONF,
    EML_FATAL_IO,
    EML_FATAL_CRYPTO,
    EML_FATAL_BUG,
    EML__COUNT
} eml_err_t;

enum
{
    EML_EXIT_OK   = 0,
    EML_EXIT_CONF = 2,
    EML_EXIT_IO   = 3,
    EML_EXIT_MEM  = 4,
    EML_EXIT_BUG  = 5,
};

/* Optional writer callback; return bytes written or <0 on failure. */
typedef ssize_t (*eml_writer_fn)(eml_level_t lvl, const char* line, size_t n, void* user);

/* Init. If min_level<0, reads EMLOG_LEVEL env ("debug","info","warn","error","crit"). */
void emlog_init(int min_level, bool timestamps);
/* Change level at runtime. */
void emlog_set_level(eml_level_t min_level);
/* Enable/disable ISO8601 timestamps. */
void emlog_enable_timestamps(bool on);
/* Install a custom writer; pass NULL to restore default (stdout/stderr). */
void emlog_set_writer(eml_writer_fn fn, void* user);

/* Core logger (printf-style). */
void emlog_log(eml_level_t level, const char* comp, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* errno-preserving variant (explicit errno passed). */
void emlog_log_errno(eml_level_t level, const char* comp, int err, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Helpers/macros (short call-sites). We intentionally expose function-like
 * macros named EML_INFO/EML_WARN/etc so existing code that calls
 * EML_INFO(LOG_TAG, "msg") keeps working. Internally these map to the
 * enum values prefixed with EML_LEVEL_ to avoid preprocessor recursion.
 */
#define EML_DBG(tag, ...) emlog_log(EML_LEVEL_DBG, tag, __VA_ARGS__)
#define EML_INFO(tag, ...) emlog_log(EML_LEVEL_INFO, tag, __VA_ARGS__)
#define EML_WARN(tag, ...) emlog_log(EML_LEVEL_WARN, tag, __VA_ARGS__)
#define EML_ERROR(tag, ...) emlog_log(EML_LEVEL_ERROR, tag, __VA_ARGS__)
#define EML_CRIT(tag, ...) emlog_log(EML_LEVEL_CRIT, tag, __VA_ARGS__)

#define EML_PERR(tag, fmt, ...)                                   \
    do                                                            \
    {                                                             \
        int __e = errno;                                          \
        emlog_log_errno(EML_LEVEL_ERROR, tag, __e, fmt, ##__VA_ARGS__); \
    } while(0)

/* Note: this header intentionally exposes the canonical function-like
 * macros EML_INFO/EML_WARN/EML_ERROR/EML_CRIT and the errno helper
 * EML_PERR. The older short names (EML_ERR/EML_INF/EML_WRN/EML_CRT) and
 * LOG_PERR were removed; callers should use the canonical names to
 * keep the API consistent.
 */

/* errno mapping & metadata */
eml_err_t   eml_from_errno(int err);
const char* eml_err_name(eml_err_t e);
int         eml_err_to_exit(eml_err_t e);

/* Optional: expose current TID helper */
uint64_t eml_tid(void);

#ifdef __cplusplus
}
#endif
#endif /* EMLOG_H */
