/**
 * @file integration_test.c
 * @brief Multi-thread integration test for EMlog writer dispatch and line accounting.
 */

#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "emlog.h"

/* Integration-test parameters (no CLI args on purpose). */
enum
{
    IT_THREADS   = 10,
    IT_MESSAGES  = 1000,
    IT_ENABLE_TS = 0,
    IT_LOG_LEVEL = EML_LEVEL_INFO,
};

struct thr_arg
{
    int id;
    int messages;
};

static atomic_ulong g_lines = 0;

static ssize_t sink_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    (void)line;
    (void)user;
    atomic_fetch_add_explicit(&g_lines, 1, memory_order_relaxed);
    return (ssize_t)n;
}

static void* thread_fn(void* _arg)
{
    struct thr_arg* a    = (struct thr_arg*)_arg;
    int             id   = a->id;
    int             msgs = a->messages;
    char            buf[64];
    for(int i = 0; i < msgs; ++i)
    {
        /* simple message formatting */
        int n = snprintf(buf, sizeof buf, "msg %d from t%d", i, id);
        if(n < 0) buf[0] = '\0';
        emlog_log(EML_LEVEL_INFO, "STR", "%s", buf);
    }
    return NULL;
}

int main(void)
{
    const int nthreads  = IT_THREADS;
    const int msgs      = IT_MESSAGES;
    const int enable_ts = IT_ENABLE_TS;

    emlog_set_writer(sink_writer, NULL);
    emlog_init(-1, enable_ts);
    emlog_set_level((eml_level_t)IT_LOG_LEVEL);

    /* emlog_init() may emit informational logs; don't count those as workload. */
    atomic_store_explicit(&g_lines, 0, memory_order_relaxed);

    pthread_t*      th   = malloc(sizeof(pthread_t) * (size_t)nthreads);
    struct thr_arg* args = malloc(sizeof(struct thr_arg) * (size_t)nthreads);
    if(!th || !args)
    {
        free(th);
        free(args);
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for(int i = 0; i < nthreads; ++i)
    {
        args[i].id       = i;
        args[i].messages = msgs;
        if(pthread_create(&th[i], NULL, thread_fn, &args[i]) != 0)
        {
            perror("pthread_create");
            return 1;
        }
    }

    for(int i = 0; i < nthreads; ++i)
        pthread_join(th[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    const unsigned long expected = (unsigned long)nthreads * (unsigned long)msgs;
    const unsigned long seen     = atomic_load_explicit(&g_lines, memory_order_relaxed);

    FILE* f = fopen("tests/results/ITs/integration_result.txt", "w");
    if(f)
    {
        fprintf(f, "threads=%d msgs=%d elapsed=%.6f lines=%lu expected=%lu\n", nthreads, msgs, elapsed, seen, expected);
        fclose(f);
    }

    printf("threads=%d msgs=%d elapsed=%.6f lines=%lu expected=%lu pid=%d\n", nthreads, msgs, elapsed, seen, expected, (int)getpid());
    if(seen != expected)
    {
        fprintf(stderr, "line count mismatch: got %lu expected %lu\n", seen, expected);
        free(th);
        free(args);
        return 2;
    }

    free(th);
    free(args);
    return 0;
}
