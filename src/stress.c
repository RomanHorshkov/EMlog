#include "../include/emlog.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct thr_arg { int id; int messages; };

static void* thread_fn(void* _arg) {
    struct thr_arg* a = (struct thr_arg*)_arg;
    int id = a->id;
    int msgs = a->messages;
    char buf[64];
    for (int i = 0; i < msgs; ++i) {
        /* simple message formatting */
        int n = snprintf(buf, sizeof buf, "msg %d from t%d", i, id);
        if (n < 0) buf[0] = '\0';
        emlog_log(EML_LEVEL_INFO, "STR", "%s", buf);
    }
    return NULL;
}

int main(int argc, char** argv) {
    int nthreads = 10;
    int msgs = 1000;
    int enable_ts = 0;
    if (argc >= 2) nthreads = atoi(argv[1]);
    if (argc >= 3) msgs = atoi(argv[2]);
    if (argc >= 4) enable_ts = atoi(argv[3]);

    /* redirect stdout to /dev/null so logging doesn't hit console */
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);

    emlog_init(-1, enable_ts);
    emlog_set_level(EML_LEVEL_DBG);

    pthread_t* th = malloc(sizeof(pthread_t) * nthreads);
    struct thr_arg* args = malloc(sizeof(struct thr_arg) * nthreads);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < nthreads; ++i) {
        args[i].id = i;
        args[i].messages = msgs;
        if (pthread_create(&th[i], NULL, thread_fn, &args[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    for (int i = 0; i < nthreads; ++i) pthread_join(th[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec)/1e9;

    /* print timing to stderr (already redirected to /dev/null when run normally)
     * but also write to a file in /tmp so the harness can pick it up if needed.
     */
    FILE* f = fopen("/tmp/emlog_stress_result.txt", "w");
    if (f) {
        fprintf(f, "threads=%d msgs=%d elapsed=%.6f\n", nthreads, msgs, elapsed);
        fclose(f);
    }

    /* Also write the result to stdout (which the caller may have redirected to file)
     * but since we've redirected stdout to /dev/null, the primary record is the file.
     */
    printf("threads=%d msgs=%d elapsed=%.6f\n", nthreads, msgs, elapsed);

    free(th);
    free(args);
    return 0;
}
