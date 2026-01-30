/* emlog.c - unified logger implementation (single source)
 * This is the canonical implementation for the emlog API. It replaces the
 * previous split of `logger.h` + `logger.c` compatibility and provides a
 * single, stable `emlog.h`/`emlog.c` pair.
 */

#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif
#include "emlog.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#    include <sys/syscall.h>
#endif

/* Global runtime state (protected by mutex) */
static struct
{
    eml_level_t     min_level;    /**< Minimum level to emit */
    int             use_ts;       /**< Whether timestamps are enabled */
    pthread_mutex_t mutex;        /**< Mutex protecting the struct */
    eml_writer_fn   writer;       /**< Optional custom writer */
    void*           writer_ud;    /**< User data passed to writer */
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

/* Maximum single write size we try to emit atomically. Prefer to use
 * the POSIX PIPE_BUF if available (writes <= PIPE_BUF to a pipe are
 * atomic). Fallback to 4096 if not defined. Keeping messages <= this
 * size reduces the risk of kernel-level splitting/interleaving when
 * stdout/stderr are pipes (e.g., captured by a supervisor).
 */
#if defined(PIPE_BUF)
#    define LOG_MAX_WRITE ((size_t)PIPE_BUF)
#else
#    define LOG_MAX_WRITE ((size_t)4096)
#endif

/* ------------------------------------------------------------------
 * Timestamp cache
 *
 * We maintain a tiny cache for the ISO8601 timestamp prefix (everything
 * up to the second) so that high-frequency logging that only differs by
 * milliseconds does not repeatedly reformat the date/time fields or hit
 * any underlying timezone parsing logic. The cache is protected by a
 * lightweight mutex and updated only when the second changes.
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
 * Use a thread-local small cache so each thread updates its own
 * formatted second-prefix and timezone string. This eliminates the
 * mutex and contention when many threads log at high rate.
 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#    define EML_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#    define EML_THREAD_LOCAL __thread
#else
#    define EML_THREAD_LOCAL /* fallback: single global cache (will be slow) */
#endif

EML_THREAD_LOCAL static time_t ts_cache_sec_tls        = 0;
EML_THREAD_LOCAL static char   ts_cache_prefix_tls[32] = "";
EML_THREAD_LOCAL static char   ts_cache_tz_tls[8]      = "+00:00";

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
static uint64_t eml_tid(void);

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

void emlog_init(int min_level, bool timestamps)
{
    pthread_mutex_lock(&G.mutex);
    eml_level_t new_level;
    if(min_level < 0)
    {
        const char* env = getenv("EMLOG_LEVEL");
        new_level       = parse_level(env);
    }
    else
    {
        new_level = (eml_level_t)min_level;
    }
    int new_use_ts = timestamps ? 1 : 0;
    int need_tz    = new_use_ts && (!G.initialized || !G.use_ts);

    G.min_level = new_level;
    G.use_ts    = new_use_ts;
    if(need_tz) tzset();
    G.initialized = 1;
    ++G.init_gen;
    pthread_mutex_unlock(&G.mutex);
    EML_INFO("emlog", "Initialized emlog (level=%s, timestamps=%s)", lvl_str(new_level),
             new_use_ts ? "enabled" : "disabled");
}

void emlog_set_level(eml_level_t min_level)
{
    pthread_mutex_lock(&G.mutex);
    G.min_level = min_level;
    pthread_mutex_unlock(&G.mutex);
}

void emlog_enable_timestamps(bool on)
{
    pthread_mutex_lock(&G.mutex);
    G.use_ts = on ? 1 : 0;
    pthread_mutex_unlock(&G.mutex);
}

void emlog_set_writer(eml_writer_fn fn, void* user)
{
    pthread_mutex_lock(&G.mutex);
    G.writer    = fn;
    G.writer_ud = user;
    pthread_mutex_unlock(&G.mutex);
}

void emlog_set_writev_flush(bool on)
{
    pthread_mutex_lock(&G.mutex);
    G.writev_flush = on ? 1 : 0;
    pthread_mutex_unlock(&G.mutex);
}

void emlog_log(eml_level_t level, const char* comp, const char* fmt, ...)
{
    pthread_mutex_lock(&G.mutex);
    va_list ap;
    va_start(ap, fmt);
    vlog(level, comp, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&G.mutex);
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

static uint64_t eml_tid(void)
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

static void copy_cached_ts(char* out, size_t n, unsigned ms)
{
    char buf[64];
    int  w = snprintf(buf, sizeof buf, "%s.%03u%s", ts_cache_prefix_tls, ms, ts_cache_tz_tls);
    if(!n) return;
    if(w < 0)
    {
        out[0] = '\0';
        return;
    }
    size_t copy = (size_t)w;
    if(copy >= n) copy = n - 1;
    memcpy(out, buf, copy);
    out[copy] = '\0';
}

static void fmt_time_iso8601(char* out, size_t n, unsigned* msec_out)
{
    /*
     * New cached strategy:
     * - Fetch CLOCK_REALTIME once.
     * - If seconds differ from cache, compute a new prefix and cached tz.
     * - Always compute milliseconds from ts.tv_nsec.
     * - Compose final string as: <prefix>.<mmm><tz>
     *
     * We keep the prefix and tz in small static buffers and protect
     * updates with a mutex. Readers only take the mutex when the
     * second rolls over which is rare for high-frequency logging within
     * the same second.
     */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    /* compute ms from ns (0..999) */
    unsigned ms = (unsigned)((ts.tv_nsec / 1000000u) % 1000u);

    time_t sec = ts.tv_sec;

    /* Fast path: if seconds match the thread-local cache, avoid any
     * locking or further calls. We append ms and tz to the cached
     * prefix residing in thread-local storage.
     */
    if(sec == (__time_t)ts_cache_sec_tls)
    {
        copy_cached_ts(out, n, ms);
        if(msec_out) *msec_out = ms;
        return;
    }

    /* Slow path: second changed for this thread -> rebuild this thread's
     * cache using thread-local storage. No global mutex needed since
     * each thread updates its own cache.
     */
    if(sec != (__time_t)ts_cache_sec_tls)
    {
        struct tm tm;
        /* localtime_r is thread-safe and will populate tm for the
         * current timezone. This is where libc may need tz data but it
         * should already be initialized by emlog_init() calling tzset().
         */
        localtime_r(&sec, &tm);

        /* Fill prefix: YYYY-MM-DDTHH:MM:SS
         * Use strftime which is safer for locale-aware date/time
         * formatting and avoids compiler warnings about format
         * truncation on snprintf. strftime writes a NUL-terminated
         * string on success; fall back to empty prefix on failure.
         */
        if(!strftime(ts_cache_prefix_tls, sizeof ts_cache_prefix_tls, "%Y-%m-%dT%H:%M:%S", &tm))
        {
            ts_cache_prefix_tls[0] = '\0';
        }

        /* Build timezone offset as +HH:MM or -HH:MM. Using strftime(%z)
         * yields "+HHMM" (no colon) on many platforms, so we read that
         * and insert a colon. If strftime fails, fall back to "+00:00".
         */
        char z[8] = "";
        if(strftime(z, sizeof z, "%z", &tm) && strlen(z) >= 5)
        {
            /* z is e.g. +0200 or -0530; convert to +02:00 */
            ts_cache_tz_tls[0] = z[0];
            ts_cache_tz_tls[1] = z[1];
            ts_cache_tz_tls[2] = z[2];
            ts_cache_tz_tls[3] = ':';
            ts_cache_tz_tls[4] = z[3];
            ts_cache_tz_tls[5] = z[4];
            ts_cache_tz_tls[6] = '\0';
        }
        else
        {
            memcpy(ts_cache_tz_tls, "+00:00", sizeof "+00:00");
        }

        ts_cache_sec_tls = sec; /* publish updated cache for this thread */
    }
    /* Compose final string using thread-local cache. */
    copy_cached_ts(out, n, ms);
    if(msec_out) *msec_out = ms;
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

/*
 * write_line_iov
 *
 * A variant of write_line that accepts an iovec array. On POSIX
 * platforms (Linux) we use writev() to write header+message+"\n" in a
 * single syscall, avoiding a temporary allocation. If a custom writer
 * is installed we fall back to calling the writer with a contiguous
 * buffer (constructed on the stack when small, or via malloc when
 * necessary).
 */
static void write_line_iov(eml_level_t level, struct iovec* iov, int iovcnt)
{
    if(G.writer)
    {
        /* custom writer: needs a contiguous buffer; assemble quickly */
        size_t total = 0;
        for(int i = 0; i < iovcnt; ++i)
            total += iov[i].iov_len;

        /* try stack allocate when small */
        if(total <= 2048)
        {
            char   buf[2048];
            size_t off = 0;
            for(int i = 0; i < iovcnt; ++i)
            {
                memcpy(buf + off, iov[i].iov_base, iov[i].iov_len);
                off += iov[i].iov_len;
            }
            (void)G.writer(level, buf, off, G.writer_ud);
            return;
        }

        /* otherwise use heap */
        char* buf = malloc(total);
        if(!buf) return; /* if malloc fails, drop the line */
        size_t off = 0;
        for(int i = 0; i < iovcnt; ++i)
        {
            memcpy(buf + off, iov[i].iov_base, iov[i].iov_len);
            off += iov[i].iov_len;
        }
        (void)G.writer(level, buf, off, G.writer_ud);
        free(buf);
        return;
    }

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    /* Default writer: use writev on the underlying FILE* descriptor. We
     * use fileno() to obtain the FD and writev to emit all iovecs and a
     * trailing newline atomically at the syscall level. This reduces
     * allocations and syscalls for the common case.
     */
    FILE* out = default_stream(level);
    int   fd  = fileno(out);
    /* If configured, flush stdio buffers to avoid interleaving with other
     * code that may be using stdio on the same stream (safer but slower).
     */
    if(G.writev_flush) fflush(out);
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
    FILE* out = default_stream(level);
    for(int i = 0; i < iovcnt; ++i)
        fwrite(iov[i].iov_base, 1, iov[i].iov_len, out);
    fputc('\n', out);
    fflush(out);
#endif
}

static void vlog(eml_level_t level, const char* comp, const char* fmt, va_list ap)
{
    /*
     * -----------------------------------------------------------------
     * vlog — the core, varargs logger implementation
     * -----------------------------------------------------------------
     *
     * This function is the heart of the logging pipeline. It is invoked
     * with the global mutex held (emlog_log acquires G.mutex before calling
     * into here), so the implementation can safely read and write global
     * state without additional synchronization. The function is
     * carefully designed to avoid heap allocations for common short
     * messages while supporting arbitrarily long messages via a heap
     * fallback path.
     *
     * Step-by-step behavior (annotated):
     * 1) Level filtering: if the provided level is below G.min_level,
     *    drop the message immediately and return. This is an important
     *    early-out to avoid any formatting work for messages that would
     *    be discarded.
     *
     * 2) Timestamp formatting: if timestamps are enabled (G.use_ts), we
     *    call fmt_time_iso8601 to produce an ISO8601 timestamp string.
     *    The implementation of fmt_time_iso8601 uses a per-second cache
     *    to avoid expensive localtime/strftime operations in the hot
     *    path. fmt_time_iso8601 also returns milliseconds which could be
     *    used if desired by callers (currently unused beyond storage).
     *
     * 3) Message formatting: we first attempt to format the message into
     *    a stack-allocated buffer (stackbuf[1024]). This avoids heap
     *    allocations for the common case where formatted messages are
     *    small. We use vsnprintf twice if needed:
     *      - First pass to compute the required size (`need`).
     *      - If `need` >= sizeof(stackbuf) we allocate `need+1` bytes on
     *        the heap, re-run vsnprintf to fill it, and use that as the
     *        message. If malloc fails we fall back to the truncated
     *        stack buffer contents.
     *
     * 4) Header composition: we build a small header containing either
     *    "<ts> <lvl> [tid] [comp] " when timestamps are enabled, or
     *    "<lvl> [tid] [comp] " without timestamps. The thread id is
     *    acquired via eml_tid() which returns a numeric identifier
     *    suitable for human-readable logs.
     *
     * 5) Single-allocation line assembly: to ensure the writer sees a
     *    contiguous line we allocate a buffer of size header+msg and
     *    memcpy both pieces into it, NUL-terminate, and pass it to
     *    write_line. If allocation fails we degrade gracefully by
     *    writing the header and message separately (two writes).
     *
     * 6) Cleanup: free any heap memory allocated for the message or
     *    composed line.
     *
     * Important design and safety notes:
     * - The global mutex prevents concurrent modification of writer and
     *   configuration. Writers must be careful if invoked reentrantly.
     * - We intentionally do not propagate writer errors back to the
     *   caller — logging is best-effort.
     * - This function is conservative about stack usage: the stackbuf
     *   size (1024) is a compromise between avoiding heap use and not
     *   growing stack frames too much.
     *
     * Potential micro-optimizations (documented for future work):
     * - Avoid single allocation for the full line by using writev(2) on
     *   platforms where it's available and safe: header+msg could be
     *   written atomically to a FD. That would avoid the malloc/free
     *   for the assembled line.
     * - Support a per-thread scratch buffer to avoid frequent small
     *   heap mallocs when messages are slightly larger than stackbuf.
     */
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

    char     head[128];
    uint64_t tid  = eml_tid();
    int      hlen = G.use_ts ? snprintf(head, sizeof head, "%s %s [%llu] [%s] ", ts, lvl_str(level),
                                        (unsigned long long)tid, comp ? comp : "-")
                             : snprintf(head, sizeof head, "%s [%llu] [%s] ", lvl_str(level),
                                        (unsigned long long)tid, comp ? comp : "-");
    if(hlen < 0) hlen = 0;

    /* Build iovec for header and message, then call write_line_iov which
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

    /* write_line_iov will append the trailing newline */
    /* If total size would exceed LOG_MAX_WRITE, truncate the message
     * payload so the emitted iovec fits in a single atomic write. This
     * avoids kernel-level splitting on pipes and improves atomicity.
     * We prefer dropping tail content over calling fflush.
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
        write_line_iov(level, iov, iovcnt);
        /* emit a small warning about truncation (low verbosity):
         * "TRUNCATED: <lvl> <comp> ..."
         */
        char warnbuf[128];
        int  w = snprintf(warnbuf, sizeof warnbuf, "TRUNCATED: %s [%llu] [%s]", lvl_str(level),
                          (unsigned long long)tid, comp ? comp : "-");
        struct iovec wiov[1];
        wiov[0].iov_base = warnbuf;
        wiov[0].iov_len  = (w > 0) ? (size_t)w : 0;
        if(wiov[0].iov_len > 0) write_line_iov(level, wiov, 1);
    }
    else
    {
        write_line_iov(level, iov, iovcnt);
    }
    free(heap);
}
