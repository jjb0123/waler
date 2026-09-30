#!/usr/bin/env bash
# Put the two Solarflare X4 loopback ports (enp65s0f0 <-> enp65s0f1) in separate network namespaces so kernel-stack
# traffic between 10.10.10.1 and 10.10.10.2 actually crosses the cable instead
# of being short-circuited through the local routing table.
#
#   sudo scripts/netns.sh up     # create namespaces, move + address ports
#   sudo scripts/netns.sh down   # move ports back to the root namespace
#   sudo scripts/netns.sh status
#
# Run things inside with: sudo ip netns exec wexch <cmd>  /  ... wstrat <cmd>
set -euo pipefail

EXCH_IF=enp65s0f0; EXCH_NS=wexch; EXCH_IP=10.10.10.1/24
STRAT_IF=enp65s0f1; STRAT_NS=wstrat; STRAT_IP=10.10.10.2/24

up_one() {  # ifname ns cidr
  local ifn=$1 ns=$2 cidr=$3
  ip netns list | grep -qw "$ns" || ip netns add "$ns"
  if ip link show "$ifn" &>/dev/null; then
    ip link set "$ifn" netns "$ns"
  fi
  ip -n "$ns" link set lo up
  ip -n "$ns" addr flush dev "$ifn"
  ip -n "$ns" addr add "$cidr" dev "$ifn"
  # X4522-PLUS is SFP56: 50G max per port, single PAM4 lane, needs RS FEC.
  ip netns exec "$ns" ethtool -s "$ifn" speed 50000 duplex full autoneg off
  ip netns exec "$ns" ethtool --set-fec "$ifn" encoding rs
  ip -n "$ns" link set "$ifn" up
}

down_one() {  # ifname ns
  local ifn=$1 ns=$2
  if ip netns list | grep -qw "$ns"; then
    ip -n "$ns" link set "$ifn" netns 1 2>/dev/null || true
    ip netns del "$ns"
  fi
  ip link set "$ifn" up 2>/dev/null || true
}

status() {
  for ns in "$EXCH_NS" "$STRAT_NS"; do
    echo "== $ns"; ip -n "$ns" -br addr 2>/dev/null || echo "(absent)"
  done
  echo "== root"; ip -br link show | grep -E "$EXCH_IF|$STRAT_IF" || true
}

case "${1:-}" in
  up)
    up_one "$EXCH_IF" "$EXCH_NS" "$EXCH_IP"
    up_one "$STRAT_IF" "$STRAT_NS" "$STRAT_IP"
    status
    echo "== ping across the cable"
    ip netns exec "$EXCH_NS" ping -c 3 -W 1 "${STRAT_IP%/*}" || echo "no reply: check cable / carrier"
    ;;
  down)   down_one "$EXCH_IF" "$EXCH_NS"; down_one "$STRAT_IF" "$STRAT_NS"; status ;;
  status) status ;;
  *) echo "usage: $0 up|down|status" >&2; exit 1 ;;
esac
