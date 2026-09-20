/**
 * @file stress_mt.c
 * @brief EMlog multi-thread stress benchmark: STRESS_MT_THREADS threads log concurrently through one no-op writer (the
 *        contention point is EMlog's writer dispatch + per-thread timestamp cache), behind a start gate so every thread's
 *        measured loop overlaps. Reports per-thread cost samples across all runs and the aggregate wall-clock throughput per run,
 *        with the same statistics as the uuid7 matrix. Line accounting is asserted every run: threads*lines must reach the writer.
 */
#include "stress_common.h"

#include <pthread.h>

#define STRESS_MT_THREADS          8u
#define STRESS_MT_LINES_PER_THREAD 50000u
#define STRESS_MT_WARMUP_LINES     5000u
#define STRESS_MT_RUNS             5u

typedef struct
{
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    unsigned        ready;
    int             go;
} gate_t;

typedef struct
{
    size_t   id;
    size_t   lines;
    gate_t*  gate;
    uint64_t elapsed_ns;
} worker_t;

static void* worker(void* arg)
{
    worker_t* w = (worker_t*)arg;
    pthread_mutex_lock(&w->gate->lock);
    w->gate->ready++;
    pthread_cond_broadcast(&w->gate->cond);
    while(!w->gate->go) pthread_cond_wait(&w->gate->cond, &w->gate->lock);
    pthread_mutex_unlock(&w->gate->lock);

    const uint64_t t0 = stress_now_ns();
    for(size_t i = 0; i < w->lines; ++i)
    {
        EML_INFO("stress-mt", "thread %zu line %zu", w->id, i);
    }
    w->elapsed_ns = stress_now_ns() - t0;
    return NULL;
}

/* One round: spawn, gate, measure. Returns wall-clock ns of the overlapped region; fills per-thread elapsed. */
static uint64_t run_round(size_t lines, double thread_elapsed[STRESS_MT_THREADS])
{
    gate_t    gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0u, 0};
    pthread_t tid[STRESS_MT_THREADS];
    worker_t  w[STRESS_MT_THREADS];
    for(size_t i = 0; i < STRESS_MT_THREADS; ++i)
    {
        w[i] = (worker_t){.id = i, .lines = lines, .gate = &gate, .elapsed_ns = 0u};
        if(pthread_create(&tid[i], NULL, worker, &w[i]) != 0)
        {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }
    pthread_mutex_lock(&gate.lock);
    while(gate.ready < STRESS_MT_THREADS) pthread_cond_wait(&gate.cond, &gate.lock);
    const uint64_t t0 = stress_now_ns();
    gate.go = 1;
    pthread_cond_broadcast(&gate.cond);
    pthread_mutex_unlock(&gate.lock);
    for(size_t i = 0; i < STRESS_MT_THREADS; ++i) pthread_join(tid[i], NULL);
    const uint64_t wall = stress_now_ns() - t0;
    for(size_t i = 0; i < STRESS_MT_THREADS; ++i) thread_elapsed[i] = (double)w[i].elapsed_ns;
    return wall;
}

int main(void)
{
    emlog_set_writer(stress_noop_writer, NULL);
    emlog_init(EML_LEVEL_INFO, true);

    printf("emlog multi-thread stress benchmark\n");
    printf("configuration\n");
    printf("  measured runs:               %u\n", STRESS_MT_RUNS);
    printf("  threads:                     %u\n", STRESS_MT_THREADS);
    printf("  lines per thread per run:    %u\n", STRESS_MT_LINES_PER_THREAD);
    printf("  warmup lines per thread:     %u\n", STRESS_MT_WARMUP_LINES);
    printf("  total lines per measured run:%u\n", STRESS_MT_THREADS * STRESS_MT_LINES_PER_THREAD);
    printf("  writer:                      no-op (counts lines; isolates dispatch contention from I/O)\n");
    printf("  measured region:             each thread's loop of EML_INFO() calls, threads released together\n");

    double warm[STRESS_MT_THREADS];
    (void)run_round(STRESS_MT_WARMUP_LINES, warm);

    double cost_samples[STRESS_MT_RUNS * STRESS_MT_THREADS];
    double aggregate[STRESS_MT_RUNS];
    for(size_t run = 0; run < STRESS_MT_RUNS; ++run)
    {
        atomic_store_explicit(&g_stress_lines_seen, 0u, memory_order_relaxed);
        double         per_thread[STRESS_MT_THREADS];
        const uint64_t wall = run_round(STRESS_MT_LINES_PER_THREAD, per_thread);
        const uint64_t seen = atomic_load_explicit(&g_stress_lines_seen, memory_order_relaxed);
        const uint64_t want = (uint64_t)STRESS_MT_THREADS * STRESS_MT_LINES_PER_THREAD;
        printf("run %zu\n", run + 1u);
        for(size_t t = 0; t < STRESS_MT_THREADS; ++t)
        {
            const double cost = ns_per_line(STRESS_MT_LINES_PER_THREAD, (uint64_t)per_thread[t]);
            cost_samples[run * STRESS_MT_THREADS + t] = cost;
            printf("  thread %2zu  elapsed: %12.0f ns  ns/line: %9.3f  lines/s: %14.3f\n", t, per_thread[t], cost,
                   lines_per_second(STRESS_MT_LINES_PER_THREAD, (uint64_t)per_thread[t]));
        }
        aggregate[run] = lines_per_second((size_t)want, wall);
        printf("  wall-clock elapsed: %" PRIu64 " ns\n", wall);
        printf("  aggregate lines/s:  %.3f\n", aggregate[run]);
        if(seen != want)
        {
            fprintf(stderr, "line accounting mismatch in run %zu: writer saw %" PRIu64 " of %" PRIu64 "\n", run + 1u, seen, want);
            return EXIT_FAILURE;
        }
        printf("  accounting:         writer saw all %" PRIu64 " lines\n", seen);
    }
    emlog_set_writer(NULL, NULL);

    sample_summary_t s;
    printf("\nsummary\n");
    compute_sample_summary(cost_samples, STRESS_MT_RUNS * STRESS_MT_THREADS, &s);
    print_summary(stdout, "all per-thread cost samples", "ns/line", &s);
    compute_sample_summary(aggregate, STRESS_MT_RUNS, &s);
    print_summary(stdout, "aggregate throughput per run", "lines/s", &s);
    return EXIT_SUCCESS;
}
