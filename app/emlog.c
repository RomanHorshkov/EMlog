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
#include <strings.h>
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
    _Atomic int     use_ts;       /**< Whether timestamps are enabled — atomic: read lock-free on every emit */
    _Atomic int     journal;      /**< journald-stream mode: one stream (stderr) + "<N>" priority prefix — atomic, same reason */
    _Atomic int     writev_flush; /**< Whether to fflush before the write — atomic, same reason */
    pthread_mutex_t mutex;        /**< Mutex protecting the NON-atomic fields (writer + init bookkeeping) */
    eml_writer_fn   writer; /**< Optional custom writer — REPLACED under g_emit_mutex, read under g_emit_mutex (see emlog_set_writer) */
    void*           writer_ud;   /**< User data passed to writer — same discipline as writer */
    unsigned        init_gen;    /**< Counts successful init calls */
    int             initialized; /**< Tracks whether init ran at least once */
} G = {.min_level    = EML_LEVEL_INFO,
       .use_ts       = 1,
       .journal      = 0,
       /* default: fastest path, do NOT fflush before the write. The
        * caller controls this via emlog_set_writev_flush(). */
       .writev_flush = 0,
       .mutex        = PTHREAD_MUTEX_INITIALIZER,
       .writer       = NULL,
       .writer_ud    = NULL,
       .init_gen     = 0,
       .initialized  = 0};

/**
 * @brief Per-call snapshot of G, taken under G.mutex, used lock-free for the
 *        rest of the emit so config mutation can never race the write path.
 */
typedef struct
{
    int use_ts;
    int journal;
    int writev_flush;
} log_cfg_t;

/**
 * @brief Serializes CUSTOM-writer callbacks only (the writer-lifetime guarantee
 *        emlog_set_writer documents needs a lock to wait on). The default
 *        stdout/stderr/journal path takes NO lock: each thread formats into its
 *        own thread-local buffer and emits with one writev(2), which the kernel
 *        keeps whole for lines under PIPE_BUF on a pipe and does not interleave
 *        on a stream socket either. Separate from G.mutex by design.
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
 * @brief Per-thread line assembly buffer. The emitted line (header + message +
 *        '\n') is hard-capped at LOG_MAX_WRITE so it is one atomic pipe write;
 *        one extra byte keeps the custom-writer NUL terminator inside bounds.
 *        Thread-local: no lock, no heap, and a writer callback receives a
 *        pointer into ITS calling thread's buffer, valid for the callback only.
 */
EML_THREAD_LOCAL static char _line_tls[LOG_MAX_WRITE + 1];

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

/** @brief Map a level to the syslog/journald priority journald parses from a "<N>" line prefix. */
static int _level_to_journal_priority(eml_level_t l);
/**
 * @brief Write every byte of @p iov to @p fd: retries EINTR and continues after a partial write, so a line is never dropped or split
 *        by a signal or a momentarily full stream socket. Best-effort beyond that (a dead sink cannot be logged about).
 */
static void _writev_all(int fd, struct iovec* iov, int iovcnt);
/**
 * @brief Emit one finished line from the calling thread's _line_tls: default path (lock-free write to the level's stream, or the single
 *        journal stream with the "<N>" prefix) or the custom writer (serialized by g_emit_mutex, NUL-terminated per the contract).
 */
static void _emit_line(const log_cfg_t* cfg, eml_level_t level, size_t len);
/**
 * @brief Format the header (optional timestamp, level, thread id, component) into the calling thread's _line_tls.
 * @return header length; always leaves room for a 3-byte marker and the newline.
 */
static size_t _format_header(const log_cfg_t* cfg, eml_level_t level, const char* comp, uint64_t tid);
/** @brief Core varargs logger implementation: header + message + sanitize + emit (+ TRUNCATED notice). Lock-free on the default path. */
static void _vlog(const log_cfg_t* cfg, eml_level_t level, const char* comp, const char* fmt, va_list ap);

/*****************************************************************************************************************************************
 * MARK: PUBLIC FUNCTIONS DEFINITIONS
 *****************************************************************************************************************************************
 */

void emlog_init(int min_level, bool timestamps)
{
    const int saved_errno = errno;
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
    int need_tz    = new_use_ts && (!G.initialized || !atomic_load_explicit(&G.use_ts, memory_order_relaxed));

    atomic_store_explicit(&G.min_level, (int)new_level, memory_order_relaxed);
    atomic_store_explicit(&G.use_ts, new_use_ts, memory_order_relaxed);
    /* systemd sets JOURNAL_STREAM ("<dev>:<inode>") when stdout/stderr ARE the journal. Then one stream and a "<N>" priority
     * prefix per line give journald real priorities (journalctl -p) and keep emission order (two streams are stamped
     * independently on arrival). Overridable after init with emlog_set_journal_mode(). */
    atomic_store_explicit(&G.journal, getenv("JOURNAL_STREAM") != NULL ? 1 : 0, memory_order_relaxed);
    if(need_tz) tzset();
    G.initialized = 1;
    ++G.init_gen;
    pthread_mutex_unlock(&G.mutex);
    EML_INFO("emlog", "Initialized emlog (level=%s, timestamps=%s, journal=%s)", _level_to_string(new_level),
             new_use_ts ? "enabled" : "disabled", atomic_load_explicit(&G.journal, memory_order_relaxed) ? "stream" : "off");
    errno = saved_errno;
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
    atomic_store_explicit(&G.use_ts, on ? 1 : 0, memory_order_relaxed);
}

void emlog_set_journal_mode(bool on)
{
    atomic_store_explicit(&G.journal, on ? 1 : 0, memory_order_relaxed);
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
    atomic_store_explicit(&G.writev_flush, on ? 1 : 0, memory_order_relaxed);
}

void emlog_log(eml_level_t level, const char* comp, const char* fmt, ...)
{
    /* Lock-free early drop: rejected lines cost one relaxed atomic load — and leave errno alone. */
    if((int)level < atomic_load_explicit(&G.min_level, memory_order_relaxed)) return;
    /* A custom writer logging from inside its own callback would self-deadlock on g_emit_mutex and clobber the thread's line
     * buffer mid-callback; drop the reentrant line (the outer line still emits). */
    if(_emitting_tls) return;
    /* errno-transparent: everything below (clock, localtime, snprintf, write) may set errno; a logger must never change what
     * the caller is about to report — `EML_ERROR(...); return -errno;` is a common and legitimate shape. */
    const int saved_errno = errno;
    log_cfg_t cfg;
    cfg.use_ts       = atomic_load_explicit(&G.use_ts, memory_order_relaxed);
    cfg.journal      = atomic_load_explicit(&G.journal, memory_order_relaxed);
    cfg.writev_flush = atomic_load_explicit(&G.writev_flush, memory_order_relaxed);
    _emitting_tls    = 1;
    va_list ap;
    va_start(ap, fmt);
    _vlog(&cfg, level, comp, fmt, ap);
    va_end(ap);
    _emitting_tls = 0;
    errno         = saved_errno;
}

void emlog_log_errno(eml_level_t level, const char* comp, int err, const char* fmt, ...)
{
    const int saved_errno = errno;
    char      base[768];
    va_list   ap;
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
    errno = saved_errno;
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

    /* Compose in a buffer that provably fits the worst case (prefix 31 + '.' + 3 digits +
     * tz 7 + NUL = 43): -Wformat-truncation=2 assumes an unknown-size destination is size 1,
     * so formatting straight into the caller's buffer can never be warning-clean. */
    char tmp[sizeof _ts_cache_prefix_tls + 4 + sizeof _ts_cache_tz_tls];
    int  w = snprintf(tmp, sizeof tmp, "%s.%03u%s", _ts_cache_prefix_tls, ms, _ts_cache_tz_tls);
    if(w < 0)
    {
        out[0] = '\0';
        return;
    }

    size_t len = (size_t)w;
    if(len >= n) len = n - 1;
    memcpy(out, tmp, len);
    out[len] = '\0';
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

static int _level_to_journal_priority(eml_level_t l)
{
    switch(l)
    {
        case EML_LEVEL_DBG:
            return 7; /* LOG_DEBUG   */
        case EML_LEVEL_INFO:
            return 6; /* LOG_INFO    */
        case EML_LEVEL_WARN:
            return 4; /* LOG_WARNING */
        case EML_LEVEL_ERROR:
            return 3; /* LOG_ERR     */
        case EML_LEVEL_CRIT:
            return 2; /* LOG_CRIT    */
        default:
            return 6;
    }
}

static void _writev_all(int fd, struct iovec* iov, int iovcnt)
{
    while(iovcnt > 0)
    {
        ssize_t w = writev(fd, iov, iovcnt);
        if(w < 0)
        {
            if(errno == EINTR) continue;
            return; /* dead or closed sink: nothing further can be done; the caller's errno is restored by emlog_log */
        }
        size_t done = (size_t)w;
        while(iovcnt > 0 && done >= iov[0].iov_len)
        {
            done -= iov[0].iov_len;
            ++iov;
            --iovcnt;
        }
        if(iovcnt > 0)
        {
            iov[0].iov_base  = (char*)iov[0].iov_base + done;
            iov[0].iov_len  -= done;
        }
    }
}

static void _emit_line(const log_cfg_t* cfg, eml_level_t level, size_t len)
{
    /* Custom writer: serialized by g_emit_mutex (the lifetime guarantee in emlog_set_writer relies on it), handed the line
     * NUL-terminated and without the newline, per the emlog.h contract. */
    pthread_mutex_lock(&g_emit_mutex);
    pthread_mutex_lock(&G.mutex);
    eml_writer_fn writer    = G.writer;
    void*         writer_ud = G.writer_ud;
    pthread_mutex_unlock(&G.mutex);
    if(writer)
    {
        _line_tls[len] = '\0';
        (void)writer(level, _line_tls, len, writer_ud);
        pthread_mutex_unlock(&g_emit_mutex);
        return;
    }
    pthread_mutex_unlock(&g_emit_mutex);

    /* Default path: no lock. journald mode -> ONE stream (stderr) so emission order is the journal order, with the "<N>"
     * priority prefix journald strips and stores; stdio mode -> stdout for DBG/INF, stderr above. */
    FILE* out = cfg->journal ? stderr : _default_stream(level);
    int   fd  = fileno(out);
    if(cfg->writev_flush) fflush(out);
    _line_tls[len] = '\n';
    char         pfx[3];
    struct iovec iov[2];
    int          cnt = 0;
    if(cfg->journal)
    {
        pfx[0]            = '<';
        pfx[1]            = (char)('0' + _level_to_journal_priority(level));
        pfx[2]            = '>';
        iov[cnt].iov_base = pfx;
        iov[cnt].iov_len  = 3;
        ++cnt;
    }
    iov[cnt].iov_base = _line_tls;
    iov[cnt].iov_len  = len + 1;
    ++cnt;
    _writev_all(fd, iov, cnt);
}

static size_t _format_header(const log_cfg_t* cfg, eml_level_t level, const char* comp, uint64_t tid)
{
    const size_t cap    = LOG_MAX_WRITE - 4; /* room for a 3-byte marker + newline */
    char         ts[40] = {0};
    if(cfg->use_ts)
    {
        unsigned dummy_ms;
        _fmt_time_iso8601(ts, sizeof ts, &dummy_ms);
    }
    int h = cfg->use_ts
              ? snprintf(_line_tls, cap, "%s %s [%llu] [%s] ", ts, _level_to_string(level), (unsigned long long)tid, comp ? comp : "-")
              : snprintf(_line_tls, cap, "%s [%llu] [%s] ", _level_to_string(level), (unsigned long long)tid, comp ? comp : "-");
    if(h < 0) h = 0;
    if((size_t)h >= cap)
    {
        h = (int)cap - 1; /* absurd component name: keep the header, mark the cut */
        memcpy(_line_tls + h - 3, "...", 3);
    }
    return (size_t)h;
}

static void _vlog(const log_cfg_t* cfg, eml_level_t level, const char* comp, const char* fmt, va_list ap)
{
    /*
     * Runs on the calling thread with no lock held: header and message are formatted into the thread's own _line_tls, capped at
     * LOG_MAX_WRITE so the line is one atomic pipe write. The logger sizes nothing from its input and never touches the heap.
     * A message that does not fit is cut with a "..." marker and followed by a TRUNCATED notice carrying the same header.
     */
    const uint64_t tid   = _get_thread_id();
    const size_t   hlen  = _format_header(cfg, level, comp, tid);
    const size_t   cap   = LOG_MAX_WRITE - 1; /* the newline (or NUL) goes at index len <= cap */
    const size_t   avail = cap - hlen;        /* >= 3 by construction */
    va_list        ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(_line_tls + hlen, avail + 1, fmt, ap2);
    va_end(ap2);
    size_t len;
    int    truncated = 0;
    if(need < 0)
    {
        len = hlen;
    }
    else if((size_t)need > avail)
    {
        len       = hlen + avail;
        truncated = 1;
        memcpy(_line_tls + len - 3, "...", 3);
    }
    else
    {
        len = hlen + (size_t)need;
    }
    /* Log-injection defense: a %s argument carrying "\n" would forge a second line (and "\r" can hide one on a terminal).
     * Every control byte in the message becomes a visible '?'; the header is generated text and needs no scan. */
    for(size_t i = hlen; i < len; ++i)
    {
        const unsigned char c = (unsigned char)_line_tls[i];
        if(c < 0x20u || c == 0x7fu) _line_tls[i] = '?';
    }
    _emit_line(cfg, level, len);
    if(truncated)
    {
        const size_t h2 = _format_header(cfg, level, comp, tid);
        int          n = snprintf(_line_tls + h2, cap - h2 + 1, "TRUNCATED: message exceeded %zu bytes and was cut", (size_t)LOG_MAX_WRITE);
        if(n < 0) n = 0;
        size_t len2 = h2 + (size_t)n;
        if(len2 > cap) len2 = cap;
        _emit_line(cfg, level, len2);
    }
}
