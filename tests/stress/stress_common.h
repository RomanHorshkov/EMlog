/**
 * @file stress_common.h
 * @brief Shared helpers for the EMlog stress benchmarks: monotonic timing, sample statistics (min/max/mean/median/stddev/CI95),
 *        the sinks a benchmark logs into, and the stdout redirection used for the "real writer" configuration.
 *
 * The statistics helpers are the same ones the uuid7 stress matrix uses, so the two libraries' reports read the same way.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef EMLOG_STRESS_COMMON_H
#define EMLOG_STRESS_COMMON_H

#include "emlog.h"

#include <fcntl.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct sample_summary
{
    double min;             /**< Smallest observed sample. */
    double max;             /**< Largest observed sample. */
    double mean;            /**< Arithmetic mean. */
    double median;          /**< Median after sorting a copy of the sample set. */
    double stddev;          /**< Sample standard deviation. */
    double coeff_var_pct;   /**< Coefficient of variation, expressed as a percentage. */
    double ci95_half_width; /**< Half-width of the approximate 95% confidence interval. */
} sample_summary_t;

/** @brief CLOCK_MONOTONIC in nanoseconds; aborts on a clock failure (a benchmark without a clock is meaningless). */
static inline uint64_t stress_now_ns(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return ((uint64_t)ts.tv_sec * UINT64_C(1000000000)) + (uint64_t)ts.tv_nsec;
}

static inline double ns_per_line(size_t line_count, uint64_t elapsed_ns)
{
    return (double)elapsed_ns / (double)line_count;
}

static inline double lines_per_second(size_t line_count, uint64_t elapsed_ns)
{
    return ((double)line_count * 1e9) / (double)elapsed_ns;
}

/** @brief Lines that reached a writer — the accounting every benchmark checks against what it sent. */
static _Atomic uint64_t g_stress_lines_seen = 0;

/** @brief A writer that costs nothing: counts the line and reports it consumed (isolates format + dispatch from I/O). */
static inline ssize_t stress_noop_writer(eml_level_t lvl, const char* line, size_t n, void* user)
{
    (void)lvl;
    (void)line;
    (void)user;
    atomic_fetch_add_explicit(&g_stress_lines_seen, 1u, memory_order_relaxed);
    return (ssize_t)n;
}

/**
 * @brief Point stdout (the default writer's INFO sink) at /dev/null for the "real writer" configuration.
 * @return the saved stdout fd to hand back to stress_restore_stdout(), or -1 on failure.
 */
static inline int stress_redirect_stdout_to_devnull(void)
{
    fflush(stdout);
    const int saved = dup(STDOUT_FILENO);
    if(saved < 0) return -1;
    const int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if(nul < 0)
    {
        close(saved);
        return -1;
    }
    /* dup2() onto STDOUT_FILENO returns that same fd, not a new one. GCC's -fanalyzer models the
     * return as a freshly opened descriptor that must be closed (a known false positive for dup2 to
     * a standard fd); closing it would close stdout. Scoped suppression with that reason. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
    if(dup2(nul, STDOUT_FILENO) < 0)
    {
        close(nul);
        close(saved);
        return -1;
    }
#pragma GCC diagnostic pop
    close(nul);
    return saved;
}

/** @brief Point any fd at /dev/null; returns the saved duplicate or -1. */
static inline int stress_redirect_fd_to_devnull(int fd)
{
    fflush(NULL);
    const int saved = dup(fd);
    if(saved < 0) return -1;
    const int nul = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if(nul < 0)
    {
        close(saved);
        return -1;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
    if(dup2(nul, fd) < 0)
    {
        close(nul);
        close(saved);
        return -1;
    }
#pragma GCC diagnostic pop
    close(nul);
    return saved;
}

static inline void stress_restore_fd(int fd, int saved)
{
    fflush(NULL);
    if(saved >= 0)
    {
        dup2(saved, fd);
        close(saved);
    }
}

static inline void stress_restore_stdout(int saved)
{
    fflush(stdout);
    if(saved >= 0)
    {
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }
}

/**
 * @brief qsort() comparator for ascending double samples.
 */
static inline int compare_double(const void* a, const void* b)
{
    const double da = *(const double*)a;
    const double db = *(const double*)b;

    return (da > db) - (da < db);
}

/**
 * @brief Compute min, max, mean, median, standard deviation, coefficient of variation, and approximate 95% confidence interval.
 */
static inline void compute_sample_summary(const double* samples, size_t sample_count, sample_summary_t* out)
{
    if(sample_count == 0u)
    {
        fprintf(stderr, "compute_sample_summary requires at least one sample\n");
        exit(EXIT_FAILURE);
    }

    double* sorted = malloc(sample_count * sizeof(*sorted));
    if(!sorted)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    double sum = 0.0;
    out->min   = samples[0];
    out->max   = samples[0];

    for(size_t i = 0; i < sample_count; ++i)
    {
        const double value = samples[i];

        sorted[i]  = value;
        sum       += value;

        if(value < out->min) out->min = value;
        if(value > out->max) out->max = value;
    }

    out->mean = sum / (double)sample_count;

    double variance_acc = 0.0;
    for(size_t i = 0; i < sample_count; ++i)
    {
        const double delta  = samples[i] - out->mean;
        variance_acc       += delta * delta;
    }

    if(sample_count > 1u)
    {
        out->stddev          = sqrt(variance_acc / (double)(sample_count - 1u));
        out->ci95_half_width = 1.96 * (out->stddev / sqrt((double)sample_count));
    }
    else
    {
        out->stddev          = 0.0;
        out->ci95_half_width = 0.0;
    }

    out->coeff_var_pct = (fabs(out->mean) > DBL_EPSILON) ? ((out->stddev / out->mean) * 100.0) : 0.0;

    qsort(sorted, sample_count, sizeof(*sorted), compare_double);
    if((sample_count & 1u) != 0u)
    {
        out->median = sorted[sample_count / 2u];
    }
    else
    {
        const size_t hi = sample_count / 2u;
        const size_t lo = hi - 1u;
        out->median     = (sorted[lo] + sorted[hi]) / 2.0;
    }

    free(sorted);
}

/**
 * @brief Print a formatted benchmark summary to @p out.
 */
static inline void print_summary(FILE* out, const char* label, const char* unit, const sample_summary_t* summary)
{
    fprintf(out, "%s\n", label);
    fprintf(out, "  min:            %.3f %s\n", summary->min, unit);
    fprintf(out, "  max:            %.3f %s\n", summary->max, unit);
    fprintf(out, "  mean:           %.3f %s\n", summary->mean, unit);
    fprintf(out, "  median:         %.3f %s\n", summary->median, unit);
    fprintf(out, "  stddev:         %.3f %s\n", summary->stddev, unit);
    fprintf(out, "  coeff var:      %.3f %%\n", summary->coeff_var_pct);
    fprintf(out, "  95%% CI +/-:     %.3f %s\n", summary->ci95_half_width, unit);
}

#endif /* EMLOG_STRESS_COMMON_H */
