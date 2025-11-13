/* tests/unit/test_emlog_errors.c
 * Exercises error-conversion helpers and emlog_log_errno().
 */

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>

#include "emlog.h"
#include "unit_tests.h"

struct capture
{
    char*  buf;
    size_t cap;
    size_t len;
};

static ssize_t capture_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    struct capture* c       = (struct capture*)user;
    size_t          avail   = (c->cap > 0) ? c->cap - 1 - c->len : 0;
    size_t          to_copy = (n < avail) ? n : avail;
    if(to_copy > 0)
    {
        memcpy(c->buf + c->len, line, to_copy);
        c->len         += to_copy;
        c->buf[c->len]  = '\0';
    }
    return (ssize_t)n;
}

static void test_eml_from_errno_mappings(void** state)
{
    (void)state;
    assert_int_equal(eml_from_errno(0), EML_OK);
    assert_int_equal(eml_from_errno(EINTR), EML_TRY_AGAIN);
    assert_int_equal(eml_from_errno(EAGAIN), EML_TRY_AGAIN);
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
    assert_int_equal(eml_from_errno(EWOULDBLOCK), EML_TRY_AGAIN);
#endif
    assert_int_equal(eml_from_errno(EMFILE), EML_TEMP_RESOURCE);
    assert_int_equal(eml_from_errno(ENFILE), EML_TEMP_RESOURCE);
    assert_int_equal(eml_from_errno(ENOMEM), EML_TEMP_RESOURCE);
    assert_int_equal(eml_from_errno(EBUSY), EML_TEMP_UNAVAILABLE);
#if defined(ENETDOWN)
    assert_int_equal(eml_from_errno(ENETDOWN), EML_TEMP_UNAVAILABLE);
#endif
#if defined(ENETUNREACH)
    assert_int_equal(eml_from_errno(ENETUNREACH), EML_TEMP_UNAVAILABLE);
#endif
    assert_int_equal(eml_from_errno(ENOENT), EML_NOT_FOUND);
    assert_int_equal(eml_from_errno(ESRCH), EML_NOT_FOUND);
    assert_int_equal(eml_from_errno(EINVAL), EML_BAD_INPUT);
#if defined(EPROTO)
    assert_int_equal(eml_from_errno(EPROTO), EML_BAD_INPUT);
#endif
#if defined(EBADMSG)
    assert_int_equal(eml_from_errno(EBADMSG), EML_BAD_INPUT);
#endif
    assert_int_equal(eml_from_errno(EACCES), EML_PERM);
    assert_int_equal(eml_from_errno(EPERM), EML_PERM);
    assert_int_equal(eml_from_errno(EEXIST), EML_CONFLICT);
#if defined(EADDRINUSE)
    assert_int_equal(eml_from_errno(EADDRINUSE), EML_CONFLICT);
#endif
    assert_int_equal(eml_from_errno(EIO), EML_FATAL_IO);
    assert_int_equal(eml_from_errno(ENOSPC), EML_FATAL_IO);
    assert_int_equal(eml_from_errno(EPIPE), EML_FATAL_BUG);
}

static void test_eml_err_name_strings(void** state)
{
    (void)state;
    struct
    {
        eml_err_t    err;
        const char*  name;
    } cases[] = {
        {EML_OK, "EML_OK"},                 {EML_TRY_AGAIN, "EML_TRY_AGAIN"},
        {EML_TEMP_RESOURCE, "EML_TEMP_RESOURCE"},
        {EML_TEMP_UNAVAILABLE, "EML_TEMP_UNAVAILABLE"},
        {EML_BAD_INPUT, "EML_BAD_INPUT"},   {EML_NOT_FOUND, "EML_NOT_FOUND"},
        {EML_PERM, "EML_PERM"},             {EML_CONFLICT, "EML_CONFLICT"},
        {EML_FATAL_CONF, "EML_FATAL_CONF"}, {EML_FATAL_IO, "EML_FATAL_IO"},
        {EML_FATAL_CRYPTO, "EML_FATAL_CRYPTO"},
        {EML_FATAL_BUG, "EML_FATAL_BUG"},   {EML__COUNT, "EML__COUNT"},
    };

    for(size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        assert_string_equal(eml_err_name(cases[i].err), cases[i].name);

    assert_string_equal(eml_err_name((eml_err_t)9999), "EML_UNKNOWN");
}

static void test_eml_err_to_exit_codes(void** state)
{
    (void)state;
    assert_int_equal(eml_err_to_exit(EML_OK), EML_EXIT_OK);
    assert_int_equal(eml_err_to_exit(EML_TRY_AGAIN), EML_EXIT_OK);
    assert_int_equal(eml_err_to_exit(EML_TEMP_UNAVAILABLE), EML_EXIT_OK);
    assert_int_equal(eml_err_to_exit(EML_BAD_INPUT), EML_EXIT_OK);
    assert_int_equal(eml_err_to_exit(EML_NOT_FOUND), EML_EXIT_OK);
    assert_int_equal(eml_err_to_exit(EML_PERM), EML_EXIT_OK);
    assert_int_equal(eml_err_to_exit(EML_CONFLICT), EML_EXIT_OK);
    assert_int_equal(eml_err_to_exit(EML_FATAL_CRYPTO), EML_EXIT_CONF);
    assert_int_equal(eml_err_to_exit(EML_FATAL_CONF), EML_EXIT_CONF);
    assert_int_equal(eml_err_to_exit(EML_FATAL_IO), EML_EXIT_IO);
    assert_int_equal(eml_err_to_exit(EML_TEMP_RESOURCE), EML_EXIT_MEM);
    assert_int_equal(eml_err_to_exit(EML_FATAL_BUG), EML_EXIT_BUG);
    assert_int_equal(eml_err_to_exit(EML__COUNT), EML_EXIT_BUG);
    assert_int_equal(eml_err_to_exit((eml_err_t)1234), EML_EXIT_OK);
}

static void test_emlog_log_errno_includes_context(void** state)
{
    (void)state;
    struct capture c = {.cap = 2048};
    c.buf            = calloc(1, c.cap);
    assert_non_null(c.buf);
    emlog_set_writer(capture_writer, &c);
    emlog_set_level(EML_LEVEL_DBG);
    emlog_enable_timestamps(false);

    const char* arg = "config.yaml";
    emlog_log_errno(EML_LEVEL_ERROR, "ERR", ENOENT, "failed to open %s", arg);

    const char* errstr = strerror(ENOENT);
    assert_non_null(errstr);
    assert_non_null(strstr(c.buf, "failed to open"));
    assert_non_null(strstr(c.buf, arg));
    assert_non_null(strstr(c.buf, errstr));
    assert_non_null(strstr(c.buf, "(2)"));

    emlog_set_writer(NULL, NULL);
    free(c.buf);
}

/* cmocka wrappers */
void emlog_error_from_errno(void** state)
{
    test_eml_from_errno_mappings(state);
}

void emlog_error_name_strings(void** state)
{
    test_eml_err_name_strings(state);
}

void emlog_error_exit_codes(void** state)
{
    test_eml_err_to_exit_codes(state);
}

void emlog_log_errno_captures_context(void** state)
{
    test_emlog_log_errno_includes_context(state);
}
