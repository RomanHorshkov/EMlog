/* tests/unit/test_emlog_default_writer.c
 * Covers default writev path (stdout/stderr) and flush toggle.
 */

#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <cmocka.h>
#include <string.h>

#include "emlog.h"
#include "unit_tests.h"

static ssize_t read_all(int fd, char* buf, size_t cap)
{
    size_t off = 0;
    while(off + 1 < cap)
    {
        ssize_t r = read(fd, buf + off, cap - 1 - off);
        if(r <= 0) break;
        off += (size_t)r;
    }
    buf[off] = '\0';
    return (ssize_t)off;
}

static void redirect_fd(int fd, int pipefd[2], int* saved)
{
    assert_int_equal(pipe(pipefd), 0);
    if(fd == STDOUT_FILENO)
        fflush(stdout);
    else
        fflush(stderr);
    *saved = dup(fd);
    assert_true(*saved >= 0);
    assert_true(dup2(pipefd[1], fd) >= 0);
    close(pipefd[1]);
}

static void restore_fd(int fd, int saved)
{
    assert_true(dup2(saved, fd) >= 0);
    close(saved);
}

static void assert_default_route(eml_level_t level, const char* comp, const char* msg,
                                 int fd, FILE* stream)
{
    int pipefd[2];
    int saved_fd = -1;
    redirect_fd(fd, pipefd, &saved_fd);

    emlog_set_writer(NULL, NULL);
    emlog_init(EML_LEVEL_DBG, false);
    emlog_set_writev_flush(true);
    emlog_log(level, comp, "%s", msg);

    fflush(stream);
    restore_fd(fd, saved_fd);

    char buf[512];
    ssize_t n = read_all(pipefd[0], buf, sizeof buf);
    close(pipefd[0]);
    assert_true(n > 0);
    assert_non_null(strstr(buf, msg));
    assert_non_null(strstr(buf, comp));
}

static void test_default_writer_routes_stdout(void** state)
{
    (void)state;
    assert_default_route(EML_LEVEL_INFO, "STD", "info-route", STDOUT_FILENO, stdout);
}

static void test_default_writer_routes_stderr(void** state)
{
    (void)state;
    assert_default_route(EML_LEVEL_WARN, "ERR", "warn-route", STDERR_FILENO, stderr);
}

void emlog_default_writer_stdout(void** state)
{
    test_default_writer_routes_stdout(state);
}

void emlog_default_writer_stderr(void** state)
{
    test_default_writer_routes_stderr(state);
}
