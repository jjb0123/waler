#!/usr/bin/env python3
"""Overlay NIC latency distributions from several x4_loopback_latency runs,
e.g. DMA vs CTPIO.

  compare_nic_latency.py [-o out.png] <outdir> <outdir> ...

Each <outdir> is a results directory from nic_latency.sh (samples.csv +
run.txt). The first run is the baseline for the delta column.

CTPIO only changes the TX side, so the thing to read is egress. Egress needs
the TSC<->PHC clock fit; egress+ingress does not, and since ingress is
unchanged between modes, the shift in egress+ingress is an exact measure of
what CTPIO saves.
"""
import argparse
import os

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument('-o', '--out', default='nic_compare.png')
ap.add_argument('outdirs', nargs='+')
args = ap.parse_args()


def run_label(outdir):
    """TX mode from run.txt; older runs without the line were DMA."""
    label = 'dma'
    express = False
    try:
        with open(os.path.join(outdir, 'run.txt')) as f:
            for line in f:
                if line.startswith('# tx mode = '):
                    label = line.split('=', 1)[1].strip()
                    label = label.replace(', ct_threshold = ', ' ct=').replace(' bytes', '')
                    label = label.split(' (')[0]
                elif line.startswith('# datapath = express'):
                    express = True
    except OSError:
        pass
    return label + ' express' if express else label


runs = []
for outdir in args.outdirs:
    d = np.genfromtxt(os.path.join(outdir, 'samples.csv'), delimiter=',', names=True)
    label = run_label(outdir)
    if 'tx_ctpio' in d.dtype.names and not label.startswith('dma'):
        n_fb = int((d['tx_ctpio'] == 0).sum())
        if n_fb:
            label += f' ({n_fb} fallbacks)'
    runs.append((label, d))

SERIES = [
    ('egress',              'Egress: doorbell → wire (clock fit)'),
    ('egress_plus_ingress', 'Egress + ingress (exact)'),
    ('ingress',             'Ingress: wire → host memory (should not move)'),
]
PCTS = [50, 90, 99, 99.9, 99.99]
COLORS = ['#2a78d6', '#eb6834', '#1baf7a', '#8a5cd6', '#d6a12a']

# ── Table ────────────────────────────────────────────────────────────────────
lines = []
for key, title in SERIES:
    lines.append(title)
    lines.append(f'  {"run":<28}' + ''.join(f'{"p" + str(p):>9}' for p in PCTS)
                 + f'{"Δp50":>9}{"Δp99":>9}')
    base = runs[0][1][key]
    for label, d in runs:
        v = d[key]
        dp50 = np.percentile(v, 50) - np.percentile(base, 50)
        dp99 = np.percentile(v, 99) - np.percentile(base, 99)
        lines.append(f'  {label:<28}'
                     + ''.join(f'{np.percentile(v, p):>9.1f}' for p in PCTS)
                     + f'{dp50:>+9.1f}{dp99:>+9.1f}')
    lines.append('')
table = '\n'.join(lines)
print(table)
with open(os.path.splitext(args.out)[0] + '.txt', 'w') as f:
    f.write(table)

# ── Plot: histogram (body) + tail CDF per series ─────────────────────────────
fig, axes = plt.subplots(len(SERIES), 2, figsize=(14, 3.2 * len(SERIES)))
for row, (key, title) in zip(axes, SERIES):
    ax_h, ax_c = row
    all_v = np.concatenate([d[key] for _, d in runs])
    lo, hi = all_v.min() * 0.9, np.percentile(all_v, 99.9) * 1.05
    bins = np.arange(lo, hi + 10, 10)
    for (label, d), color in zip(runs, COLORS):
        v = d[key]
        ax_h.hist(v, bins=bins, density=True, histtype='step', linewidth=1.3,
                  color=color, label=f'{label}  p50={np.percentile(v, 50):.0f}')
        s = np.sort(v)
        ax_c.plot(s, 1.0 - np.arange(len(s)) / len(s), linewidth=1.2,
                  color=color, label=label)
    ax_h.set_title(title, loc='left', fontsize=10)
    ax_h.set_yticks([])
    ax_h.legend(fontsize=8)
    ax_h.grid(True, axis='x', alpha=0.3)
    ax_c.set_yscale('log')
    ax_c.set_ylabel('P(latency > x)', fontsize=8)
    for p in [99, 99.9, 99.99]:
        ax_c.axhline(1 - p / 100, linestyle=':', linewidth=0.8, color='gray')
    ax_c.grid(True, alpha=0.3)
    ax_c.legend(fontsize=8)
axes[-1][0].set_xlabel('latency (ns, 10 ns bins)')
axes[-1][1].set_xlabel('latency (ns)')
fig.suptitle('Solarflare X4 loopback: NIC latency by TX mode', x=0.01, ha='left')
fig.tight_layout()
fig.savefig(args.out, dpi=150)
print(f'wrote {args.out}, {os.path.splitext(args.out)[0]}.txt')
