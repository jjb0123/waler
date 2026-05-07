#pragma once
#include <stdint.h>
#include <time.h>
#include <stdio.h>

/* ── TSC reading ──────────────────────────────────────────────────────────── */

/* RDTSCP serializes: prevents reorder with surrounding memory ops.
 * Use this at the boundary of each measured window. */
static inline uint64_t tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* RDTSC (non-serializing) — for hot inner loops where ordering is not needed */
static inline uint64_t tsc_read_fast(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Calibration ──────────────────────────────────────────────────────────── */

/* Global TSC frequency in Hz, set by tsc_calibrate().
 * Call once at startup before spawning any threads. */
static double g_tsc_hz = 0.0;

static void tsc_calibrate(void) {
    struct timespec t0_ts, t1_ts; // before & after
    uint64_t t0_tsc, t1_tsc; 

    clock_gettime(CLOCK_MONOTONIC, &t0_ts); // call before measurement loop 
    t0_tsc = tsc_read();

    struct timespec sleep_req = {0, 200000000}; /* 200 ms */
    nanosleep(&sleep_req, NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1_ts);
    t1_tsc = tsc_read();

    double wall_ns = (double)(t1_ts.tv_sec - t0_ts.tv_sec) * 1e9
                   + (double)(t1_ts.tv_nsec - t0_ts.tv_nsec);
    g_tsc_hz = (double)(t1_tsc - t0_tsc) / wall_ns * 1e9;
    fprintf(stderr, "TSC calibrated: %.3f GHz\n", g_tsc_hz / 1e9);
}

/* ── Conversion ───────────────────────────────────────────────────────────── */

static inline double tsc_to_ns(uint64_t cycles) {
    return (double)cycles * 1e9 / g_tsc_hz;
}

static inline uint64_t ns_to_tsc(double ns) {
    return (uint64_t)(ns * g_tsc_hz / 1e9);
}
