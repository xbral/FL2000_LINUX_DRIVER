#!/bin/sh
# Install the current source tree as a new DKMS version.
# Run AFTER unloading the module: unplug dongle -> rmmod fl2000 -> this script -> modprobe.
set -eu

SRC=$(cd "$(dirname "$0")" && pwd)
VER=1.0.4
KVER=${1:-$(uname -r)}
DKMS_SRC=/usr/src/fl2000_drm-$VER

if lsmod | grep -q '^fl2000 '; then
    cat <<'EOF'
ERROR: fl2000 is still loaded in memory. Installing now would only replace
the file on disk; the OLD module keeps running until reboot (the crash of
2026-09-08 18:30 happened exactly because the old build stayed loaded).

Do, in order:
  1. Disable the VGA output in the desktop display settings (stream stopped).
  2. Physically unplug the dongle.
  3. sudo rmmod fl2000
  4. Re-run this script.
  5. Replug the dongle (USB-3 port) and confirm: dmesg | grep fl2000
EOF
    exit 1
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

echo
echo "Installed fl2000_drm/$VER for $KVER."
echo "Next: modprobe fl2000 && replug the dongle"
