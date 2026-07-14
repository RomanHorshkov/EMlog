/**
 * @file emlog.c
 * @brief Thread-safe logger implementation with configurable writers, timestamps, and errno categorization.
 *
 * This file owns the global logger state, timestamp formatting cache, default stdout/stderr writer path, custom writer dispatch, and POSIX
 * errno mapping helpers exposed by emlog.h.
 */

#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif
#include "emlog.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#    include <sys/syscall.h>
#endif

/*****************************************************************************************************************************************
 * MARK: PRIVATE DEFINES
 *****************************************************************************************************************************************
 */

/* Maximum single write size we try to emit atomically. Prefer to use
 * the POSIX PIPE_BUF if available (writes <= PIPE_BUF to a pipe are atomic). Fallback to 4096 if not defined. Keeping messages <= this size
 * reduces the risk of kernel-level splitting/interleaving when stdout/stderr are pipes (e.g., captured by a supervisor).
 */
#if defined(PIPE_BUF)
#    define LOG_MAX_WRITE ((size_t)PIPE_BUF)
#else
#    define LOG_MAX_WRITE ((size_t)4096)
#endif

/* ------------------------------------------------------------------
 * Timestamp cache
 *
 * We maintain a tiny cache for the ISO8601 timestamp prefix (everything up to the second) so that high-frequency logging that only differs
 * by milliseconds does not repeatedly reformat the date/time fields or hit any underlying timezone parsing logic. The cache is protected by
 * a lightweight mutex and updated only when the second changes.
 *
 * Rationale and behavior:
 * - Most log messages in a tight loop will share the same second. By
 *   caching the "YYYY-MM-DDTHH:MM:SS" prefix we avoid re-running
 *   strftime/localtime conversions on every call.
 * - The milliseconds part (.
 *   mmm) is computed every call from the high-resolution clock and
 *   appended to the cached prefix without acquiring the cache mutex.
 * - The timezone offset ("+HH:MM" or "-HH:MM") is sampled when the
 *   cache is updated and stored alongside the prefix. This keeps the
 *   formatting cheap while still reflecting the local timezone.
 * - The cache is intentionally simple and conservative: it trades a few
 *   bytes of static storage for avoiding repeated small heap allocations
 *   and expensive libc tzfile parsing in the hot path.
 * ------------------------------------------------------------------ */
/*
 * Per-thread timestamp cache
 *
 * Use a thread-local small cache so each thread updates its own formatted second-prefix and timezone string. This eliminates the mutex and
 * contention when many threads log at high rate.
 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#    define EML_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#    define EML_THREAD_LOCAL __thread
#else
#    define EML_THREAD_LOCAL /* fallback: single global cache (will be slow) */
#endif

/*****************************************************************************************************************************************
 * MARK: PRIVATE ENUMERATED VARIABLES
 *****************************************************************************************************************************************
 */
/* None */

/*****************************************************************************************************************************************
 * MARK: PRIVATE STRUCTURED TYPES
 *****************************************************************************************************************************************
 */

/**
 * @brief Global logger configuration. G.mutex protects ONLY this struct
 *        (setters + the per-call snapshot in emlog_log) and is never held
 *        while formatting or invoking a writer — so a custom writer callback
 *        may safely call emlog_set_level()/emlog_set_writer()/etc.
 */
static struct
{
    _Atomic int     min_level;    /**< Minimum level to emit — atomic so the dropped-call fast path takes no lock */
    int             use_ts;       /**< Whether timestamps are enabled */
    pthread_mutex_t mutex;        /**< Mutex protecting the struct */
    eml_writer_fn   writer;       /**< Optional custom writer — REPLACED under g_emit_mutex, read under g_emit_mutex (see emlog_set_writer) */
    void*           writer_ud;    /**< User data passed to writer — same discipline as writer */
    int             writev_flush; /**< Whether to fflush before writev */
    unsigned        init_gen;     /**< Counts successful init calls */
    int             initialized;  /**< Tracks whether init ran at least once */
} G = {.min_level    = EML_LEVEL_INFO,
       .use_ts       = 1,
       .mutex        = PTHREAD_MUTEX_INITIALIZER,
       .writer       = NULL,
       .writer_ud    = NULL,
       /* default: fastest path, do NOT fflush before writev. The
        * caller controls this via emlog_set_writev_flush(). */
       .writev_flush = 0,
       .init_gen     = 0,
       .initialized  = 0};

/**
 * @brief Per-call snapshot of G, taken under G.mutex, used lock-free for the
 *        rest of the emit so config mutation can never race the write path.
 */
typedef struct
{
    eml_level_t   min_level;
    int           use_ts;
    eml_writer_fn writer;
    void*         writer_ud;
    int           writev_flush;
} log_cfg_t;

/**
 * @brief Serializes formatting + emission (line ordering and the static emit
 *        buffers below). Separate from G.mutex by design: a slow or blocked
 *        writer stalls other LOGGING threads but never configuration calls.
 */
static pthread_mutex_t g_emit_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Reentry guard: set while this thread is inside the emit path. A
 *        custom writer logging from its own callback would self-deadlock on
 *        g_emit_mutex — reentrant lines are dropped instead. (Without TLS
 *        support this degrades to a global flag: reentry protection stays
 *        correct, but other threads' lines may be dropped while a writer
 *        callback runs.)
 */
EML_THREAD_LOCAL static int _emitting_tls = 0;

/**
 * @brief Message-format buffer, owned by g_emit_mutex. The emitted line is
 *        hard-capped at LOG_MAX_WRITE, so formatting directly into bounded
 *        static storage replaces the old malloc-then-truncate path: the
 *        logger performs no heap allocation on any path.
 */
static char g_msgbuf[LOG_MAX_WRITE];

/**
 * @brief Contiguous-line assembly buffer for custom writers, owned by
 *        g_emit_mutex. _vlog guarantees header+message <= LOG_MAX_WRITE - 1,
 *        leaving room for the terminating NUL the writer contract promises.
 */
static char g_linebuf[LOG_MAX_WRITE];

/*****************************************************************************************************************************************
 * MARK: PRIVATE VARIABLES DEFINITIONS
 *****************************************************************************************************************************************
 */

/**
 * @brief Per-thread timestamp cache variables.
 */
EML_THREAD_LOCAL static time_t _ts_cache_sec_tls = 0;

/**
 * @brief Per-thread timestamp cache strings.
 */
EML_THREAD_LOCAL static char _ts_cache_prefix_tls[32] = "";

/**
 * @brief Per-thread timezone offset cache string.
 */
EML_THREAD_LOCAL static char _ts_cache_tz_tls[8] = "+00:00";

/*****************************************************************************************************************************************
 * MARK: PRIVATE FUNCTION DECLARATIONS
 *****************************************************************************************************************************************
 */

/**
 * @brief Convert a log level to a short string.
 *
 * Maps EML_LEVEL_DBG -> "DBG", EML_LEVEL_INFO -> "INF", etc.
 *
 * @param l Log level
 * @return const char* Short level name ("DBG","INF","WRN","ERR","CRT").
 */
static const char* _level_to_string(eml_level_t l);

/** @brief Parse textual level name (from env) into eml_level_t.
 *
 * Handles "debug", "info", "warn", "error", "crit" (case-insensitive).
 *
 *  @param s Level name string
 *  @return eml_level_t Parsed level, or EML_LEVEL_INFO on unrecognized/NULL
 */
static eml_level_t _string_to_level(const char* s);

/**
 * @brief Return a numeric thread identifier suitable for logging.
 *
 * On Linux this returns the kernel thread id via syscall(SYS_gettid). On other platforms it converts the pthread_t value to a 64-bit value.
 * The value is intended for human-readable logs, not for strict comparisons across processes.
 *
 * @return uint64_t Numeric thread identifier.
 */
static uint64_t _get_thread_id(void);

/** @brief Choose default FILE stream for a level (stdout/stderr).
 *
 * Logs at DBG/INF go to stdout, others to stderr.
 *
 * @param l Log level
 * @return FILE* stdout for DBG/INF, stderr for WRN/ERR/CRT
 */
static FILE* _default_stream(eml_level_t l);

/**
 * @brief Copy cached timestamp prefix + ms + tz into output buffer.
 *
 * @param out Output buffer
 * @param n Size of output buffer
 * @param ms Milliseconds part to append
 */
static void _copy_cached_ts(char* out, size_t n, unsigned ms);

/** @brief Format current time as ISO8601 into buffer.
 *
 * Produces a string like "2025-08-15T14:23:30.123Z".
 *
 * @param out Output buffer
 * @param n Size of output buffer
 * @param msec_out Optional pointer to receive milliseconds part
 */
static void _fmt_time_iso8601(char* out, size_t n, unsigned* msec_out);

/**
 * @brief Write a log line given as an iovec array.
 *
 * On POSIX platforms (Linux) we use writev() to write header+message+"\n" in a single syscall. If a custom writer is installed the line is
 * assembled into g_linebuf (serialized by g_emit_mutex, never heap) and handed over NUL-terminated per the emlog.h contract.
 */
static void _write_line_iov(const log_cfg_t* cfg, eml_level_t level, struct iovec* iov, int iovcnt);

/** @brief Core varargs logger implementation (expects g_emit_mutex to be held).
 *
 * Formats and emits a log line. Level filtering already happened in emlog_log against the same snapshot.
 *
 * @param cfg  Config snapshot taken under G.mutex
 * @param level Log level
 * @param comp Component name (nullable)
 * @param fmt Printf-style format string
 * @param ap   va_list of arguments
 */
static void _vlog(const log_cfg_t* cfg, eml_level_t level, const char* comp, const char* fmt, va_list ap);

/*****************************************************************************************************************************************
 * MARK: PUBLIC FUNCTIONS DEFINITIONS
 *****************************************************************************************************************************************
 */

void emlog_init(int min_level, bool timestamps)
{
    pthread_mutex_lock(&G.mutex);
    eml_level_t new_level;
    if(min_level < 0)
    {
        /* Negative (and only negative) means "read EMLOG_LEVEL from the
         * environment" — the documented contract. A value above CRIT is a
         * deliberate "disable all output" request (nothing can reach it),
         * so it is stored as-is, never silently rerouted to the env. */
        const char* env = getenv("EMLOG_LEVEL");
        new_level       = _string_to_level(env);
    }
    else
    {
        new_level = (eml_level_t)min_level;
    }
    int new_use_ts = timestamps ? 1 : 0;
    int need_tz    = new_use_ts && (!G.initialized || !G.use_ts);

    atomic_store_explicit(&G.min_level, (int)new_level, memory_order_relaxed);
    G.use_ts = new_use_ts;
    if(need_tz) tzset();
    G.initialized = 1;
    ++G.init_gen;
    pthread_mutex_unlock(&G.mutex);
    EML_INFO("emlog", "Initialized emlog (level=%s, timestamps=%s)", _level_to_string(new_level), new_use_ts ? "enabled" : "disabled");
}

void emlog_set_level(eml_level_t min_level)
{
    /* Negative would enable levels that don't exist; clamp to DBG. Values
     * above CRIT are legitimate: they disable all output. */
    if((int)min_level < 0) min_level = EML_LEVEL_DBG;
    atomic_store_explicit(&G.min_level, (int)min_level, memory_order_relaxed);
}

void emlog_enable_timestamps(bool on)
{
    pthread_mutex_lock(&G.mutex);
    G.use_ts = on ? 1 : 0;
    pthread_mutex_unlock(&G.mutex);
}

void emlog_set_writer(eml_writer_fn fn, void* user)
{
    /* Synchronize with in-flight emission: the emit path reads writer/
     * writer_ud only while holding g_emit_mutex, so once this returns no
     * thread can invoke the OLD writer or touch the OLD context — the caller
     * may reclaim it immediately (`emlog_set_writer(NULL, NULL); free(ctx);`
     * is safe). When called from inside a writer callback this thread
     * already holds g_emit_mutex (taking it again would self-deadlock);
     * the swap is then sequenced within the callback and takes effect for
     * every later line. */
    int own_emit = !_emitting_tls;
    if(own_emit) pthread_mutex_lock(&g_emit_mutex);
    pthread_mutex_lock(&G.mutex);
    G.writer    = fn;
    G.writer_ud = user;
    pthread_mutex_unlock(&G.mutex);
    if(own_emit) pthread_mutex_unlock(&g_emit_mutex);
}

void emlog_set_writev_flush(bool on)
{
    pthread_mutex_lock(&G.mutex);
    G.writev_flush = on ? 1 : 0;
    pthread_mutex_unlock(&G.mutex);
}

void emlog_log(eml_level_t level, const char* comp, const char* fmt, ...)
{
    /* Lock-free early drop: rejected lines cost one relaxed atomic load. */
    if((int)level < atomic_load_explicit(&G.min_level, memory_order_relaxed)) return;

    /* A custom writer logging from inside its own callback would self-deadlock
     * on g_emit_mutex; drop the reentrant line (the outer line still emits). */
    if(_emitting_tls) return;

    log_cfg_t cfg;
    cfg.min_level = (eml_level_t)atomic_load_explicit(&G.min_level, memory_order_relaxed);
    pthread_mutex_lock(&G.mutex);
    cfg.use_ts       = G.use_ts;
    cfg.writev_flush = G.writev_flush;
    pthread_mutex_unlock(&G.mutex);

    _emitting_tls = 1;
    pthread_mutex_lock(&g_emit_mutex);
    /* Writer identity + context are read ONLY under the emit lock: paired
     * with emlog_set_writer taking the same lock, a replaced writer's
     * context can never be used after set_writer returns (lifetime hole
     * fixed in 1.2.0). */
    cfg.writer    = G.writer;
    cfg.writer_ud = G.writer_ud;
    va_list ap;
    va_start(ap, fmt);
    _vlog(&cfg, level, comp, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_emit_mutex);
    _emitting_tls = 0;
}

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
#if defined(ENETUNREACH) && (!defined(ENETDOWN) || (ENETUNREACH != ENETDOWN && ENETUNREACH != EBUSY))
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
        case EML__COUNT:
            return "EML__COUNT";
        default:
            return "EML_UNKNOWN";
    }
}

int eml_err_to_exit(eml_err_t e)
{
    switch(e)
    {
        case EML_OK:
        case EML_TRY_AGAIN:
        case EML_TEMP_UNAVAILABLE:
        case EML_BAD_INPUT:
        case EML_NOT_FOUND:
        case EML_PERM:
        case EML_CONFLICT:
            return EML_EXIT_OK;
        case EML_FATAL_CRYPTO:
        case EML_FATAL_CONF:
            return EML_EXIT_CONF;
        case EML_FATAL_IO:
            return EML_EXIT_IO;
        case EML_TEMP_RESOURCE:
            return EML_EXIT_MEM;
        case EML_FATAL_BUG:
        case EML__COUNT:
            return EML_EXIT_BUG;
        default:
            return EML_EXIT_OK;
    }
}

/*****************************************************************************************************************************************
 * MARK: PRIVATE FUNCTION DEFINITIONS
 *****************************************************************************************************************************************
 */

static const char* _level_to_string(eml_level_t level)
{
    switch(level)
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
static eml_level_t _string_to_level(const char* s)
{
    if(!s) return EML_LEVEL_INFO;
    if(!strcasecmp(s, "debug")) return EML_LEVEL_DBG;
    if(!strcasecmp(s, "info")) return EML_LEVEL_INFO;
    if(!strcasecmp(s, "warn") || !strcasecmp(s, "warning")) return EML_LEVEL_WARN;
    if(!strcasecmp(s, "error")) return EML_LEVEL_ERROR;
    if(!strcasecmp(s, "crit") || !strcasecmp(s, "fatal")) return EML_LEVEL_CRIT;
    return EML_LEVEL_INFO;
}

static FILE* _default_stream(eml_level_t l)
{
    return (l <= EML_LEVEL_INFO) ? stdout : stderr;
}

static uint64_t _get_thread_id(void)
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

static void _copy_cached_ts(char* out, size_t n, unsigned ms)
{
    if(!out || n == 0) return;

    if(ms > 999) ms %= 1000; /* optional policy: normalize */

    int w = snprintf(out, n, "%s.%03u%s", _ts_cache_prefix_tls, ms, _ts_cache_tz_tls);
    if(w < 0) out[0] = '\0';
}

static void _fmt_time_iso8601(char* out, size_t n, unsigned* msec_out)
{
    /*
     * New cached strategy:
     * - Fetch CLOCK_REALTIME once.
     * - If seconds differ from cache, compute a new prefix and cached tz.
     * - Always compute milliseconds from ts.tv_nsec.
     * - Compose final string as: <prefix>.<mmm><tz>
     *
     * We keep the prefix and tz in small static buffers and protect updates with a mutex. Readers only take the mutex when the second rolls
     * over which is rare for high-frequency logging within the same second.
     */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    /* compute ms from ns (0..999) */
    unsigned ms = (unsigned)((ts.tv_nsec / 1000000u) % 1000u);

    time_t sec = ts.tv_sec;

    /* Fast path: if seconds match the thread-local cache, avoid any
     * locking or further calls. We append ms and tz to the cached prefix residing in thread-local storage.
     */
    if(sec == (time_t)_ts_cache_sec_tls)
    {
        _copy_cached_ts(out, n, ms);
        if(msec_out) *msec_out = ms;
        return;
    }

    /* Slow path: second changed for this thread -> rebuild this thread's
     * cache using thread-local storage. No global mutex needed since each thread updates its own cache.
     */
    if(sec != (time_t)_ts_cache_sec_tls)
    {
        struct tm tm;
        /* localtime_r is thread-safe and will populate tm for the
         * current timezone. This is where libc may need tz data but it should already be initialized by emlog_init() calling tzset().
         */
        localtime_r(&sec, &tm);

        /* Fill prefix: YYYY-MM-DDTHH:MM:SS
         * Use strftime which is safer for locale-aware date/time formatting and avoids compiler warnings about format truncation on
         * snprintf. strftime writes a NUL-terminated string on success; fall back to empty prefix on failure.
         */
        if(!strftime(_ts_cache_prefix_tls, sizeof _ts_cache_prefix_tls, "%Y-%m-%dT%H:%M:%S", &tm))
        {
            _ts_cache_prefix_tls[0] = '\0';
        }

        /* Build timezone offset as +HH:MM or -HH:MM. Using strftime(%z)
         * yields "+HHMM" (no colon) on many platforms, so we read that and insert a colon. If strftime fails, fall back to "+00:00".
         */
        char z[8] = "";
        if(strftime(z, sizeof z, "%z", &tm) && strlen(z) >= 5)
        {
            /* z is e.g. +0200 or -0530; convert to +02:00 */
            _ts_cache_tz_tls[0] = z[0];
            _ts_cache_tz_tls[1] = z[1];
            _ts_cache_tz_tls[2] = z[2];
            _ts_cache_tz_tls[3] = ':';
            _ts_cache_tz_tls[4] = z[3];
            _ts_cache_tz_tls[5] = z[4];
            _ts_cache_tz_tls[6] = '\0';
        }
        else
        {
            memcpy(_ts_cache_tz_tls, "+00:00", sizeof "+00:00");
        }

        _ts_cache_sec_tls = sec; /* publish updated cache for this thread */
    }
    /* Compose final string using thread-local cache. */
    _copy_cached_ts(out, n, ms);
    if(msec_out) *msec_out = ms;
}

static void _write_line_iov(const log_cfg_t* cfg, eml_level_t level, struct iovec* iov, int iovcnt)
{
    if(cfg->writer)
    {
        /* Custom writer: contiguous NUL-terminated line per the emlog.h
         * contract, assembled into g_linebuf (we hold g_emit_mutex). _vlog
         * guarantees the line fits LOG_MAX_WRITE - 1; the clamp below is a
         * defensive truncation, never an over-read. */
        size_t off = 0;
        for(int i = 0; i < iovcnt; ++i)
        {
            size_t len   = iov[i].iov_len;
            size_t space = sizeof(g_linebuf) - 1 - off;
            if(len > space) len = space;
            memcpy(g_linebuf + off, iov[i].iov_base, len);
            off += len;
        }
        g_linebuf[off] = '\0';
        (void)cfg->writer(level, g_linebuf, off, cfg->writer_ud);
        return;
    }

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    /* Default writer: use writev on the underlying FILE* descriptor. We
     * use fileno() to obtain the FD and writev to emit all iovecs and a trailing newline atomically at the syscall level. This reduces
     * allocations and syscalls for the common case.
     */
    FILE* out = _default_stream(level);
    int   fd  = fileno(out);
    /* If configured, flush stdio buffers to avoid interleaving with other
     * code that may be using stdio on the same stream (safer but slower).
     */
    if(cfg->writev_flush) fflush(out);
    /* prepare newline iovec */
    char         nl = '\n';
    struct iovec local_iov[16];
    int          cnt = 0;
    for(int i = 0; i < iovcnt && cnt < (int)(sizeof local_iov / sizeof local_iov[0]) - 1; ++i)
    {
        local_iov[cnt++] = iov[i];
    }
    local_iov[cnt].iov_base = (void*)&nl;
    local_iov[cnt].iov_len  = 1;
    ++cnt;

    ssize_t r = writev(fd, local_iov, cnt);
    (void)r; /* best-effort, ignore errors */
#else
    /* Fallback: write each iovec with fwrite and append newline */
    FILE* out = _default_stream(level);
    for(int i = 0; i < iovcnt; ++i)
        fwrite(iov[i].iov_base, 1, iov[i].iov_len, out);
    fputc('\n', out);
    fflush(out);
#endif
}

static void _vlog(const log_cfg_t* cfg, eml_level_t level, const char* comp, const char* fmt, va_list ap)
{
    /*
     * The core emit path. Runs under g_emit_mutex (owner of g_msgbuf and
     * g_linebuf); reads configuration only through the cfg snapshot, never
     * through G, so a concurrent setter can't race it.
     *
     * Embedded discipline: the emitted line is hard-capped at LOG_MAX_WRITE
     * (atomic pipe write size), so the message is formatted DIRECTLY into
     * bounded static storage. Anything longer is truncated with a "..."
     * marker plus a follow-up TRUNCATED notice — the logger never sizes
     * storage from its input, and never touches the heap.
     */
    if(level < cfg->min_level) return;

    char ts[40] = {0};
    if(cfg->use_ts)
    {
        unsigned dummy_ms;
        _fmt_time_iso8601(ts, sizeof ts, &dummy_ms);
    }

    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(g_msgbuf, sizeof g_msgbuf, fmt, ap2);
    va_end(ap2);

    char*  msg    = g_msgbuf;
    size_t msglen;
    if(need < 0)
    {
        msglen = 0;
    }
    else if((size_t)need >= sizeof g_msgbuf)
    {
        /* vsnprintf wanted more than the atomic-write cap: it already wrote
         * a truncated, NUL-terminated prefix — exactly what we can emit.
         * (header + this always exceeds LOG_MAX_WRITE, so the truncation
         * block below fires and appends the "..." marker + notice.) */
        msglen = sizeof g_msgbuf - 1;
    }
    else
    {
        msglen = (size_t)need;
    }

    char     head[128];
    uint64_t tid = _get_thread_id();
    int      hlen =
        cfg->use_ts
                 ? snprintf(head, sizeof head, "%s %s [%llu] [%s] ", ts, _level_to_string(level), (unsigned long long)tid, comp ? comp : "-")
                 : snprintf(head, sizeof head, "%s [%llu] [%s] ", _level_to_string(level), (unsigned long long)tid, comp ? comp : "-");
    /* snprintf returns the length it WOULD have written; on truncation that is
     * larger than the buffer. Clamp so the iovec length never runs past head[]
     * (a long component name would otherwise read out of bounds). */
    if(hlen < 0)
        hlen = 0;
    else if(hlen >= (int)sizeof head)
        hlen = (int)sizeof head - 1;

    /* Build iovec for header and message, then call _write_line_iov which
     * will choose an efficient path (writev or writer callback).
     */
    struct iovec iov[3];
    int          iovcnt  = 0;
    iov[iovcnt].iov_base = head;
    iov[iovcnt].iov_len  = (size_t)hlen;
    ++iovcnt;

    if(msglen > 0)
    {
        iov[iovcnt].iov_base = msg;
        iov[iovcnt].iov_len  = msglen;
        ++iovcnt;
    }

    /* _write_line_iov will append the trailing newline */
    /* If total size would exceed LOG_MAX_WRITE, truncate the message
     * payload so the emitted iovec fits in a single atomic write. This avoids kernel-level splitting on pipes and improves atomicity. We
     * prefer dropping tail content over calling fflush.
     */
    size_t total = 0;
    for(int i = 0; i < iovcnt; ++i)
        total += iov[i].iov_len;
    if(total + 1 /* newline */ > LOG_MAX_WRITE && msglen > 0)
    {
        /* compute max msglen that fits */
        size_t allowed = LOG_MAX_WRITE - 1; /* reserve for NL */
        if((size_t)hlen >= allowed)
        {
            /* header alone exceeds allowed size: truncate header (unlikely)
             * and emit a tiny fallback message.
             */
            iov[0].iov_len = allowed - 3; /* leave space for "..." */
            iovcnt         = 1;
        }
        else
        {
            size_t remain = allowed - (size_t)hlen;
            if(remain < 4)
            {
                /* not enough room for useful payload; drop payload */
                iovcnt = 1;
            }
            else
            {
                /* truncate message to remain-3 and append "..." */
                size_t new_msglen = remain - 3;
                /* modify stack or heap message buffer in-place if possible */
                if(msglen > 0)
                {
                    if(msglen > new_msglen)
                    {
                        /* ensure we can write '...' into the buffer */
                        if((size_t)msglen >= new_msglen + 3)
                        {
                            /* write '...' at truncation point */
                            ((char*)msg)[new_msglen]     = '.';
                            ((char*)msg)[new_msglen + 1] = '.';
                            ((char*)msg)[new_msglen + 2] = '.';
                        }
                        iov[1].iov_len = new_msglen + 3;
                    }
                }
            }
        }
        /* emit truncated line */
        _write_line_iov(cfg, level, iov, iovcnt);
        /* emit a small warning about truncation (low verbosity):
         * "TRUNCATED: <lvl> <comp> ..."
         */
        char warnbuf[128];
        int  w = snprintf(warnbuf, sizeof warnbuf, "TRUNCATED: %s [%llu] [%s]", _level_to_string(level), (unsigned long long)tid,
                          comp ? comp : "-");
        /* snprintf returns the length it WOULD have written; on truncation
         * (a long component name) that is larger than warnbuf — clamp so the
         * iovec never reads past the buffer (same class as the head[] clamp). */
        if(w < 0)
            w = 0;
        else if(w >= (int)sizeof warnbuf)
            w = (int)sizeof warnbuf - 1;
        struct iovec wiov[1];
        wiov[0].iov_base = warnbuf;
        wiov[0].iov_len  = (size_t)w;
        if(wiov[0].iov_len > 0) _write_line_iov(cfg, level, wiov, 1);
    }
    else
    {
        _write_line_iov(cfg, level, iov, iovcnt);
    }
}
