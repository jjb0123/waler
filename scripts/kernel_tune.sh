#!/usr/bin/env bash
# Low-latency host tuning for the EPYC 7443P box.
#
#   sudo scripts/kernel_tune.sh cmdline   # write GRUB cmdline + update-grub (reboot after)
#   sudo scripts/kernel_tune.sh runtime   # after reboot: stop irqbalance, steer IRQs
#   sudo scripts/kernel_tune.sh revert    # restore the GRUB backup + update-grub
#   scripts/kernel_tune.sh status         # show what is currently in effect
#
# Isolates CCX 3 (cores 18-23 + SMT siblings 42-47) so the benchmark core
# shares its L3 with nothing but other isolated cores. Housekeeping (IRQs,
# kthreads, RCU callbacks, timers) runs on 0-17,24-41.
set -euo pipefail

ISO=18-23,42-47
HK=0-17,24-41
HK_MASK=3ff,ff03ffff     # cpus 0-17 and 24-41, in the kernel's 32-bit comma-separated form
# nohz_full=/rcu_nocbs= only take effect on the custom 5.15.222-nohz kernel
# (scripts/nohz_kernel.sh); Ubuntu's 5.15 generic is built without
# CONFIG_NO_HZ_FULL/CONFIG_RCU_NOCB_CPU and just warns about them. Don't put
# a "nohz" flag in isolcpus=: on the generic kernel that makes it discard the
# whole isolcpus= setting.
CMDLINE="isolcpus=domain,managed_irq,$ISO nohz_full=$ISO rcu_nocbs=$ISO irqaffinity=$HK idle=poll nosoftlockup nmi_watchdog=0 skew_tick=1 tsc=reliable"
GRUB=/etc/default/grub
BACKUP=$GRUB.pre-waler-tune

need_root() { [[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }; }

case ${1:-status} in
  cmdline)
    need_root
    [[ -e $BACKUP ]] || cp -a "$GRUB" "$BACKUP"
    sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"$CMDLINE\"|" "$GRUB"
    grep '^GRUB_CMDLINE_LINUX_DEFAULT' "$GRUB"
    update-grub
    echo "backup at $BACKUP; reboot to apply"
    ;;
  runtime)
    need_root
    systemctl stop irqbalance
    moved=0 pinned=()
    for f in /proc/irq/*/smp_affinity_list; do
      if echo "$HK" > "$f" 2>/dev/null; then
        moved=$((moved + 1))
      else
        pinned+=("$(basename "$(dirname "$f")")")   # managed / per-cpu IRQs
      fi
    done
    echo "$HK_MASK" > /proc/irq/default_smp_affinity
    sysctl -qw kernel.timer_migration=0 vm.stat_interval=120
    # Express (efct) RX queues buffer into 2 MB huge pages:
    # ROUNDUP(rx_ring / 1024) + 1 per queue, so 64 covers plenty of queues.
    sysctl -qw vm.nr_hugepages=64
    echo "irqbalance stopped; $moved IRQs steered to $HK"
    [[ ${#pinned[@]} -eq 0 ]] || echo "not movable: ${pinned[*]}"
    ;;
  revert)
    need_root
    [[ -e $BACKUP ]] || { echo "no backup at $BACKUP" >&2; exit 1; }
    cp -a "$BACKUP" "$GRUB"
    update-grub
    systemctl start irqbalance
    echo "restored $GRUB; reboot to apply"
    ;;
  status)
    echo "cmdline:   $(cat /proc/cmdline)"
    echo "isolated:  $(cat /sys/devices/system/cpu/isolated)"
    echo "nohz_full: $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)"
    echo "irqbalance: $(systemctl is-active irqbalance || true)"
    echo "irqs on cpu18 (non-zero counts):"
    awk 'NR==1 {for (i=1;i<=NF;i++) if ($i=="CPU18") c=i+1; next}
         c && $c+0 > 0 {d=""; for (i=c+30;i<=NF;i++) d=d" "$i; printf "  %-6s %12s %s\n", $1, $c, d}' \
        /proc/interrupts
    ;;
  *)
    echo "usage: $0 {cmdline|runtime|revert|status}" >&2; exit 1 ;;
esac
