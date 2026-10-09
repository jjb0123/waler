#!/usr/bin/env python3
"""Plot NIC egress / ingress latency distributions from x4_loopback_latency's
samples.csv.  Usage: plot_nic_latency.py <outdir>  (reads <outdir>/samples.csv)
"""
import os
import sys

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
d = np.genfromtxt(os.path.join(outdir, 'samples.csv'), delimiter=',', names=True)

# TX mode comes from x4_loopback_latency's "# tx mode = ..." line; runs from
# before CTPIO support have no such line and were all DMA.
mode = 'dma'
try:
    with open(os.path.join(outdir, 'run.txt')) as f:
        for line in f:
            if line.startswith('# tx mode = '):
                mode = line.split('=', 1)[1].strip().split(',')[0].split(' ')[0]
except OSError:
    pass
mode_label = f'tx={mode}'
if mode != 'dma' and 'tx_ctpio' in d.dtype.names:
    n_fb = int((d['tx_ctpio'] == 0).sum())
    mode_label += f', {n_fb} DMA fallbacks' if n_fb else ', no fallbacks'

# egress = TX doorbell -> wire departure, ingress = wire arrival -> DMA in
# host memory. Both need a TSC<->PHC clock fit; their sum does not.
SERIES = [
    ('egress',              'egress (doorbell → wire)',        '#2a78d6'),
    ('ingress',             'ingress (wire → host memory)',    '#d6612a'),
    ('egress_plus_ingress', 'egress + ingress (exact)',        '#3a9a4a'),
]
PCTS = [50, 90, 99, 99.9, 99.99]

lines = [f'{"":<22}{"n":>8}{"min":>9}' + ''.join(f'{"p"+str(p):>9}' for p in PCTS)
         + f'{"max":>10}{"mean":>9}   (ns)']
for key, label, _ in SERIES:
    v = d[key]
    lines.append(f'{key:<22}{len(v):>8}{v.min():>9.1f}'
                 + ''.join(f'{np.percentile(v, p):>9.1f}' for p in PCTS)
                 + f'{v.max():>10.1f}{v.mean():>9.1f}')
summary = '\n'.join(lines)
print(summary)
with open(os.path.join(outdir, 'summary.txt'), 'w') as f:
    f.write(summary + '\n')

# Clip the x range at p99.99 across all series so one outlier doesn't
# squash the body of the distribution; the tail is still in the CDF.
all_v = np.concatenate([d[k] for k, _, _ in SERIES])
lo, hi = min(0.0, all_v.min()), np.percentile(all_v, 99.99)

fig, (ax_h, ax_c) = plt.subplots(1, 2, figsize=(14, 5))

bins = np.linspace(lo, hi, 300)
for key, label, color in SERIES:
    ax_h.hist(d[key], bins=bins, histtype='step', linewidth=1.2, color=color,
              label=f'{label}  p50={np.percentile(d[key], 50):.0f}')
ax_h.set_yscale('log')
ax_h.set_xlabel('latency (ns)')
ax_h.set_ylabel('count (log)')
ax_h.set_title('histogram (x clipped at p99.99)')
ax_h.grid(True, alpha=0.3)
ax_h.legend(fontsize=8)

# Tail CDF: y = 1 - F on a log axis makes p99 / p99.9 / p99.99 readable.
for key, label, color in SERIES:
    v = np.sort(d[key])
    tail = 1.0 - np.arange(len(v)) / len(v)
    ax_c.plot(v, tail, linewidth=1.2, color=color, label=label)
for p in [99, 99.9, 99.99]:
    ax_c.axhline(1 - p / 100, linestyle=':', linewidth=0.8, color='gray')
    ax_c.text(ax_c.get_xlim()[0], 1 - p / 100, f' p{p}', fontsize=7,
              va='bottom', color='gray')
ax_c.set_yscale('log')
ax_c.set_xlabel('latency (ns)')
ax_c.set_ylabel('P(latency > x)')
ax_c.set_title('tail CDF')
ax_c.grid(True, alpha=0.3)
ax_c.legend(fontsize=8)

fig.suptitle(f'Solarflare X4 NIC egress / ingress latency (ef_vi, loopback, {mode_label})')
fig.tight_layout()
fig.savefig(os.path.join(outdir, 'nic_latency.png'), dpi=150)
print(f'wrote {outdir}/nic_latency.png, {outdir}/summary.txt')

# ── Distribution: one panel per measure, shared x, linear density ────────────
# Linear y shows the shape of the body; the log-scale plots above show the tail.
INK, INK2, GRID = '#0b0b0b', '#52514e', '#e4e3df'
DIST = [
    ('egress',              'Egress: doorbell → wire',          '#2a78d6'),
    ('ingress',             'Ingress: wire → host memory',      '#eb6834'),
    ('egress_plus_ingress', 'Egress + ingress (exact, no clock fit)', '#1baf7a'),
]
x_hi = max(np.percentile(d[k], 99.9) for k, _, _ in DIST) * 1.05
x_lo = min(d[k].min() for k, _, _ in DIST) * 0.9
bins = np.arange(x_lo, x_hi + 10, 10)          # 10 ns bins

fig, axes = plt.subplots(len(DIST), 1, figsize=(10, 7.5), sharex=True)
fig.patch.set_facecolor('#fcfcfb')
for ax, (key, title, color) in zip(axes, DIST):
    v = d[key]
    ax.set_facecolor('#fcfcfb')
    ax.hist(v, bins=bins, density=True, color=color, edgecolor='#fcfcfb',
            linewidth=0.3)
    ymax = ax.get_ylim()[1]
    # Stagger label heights so p99 / p99.9 don't collide when close.
    for (p, style), y in zip([(50, '-'), (99, '--'), (99.9, ':')], [0.97, 0.97, 0.84]):
        x = np.percentile(v, p)
        ax.axvline(x, color=INK2, linewidth=1, linestyle=style)
        ax.text(x, ymax * y, f' p{p:g} {x:.0f}', color=INK, fontsize=8,
                va='top', ha='left')
    beyond = (v > x_hi).sum()
    ax.set_title(title, loc='left', fontsize=10, color=INK)
    if beyond:
        ax.text(1.0, 1.02, f'{beyond} samples > {x_hi:.0f} ns not shown',
                transform=ax.transAxes, ha='right', va='bottom',
                fontsize=8, color=INK2)
    ax.set_yticks([])
    for s in ('top', 'right', 'left'):
        ax.spines[s].set_visible(False)
    ax.spines['bottom'].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=8)
    ax.grid(True, axis='x', color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
axes[-1].set_xlabel('latency (ns)', color=INK2)
fig.suptitle(f'NIC latency distribution, Solarflare X4 loopback '
             f'(n={len(d):,}, 10 ns bins, {mode_label})', x=0.01, ha='left', fontsize=12,
             color=INK)
fig.tight_layout()
fig.savefig(os.path.join(outdir, 'distribution.png'), dpi=150,
            facecolor=fig.get_facecolor())
print(f'wrote {outdir}/distribution.png')
