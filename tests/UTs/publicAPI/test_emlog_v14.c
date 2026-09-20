/**
 * @file test_emlog_v14.c
 * @brief Black-box tests for the v1.4 guarantees: errno transparency, control-byte sanitizing, journald single-stream priority mode,
 *        the TRUNCATED notice's header, and line integrity on the lock-free default path under concurrent writers.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emlog.h"
#include "unit_tests.h"

typedef struct
{
    char   lines[8][4200];
    size_t count;
} cap_t;

static ssize_t cap_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    cap_t* c = (cap_t*)user;
    if(c->count < 8)
    {
        size_t m = n < sizeof c->lines[0] - 1 ? n : sizeof c->lines[0] - 1;
        memcpy(c->lines[c->count], line, m);
        c->lines[c->count][m] = '\0';
        c->count++;
    }
    return (ssize_t)n;
}

static ssize_t clobbering_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl; (void)line; (void)user;
    errno = EBADF; /* a failing sink must not leak its errno to the logging caller */
    return -1;
}

static int redirect_to_pipe(int fd, int* saved)
{
    int p[2];
    assert_int_equal(pipe(p), 0);
    *saved = dup(fd);
    assert_true(*saved >= 0);
    assert_true(dup2(p[1], fd) >= 0);
    close(p[1]);
    return p[0];
}
static void restore(int fd, int saved)
{
    assert_true(dup2(saved, fd) >= 0);
    close(saved);
}
static size_t read_all(int rfd, char* buf, size_t cap)
{
    size_t  off = 0;
    ssize_t r;
    while(off < cap - 1 && (r = read(rfd, buf + off, cap - 1 - off)) > 0) off += (size_t)r;
    buf[off] = '\0';
    return off;
}

static void test_errno_preserved_custom_writer(void** state)
{
    (void)state;
    emlog_init(EML_LEVEL_DBG, true);
    emlog_set_writer(clobbering_writer, NULL);
    errno = ENOENT;
    EML_ERROR("t", "the caller is about to return -errno");
    assert_int_equal(errno, ENOENT);
    errno = EAGAIN;
    emlog_log_errno(EML_LEVEL_WARN, "t", EIO, "with errno text");
    assert_int_equal(errno, EAGAIN);
    errno = EPERM;
    EML_DBG("t", "still preserved");
    assert_int_equal(errno, EPERM);
    emlog_set_writer(NULL, NULL);
}

static void test_errno_preserved_default_path_failed_write(void** state)
{
    (void)state;
    emlog_set_writer(NULL, NULL);
    emlog_init(EML_LEVEL_DBG, true);
    emlog_set_journal_mode(false);
    /* stdout -> a READ-ONLY descriptor: writev fails with EBADF, no SIGPIPE involved */
    int saved = dup(STDOUT_FILENO);
    int ro    = open("/dev/null", O_RDONLY);
    assert_true(saved >= 0 && ro >= 0);
    assert_true(dup2(ro, STDOUT_FILENO) >= 0);
    close(ro);
    errno = ENOENT;
    EML_INFO("t", "this write fails inside the logger");
    int after = errno;
    assert_true(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    assert_int_equal(after, ENOENT);
}

static void test_control_bytes_sanitized(void** state)
{
    (void)state;
    cap_t c = {.count = 0};
    emlog_init(EML_LEVEL_DBG, false);
    emlog_set_writer(cap_writer, &c);
    EML_WARN("upload", "name: %s", "invoice.pdf\nERR [1] [auth] forged\r\x7f\tend");
    emlog_set_writer(NULL, NULL);
    assert_int_equal(c.count, 1);
    assert_null(strchr(c.lines[0], '\n'));
    assert_null(strchr(c.lines[0], '\r'));
    assert_null(strchr(c.lines[0], '\x7f'));
    assert_null(strchr(c.lines[0], '\t'));
    assert_non_null(strstr(c.lines[0], "invoice.pdf?ERR [1] [auth] forged???end"));
}

static void test_journal_mode_single_stream_with_priority(void** state)
{
    (void)state;
    emlog_set_writer(NULL, NULL);
    emlog_init(EML_LEVEL_DBG, false);
    emlog_set_journal_mode(true);
    emlog_set_writev_flush(true);
    int so, se;
    int rout = redirect_to_pipe(STDOUT_FILENO, &so);
    int rerr = redirect_to_pipe(STDERR_FILENO, &se);
    EML_INFO("j", "info line");
    EML_WARN("j", "warn line");
    EML_ERROR("j", "error line");
    EML_DBG("j", "debug line");
    restore(STDOUT_FILENO, so);
    restore(STDERR_FILENO, se);
    emlog_set_journal_mode(false);
    char   out[4096], err[4096];
    size_t nout = read_all(rout, out, sizeof out);
    size_t nerr = read_all(rerr, err, sizeof err);
    close(rout);
    close(rerr);
    assert_int_equal(nout, 0); /* ONE stream: nothing on stdout, even for INFO/DBG */
    assert_true(nerr > 0);
    assert_non_null(strstr(err, "<6>INF"));
    assert_non_null(strstr(err, "<4>WRN"));
    assert_non_null(strstr(err, "<3>ERR"));
    assert_non_null(strstr(err, "<7>DBG"));
    assert_true(strstr(err, "<6>INF") < strstr(err, "<4>WRN") && strstr(err, "<4>WRN") < strstr(err, "<3>ERR"));
}

static void test_journal_mode_autodetected_from_env(void** state)
{
    (void)state;
    emlog_set_writer(NULL, NULL);
    setenv("JOURNAL_STREAM", "9:12345", 1);
    int so, se;
    int rout = redirect_to_pipe(STDOUT_FILENO, &so);
    int rerr = redirect_to_pipe(STDERR_FILENO, &se);
    emlog_init(EML_LEVEL_INFO, false); /* init logs one INFO line: it must already land on the journal stream */
    EML_INFO("j", "after init");
    restore(STDOUT_FILENO, so);
    restore(STDERR_FILENO, se);
    unsetenv("JOURNAL_STREAM");
    char   out[4096], err[4096];
    size_t nout = read_all(rout, out, sizeof out);
    read_all(rerr, err, sizeof err);
    close(rout);
    close(rerr);
    assert_int_equal(nout, 0);
    assert_non_null(strstr(err, "<6>INF"));
    assert_non_null(strstr(err, "journal=stream"));
    rout = redirect_to_pipe(STDOUT_FILENO, &so);
    emlog_init(EML_LEVEL_INFO, false);
    restore(STDOUT_FILENO, so);
    nout = read_all(rout, out, sizeof out);
    close(rout);
    assert_true(nout > 0);
    assert_null(strstr(out, "<6>"));
    assert_non_null(strstr(out, "journal=off"));
}

static void test_truncation_notice_has_header(void** state)
{
    (void)state;
    cap_t c = {.count = 0};
    emlog_init(EML_LEVEL_DBG, true);
    emlog_set_writer(cap_writer, &c);
    char big[9000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    EML_INFO("big", "%s", big);
    emlog_set_writer(NULL, NULL);
    assert_int_equal(c.count, 2);
    size_t l0 = strlen(c.lines[0]);
    assert_true(l0 <= 4095);
    assert_string_equal(c.lines[0] + l0 - 3, "...");
    assert_non_null(strstr(c.lines[1], "TRUNCATED"));
    assert_non_null(strstr(c.lines[1], " INF ["));
    assert_non_null(strstr(c.lines[1], "[big]"));
    assert_true(c.lines[1][4] == '-' && c.lines[1][7] == '-'); /* ISO date first: timestamped like every other line */
}

#define CT_THREADS 6
#define CT_LINES   1500
static void* ct_worker(void* arg)
{
    long id = (long)arg;
    for(int i = 0; i < CT_LINES; ++i) EML_INFO("ct", "thread %ld line %d payload-%s", id, i, "0123456789abcdef0123456789abcdef");
    return NULL;
}
static void* ct_reader(void* arg)
{
    int*  fds  = (int*)arg;
    FILE* f    = fdopen(fds[0], "r");
    long  good = 0, bad = 0;
    char  line[8192];
    while(fgets(line, sizeof line, f))
    {
        size_t n = strlen(line);
        if(n > 0 && line[n - 1] == '\n' && strncmp(line, "<6>INF [", 8) == 0 && strstr(line, "] [ct] thread ") &&
           strstr(line, " payload-0123456789abcdef0123456789abcdef"))
            good++;
        else if(strstr(line, "Initialized emlog") == NULL)
            bad++;
    }
    fclose(f);
    long* out = malloc(2 * sizeof(long));
    out[0]    = good;
    out[1]    = bad;
    return out;
}
static void test_concurrent_default_path_lines_intact(void** state)
{
    (void)state;
    emlog_set_writer(NULL, NULL);
    emlog_init(EML_LEVEL_INFO, false);
    emlog_set_journal_mode(true); /* single stream: one pipe to check */
    int       se;
    int       rerr   = redirect_to_pipe(STDERR_FILENO, &se);
    int       fds[1] = {rerr};
    pthread_t reader;
    assert_int_equal(pthread_create(&reader, NULL, ct_reader, fds), 0);
    pthread_t th[CT_THREADS];
    for(long i = 0; i < CT_THREADS; ++i) assert_int_equal(pthread_create(&th[i], NULL, ct_worker, (void*)i), 0);
    for(int i = 0; i < CT_THREADS; ++i) pthread_join(th[i], NULL);
    restore(STDERR_FILENO, se); /* closes the pipe's write end: the reader sees EOF */
    long* res = NULL;
    pthread_join(reader, (void**)&res);
    emlog_set_journal_mode(false);
    assert_non_null(res);
    assert_int_equal(res[1], 0);                           /* no torn or interleaved line */
    assert_int_equal(res[0], (long)CT_THREADS * CT_LINES); /* every line arrived whole */
    free(res);
}

void emlog_errno_preserved_custom_writer(void** state) { test_errno_preserved_custom_writer(state); }
void emlog_errno_preserved_default_path_failed_write(void** state) { test_errno_preserved_default_path_failed_write(state); }
void emlog_control_bytes_sanitized(void** state) { test_control_bytes_sanitized(state); }
void emlog_journal_mode_single_stream_with_priority(void** state) { test_journal_mode_single_stream_with_priority(state); }
void emlog_journal_mode_autodetected_from_env(void** state) { test_journal_mode_autodetected_from_env(state); }
void emlog_truncation_notice_has_header(void** state) { test_truncation_notice_has_header(state); }
void emlog_concurrent_default_path_lines_intact(void** state) { test_concurrent_default_path_lines_intact(state); }
