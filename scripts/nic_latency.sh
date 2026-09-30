#!/usr/bin/env bash
# Measure NIC egress and ingress latency distributions on the X4 loopback
# pair with x4_loopback_latency, then plot them.
#
#   sudo scripts/nic_latency.sh [iters=100000] [core=3] [outdir]
#
# Must run as root (ef_vi + PHC access). Outputs are chowned back to the
# invoking user. If the ports are in the netns.sh
# namespaces, they are moved back to the root namespace for the run (ef_vi
# looks them up by name) and restored afterwards.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "run with sudo" >&2; exit 1; fi
OWNER=${SUDO_UID:-0}:${SUDO_GID:-0}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TX_IF=enp65s0f0
RX_IF=enp65s0f1
ITERS=${1:-100000}
CORE=${2:-3}
OUTDIR=${3:-$ROOT/benchmarks/nic/results/$(date +%Y%m%d-%H%M%S)}
BIN=$ROOT/build/bin/x4_loopback_latency

mkdir -p "$(dirname "$BIN")" "$OUTDIR"
gcc -O2 -Wall -o "$BIN" "$ROOT/x4_loopback_latency.c" -lciul1 -lm

if ip netns list | grep -qw wexch; then
  "$ROOT/scripts/netns.sh" down >/dev/null
  trap '"$ROOT/scripts/netns.sh" up >/dev/null && echo "namespaces restored"' EXIT
fi

# PHC that timestamps this port (e.g. /dev/ptp2 for the X4).
PHC_IDX=$(ethtool -T "$TX_IF" | awk -F': ' '/PTP Hardware Clock/ {print $2}')
if [[ -z "$PHC_IDX" || "$PHC_IDX" == "none" ]]; then
  echo "no PTP hardware clock for $TX_IF" >&2; exit 1
fi
PHC=/dev/ptp$PHC_IDX

echo "tx=$TX_IF rx=$RX_IF phc=$PHC iters=$ITERS core=$CORE -> $OUTDIR"
taskset -c "$CORE" "$BIN" "$TX_IF" "$RX_IF" "$PHC" "$ITERS" \
    "$OUTDIR/samples.csv" | tee "$OUTDIR/run.txt"

python3 "$ROOT/scripts/plot_nic_latency.py" "$OUTDIR"
chown -R "$OWNER" "$OUTDIR" "$BIN"
