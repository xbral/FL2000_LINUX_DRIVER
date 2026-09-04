# FL2000DX VGA dongle support (fork)

This is a fork of [klogg/fl2000_drm](https://github.com/klogg/fl2000_drm) that
makes **FL2000DX USB3-to-VGA dongles** work as proper DRM/KMS displays on
current kernels. The upstream driver targets HDMI dongles built around the
IT66121 bridge chip; on VGA dongles there is no bridge, the component match
never completes, and the display never comes up. This fork adds a direct path
for the chip's integrated VGA DAC.

### What this fork adds

- **VGA dongle support** (`vga_mode=1`): a `DRM_MODE_CONNECTOR_VGA` connector
  driving the FL2000DX's integrated DAC, no bridge chip required.
- **EDID-less operation with VESA fallback modes.** These VGA dongles have no
  usable DDC path to the monitor — established by a lengthy investigation
  culminating in a USB capture of the working Windows driver, which performs
  no DDC traffic either (it serves EDID from the OS cache). Modes up to
  1920x1200 are offered with 1024x768@60 preferred.
- **Hotplug detect via the undocumented `0x805C` connector-sense register**,
  found by raw-diffing chip state against physical cable changes. It reads
  zero with no cable and a stable analog-sense pattern with a monitor
  attached, which lets `detect()` report real `connected`/`disconnected`
  states — desktops auto-enable the display on plug-in.
- **Port to current kernels** (developed and tested on 7.0; upstream baseline
  was 6.5): fbdev client API, `struct drm_edid` API, bridge attach signature.
- **Five teardown/lifetime fixes**, several of which likely correspond to
  long-standing upstream issues
  [#12](https://github.com/klogg/fl2000_drm/issues/12) and
  [#33](https://github.com/klogg/fl2000_drm/issues/33): a devres lifetime bug
  that left a registered i2c adapter pointing into freed module text after
  rmmod; a disconnect deadlock between `i2c_del_adapter()` and the DRM
  device's devres parenting; a stream-worker semaphore deadlock that froze
  every display in the system on hot-unplug; an anchored-URB leak that could
  wedge a USB port for good on a flaky unplug; and an atomic shutdown called
  from the DRM release callback after the refcount already hit zero.

The full investigation record — every dead end included — is in
[NOTES.md](NOTES.md).

### Install in 5 minutes (Ubuntu / Debian / Pop!_OS)

> **VGA dongles must be plugged into a USB 3 port (usually blue).**
> On USB 2 the device fails to enumerate — see [troubleshooting](#troubleshooting).

**1. Install dependencies** (compiler, kernel headers, DKMS so the driver
survives kernel updates):

```
sudo apt update
sudo apt install -y git build-essential linux-headers-$(uname -r) dkms
```

<details>
<summary>Fedora / Arch commands</summary>

```
# Fedora
sudo dnf install -y git gcc make kernel-devel dkms
# Arch
sudo pacman -S --needed git base-devel linux-headers dkms
```

</details>

**2. Clone the driver:**

```
git clone https://github.com/xbral/FL2000_LINUX_DRIVER.git
cd FL2000_LINUX_DRIVER
```

**3. Install with DKMS** (unplug the dongle first if it is plugged in):

```
sudo ./install.sh
```

**4. Enable VGA mode** (only for VGA dongles; skip for HDMI/IT66121 dongles):

```
echo "options fl2000 vga_mode=1" | sudo tee /etc/modprobe.d/fl2000.conf
```

**5. Load the driver and replug the dongle:**

```
sudo modprobe fl2000
```

Unplug and replug the dongle if it was already connected. Your monitor
should appear as a normal display and be enabled by the desktop
automatically (check *Settings → Displays*).

**Verify it worked:**

```
lsusb -d 1d5c:2000             # the dongle must be listed (Fresco Logic)
dmesg | grep -i fl2000 | tail  # probe lines, no errors
ls /sys/class/drm/             # a card*-VGA-1 connector should exist
```

### Manual install without DKMS (quick test)

If you just want to try the driver once without installing anything:

```
sudo apt install -y git build-essential linux-headers-$(uname -r)
git clone https://github.com/xbral/FL2000_LINUX_DRIVER.git
cd FL2000_LINUX_DRIVER
make
sudo modprobe drm_kms_helper
sudo insmod fl2000.ko vga_mode=1
```

To unload: unplug the dongle first, then `sudo rmmod fl2000`.
After a kernel update you must `make` again — prefer the DKMS method
above for permanent use.

### Secure Boot

If `modprobe`/`insmod` fails with `Required key not available`, your
system has Secure Boot on and refuses unsigned modules. Easiest options:

- **Enroll a key and sign (Ubuntu/Debian):** after `make`, run from the
  repo root `sudo ./scripts/sign.sh`, then
  `sudo mokutil --import /var/lib/shim-signed/mok/MOK.der` and reboot to
  enroll the key. Re-sign after every rebuild.
- **Or disable Secure Boot** in the BIOS/UEFI settings.

### Troubleshooting

| Symptom | Fix |
|---|---|
| `dmesg` shows `can't set config #1, error -71` | The dongle enumerated at USB 2 speed. The FL2000's descriptors declare duplicate endpoints, which modern kernels reject on USB 2 (on USB 3 the interfaces use alternate settings and enumerate fine). Plug into a **USB 3 port directly**, no USB 2 hub or extension. |
| `Required key not available` on load | Secure Boot is blocking the unsigned module — see [Secure Boot](#secure-boot). |
| `make` fails with missing headers | Install `linux-headers-$(uname -r)` and make sure the version matches `uname -r` exactly. |
| Driver worked, then broke after an update | Kernel was updated. DKMS installs rebuild automatically (`AUTOINSTALL=yes`); check with `dkms status`. Manual installs need `make` + reload again. |
| Monitor detected but stays black | Check the cable and try a lower mode (start with 1024x768@60). To isolate a framebuffer-copy problem, load with `test_fill=1` (`sudo modprobe -r fl2000 && sudo modprobe fl2000 vga_mode=1 test_fill=1`): a white screen proves the chip/DAC path works. |
| `rmmod` hangs or says module in use | Unplug the dongle first and close anything using the display, then retry. |
| HDMI dongle (IT66121 bridge) | Skip step 4 (`vga_mode`) and additionally load the bridge: `sudo insmod it66121.ko` (manual method). |

Still stuck? Open an issue with the output of `uname -r`,
`lsusb -d 1d5c:2000`, `dmesg | grep -i -E 'fl2000|usb' | tail -30`,
and whether the port is USB 2 or 3.

### Uninstall

```
sudo dkms remove fl2000_drm/1.0.2 --all
sudo rm -f /etc/modprobe.d/fl2000.conf
sudo modprobe -r fl2000
```

### Module options

| Option | Default | What it does |
|---|---|---|
| `vga_mode=1` | `0` | Drive the integrated VGA DAC directly (VGA dongles). Without it the driver uses the upstream HDMI/IT66121 path. |
| `test_fill=1` | `0` | Stream solid white instead of framebuffer content (black-screen diagnostic, changeable at runtime). |
| `force_bpp=2` | `0` (auto) | Force bytes per pixel: `2` = RGB565, `3` = RGB888 (changeable at runtime, applies on next modeset). |
| `probe_only=1`, `dump_regs=1` | `0` | Bring up only USB/register/I2C without a DRM device, and dump raw chip state — debugging aids, see NOTES.md. |

Runtime-tunable options can also be changed without reloading, e.g.
`echo 1 | sudo tee /sys/module/fl2000/parameters/test_fill`.

### Tested on

One FL2000DX-based VGA dongle with one VGA monitor, on Pop!_OS 24.04
(kernel 7.0.11, COSMIC desktop, AMD primary GPU). Hot-unplug and replug
while the desktop is extended have been exercised deliberately and survive
cleanly. That is the honest extent of the testing — reports from other
dongles, monitors, kernels and desktops are very welcome in the issues.

### Attribution and license

Fork of [klogg/fl2000_drm](https://github.com/klogg/fl2000_drm) by Artem
Mygaiev — the clean DRM implementation this work builds on. Register-level
reference material from the abandoned official
[FrescoLogic/FL2000](https://github.com/FrescoLogic/FL2000) driver. Same
license as upstream: **GPL-2.0**.

---

## Original upstream README

[![Gitter](https://badges.gitter.im/fl2000_drm/community.svg)](https://gitter.im/fl2000_drm/community?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge)
[![Build](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml)
[![Stylecheck](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=klogg_fl2000_drm&metric=alert_status)](https://sonarcloud.io/dashboard?id=klogg_fl2000_drm)

# Linux kernel FL2000DX/IT66121FN dongle DRM driver

Clean re-implementation of FrescoLogic FL2000DX DRM driver and ITE Tech IT66121F driver, allowing to enable full display controller capabilities for [USB-to-HDMI dongles](https://www.aliexpress.com/item/32821739801.html?spm=a2g0o.productlist.0.0.14ee52fb8rFfu5) based on such chips in Linux

### Building driver

Check out the code and type
```
make
```
Use
```
insmod fl2000.ko && insmod it66121.ko
```
with sudo or in root shell to start the driver. If you are running on a system with secure boot enabled, you may need to sign kernel modules. Try using provided script for this:
```
./scritps/sign.sh
```
ensure that DRM components are loaded in your system, if not - please use
```
modprobe drm
modprobe drm_kms_helper
```
**NOTE:** proper kernel headers and build tools (e.g. "build-essential" package) must be installed on the system. Driver is developed and tested on Ubuntu 22.04 with **Linux kernel 6.5.0**, so better to test it this way. Please use gcc-8 or newer to build the driver

For more information check project's [Wiki](https://github.com/klogg/fl2000_drm/wiki)
