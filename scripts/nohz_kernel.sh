#!/usr/bin/env bash
# Install and manage the custom 5.15.222-nohz kernel (CONFIG_NO_HZ_FULL +
# CONFIG_RCU_NOCB_CPU), built from ~/kbuild with the Onload 9.1.1 modules.
#
#   sudo scripts/nohz_kernel.sh install   # install, boot it ONCE on next reboot
#   sudo scripts/nohz_kernel.sh promote   # make it the default kernel
#   sudo scripts/nohz_kernel.sh demote    # make 5.15.0-73-generic the default again
#   sudo scripts/nohz_kernel.sh remove    # uninstall it
#
# install leaves 5.15.0-73-generic as the GRUB default and uses grub-reboot, so
# if the new kernel fails to come up a power cycle returns to the old one.
set -euo pipefail

KVER=5.15.222-nohz
BUILD=${BUILD:-/home/${SUDO_USER:-$USER}/kbuild}
ONLOAD_KO=$BUILD/onload/build/x86_64_linux-$KVER/driver/linux
OLD="Advanced options for Ubuntu>Ubuntu, with Linux 5.15.0-73-generic"
NEW="Advanced options for Ubuntu>Ubuntu, with Linux $KVER"

[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }

case ${1:-} in
  install)
    # linux-libc-dev is deliberately skipped: it would replace the system's
    # userspace kernel headers.
    dpkg -i "$BUILD"/linux-image-${KVER}_*.deb "$BUILD"/linux-headers-${KVER}_*.deb
    install -d /lib/modules/$KVER/extra
    install -m 644 "$ONLOAD_KO"/{sfc,sfc_char,sfc_resource,onload}.ko \
        /lib/modules/$KVER/extra/
    depmod -a $KVER
    # dpkg -i already built the initramfs, with the in-tree sfc (no X4
    # support); rebuild it so early boot loads the Onload sfc instead.
    update-initramfs -u -k $KVER
    sed -i 's/^GRUB_DEFAULT=.*/GRUB_DEFAULT=saved/' /etc/default/grub
    update-grub
    grub-set-default "$OLD"
    grub-reboot "$NEW"
    echo "installed; the next reboot (only) boots $KVER"
    ;;
  promote)
    grub-set-default "$NEW"
    echo "default kernel: $KVER"
    ;;
  demote)
    grub-set-default "$OLD"
    echo "default kernel: 5.15.0-73-generic"
    ;;
  remove)
    grub-set-default "$OLD"
    rm -rf /lib/modules/$KVER/extra
    dpkg -r linux-image-$KVER linux-headers-$KVER
    update-grub
    ;;
  *)
    echo "usage: $0 {install|promote|demote|remove}" >&2; exit 1 ;;
esac
