#include <assert.h>
#include <emlog.h>
#include <stdio.h>
#include <string.h>

static char    g_last[256];
static ssize_t sink(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    (void)user;
    size_t cap = n < sizeof g_last - 1 ? n : sizeof g_last - 1;
    memcpy(g_last, line, cap);
    g_last[cap] = '\0';
    return (ssize_t)n;
}

int main(void)
{
    emlog_set_writer(sink, NULL);
    emlog_init(EML_LEVEL_INFO, false);
    EML_INFO("SMOKE", "installed package round-trips correctly");
    assert(strstr(g_last, "installed package round-trips correctly") != NULL);
    printf("smoke test: %s", g_last);
    return 0;
}
