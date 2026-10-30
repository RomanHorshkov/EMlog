/**
 * @file test_emlog_dropped.c
 * @brief Black-box tests for the v1.5 loss accounting: a line a sink refuses is counted (emlog_dropped_lines) and announced by one WRN
 *        line before the next line that reaches a sink, on the default path (a full non-blocking pipe) and through a custom writer.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emlog.h"
#include "unit_tests.h"

typedef struct
{
    char lines[4][512];
    int  levels[4];
    int  count;
    int  refuse; /* answer -1 while set */
} rec_t;

static ssize_t rec_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    rec_t* r = user;
    if(r->refuse) return -1;
    if(r->count < 4)
    {
        size_t m = n < sizeof r->lines[0] - 1 ? n : sizeof r->lines[0] - 1;
        memcpy(r->lines[r->count], line, m);
        r->lines[r->count][m] = '\0';
        r->levels[r->count]   = (int)lvl;
        r->count++;
    }
    return (ssize_t)n;
}

void emlog_drain_dropped_notice(void)
{
    static rec_t sink;
    memset(&sink, 0, sizeof sink);
    emlog_set_writer(rec_writer, &sink);
    emlog_log(EML_LEVEL_CRIT, "drain", "a line that reaches the sink");
    emlog_set_writer(NULL, NULL);
}

/** A custom writer answering negative: each refused line counted, then one WRN notice with the count before the next line. */
void emlog_refused_writer_lines_counted_and_announced(void** state)
{
    (void)state;
    static rec_t r;
    memset(&r, 0, sizeof r);
    emlog_init(EML_LEVEL_DBG, false);
    emlog_drain_dropped_notice();
    const unsigned long before = emlog_dropped_lines();
    r.refuse                   = 1;
    emlog_set_writer(rec_writer, &r);
    EML_ERROR("t", "refused one");
    EML_INFO("t", "refused two");
    assert_int_equal(emlog_dropped_lines() - before, 2);
    r.refuse = 0;
    EML_ERROR("t", "accepted");
    emlog_set_writer(NULL, NULL);
    assert_int_equal(r.count, 2);
    assert_int_equal(r.levels[0], EML_LEVEL_WARN);
    assert_non_null(strstr(r.lines[0], "[emlog] 2 line(s) lost before this one"));
    assert_null(strchr(r.lines[0], '\n'));
    assert_non_null(strstr(r.lines[1], "accepted"));
    assert_int_equal(emlog_dropped_lines() - before, 2); /* monotonic: the notice does not reset it */
}

/** The default path on a full non-blocking pipe: the line waits once (20 ms), is refused and counted; after the pipe drains the next
 *  line arrives preceded by the notice, in the journal's single stream with the WRN priority prefix. */
void emlog_full_nonblocking_sink_counted_and_announced(void** state)
{
    (void)state;
    emlog_set_writer(NULL, NULL);
    emlog_init(EML_LEVEL_DBG, false);
    emlog_drain_dropped_notice();
    emlog_set_journal_mode(true);
    int p[2];
    assert_int_equal(pipe(p), 0);
    assert_int_equal(fcntl(p[0], F_SETFL, O_NONBLOCK), 0);
    assert_int_equal(fcntl(p[1], F_SETFL, O_NONBLOCK), 0);
    static char fill[4096];
    memset(fill, 'x', sizeof fill);
    while(write(p[1], fill, sizeof fill) > 0)
    {
    }
    assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    while(write(p[1], fill, 1) > 0) /* every last byte: not even a short line fits */
    {
    }
    const int saved = dup(STDERR_FILENO);
    assert_true(saved >= 0);
    assert_true(dup2(p[1], STDERR_FILENO) >= 0);

    const unsigned long before = emlog_dropped_lines();
    errno                      = ENOENT;
    EML_ERROR("t", "lost to a full pipe");
    assert_int_equal(errno, ENOENT); /* the refusal leaves the caller's errno alone */
    assert_int_equal(emlog_dropped_lines() - before, 1);

    static char drained[1 << 17];
    while(read(p[0], drained, sizeof drained) > 0)
    {
    }
    EML_ERROR("t", "after the drain");
    assert_true(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    close(p[1]);
    emlog_set_journal_mode(false);

    static char got[4096];
    ssize_t     n = read(p[0], got, sizeof got - 1);
    close(p[0]);
    assert_true(n > 0);
    got[n]           = '\0';
    const char* note = strstr(got, "<4>WRN [");
    const char* line = strstr(got, "after the drain");
    assert_non_null(note);
    assert_non_null(strstr(note, "[emlog] 1 line(s) lost before this one: the sink refused them\n"));
    assert_non_null(line);
    assert_true(note < line);
    assert_null(strstr(got, "lost to a full pipe"));
}
