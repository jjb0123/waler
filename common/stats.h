#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "timing.h"

/* ── Sample collector ─────────────────────────────────────────────────────── */

typedef struct {
    uint64_t *samples;  /* raw latency samples in TSC cycles */
    size_t    count;
    size_t    capacity;
} stats_t;

static inline void stats_init(stats_t *s, size_t capacity) {
    s->samples  = (uint64_t *)malloc(capacity * sizeof(uint64_t));
    s->count    = 0;
    s->capacity = capacity;
}

static inline void stats_free(stats_t *s) {
    free(s->samples);
    s->samples  = NULL;
    s->count    = s->capacity = 0;
}

/* Hot-path record: just a bounds-checked store, no arithmetic. */
static inline void stats_record(stats_t *s, uint64_t cycles) {
    if (__builtin_expect(s->count < s->capacity, 1))
        s->samples[s->count++] = cycles;
}

/* ── Results ──────────────────────────────────────────────────────────────── */

typedef struct {
    size_t n;
    double min_ns;
    double max_ns;
    double mean_ns;
    double stddev_ns;
    double p50_ns;
    double p90_ns;
    double p95_ns;
    double p99_ns;
    double p999_ns;    /* p99.9  */
    double p9999_ns;   /* p99.99 */
    double p99999_ns;  /* p99.999 */
    double jitter_ns;  /* mean |rtt[i] - rtt[i-1]|, packet delay variation */
} stats_result_t;

/* ── Compute ──────────────────────────────────────────────────────────────── */

static int _stats_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static inline double _pct(const uint64_t *sorted, size_t n, double p) {
    double idx  = p * (double)(n - 1);
    size_t lo   = (size_t)idx;
    size_t hi   = (lo + 1 < n) ? lo + 1 : lo;
    double frac = idx - (double)lo;
    return tsc_to_ns(sorted[lo]) * (1.0 - frac)
         + tsc_to_ns(sorted[hi]) * frac;
}

/* Computes all metrics.  Sorts s->samples in-place; jitter is computed first
 * so the original recording order is used for PDV. */
static void stats_compute(stats_t *s, stats_result_t *r) {
    size_t n = s->count;
    r->n = n;
    if (n == 0) return;

    /* Jitter (PDV): mean |delta| in recording order, before sort. */
    double jitter_sum = 0.0;
    for (size_t i = 1; i < n; i++) {
        int64_t d = (int64_t)s->samples[i] - (int64_t)s->samples[i - 1];
        jitter_sum += tsc_to_ns((uint64_t)(d < 0 ? -d : d));
    }
    r->jitter_ns = (n > 1) ? jitter_sum / (double)(n - 1) : 0.0;

    /* Mean and stddev in cycles to avoid repeated tsc_to_ns calls. */
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += s->samples[i];
    double mean_cyc = (double)sum / (double)n;

    double var = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)s->samples[i] - mean_cyc;
        var += d * d;
    }
    r->mean_ns   = tsc_to_ns((uint64_t)mean_cyc);
    r->stddev_ns = tsc_to_ns((uint64_t)sqrt(var / (double)n));

    qsort(s->samples, n, sizeof(uint64_t), _stats_cmp_u64);

    r->min_ns    = tsc_to_ns(s->samples[0]);
    r->max_ns    = tsc_to_ns(s->samples[n - 1]);
    r->p50_ns    = _pct(s->samples, n, 0.50);
    r->p90_ns    = _pct(s->samples, n, 0.90);
    r->p95_ns    = _pct(s->samples, n, 0.95);
    r->p99_ns    = _pct(s->samples, n, 0.99);
    r->p999_ns   = _pct(s->samples, n, 0.999);
    r->p9999_ns  = _pct(s->samples, n, 0.9999);
    r->p99999_ns = _pct(s->samples, n, 0.99999);
}

/* ── Print ────────────────────────────────────────────────────────────────── */

static void stats_print(const stats_result_t *r, FILE *f, const char *label) {
    fprintf(f, "── %s (n=%zu) ─────────────────────────────\n", label, r->n);
    fprintf(f, "  min       %8.1f ns\n", r->min_ns);
    fprintf(f, "  mean      %8.1f ns\n", r->mean_ns);
    fprintf(f, "  stddev    %8.1f ns\n", r->stddev_ns);
    fprintf(f, "  jitter    %8.1f ns  (mean |Δ| PDV)\n", r->jitter_ns);
    fprintf(f, "  p50       %8.1f ns\n", r->p50_ns);
    fprintf(f, "  p90       %8.1f ns\n", r->p90_ns);
    fprintf(f, "  p95       %8.1f ns\n", r->p95_ns);
    fprintf(f, "  p99       %8.1f ns\n", r->p99_ns);
    fprintf(f, "  p99.9     %8.1f ns\n", r->p999_ns);
    fprintf(f, "  p99.99    %8.1f ns\n", r->p9999_ns);
    fprintf(f, "  p99.999   %8.1f ns\n", r->p99999_ns);
    fprintf(f, "  max       %8.1f ns\n", r->max_ns);
}

/* ── CSV export (one row per sample, sorted order) ────────────────────────── */

static void stats_write_csv(const stats_t *s, FILE *f) {
    fprintf(f, "sample_ns\n");
    for (size_t i = 0; i < s->count; i++)
        fprintf(f, "%.1f\n", tsc_to_ns(s->samples[i]));
}
