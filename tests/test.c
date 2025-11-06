/* extended test suite for emlog
 * - exercises all logging levels and macros
 * - verifies writer capture and restore behavior
 * - checks eml_log_errno formatting
 * - validates eml_from_errno mappings for available errno values
 * - checks long message handling
 * - asserts eml_err_to_exit mappings
 */

#include "emlog.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* capture struct used by the custom writer */
struct capture {
    char *buf;
    size_t cap;
    size_t len;
};

static ssize_t capture_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    struct capture* c = (struct capture*)user;
    if(n >= c->cap) n = c->cap - 1;
    memcpy(c->buf, line, n);
    c->buf[n] = '\0';
    c->len = n;
    return (ssize_t)n;
}

/* helper to allocate capture buffer */
static void capture_init(struct capture* c, size_t cap)
{
    c->buf = malloc(cap);
    c->cap = cap;
    c->len = 0;
}

static void capture_free(struct capture* c)
{
    free(c->buf);
    c->buf = NULL;
    c->cap = 0;
    c->len = 0;
}

static void test_levels_and_macros(void)
{
    struct capture c;
    capture_init(&c, 4096);
    emlog_set_writer(capture_writer, &c);

    /* ensure all macros emit something when level set to debug */
    emlog_set_level(EML_LEVEL_DBG);
    EML_DBG("TST", "dbg-msg %d", 1);
    assert(strstr(c.buf, "dbg-msg 1") != NULL);

    EML_INFO("TST", "info-msg %d", 2);
    assert(strstr(c.buf, "info-msg 2") != NULL);

    EML_WARN("TST", "warn-msg %d", 3);
    assert(strstr(c.buf, "warn-msg 3") != NULL);

    EML_ERROR("TST", "err-msg %d", 4);
    assert(strstr(c.buf, "err-msg 4") != NULL);

    EML_CRIT("TST", "crit-msg %d", 5);
    assert(strstr(c.buf, "crit-msg 5") != NULL);

    /* level filtering: set level to WARN -> DBG/INFO should not appear */
    emlog_set_level(EML_LEVEL_WARN);
    EML_INFO("TST", "should-not-print %d", 99);
    /* since writer overwrites buffer, ensure captured message is not the info one */
    assert(strstr(c.buf, "should-not-print") == NULL);

    emlog_set_writer(NULL, NULL); /* restore default writer */
    capture_free(&c);
}

static void test_errno_logging(void)
{
    struct capture c;
    capture_init(&c, 4096);
    emlog_set_writer(capture_writer, &c);

    errno = ENOENT;
    emlog_log_errno(EML_LEVEL_ERROR, "ERRT", errno, "open %s", "file.txt");
    assert(strstr(c.buf, "open file.txt") != NULL);
    /* strerror text should be present somewhere (platform dependent wording) */
    assert(c.len > 0);

    /* test with another errno if available */
#ifdef EACCES
    errno = EACCES;
    emlog_log_errno(EML_LEVEL_ERROR, "ERRT", errno, "access %s", "secret");
    assert(strstr(c.buf, "access secret") != NULL);
#endif

    emlog_set_writer(NULL, NULL);
    capture_free(&c);
}

static void test_eml_from_errno_mappings(void)
{
    /* Check a selection of errno values map to expected categories. Not all
     * errnos are available on every platform, so guard with #ifdef. */
#ifdef EINVAL
    assert(eml_from_errno(EINVAL) == EML_BAD_INPUT);
#endif
#ifdef ENOENT
    assert(eml_from_errno(ENOENT) == EML_NOT_FOUND);
#endif
#ifdef EACCES
    assert(eml_from_errno(EACCES) == EML_PERM);
#endif
#ifdef ENOMEM
    assert(eml_from_errno(ENOMEM) == EML_TEMP_RESOURCE);
#endif
#ifdef EIO
    assert(eml_from_errno(EIO) == EML_FATAL_IO);
#endif

    /* Unknown/rare errno should map to FATAL_BUG fallback (can't easily
     * force one portably here) */
}

static void test_long_message_handling(void)
{
    struct capture c;
    capture_init(&c, 16384);
    emlog_set_writer(capture_writer, &c);

    emlog_set_level(EML_LEVEL_DBG);
    /* create a long message > 2048 to force heap allocation path */
    size_t n = 5000;
    char* longmsg = malloc(n + 1);
    for(size_t i = 0; i < n; ++i) longmsg[i] = (i % 26) + 'a';
    longmsg[n] = '\0';

    emlog_log(EML_LEVEL_INFO, "LONG", "%s", longmsg);
    assert(c.len > 0);
    /* ensure at least a portion of message copied OR truncation was
        * performed. The logger may truncate very long messages for
        * performance (emit '...' and a TRUNCATED warning). Accept either
        * condition so tests remain robust.
        */
    if (!(strstr(c.buf, "abcd") != NULL || strstr(c.buf, "bcde") != NULL ||
                strstr(c.buf, "...") != NULL || strstr(c.buf, "TRUNCATED") != NULL))
    {
        /* if we didn't see the message content or a truncation notice,
            * fail with the captured buffer printed for debugging.
            */
        fprintf(stderr, "Captured buffer: '%s'\n", c.buf);
    }
    assert(strstr(c.buf, "abcd") != NULL || strstr(c.buf, "bcde") != NULL ||
                    strstr(c.buf, "...") != NULL || strstr(c.buf, "TRUNCATED") != NULL);

    free(longmsg);
    emlog_set_writer(NULL, NULL);
    capture_free(&c);
}

static void test_err_to_exit_and_names(void)
{
    assert(eml_err_to_exit(EML_FATAL_CONF) == EML_EXIT_CONF);
    assert(eml_err_to_exit(EML_FATAL_IO) == EML_EXIT_IO);
    assert(strcmp(eml_err_name(EML_OK), "EML_OK") == 0);
    assert(strcmp(eml_err_name(EML_FATAL_BUG), "EML_FATAL_BUG") == 0);
}

int main(void)
{
    /* initialize logger (read ENV or defaults) */
    emlog_init(-1, true);

    test_levels_and_macros();
    test_errno_logging();
    test_eml_from_errno_mappings();
    test_long_message_handling();
    test_err_to_exit_and_names();

    printf("Extended tests passed.\n");
    return 0;
}
