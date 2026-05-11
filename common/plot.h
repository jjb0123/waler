#pragma once
#include <stdio.h>
#include <stdlib.h>
#include "stats.h"
#include "timing.h"

/* Writes samples to CSV then generates and runs a Python/matplotlib script
 * producing three PNGs in outdir:
 *   cdf.png        — cumulative distribution of RTT (tail latency)
 *   timeseries.png — RTT vs sample index (spot periodic spikes)
 *   histogram.png  — log-scale bucket distribution
 *
 * Requires: python3 + matplotlib + numpy (pip install matplotlib numpy)
 * Call after stats_compute() since that sorts samples in-place. */

static void plot_write_csv(const stats_t *s, const char *csv_path) {
    FILE *f = fopen(csv_path, "w");
    if (!f) { perror("plot_write_csv: fopen"); return; }
    fprintf(f, "sample_ns\n");
    for (size_t i = 0; i < s->count; i++)
        fprintf(f, "%.2f\n", tsc_to_ns(s->samples[i]));
    fclose(f);
}

static void plot_generate(const stats_t *s,
                          const char *label,
                          const char *outdir) {
    /* Write CSV alongside the plots */
    char csv_path[512];
    snprintf(csv_path, sizeof(csv_path), "%s/samples.csv", outdir);
    plot_write_csv(s, csv_path);

    /* Write the Python script to a temp file then execute it */
    char py_path[512];
    snprintf(py_path, sizeof(py_path), "%s/_plot.py", outdir);
    FILE *f = fopen(py_path, "w");
    if (!f) { perror("plot_generate: fopen"); return; }

    fprintf(f,
"import numpy as np\n"
"import matplotlib\n"
"matplotlib.use('Agg')\n"
"import matplotlib.pyplot as plt\n"
"import os, sys\n"
"\n"
"outdir = '%s'\n"
"label  = '%s'\n"
"data   = np.loadtxt(os.path.join(outdir, 'samples.csv'),\n"
"                    delimiter=',', skiprows=1)\n"
"\n"
"# ── CDF ──────────────────────────────────────────────────────────────────\n"
"fig, ax = plt.subplots(figsize=(8, 5))\n"
"sorted_data = np.sort(data)\n"
"pct = np.linspace(0, 100, len(sorted_data))\n"
"ax.plot(sorted_data, pct, linewidth=1)\n"
"for p, c in [(50,'#2196F3'),(90,'#4CAF50'),(99,'#FF9800'),(99.9,'#F44336'),(99.99,'#9C27B0')]:\n"
"    v = np.percentile(data, p)\n"
"    ax.axvline(v, linestyle='--', linewidth=0.8, color=c, label=f'p{p} = {v:.1f} ns')\n"
"ax.set_xlabel('RTT (ns)')\n"
"ax.set_ylabel('Percentile (%%)')\n"
"ax.set_title(f'{label} — CDF')\n"
"ax.legend(fontsize=8)\n"
"ax.grid(True, alpha=0.3)\n"
"fig.tight_layout()\n"
"fig.savefig(os.path.join(outdir, 'cdf.png'), dpi=150)\n"
"plt.close(fig)\n"
"\n"
"# ── Time series ──────────────────────────────────────────────────────────\n"
"fig, ax = plt.subplots(figsize=(10, 4))\n"
"ax.plot(data, linewidth=0.4, alpha=0.7)\n"
"ax.axhline(np.percentile(data, 99),  linestyle='--', linewidth=0.8,\n"
"           color='#FF9800', label='p99')\n"
"ax.axhline(np.percentile(data, 99.9), linestyle='--', linewidth=0.8,\n"
"           color='#F44336', label='p99.9')\n"
"ax.set_xlabel('Sample index')\n"
"ax.set_ylabel('RTT (ns)')\n"
"ax.set_title(f'{label} — RTT over time')\n"
"ax.legend(fontsize=8)\n"
"ax.grid(True, alpha=0.3)\n"
"fig.tight_layout()\n"
"fig.savefig(os.path.join(outdir, 'timeseries.png'), dpi=150)\n"
"plt.close(fig)\n"
"\n"
"# ── Histogram ────────────────────────────────────────────────────────────\n"
"fig, ax = plt.subplots(figsize=(8, 5))\n"
"ax.hist(data, bins=200, log=True, color='#2196F3', edgecolor='none')\n"
"ax.set_xlabel('RTT (ns)')\n"
"ax.set_ylabel('Count (log scale)')\n"
"ax.set_title(f'{label} — Histogram')\n"
"ax.grid(True, alpha=0.3, axis='y')\n"
"fig.tight_layout()\n"
"fig.savefig(os.path.join(outdir, 'histogram.png'), dpi=150)\n"
"plt.close(fig)\n"
"\n"
"print(f'Plots written to {outdir}/')\n",
        outdir, label);

    fclose(f);

    char cmd[640];
    snprintf(cmd, sizeof(cmd), "python3 %s", py_path);
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "plot_generate: python3 exited with %d\n", rc);
}
