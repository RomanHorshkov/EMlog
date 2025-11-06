/* emlog.c - unified logger implementation (single source)
 * This is the canonical implementation for the emlog API. It replaces the
 * previous split of `logger.h` + `logger.c` compatibility and provides a
 * single, stable `emlog.h`/`emlog.c` pair.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "emlog.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#    include <sys/syscall.h>
#endif

/* Global runtime state (protected by mutex) */
static struct {
    eml_level_t     min_level; /**< Minimum level to emit */
    int             use_ts;    /**< Whether timestamps are enabled */
    pthread_mutex_t mu;        /**< Mutex protecting the struct */
    eml_writer_fn   writer;    /**< Optional custom writer */
    void*           writer_ud; /**< User data passed to writer */
} G = {.min_level = EML_LEVEL_INFO,
       .use_ts    = 1,
       .mu        = PTHREAD_MUTEX_INITIALIZER,
       .writer    = NULL,
       .writer_ud = NULL};

/* --------------------------------------------------------------------------
 * Static function declarations (private helpers)
 *
 * These are documented here with Doxygen so users reading the implementation
 * can understand the internals. They remain static (translation-unit
 * private) and are not part of the public API in emlog.h.
 * -------------------------------------------------------------------------- */

/**
 * @brief Convert a log level to a short string.
 * 
 * Maps EML_LEVEL_DBG -> "DBG", EML_LEVEL_INFO -> "INF", etc.
 * 
 * @param l Log level
 * @return const char* Short level name ("DBG","INF","WRN","ERR","CRT").
 */
static const char* lvl_str(eml_level_t l);

/** @brief Choose default FILE stream for a level (stdout/stderr).
 * 
 * Logs at DBG/INF go to stdout, others to stderr.
 * 
 * @param l Log level
 * @return FILE* stdout for DBG/INF, stderr for WRN/ERR/CRT
 */
static FILE* default_stream(eml_level_t l);

/** @brief Format current time as ISO8601 into buffer.
 * 
 * Produces a string like "2025-08-15T14:23:30.123Z".
 * 
 * @param out Output buffer
 * @param n Size of output buffer
 * @param msec_out Optional pointer to receive milliseconds part
 */
static void fmt_time_iso8601(char* out, size_t n, unsigned* msec_out);

/** @brief Parse textual level name (from env) into eml_level_t.
 * 
 * Handles "debug", "info", "warn", "error", "crit" (case-insensitive).
 * 
 *  @param s Level name string
 *  @return eml_level_t Parsed level, or EML_LEVEL_INFO on unrecognized/NULL
 */
static eml_level_t parse_level(const char* s);

/** @brief Low-level writer that emits a single line (no trailing \n expected).
 * 
 * Writes the line using the custom writer if set, else to stdout/stderr
 * 
 * @param level Log level
 * @param line Pointer to line data (not NUL-terminated necessarily)
 * @param n Length of line
 */
static void write_line(eml_level_t level, const char* line, size_t n);

/** @brief Core varargs logger implementation (expects mutex to be held).
 * 
 * Formats and emits a log line if the level is >= current min_level.
 * 
 * @param level Log level
 * @param comp Component name (nullable)
 * @param fmt Printf-style format string
 * @param ap   va_list of arguments
 */
static void vlog(eml_level_t level, const char* comp, const char* fmt, va_list ap);

/* --------------------------------------------------------------------------
 * Public API implementations
 *
 * These definitions are placed before the static implementations so the file
 * reads as: helpers declarations -> public API -> private helpers. Each
 * public function has a brief Doxygen comment matching the header to keep
 * docs consistent for readers of the .c file.
 * -------------------------------------------------------------------------- */

/**
 * @see emlog_init in emlog.h
 */
void emlog_init(int min_level, bool timestamps)
{
    pthread_mutex_lock(&G.mu);
    if(min_level < 0)
    {
        const char* env = getenv("EMLOG_LEVEL");
        G.min_level     = parse_level(env);
    }
    else
    {
        G.min_level = (eml_level_t)min_level;
    }
    G.use_ts = timestamps ? 1 : 0;
    /* Ensure timezone data is initialized once to avoid repeated tzfile
     * reads and allocations when formatting timestamps (strftime with %z)
     * on every log call. tzset() is idempotent and cheap when already
     * initialized. */
    if (G.use_ts) tzset();
    pthread_mutex_unlock(&G.mu);
}

/**
 * @see emlog_set_level in emlog.h
 */
void emlog_set_level(eml_level_t min_level)
{
    pthread_mutex_lock(&G.mu);
    G.min_level = min_level;
    pthread_mutex_unlock(&G.mu);
}

/**
 * @see emlog_enable_timestamps in emlog.h
 */
void emlog_enable_timestamps(bool on)
{
    pthread_mutex_lock(&G.mu);
    G.use_ts = on ? 1 : 0;
    pthread_mutex_unlock(&G.mu);
}

/**
 * @see emlog_set_writer in emlog.h
 */
void emlog_set_writer(eml_writer_fn fn, void* user)
{
    pthread_mutex_lock(&G.mu);
    G.writer    = fn;
    G.writer_ud = user;
    pthread_mutex_unlock(&G.mu);
}

/**
 * @see emlog_log in emlog.h
 */
void emlog_log(eml_level_t level, const char* comp, const char* fmt, ...)
{
    pthread_mutex_lock(&G.mu);
    va_list ap;
    va_start(ap, fmt);
    vlog(level, comp, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&G.mu);
}

/**
 * @see emlog_log_errno in emlog.h
 */
void emlog_log_errno(eml_level_t level, const char* comp, int err, const char* fmt, ...)
{
    char    base[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(base, sizeof base, fmt, ap);
    va_end(ap);

    char eb[128];
#if defined(__GLIBC__) && !defined(__APPLE__)
    char* s = strerror_r(err, eb, sizeof eb); /* GNU variant */
    emlog_log(level, comp, "%s: %s (%d)", base, s, err);
#else
    strerror_r(err, eb, sizeof eb); /* POSIX variant */
    emlog_log(level, comp, "%s: %s (%d)", base, eb, err);
#endif
}

/**
 * @see eml_from_errno in emlog.h
 */
eml_err_t eml_from_errno(int e)
{
    switch(e)
    {
        case 0:
            return EML_OK;
        case EINTR:
        case EAGAIN:
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
        case EWOULDBLOCK:
#endif
            return EML_TRY_AGAIN;
        case EMFILE:
        case ENFILE:
        case ENOMEM:
            return EML_TEMP_RESOURCE;
        case EBUSY:
#if defined(ENETDOWN) && (ENETDOWN != EBUSY)
        case ENETDOWN:
#endif
#if defined(ENETUNREACH) && \
    (!defined(ENETDOWN) || (ENETUNREACH != ENETDOWN && ENETUNREACH != EBUSY))
        case ENETUNREACH:
#endif
            return EML_TEMP_UNAVAILABLE;
        case ENOENT:
        case ESRCH:
            return EML_NOT_FOUND;
        case EINVAL:
#if defined(EPROTO) && (EPROTO != EINVAL)
        case EPROTO:
#endif
#if defined(EBADMSG) && (EBADMSG != EINVAL && (!defined(EPROTO) || EBADMSG != EPROTO))
        case EBADMSG:
#endif
            return EML_BAD_INPUT;
        case EACCES:
        case EPERM:
            return EML_PERM;
        case EEXIST:
#if defined(EADDRINUSE) && (EADDRINUSE != EEXIST)
        case EADDRINUSE:
#endif
            return EML_CONFLICT;
        case EIO:
        case ENOSPC:
            return EML_FATAL_IO;
        default:
            return EML_FATAL_BUG;
    }
}

/**
 * @see eml_err_name in emlog.h
 */
const char* eml_err_name(eml_err_t e)
{
    switch(e)
    {
        case EML_OK:
            return "EML_OK";
        case EML_TRY_AGAIN:
            return "EML_TRY_AGAIN";
        case EML_TEMP_RESOURCE:
            return "EML_TEMP_RESOURCE";
        case EML_TEMP_UNAVAILABLE:
            return "EML_TEMP_UNAVAILABLE";
        case EML_BAD_INPUT:
            return "EML_BAD_INPUT";
        case EML_NOT_FOUND:
            return "EML_NOT_FOUND";
        case EML_PERM:
            return "EML_PERM";
        case EML_CONFLICT:
            return "EML_CONFLICT";
        case EML_FATAL_CONF:
            return "EML_FATAL_CONF";
        case EML_FATAL_IO:
            return "EML_FATAL_IO";
        case EML_FATAL_CRYPTO:
            return "EML_FATAL_CRYPTO";
        case EML_FATAL_BUG:
            return "EML_FATAL_BUG";
        default:
            return "EML_UNKNOWN";
    }
}

/**
 * @see eml_err_to_exit in emlog.h
 */
int eml_err_to_exit(eml_err_t e)
{
    switch(e)
    {
        case EML_FATAL_CONF:
            return EML_EXIT_CONF;
        case EML_FATAL_IO:
            return EML_EXIT_IO;
        case EML_TEMP_RESOURCE:
            return EML_EXIT_MEM;
        case EML_FATAL_BUG:
            return EML_EXIT_BUG;
        default:
            return EML_EXIT_OK;
    }
}

/* --------------------------------------------------------------------------
 * Private static implementations
 *
 * The functions below implement the helpers declared above. They are kept
 * static to avoid leaking symbols into the public library ABI.
 * -------------------------------------------------------------------------- */

static const char* lvl_str(eml_level_t l)
{
    switch(l)
    {
        case EML_LEVEL_DBG:
            return "DBG";
        case EML_LEVEL_INFO:
            return "INF";
        case EML_LEVEL_WARN:
            return "WRN";
        case EML_LEVEL_ERROR:
            return "ERR";
        case EML_LEVEL_CRIT:
            return "CRT";
        default:
            return "UNK";
    }
}

static FILE* default_stream(eml_level_t l)
{
    return (l <= EML_LEVEL_INFO) ? stdout : stderr;
}

uint64_t eml_tid(void)
{
#if defined(__linux__)
#    if defined(SYS_gettid)
    return (uint64_t)syscall(SYS_gettid);
#    else
    return (uint64_t)getpid(); /* fallback */
#    endif
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

static void fmt_time_iso8601(char* out, size_t n, unsigned* msec_out)
{
    struct timespec ts;  clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;        localtime_r(&ts.tv_sec, &tm);

    unsigned ms = (unsigned)((ts.tv_nsec / 1000000u) % 1000u);

    char z[6] = "";
    if(!strftime(z, sizeof z, "%z", &tm)) memcpy(z, "+0000", 5);
    char tz[7]; /* +HH:MM */
    (void)snprintf(tz, sizeof tz, "%c%c%c:%c%c", z[0], z[1], z[2], z[3], z[4]);

    /* 29 bytes without NUL: YYYY-MM-DDTHH:MM:SS.mmm+HH:MM */
    enum { ISO8601_LEN = 29, ISO8601_BUFSZ = ISO8601_LEN + 1 };

    char tmp[ISO8601_BUFSZ];
    int written = snprintf(tmp, sizeof tmp,
                           "%04d-%02d-%02dT%02d:%02d:%02d.%03u%s",
                           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                           tm.tm_hour, tm.tm_min, tm.tm_sec, ms, tz);
    if (written < 0) { if (n) out[0] = '\0'; return; }

    if (n) {
        size_t tocpy = (size_t)written;
        if (tocpy >= n) tocpy = n - 1;
        memcpy(out, tmp, tocpy);
        out[tocpy] = '\0';
    }
    if (msec_out) *msec_out = ms;
}

static eml_level_t parse_level(const char* s)
{
    if(!s) return EML_LEVEL_INFO;
    if(!strcasecmp(s, "debug")) return EML_LEVEL_DBG;
    if(!strcasecmp(s, "info")) return EML_LEVEL_INFO;
    if(!strcasecmp(s, "warn") || !strcasecmp(s, "warning")) return EML_LEVEL_WARN;
    if(!strcasecmp(s, "error")) return EML_LEVEL_ERROR;
    if(!strcasecmp(s, "crit") || !strcasecmp(s, "fatal")) return EML_LEVEL_CRIT;
    return EML_LEVEL_INFO;
}

static void write_line(eml_level_t level, const char* line, size_t n)
{
    if(G.writer)
    {
        (void)G.writer(level, line, n, G.writer_ud);
        return;
    }
    FILE* out = default_stream(level);
    (void)fwrite(line, 1, n, out);
    fputc('\n', out);
    fflush(out);
}

static void vlog(eml_level_t level, const char* comp, const char* fmt, va_list ap)
{
    if(level < G.min_level) return;

    char ts[40] = {0};
    if(G.use_ts)
    {
        unsigned dummy_ms;
        fmt_time_iso8601(ts, sizeof ts, &dummy_ms);
    }

    va_list ap2;
    va_copy(ap2, ap);
    char stackbuf[1024];
    int  need = vsnprintf(stackbuf, sizeof stackbuf, fmt, ap2);
    va_end(ap2);

    char*  msg    = stackbuf;
    size_t msglen = (need < 0) ? 0 : (size_t)need;
    char*  heap   = NULL;

    if(need >= (int)sizeof stackbuf)
    {
        heap = (char*)malloc((size_t)need + 1);
        if(heap)
        {
            va_list ap3;
            va_copy(ap3, ap);
            vsnprintf(heap, (size_t)need + 1, fmt, ap3);
            va_end(ap3);
            msg = heap;
        }
        else
        {
            msg    = stackbuf;
            msglen = sizeof stackbuf - 1;
        }
    }

    char     head[96];
    uint64_t tid  = eml_tid();
    int      hlen = G.use_ts ? snprintf(head, sizeof head, "%s %s [%llu] [%s] ", ts, lvl_str(level),
                                        (unsigned long long)tid, comp ? comp : "-")
                             : snprintf(head, sizeof head, "%s [%llu] [%s] ", lvl_str(level),
                                        (unsigned long long)tid, comp ? comp : "-");
    if(hlen < 0) hlen = 0;
    size_t linelen = (size_t)hlen + msglen;
    char*  line    = (char*)malloc(linelen + 1);
    if(line)
    {
        memcpy(line, head, (size_t)hlen);
        memcpy(line + hlen, msg, msglen);
        line[linelen] = 0;
        write_line(level, line, linelen);
        free(line);
    }
    else
    {
        write_line(level, head, (size_t)hlen);
        write_line(level, msg, msglen);
    }
    free(heap);
}
