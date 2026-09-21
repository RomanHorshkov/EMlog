/**
 * @file stress.c
 * @brief EMlog single-thread stress benchmark: the cost of one emlog_log() call in the three configurations that matter to a
 *        request path — level-gated (the call is a no-op), formatted + dispatched to a no-op writer (format cost, no I/O), and
 *        the default writer against /dev/null (format + real writev(2), no terminal).
 *
 * Every configuration runs STRESS_RUNS measured rounds of STRESS_LINES_PER_RUN lines after a warm-up, and reports the per-round
 * cost and throughput with the same statistics as the uuid7 stress matrix. The headline "cost per line" / "throughput" sections
 * are the no-op-writer numbers (pure library cost); the other two are reported under their own labels. Line accounting is
 * asserted for the writer-backed configurations: a line the writer never saw is a FAILED run, not a fast one.
 */
#include "stress_common.h"

#define STRESS_LINES_PER_RUN 200000u
#define STRESS_WARMUP_LINES  10000u
#define STRESS_RUNS          5u

typedef enum
{
    CFG_GATED   = 0,
    CFG_NOOP    = 1,
    CFG_DEVNULL = 2
} cfg_t;

static void configure(cfg_t cfg)
{
    switch(cfg)
    {
        case CFG_GATED:
            emlog_set_writer(stress_noop_writer, NULL);
            emlog_init(EML_LEVEL_CRIT + 1, false); /* emlog.h: "useful for benchmarks and silent operation" */
            break;
        case CFG_NOOP:
            emlog_set_writer(stress_noop_writer, NULL);
            emlog_init(EML_LEVEL_INFO, true);
            break;
        case CFG_DEVNULL:
            emlog_set_writer(NULL, NULL); /* default writer: stdout/stderr */
            emlog_init(EML_LEVEL_INFO, true);
            break;
        default:
            fprintf(stderr, "unknown configuration %d\n", (int)cfg);
            exit(EXIT_FAILURE);
    }
    atomic_store_explicit(&g_stress_lines_seen, 0u, memory_order_relaxed);
}

static uint64_t log_lines(size_t count)
{
    const uint64_t t0 = stress_now_ns();
    for(size_t i = 0; i < count; ++i)
    {
        EML_INFO("stress", "line %zu of the single-thread stress run", i);
    }
    return stress_now_ns() - t0;
}

static int run_configuration(cfg_t cfg, const char* name, const char* cost_label, const char* rate_label)
{
    int saved_stdout = -1;
    if(cfg == CFG_DEVNULL)
    {
        saved_stdout = stress_redirect_stdout_to_devnull();
        if(saved_stdout < 0)
        {
            fprintf(stderr, "stdout redirection failed\n");
            return -1;
        }
    }
    configure(cfg);
    (void)log_lines(STRESS_WARMUP_LINES);
    atomic_store_explicit(&g_stress_lines_seen, 0u, memory_order_relaxed);

    double elapsed[STRESS_RUNS], cost[STRESS_RUNS], rate[STRESS_RUNS];
    for(size_t run = 0; run < STRESS_RUNS; ++run)
    {
        const uint64_t ns = log_lines(STRESS_LINES_PER_RUN);
        elapsed[run]      = (double)ns;
        cost[run]         = ns_per_line(STRESS_LINES_PER_RUN, ns);
        rate[run]         = lines_per_second(STRESS_LINES_PER_RUN, ns);
    }
    const uint64_t seen = atomic_load_explicit(&g_stress_lines_seen, memory_order_relaxed);
    if(cfg == CFG_DEVNULL) stress_restore_stdout(saved_stdout);
    emlog_set_writer(NULL, NULL);

    printf("\nconfiguration: %s\n", name);
    for(size_t run = 0; run < STRESS_RUNS; ++run)
    {
        printf("run %2zu  elapsed: %12.0f ns  ns/line: %9.3f  lines/s: %14.3f\n", run + 1u, elapsed[run], cost[run], rate[run]);
    }
    if(cfg == CFG_NOOP)
    {
        const uint64_t expected = (uint64_t)STRESS_RUNS * STRESS_LINES_PER_RUN;
        if(seen != expected)
        {
            fprintf(stderr, "line accounting mismatch in %s: writer saw %" PRIu64 " of %" PRIu64 " lines\n", name, seen, expected);
            return -1;
        }
        printf("accounting: writer saw all %" PRIu64 " lines\n", seen);
    }
    sample_summary_t s;
    printf("\nsummary\n");
    compute_sample_summary(cost, STRESS_RUNS, &s);
    print_summary(stdout, cost_label, "ns/line", &s);
    compute_sample_summary(rate, STRESS_RUNS, &s);
    print_summary(stdout, rate_label, "lines/s", &s);
    return 0;
}

int main(void)
{
    printf("emlog single-thread stress benchmark\n");
    printf("configuration\n");
    printf("  measured runs:          %u\n", STRESS_RUNS);
    printf("  lines per run:          %u\n", STRESS_LINES_PER_RUN);
    printf("  warmup lines:           %u\n", STRESS_WARMUP_LINES);
    printf("  measured region:        loop containing only EML_INFO() calls\n");

    if(run_configuration(CFG_GATED, "level-gated (call returns before formatting)", "cost per line [level-gated]",
                         "throughput [level-gated]") != 0)
        return EXIT_FAILURE;
    if(run_configuration(CFG_DEVNULL, "default writer, stdout -> /dev/null (format + writev)", "cost per line [/dev/null sink]",
                         "throughput [/dev/null sink]") != 0)
        return EXIT_FAILURE;
    /* Headline last: the no-op writer isolates the library's own cost (format + dispatch + timestamp cache). */
    if(run_configuration(CFG_NOOP, "no-op writer (format + dispatch, no I/O)", "cost per line", "throughput") != 0) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
