#!/bin/sh
# Install the current source tree as a new DKMS version.
# Run AFTER unloading the module: unplug dongle -> rmmod fl2000 -> this script -> modprobe.
set -eu

SRC=/home/xbral/fl2000_driver
VER=1.0.2
KVER=${1:-$(uname -r)}
DKMS_SRC=/usr/src/fl2000_drm-$VER

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

echo
echo "Installed fl2000_drm/$VER for $KVER."
echo "Next: modprobe fl2000 && replug the dongle"
