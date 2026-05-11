import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os, sys

outdir = '.'
label  = 'kernel UDP RTT'
data   = np.loadtxt(os.path.join(outdir, 'samples.csv'),
                    delimiter=',', skiprows=1)

# ── CDF ──────────────────────────────────────────────────────────────────
fig, ax = plt.subplots(figsize=(8, 5))
sorted_data = np.sort(data)
pct = np.linspace(0, 100, len(sorted_data))
ax.plot(sorted_data, pct, linewidth=1)
for p, c in [(50,'#2196F3'),(90,'#4CAF50'),(99,'#FF9800'),(99.9,'#F44336'),(99.99,'#9C27B0')]:
    v = np.percentile(data, p)
    ax.axvline(v, linestyle='--', linewidth=0.8, color=c, label=f'p{p} = {v:.1f} ns')
ax.set_xlabel('RTT (ns)')
ax.set_ylabel('Percentile (%)')
ax.set_title(f'{label} — CDF')
ax.legend(fontsize=8)
ax.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig(os.path.join(outdir, 'cdf.png'), dpi=150)
plt.close(fig)

# ── Time series ──────────────────────────────────────────────────────────
fig, ax = plt.subplots(figsize=(10, 4))
ax.plot(data, linewidth=0.4, alpha=0.7)
ax.axhline(np.percentile(data, 99),  linestyle='--', linewidth=0.8,
           color='#FF9800', label='p99')
ax.axhline(np.percentile(data, 99.9), linestyle='--', linewidth=0.8,
           color='#F44336', label='p99.9')
ax.set_xlabel('Sample index')
ax.set_ylabel('RTT (ns)')
ax.set_title(f'{label} — RTT over time')
ax.legend(fontsize=8)
ax.grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig(os.path.join(outdir, 'timeseries.png'), dpi=150)
plt.close(fig)

# ── Histogram ────────────────────────────────────────────────────────────
fig, ax = plt.subplots(figsize=(8, 5))
ax.hist(data, bins=200, log=True, color='#2196F3', edgecolor='none')
ax.set_xlabel('RTT (ns)')
ax.set_ylabel('Count (log scale)')
ax.set_title(f'{label} — Histogram')
ax.grid(True, alpha=0.3, axis='y')
fig.tight_layout()
fig.savefig(os.path.join(outdir, 'histogram.png'), dpi=150)
plt.close(fig)

print(f'Plots written to {outdir}/')
