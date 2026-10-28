#!/bin/sh
# Install the current source tree as a new DKMS version. Run as root.
#
# Must be run BEFORE plugging the dongle back in, and the machine must be
# rebooted afterwards - see the initramfs note below.
set -eu

SRC=$(cd "$(dirname "$0")" && pwd)
VER=1.0.10
KVER=${1:-$(uname -r)}
DKMS_SRC=/usr/src/fl2000_drm-$VER

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root - this writes $DKMS_SRC and drives dkms." >&2
    echo "       Try: sudo $0 ${KVER}" >&2
    exit 1
fi

if lsmod | grep -q '^fl2000 '; then
    cat <<'EOF'
NOTE: fl2000 is loaded right now. Installing still replaces the build on disk
and in the initramfs, but the module already in memory keeps running until the
machine reboots. A REBOOT is required for this install to take effect.

'sudo rmmod fl2000' is not required and currently does not work on this machine:
the driver leaks a module reference per plug/unplug cycle, so the refcount never
returns to zero and rmmod fails with "Module in use". A reboot clears it.

Continuing with the install.
EOF
fi

rm -rf "$DKMS_SRC"
mkdir -p "$DKMS_SRC/bridge"
cp "$SRC"/*.c "$SRC"/*.h "$SRC"/Makefile "$DKMS_SRC"/
cp "$SRC"/bridge/*.c "$SRC"/bridge/*.h "$DKMS_SRC"/bridge/
sed "s/PACKAGE_VERSION=.*/PACKAGE_VERSION=$VER/" "$SRC"/dkms.conf > "$DKMS_SRC"/dkms.conf

for OLD in $(dkms status 2>/dev/null | awk -F'[,/ ]' '/fl2000_drm\// {print $2}'); do
    [ "$OLD" = "$VER" ] && continue
    dkms uninstall fl2000_drm/$OLD -k "$KVER" >/dev/null 2>&1 || true
    dkms remove fl2000_drm/$OLD --all >/dev/null 2>&1 || true
done

dkms uninstall fl2000_drm/$VER -k "$KVER" >/dev/null 2>&1 || true
dkms remove fl2000_drm/$VER --all >/dev/null 2>&1 || true

dkms add fl2000_drm/$VER
dkms install fl2000_drm/$VER -k "$KVER"

# Refresh the initramfs. This step is not optional on this machine.
#
# /etc/modules-load.d/fl2000.conf contains "fl2000", so update-initramfs embeds a
# COPY of the module in the initrd. On every boot the initramfs modprobes that
# copy, before the root filesystem - and therefore before DKMS - is even mounted.
# Without this step the freshly built module sits unused on disk and the old one
# that was baked into the initrd keeps running, silently, no matter how many
# times this script is re-run.
#
# This is not hypothetical: the initrd built on 2026-10-01 carried the September
# build (srcversion 89522E6B796C39ABFB45C5C) and shadowed every DKMS install
# until 2026-10-09, when the mismatch was finally caught.
INITRD=$(ls "/boot/initrd.img-$KVER" 2>/dev/null || true)

if [ -n "$INITRD" ]; then
    echo "Refreshing $INITRD ..."
    update-initramfs -u -k "$KVER"
else
    echo "WARNING: no /boot/initrd.img-$KVER found." >&2
    echo "         If this system boots from an initramfs, the new module will" >&2
    echo "         NOT be loaded until it is regenerated." >&2
fi

# Print what was actually installed, so the running module can be compared
# against it after boot:  cat /sys/module/fl2000/srcversion
INSTALLED=$(modinfo -F srcversion "/lib/modules/$KVER/updates/dkms/fl2000.ko.zst" 2>/dev/null || echo unknown)

echo
echo "Installed fl2000_drm/$VER for $KVER."
echo "srcversion installed: $INSTALLED"
echo
echo "Next: REBOOT, then replug the dongle, then check"
echo "  cat /sys/module/fl2000/srcversion      # must equal the line above"
echo "  dmesg | grep fl2000"
