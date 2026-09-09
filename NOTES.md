# FL2000DX VGA dongle — porting notes

Target hardware: `1d5c:2000` Fresco Logic FL2000DX, **VGA** dongle (no IT66121
bridge chip), VGA monitor attached and powered on.

Host: Pop!_OS 24.04, kernel `7.0.11-76070011-generic`, gcc 13.3.0, Secure Boot
disabled (no module signing needed).

## Phase 1 — build baseline

See commit "Port to kernel 7.0". Driver baseline was 6.5; the DRM API moved on
in three places that mattered (fbdev client API, EDID API, bridge attach
signature). No functional changes were made.

## Phase 2 diagnosis — where the VGA dongle stops

With `fl2000.ko` loaded alone (`it66121.ko` deliberately not loaded):

    $ sudo insmod ./fl2000.ko
    $ dmesg
    usbcore: registered new interface driver fl2000_usb

That single line is the *only* output. It is misleading — probe **does** run and
**does** succeed. Evidence:

    $ ls /sys/bus/usb/devices/2-4:1.*/driver
    2-4:1.0 -> fl2000_usb
    2-4:1.1 -> fl2000_usb
    2-4:1.2 -> fl2000_usb

All three interfaces (AVCONTROL=0, STREAMING=1, INTERRUPT=2) bind, so
`devs->active_if == FL2000_ALL_IFS` and `component_master_add_with_match()` is
reached. The regmap and I2C adapter are both created successfully:

    $ cat /sys/bus/i2c/devices/i2c-8/name
    FL2000 bridge I2C bus

So USB vendor control transfers to the chip work.

**The stall point is the component framework.** `fl2000_compare()` only ever
matches an `i2c_client` named `it66121`. On a VGA dongle that chip does not
exist, so the match list is never satisfied, `fl2000_master_ops.bind` is never
called, `fl2000_drm_bind()` never runs, and no DRM device is registered:

    $ ls /sys/class/drm/ | grep -E '^card[0-9]+$'
    card1          # amdgpu only; no fl2000 card

There is no error and no timeout — the master simply waits forever. This is
exactly the failure predicted for VGA dongles.

## Two obstacles the plan did not anticipate

### 1. EDID cannot be read with `drm_get_edid()` / `drm_edid_read_ddc()`

The FL2000 I2C master is not a general-purpose I2C controller. It is a
dword-oriented register interface (`fl2000_i2c_dword()`), and the adapter
advertises quirks to match (`fl2000_i2c.c`):

    max_num_msgs         = 2
    max_write_len        = 2
    max_read_len         = 1      <-- one byte per transaction
    max_comb_1st_msg_len = 1
    flags = I2C_AQ_COMB | I2C_AQ_COMB_WRITE_FIRST | I2C_AQ_COMB_SAME_ADDR

A DDC EDID fetch is a 128-byte block read, which `i2c_check_quirks()` rejects
outright before any hardware access:

    $ sudo i2ctransfer -y -f 8 w1@0x50 0x00 r128
    Error: Sending messages failed: Operation not supported

So `drm_edid_read_ddc(connector, adapter)` cannot work here. The EDID reader
must issue its own per-offset reads and be handed to `drm_edid_read_custom()`,
the same way `it66121_get_edid_block()` does for the HDMI bridge.

Worth noting: going through the *i2c_adapter* is doubly wasteful, because
`fl2000_i2c_xfer()` reads a full dword from the hardware and then discards
three of the four bytes. Calling `fl2000_i2c_dword()` directly gets 4 bytes per
transaction — 32 transactions per 128-byte EDID block instead of 128.

Only EDID blocks 0 and 1 are reachable: `i2c_offset` in `FL2000_VGA_I2C_SC_REG`
is 8 bits, so offsets 0..255. There is no E-DDC segment pointer support.

### 2. The I2C master appears to need chip init first

Single-byte reads pass the quirk check but fail against the hardware:

    $ sudo i2ctransfer -y -f 8 w1@0x50 0x00 r1
    Error: Sending messages failed: Input/output error

`fl2000_i2c_dword()` returns `-EIO` on either a poll timeout or a nonzero
`i2c_status` (a NAK). Most likely cause: `fl2000_reset()` and
`fl2000_usb_magic()` are only called at the *end* of `fl2000_drm_bind()`, which
never runs on a VGA dongle. The chip is therefore sitting in its power-on state
and its I2C master / monitor-detect circuit was never enabled.

`fl2000_usb_magic()` is what sets `monitor_detect` and `edid_detect` in
`FL2000_VGA_I2C_SC_REG`. Chip init must be moved ahead of any EDID attempt.

This is a hypothesis, not yet confirmed — it is the first thing to verify in the
VGA bring-up work.

## Useful register offsets

    FL2000_VGA_STATUS_REG    0x8000
    FL2000_VGA_I2C_SC_REG    0x8020   addr:7 cmd:1 offset:8 vga_status:8
                                      i2c_status:4 monitor_detect:1 i2c_ready:1
                                      edid_detect:1 i2c_done:1
    FL2000_VGA_I2C_RD_REG    0x8024
    FL2000_VGA_I2C_WR_REG    0x8028
    FL2000_RST_CTRL_REG      0x8048
    FL2000_USB_LPM_REG       0x0070
    FL2000_USB_CTRL_REG      0x0078

## CRITICAL — devres lifetime bug makes rmmod/insmod unsafe

**This must be fixed before any further bring-up work. It breaks the debugging
loop itself.** This is almost certainly what is behind klogg issues #12 / #33.

Observed: after one `rmmod fl2000` + `insmod fl2000.ko` cycle, opening the
FL2000 i2c bus from userspace oopsed the kernel:

    BUG: unable to handle page fault for address: ffffffffc2b72af0
    #PF: supervisor write access in kernel mode
    Oops: 0003 [#1] SMP NOPTI
    RIP: 0010:try_module_get+0x28/0xe0
    Call Trace:
     ? i2c_get_adapter+0x37/0x70
     i2cdev_open+0x1f/0xd0
     chrdev_open+0xba/0x250
    Modules linked in: fl2000(OE) ... [last unloaded: fl2000(OE)]

The faulting address is in module space, and `i2c_get_adapter()` was doing
`try_module_get(adapter->owner)` where `adapter->owner` is `THIS_MODULE` of the
*already unloaded* module.

### Root cause

Both the i2c adapter and the regmap are attached to the USB **device**, not to
the USB **interface** the driver actually binds to:

    fl2000_i2c.c:    devres_add(&usb_dev->dev, adapter);
    fl2000_registers.c: devm_regmap_init(&usb_dev->dev, ...)
    fl2000_drv.c:    devm_kzalloc(&usb_dev->dev, ...) and
                     dev_set_drvdata(&usb_dev->dev, devs)

devres attached to `usb_dev->dev` is released when the *usb_device* goes away —
i.e. on physical unplug — **not** when the driver's interfaces disconnect on
`rmmod`. So on module unload `fl2000_i2c_adapter_release()` never runs,
`i2c_del_adapter()` is never called, and the adapter stays registered in the
i2c core with `owner` and `algo` pointing into freed module text.

Two consequences, both observed:

1. The stale i2c bus (`i2c-8`) outlives the module. Any access to it —
   `i2cdetect`, `i2ctransfer`, or a DDC read from another driver — dereferences
   freed module memory and oopses. It is a landmine left behind by `rmmod`.
2. On reload, `dev_get_drvdata(&usb_dev->dev)` still returns the **old** `devs`
   pointer, so `fl2000_probe()` skips `fl2000_get_devices()` entirely and
   silently reuses the stale adapter and stale regmap. No new i2c bus is
   created (confirmed: still only `i2c-0`..`i2c-8`, no `i2c-9` after reload).
   Every register access then calls through freed function pointers.

This also means the "clean" first `rmmod`/`insmod` cycle in this session was not
clean at all — it had already left the stale adapter behind. The oops merely
made it visible.

### Fix direction

Move the per-device resources off `usb_dev->dev`. Options, roughly in order of
preference:

- Anchor devres to the interface the driver actually binds (the AVCONTROL
  interface, `&interface->dev`), so unbind/rmmod tears it down properly.
- Or drop devres for the adapter and regmap and manage them explicitly:
  allocate on first probe, `i2c_del_adapter()` in `fl2000_disconnect()` when the
  last interface goes away.

Either way `dev_set_drvdata()` must be cleared on disconnect so a reload does
not pick up a stale `devs`.

Until this is fixed: **do not rmmod.** Reboot between test cycles, or physically
unplug the dongle before unloading (unplug does release the usb_dev devres).

## Phase 2c — VGA DAC init needs no porting

The plan assumed DAC init and VGA mode-setting registers would have to be
lifted from the old FrescoLogic driver. Comparing the two, they do not.

The FL2000's output stage is natively VGA — the register block is named
`FL2000_VGA_*` throughout and carries `dac_output_en`, `vga565_mode`,
`vga332_mode`, `vga_color_palette_en`, `force_vga_connect`. On an HDMI dongle
the IT66121 sits *downstream* of that same parallel output and converts it.
So klogg's driver was already programming the VGA path all along; the bridge
was the only VGA-unaware part.

Concretely, the old driver's DAC enable is:

    fl2000_monitor.c:410   // Set bit 7, Enable external DAC.
                           fl2000_reg_bit_set(dev_ctx, REG_OFFSET_8004, 7);

`REG_OFFSET_8004` is `FL2000_VGA_CTRL_REG_PXCLK` (`0x8000 + 0x04`), and bit 7
of that register is `dac_output_en` in klogg's `union
fl2000_vga_cntrl_reg_pxclk`. `fl2000_set_pixfmt()` already sets it. Likewise
`fl2000_set_pll()` already sets `force_vga_connect`, and `fl2000_set_timings()`
already writes the H/V sync registers (`0x8008`/`0x800C`/`0x8010`/`0x8014`) that
the old driver writes at `REG_OFFSET_8008` onward.

Every bit the old driver touches in `0x8004` (0, 6, 7, 24, 25, 26, 27, 28, 31)
is either already handled by `fl2000_set_pixfmt()` or belongs to a colour mode
this driver does not offer. Notably `vga_timing_en` (bit 8) is set by *neither*
driver, so it is not required.

Conclusion: no register porting needed. The remaining risk for first picture is
in PLL/timing selection and the streaming path, not DAC bring-up.

## Round 1 — devres lifetime fix validated on hardware

Run after a clean reboot, module built from commit "Fix devres lifetime...".
Two full unload/reload cycles, no `vga_mode`. **Pass.**

Baseline was `i2c-0`..`i2c-7` (amdgpu + i801), no fl2000.

    insmod  -> i2c-8 = FL2000 bridge I2C bus     appears
    rmmod   -> i2c-8 GONE                         <- the fix
    insmod  -> i2c-8 recreated, full probe
    rmmod   -> i2c-8 gone
    insmod  -> i2c-8 recreated
    rmmod   -> i2c-8 gone

dmesg for the sequence (loaded with `dyndbg=+p`):

    [ 254.848457] usb 2-4: Created FL2000 bridge I2C bus 8 at interface usb-0000:00:14.0-4
    [ 254.861339] usbcore: registered new interface driver fl2000_usb
    [ 273.006128] usbcore: deregistering interface driver fl2000_usb
    [ 274.048528] usb 2-4: Created FL2000 bridge I2C bus 8 at interface usb-0000:00:14.0-4
    [ 274.060010] usbcore: registered new interface driver fl2000_usb
    [ 276.070107] usbcore: deregistering interface driver fl2000_usb

Two things worth calling out as evidence, beyond "no crash":

1. **The adapter actually disappears on rmmod.** Before the fix it stayed
   registered with `->owner` pointing into freed module text, which is what
   oopsed `try_module_get()` from `i2cdev_open()`.
2. **`Created FL2000 bridge I2C bus 8` is printed again on every reload.** That
   line lives in `fl2000_i2c_init()`, reachable only via
   `fl2000_get_devices()`. Under the old bug the reload found a stale `devs` in
   drvdata and skipped device creation entirely, so this line was absent and
   the driver silently reused the dead adapter. Its reappearance is positive
   proof of a genuine fresh probe rather than reuse.

State afterwards: `fl2000` unloaded, no oops/BUG/Call Trace in dmesg (count 0),
`/proc/sys/kernel/tainted` = 12288, i.e. exactly `TAINT_OOT_MODULE (4096)` +
`TAINT_UNSIGNED_MODULE (8192)` and no crash bits.

Note on D-state kworkers: a `kworker/u112:* events_unbound` flickers into D
throughout, including at baseline *before* fl2000 was ever loaded, and it
reports as `gfx_0.0.0` when idle. That is amdgpu background work, unrelated to
this driver. Do not mistake it for the symptom seen in the oops session.

Tip: load with `insmod ./fl2000.ko dyndbg=+p` to get dev_dbg output from the
very start of probe. Enabling dynamic debug afterwards via
`/sys/kernel/debug/dynamic_debug/control` is too late to catch probe.

## Round 2 — raw DDC over the chip's I2C master (probe_only)

Built from "Add probe_only, force EEPROM ops, stop failing on i2c_status".
`insmod ./fl2000.ko probe_only=1 dyndbg=+p` gives i2c-8 live, refcount 0, no
DRM device — so rmmod/insmod iterates freely with no compositor involvement.

### Bit 16 verdict: NOT set at power-up. The SPI theory is wrong.

First transfer after load, address 0x50 offset 0x00:

    usb 2-4: i2c rd addr 0x50 off 0x00: pre 0xcf0000d0 mask 0xdf03ffff val 0x500000d0
    usb 2-4: i2c rd addr 0x50 off 0x00: status 0xf, continuing
    usb 2-4: i2c rd addr 0x50 off 0x00 ok: post 0xcf0000d0

Decoding `pre = 0xcf0000d0`, bits 16-23 are `0x00`, so **`i2c_spi_op` (bit 16)
and `i2c_spi_erase` (bit 17) were already clear**. Transfers were never being
diverted to SPI. Clearing them explicitly is still correct (they are control
bits that were in no write mask, so the old code depended on luck), but it was
not the cause of the DDC failure.

`mask = 0xdf03ffff` confirms bits 16/17 are now covered, and `val` has them
clear, so the fix does what it claims.

Other fields of `pre`/`post = 0xcf0000d0`:

    i2c_status     bits 24-27 = 0xF   all four bytes failed (1 = failed per byte)
    monitor_detect bit 28     = 0     reads back 0 even though we just wrote 1
    i2c_ready      bit 29     = 0
    edid_detect    bit 30     = 1
    i2c_done       bit 31     = 1     transfer completed

`monitor_detect` reading back as 0 is expected, not a fault: the Fresco Logic
driver has the same behaviour and comments on it at `fl2000_i2c.c`, forcing the
bit on every read with `read_back_data |= 0x10000000` under "BUG: Bit 28 always
return zero."

### The I2C master works. 0x50 is a genuine NAK.

Full bus scan, addresses 0x08-0x77 (112 addresses), one single-byte read each:

    111 addresses NAK   post = 0xcf0000xx, i2c_status = 0xF
      1 address  ACKs   0x30, post = 0xc00000b0, i2c_status = 0x0

0x30 is not a fluke. It answers at multiple offsets, always with status 0, and
returns varying data rather than a constant:

    0x30 off 0x00 -> 0x08    post 0xc00000b0
    0x30 off 0x04 -> 0x92    post 0xc00004b0
    0x30 off 0x08 -> 0x00    post 0xc00008b0
    0x30 off 0x10 -> 0x00    post 0xc00010b0

So the controller can address a device, clock a transfer and collect an ACK.
Register programming, address handling and offset handling are all correct.
The failure at 0x50 is the monitor's DDC EEPROM not answering, not a driver
bug. Reads at 0x50 return a constant `0xf1`, which is stale/garbage from
`FL2000_VGA_I2C_RD_REG` rather than sink data.

Note 0x30 sits on the dongle's own I2C, ahead of the VGA connector — probably a
configuration EEPROM. It is not the sink.

### Consequence: i2c_status is meaningful after all

The relaxation of the `i2c_status` check in this commit was justified by the
reference driver, which waits only on the done bit and never inspects the
status nibble. The scan shows that was the wrong lesson to draw: `i2c_status`
is a clean, reliable ACK/NAK signal on this hardware — 0x0 on the one device
that answers, 0xF on all 111 that do not. klogg's original hard check was
right and the reference driver is the sloppy one here.

The check should be restored once DDC works, because `detect()` needs exactly
this signal to tell connected from disconnected. Leaving it relaxed means EDID
reads return garbage that only gets caught later by header/checksum validation.

### Where this leaves us

Nothing in the driver explains a NAK at 0x50 while 0x30 ACKs on the same bus
with the same code path. The remaining suspects are all outside the driver:

- VGA cable with no DDC wiring. Pins 12 (SDA) and 15 (SCL) are omitted on many
  cheap or older VGA cables, and on most VGA-to-DVI adapters.
- Monitor in a standby state deep enough to unpower its EDID EEPROM.
- A dongle that does not wire the VGA connector's DDC pins to the chip's I2C
  master at all. Some low-cost FL2000 VGA dongles skip this.

## Round 3 prep — hunting the missing DDC enable

New fact from the user: dongle + monitor + cable work perfectly on Windows,
including detection by name at native resolution. EDID therefore works
end-to-end on this exact hardware. The 0x50 NAK is not physical; the Windows
driver performs some enable that our probe_only init does not, i.e. the DDC
path to the connector is gated at the chip level.

### Reference driver: every HW touch before its first EDID access

Traced through `fl2000_module.c` → `fl2000_dev_init()` (fl2000_dev.c) →
`fl2000_dongle_card_initialize()` (fl2000_dongle.c) →
`fl2000_monitor_manual_check_connection()` → `fl2000_monitor_vga_status_handler()`
→ `fl2000_monitor_plugin_handler()` → `fl2000_monitor_read_edid_dsub()`.
In order, on the VGA (no ITE chip) path:

| # | Reference action | In our probe_only? |
|---|---|---|
| 1 | `usb_set_interface(if 0, alt 1)` — workaround for FL2000's buggy descriptors; kernels ≥5.x ignore the redundant endpoints unless alt 1 is selected | **MISSING** |
| 2 | `0x0070` set bit 19, set bit 20 (reject U1/U2) | yes — `usb_magic()` `u1_reject`/`u2_reject` |
| 3 | `0x8048` set bit 15 (app reset, self-clear) | yes — `fl2000_reset()` `sw_reset` bit 15, +10 ms |
| 4 | I2C probes at 0x4C (`fl2000_hdmi_find_chip`) — NAK on a VGA dongle | skipped; harmless |
| 5 | `0x8020` set bit 30 **alone**, then `msleep(750)` (`DELAY_FOR_I2C_CONNECTION_ENABLE`) | **PARTIAL** — bit is written, but combined with bit 28 in one write, and no delay |
| 6 | `0x8020` set bit 28 (ext monitor detect enable) | yes — same combined write |
| 7 | `0x8088` clear bit 10 ("turn off hardware reset") | yes — `usb_magic()` `wakeup_clr_en = 0` (klogg's `FL2000_VGA_CTRL_REG_3` *is* 0x8088) |
| 8 | Read `0x8000`; proceed to EDID **only if** bit 0 `connected` is set | **MISSING** — we never read 0x8000 |
| 9 | `0x0078` clear bit 17 | yes — `usb_magic()` `wake_nrdy = 0` |
| 10 | `0x0070` reject U1/U2 again | yes (idempotent) |
| 11 | dword EDID reads at 0x50, offsets 0,4,8..., 10 ms apart | equivalent to our reads |

Two candidates initially suspected are in fact already covered: 0x8088 bit 10
and 0x0078 bit 17 are both cleared by `usb_magic()` (the 0x8088 grep miss was
because klogg computes the offset as `0x8000 + 0x88`).

Genuinely missing, in suspicion order:

- **A. Detect-enable sequencing (row 5):** reference sets the EDID/I2C detect
  bit *by itself*, waits 750 ms for the detection circuit to settle, and only
  then enables external monitor detect. We slam both bits in one write with no
  settle time. Note the elapsed-time argument cuts both ways: our manual
  rescans ran *minutes* after insmod and still NAKed, so a pure "wait 750 ms
  after enable" cannot be the whole story — but "bit 30 must be enabled alone
  before bit 28 joins" would not be cured by elapsed time.
- **B. `usb_set_interface(0, 1)` (row 1):** a USB-level SET_INTERFACE the
  chip's firmware observes, performed before any register work. If the
  firmware keys internal power/routing off the altsetting, no amount of
  register poking fixes it.
- **C. `0x8000` gating (row 8):** the reference never even attempts EDID until
  the chip's own monitor-detect circuit reports `connected` (bit 0). If the
  DDC pads are powered by that circuit, a chip that thinks no monitor is
  present may hold DDC dead. Reading 0x8000 costs nothing and adjudicates:
  bits of interest are 0 `connected`, 28 `edid_connected`, 29
  `ext_mon_connected`, 6 `pll_powered_up`, 7 `dac_powered_up` (reference
  `struct vga_status`, fl2000_interrupt.h).

Also checked and ruled out: the reference does **no** PLL/DAC/output-stage init
before its first EDID read — `fl2000_dongle_set_params()` runs much later. So
"DDC needs the output stage powered" is not supported by the reference code.

### Round 3 mechanism

`ddc_step` module param (int, cumulative), iterated via the probe_only loop:

    ddc_step=0   baseline, current behaviour
    ddc_step=1   A: set 0x8020 bit 30 alone, msleep(750), then usual init
    ddc_step=2   A+B: usb_set_interface(0, 1) first, then step 1

probe_only now always ends by reading `0x8000` and logging the decoded
monitor-detect status (C), so each step's rescan also shows what the chip
thinks is connected.

## Round 3 results — both software candidates eliminated; the chip agrees DDC is dead

Loop run with the ddc_step build (probe_only, rmmod/insmod between steps,
0x50 rescan after each):

| step | init variation | 0x8000 decode | 0x50 |
|---|---|---|---|
| 0 | baseline | `0x600000e1` connected **1** edid_connected **0** ext_mon **1** pll_up 1 dac_up 1 | NAK |
| 1 | + staged enable (bit 30 alone, 750 ms settle) | `0x200000c1` same flags | NAK |
| 2 | + `usb_set_interface(0, 1)` first | `0x200000c1` same flags | NAK |

0x30 control-read ACKed in every round, so the I2C master stayed healthy
throughout. With the restored NAK check, a NAKed read now surfaces as a clean
I/O error instead of returning bus garbage.

The 0x8000 decode is the significant new evidence:

- **`connected = 1`** — the chip's analog monitor-detect circuit (load sensing
  on the RGB lines) sees the monitor. Detection hardware works, dongle-to-
  monitor analog wiring works.
- **`edid_connected = 0`** — the chip's *own* EDID-detect circuit cannot see
  the DDC EEPROM. The chip independently reports exactly what our reads
  experience. This is not a register we forgot; the hardware itself observes
  no device on the DDC lines.

The 750 ms settle and the altsetting workaround made no difference to either
flag or to the reads.

### The device at 0x30

Dumped offsets 0x00-0x1f: `08 05 01 52 92 04 24 09 00 00 00 00 0a 00 ...` —
structured but not an EDID and not obviously monitor-side. 7-bit 0x30 happens
to be the E-DDC segment pointer address (8-bit 0x60), which would place it in
the *monitor*, but a segment pointer does not normally back a 16-byte memory.
More likely a dongle-internal configuration EEPROM. Discriminator (physical):
detach the VGA cable at the dongle and re-read 0x30 — if it stops ACKing it
was the monitor and the DDC wires are electrically alive after all.

### Leading remaining theory: monitor deep-standby DDC power-down

Windows works on this exact chain, but with a crucial difference in dynamics:
Windows lights the display *immediately* on plug-in, so the monitor never
enters deep standby. Under our probe_only testing the monitor has been sitting
for hours with no video signal. Many monitors keep their EDID EEPROM powered
from the DDC +5V line permanently, but some cut DDC power in deep sleep while
the passive 75 ohm terminations that trip the analog load detect stay
present — which would produce precisely the observed signature:
`connected = 1`, `edid_connected = 0`, EEPROM NAK.

This theory fits every observation and explains the Windows result without
requiring any undiscovered register. Test: wake the monitor (power-cycle it so
it is actively showing its "no signal" OSD) and rescan 0x50 within the first
seconds. If it ACKs, the "fix" is behavioural, not register-level: the driver
should retry EDID after enabling video output, and the EDID-less fallback
carries the first modeset.

## Round 4 — cable diffing: the chip does watch the connector, in a register nobody models

Physical tests run with the user's help, probe_only loop throughout.

### VGA cable detached at the dongle

- 0x30 still ACKs (`0x08`) → the device at 0x30 is **dongle-internal** (config
  EEPROM, most likely). The E-DDC segment pointer theory is dead.
- `0x8000` reads **bit-identical** to the attached state (`0x200000c1`,
  connected=1, ext_mon=1) → those bits are static under our init and do NOT
  track the connector. The earlier inference "the chip sees the monitor" was
  wrong; whatever those bits mean, they are not live presence.

### Monitor awake test

Cable re-attached, monitor powered on and actively showing its "Check Cable
Connection" OSD — the most DDC-favourable state possible. 0x50 still NAKs.
**The deep-standby theory is dead.**

### Raw register diff (dump_regs=1), cable attached vs detached

Full raw dump of 0x8000-0x80FF plus 0x0070/0x0078, bypassing the regmap cache
(usb_control_msg backend directly). Two dumps per state to separate counters
from state:

    0x8040  changes between same-state dumps          -> free-running counter, ignore
    0x805C  stable within state, tracks cable exactly:
                detached: 0x00000000
                attached: 0x00ffecbb

**0x805C is a live connector-tracking register modelled by neither driver.**
Byte pattern ff/ec/bb looks like three analog sense readings — plausibly the
raw output of the load-detect block on the RGB lines. So the detection
hardware is alive and the connector is electrically visible to the chip; only
the DDC/EDID side stays dead. (0x8024 = f1f1f1f1 in the dumps also confirms
0xf1 was fill garbage in the I2C read register, as suspected.)

### Where this converges: DDC +5V (VGA pin 9)

Facts now established simultaneously:
- I2C engine works (0x30 ACKs, structured data)
- analog sense path works (0x805C tracks the cable)
- monitor is awake and Windows-proven on this exact chain
- EEPROM at 0x50 NAKs, and the chip's own EDID-detect agrees

The one element that would produce exactly this split is the **DDC +5V supply**
on VGA pin 9: the host powers the monitor's EDID EEPROM through it (that is
why EDID normally works even on an unpowered monitor — and conversely, a
monitor whose EEPROM expects pin-9 power will NAK even when awake if pin 9 is
dead). If the dongle gates +5V through a FET on an FL2000 GPIO which the
Windows driver sets, everything observed fits. Neither Linux driver contains
the string "gpio"; the FL2000's GPIO registers are in space neither driver
touches, so the enable cannot be found by reading their code.

### Next: Windows USB capture (decisive)

All software candidates derivable from the two Linux codebases are exhausted.
The Windows driver demonstrably performs the missing enable, and every
register access it makes is visible on the wire as a vendor control transfer
(bRequest 64 = read, 65 = write, register offset in wIndex, 4-byte value).
A USBPcap/Wireshark capture on the Windows machine from dongle plug-in to
desktop-on-VGA contains, by construction, the complete working init sequence.
Diffing its writes against ours is a mechanical lookup that ends the guessing.

## Round 5 — Windows USB capture analysis

Capture: `~/src/fl2000/win-capture.pcapng`, USBPcap, dongle plug-in →
desktop on VGA → Display Settings visit. 241 MB total; exactly 48 vendor
control transfers (26 reads bmRequestType 0xC0/bRequest 64, 22 writes
0x40/65, wValue 0 throughout — same addressing scheme as ours), plus two
SET_INTERFACE requests at t=3.907. Everything else is bulk video.

### (a) Full chronological vendor-write list (values little-endian)

    t=3.954  0x8048 = 0x00008004   app reset (bit 15), preserving bit 2 (RMW; read 0x04)
    t=3.954  0x8020 = 0x100000cc   I2C read @0x4C (ITE probe), monitor_detect set
    t=3.967  0x8048 = 0x00008004   reset again
    t=3.967  0x8020 = 0xcf0000cc   2nd ITE probe; writes back status F + edid_detect
    t=4.728  0x8020 = 0xdf0000cc   adds monitor_detect: bits 28+30 now both set
    t=5.479  0x8088 = 0x00000088   read 0x488, cleared bit 10
    t=5.479  0x0070 = 0x04106085   then
    t=5.479  0x0070 = 0x04186085   U1/U2 reject (bits 20, then 19+20)
    t=5.480  0x0078 = 0x18010d14   (read back same; bit 17 handling)
    t=5.759  0x803C = 0xe401084d   read 0xe001084d, +bit 26 (force PLL on)
    t=5.759  0x802C = 0x000d2102   PLL word
    t=5.759  0x8048 = 0x00008004   reset
    t=5.759  0x803C = 0xc401084d   -bit 29
    t=5.760  0x803C = 0xd501084d   +bits 24, 28
    t=5.760  0x8004 = 0x0010031c   read 0x0010239c; DAC off, modes cleared
    t=5.760  0x8004 = 0x0010039d   DAC on (bit 7) + clear_watermark (bit 0)
    t=5.760  0x8008 = 0x04000540   hactive 1024 | htotal 1344
    t=5.760  0x800C = 0x00880129   hsync width/start
    t=5.760  0x8010 = 0x03000326   vactive 768 | vtotal 806
    t=5.760  0x8014 = 0x02460024   vsync width/start
    t=5.760  0x801C = 0x00000000   read 0x00850000; ISO bits cleared
    t=5.760  0x0070 = 0x04186085   (rewrite, same value)

That is a textbook **1024x768@60 modeset** (htotal 1344, vtotal 806) with the
exact PLL word 0x000d2102 — a complete known-good recipe for this hardware.

### (b) Diff vs our init

| register | Windows end state | ours (probe_only) | verdict |
|---|---|---|---|
| 0x8048 reset | bit-15 strobes | same | match |
| 0x8020 detect bits | 28+30 set | 28+30 set | match |
| 0x8088 | 0x88 (bit 10 cleared) | 0x88 | match |
| 0x0070 | 0x04186085 | 0x04186085 (dumped) | **byte-identical** |
| 0x0078 | 0x18010d14 | 0x18010d14 (dumped) | **byte-identical** |
| 0x803C | 0xd501084d after modeset | 0xe001084d untouched | differs — modeset only |
| 0x802C PLL | 0x000d2102 | never written | differs — modeset only |
| 0x8004 | 0x0010039d | 0x0010239c untouched | differs — modeset only |
| 0x8008-0x8014 | 1024x768 timings | never written | differs — modeset only |
| 0x801C | 0 | 0x00850000 | differs — modeset only |
| GPIO/unknown regs | **none touched** | — | no hidden enable exists |

Three more things the capture settles:

1. **Windows' ITE probe at 0x4C NAKs with i2c_status = 0xF** (readback
   0x8f0000cc) — the *identical* failure signature we get at 0x50/0x4C.
   Windows treats it as "no HDMI chip" and continues. Our I2C engine behaves
   exactly like Windows' does.
2. **Windows reads 0x8000 and sees 0x600000e1 → 0x200000c1 — the same values
   we read.** It proceeds to modeset regardless. Those status words are
   normal for this dongle, not evidence of a dead detect block.
3. **No EDID read at 0x50 appears anywhere in the capture.** 26 reads total
   cannot contain a 128-byte EDID fetch (≥96 transfers). Windows served EDID
   from its registry cache; the Display Settings visit did not force DDC
   traffic. So the capture does NOT directly show the DDC enable — instead it
   shows there is no register-level enable to find: **pre-modeset chip state
   under Windows is byte-identical to ours.**

### (c) Candidate shortlist, ranked

1. **Active video output unlocks DDC (high confidence).** By elimination:
   register state is identical, I2C engine behaviour is identical, wiring is
   Windows-proven — the only remaining difference when Windows talks DDC is
   that sync/video is running. Two known mechanisms fit: monitors that power
   their DDC section only when sync is present (this Dell shows "Check Cable
   Connection", i.e. it sees no sync), and dongles that derive DDC +5V from
   the active output stage. Test: replay the captured modeset verbatim
   (`ddc_step=4`), watch the monitor react, rescan 0x50.
2. **0x803C final state 0xd501084d alone (medium).** Bit 26 is "force PLL
   always on"; bits 24/28 differ from our resting 0xe001084d. If some clock
   domain feeding the DDC/detect block hangs off these, the 0x803C sequence
   alone might suffice without full timings (`ddc_step=3`).
3. **SET_INTERFACE ×2 (low, already eliminated).** Tested as old ddc_step=2
   with no effect; capture merely confirms Windows does it.

Caveat stated plainly: since the capture contains no DDC transfer, candidate 1
is inference by elimination, not observation. If replaying the modeset does
not unlock 0x50, the next capture must be taken with Windows' EDID cache for
this monitor deleted (Device Manager → Monitors → uninstall device, or
registry `HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY` entry removal) so
Windows is forced to pull EDID over the wire.

## Hot-unplug deadlock (found by accident, likely the real #12/#33)

During Round 5 the dongle was physically unplugged and replugged while
vga_mode was loaded and cosmic-comp held the DRM fd. The DRM teardown itself
completed (card0 disappeared), but `fl2000_disconnect` then hung forever:

    wait_for_completion
    i2c_del_adapter+0x287
    fl2000_i2c_cleanup
    fl2000_disconnect
    usb_unbind_interface
    ...
    hub_event            <- usb_hub_wq kworker, permanently D-state

Hung-task dumps repeated every 120 s; USB hotplug processing on that bus was
stalled from then on. Only a reboot clears it (and shutdown itself may stall
on the D-state task — be ready to hard power off).

Root cause is circular: the DRM device was devm-allocated on the **i2c
adapter's** device, and `i2c_del_adapter()` waits for the adapter device's
final release. With a compositor fd still open, the DRM device stays alive,
its devres link keeps the adapter referenced, and the adapter release —
which `i2c_del_adapter()` is waiting on — can never happen from inside the
very disconnect that must finish first. A hot-unplug under any live DRM
client therefore wedges the USB hub queue permanently. This matches the
symptom class of klogg issues #12/#33 better than the devres-lifetime bug
did.

Fix: parent the VGA-mode DRM device on the AVCONTROL **usb_interface**
instead. `device_del()` on an interface does not wait for lingering
references, so the standard `drm_dev_unplug()` pattern (unplugged device
lingers harmlessly until the last fd closes) works as designed, and
`i2c_del_adapter()` no longer depends on the DRM device's lifetime.
`fl2000_drm_bind()` is unchanged — it takes the usb_device from
`master->parent`, which holds for an interface exactly as it did for the
adapter.

The component/HDMI path still uses the adapter as master and retains the
deadlock; untestable here (no IT66121 hardware), noted for upstream.

## FIRST PICTURE — desktop on the VGA monitor via COSMIC

Sequence that got there, same boot, no TTY needed:

- TTY run (previous boot): connector `connected` via the 0x805C sense detect,
  modetest modeset gave locked sync + 60 fps chip-paced bulk URBs but a black
  screen; DDC stayed NAKed while real frames streamed, killing the last
  sync-dependent-DDC theory. Verdict: this dongle has no usable DDC path
  (most plausibly pin-9 +5V unwired); Windows ran on cached EDID. The VESA
  fallback modes are the permanent solution, not a stopgap.
- Fixed: URB success logged as error (missing `case 0:`), and
  `spin_lock_irq()`/`spin_unlock()` imbalances through the whole streaming
  path (irqs left disabled on exit in process context; irq-disabling lock
  taken in hard-irq context). Added runtime-writable `test_fill` diagnostic.
- Loaded `vga_mode=1` under COSMIC: DRM device binds on usb interface
  `2-4:1.0` (deadlock fix in effect), connector reports `connected`, and
  **COSMIC auto-enabled the display with no manual step** — dmesg shows the
  1024x768 modeset, and the desktop is visible on the monitor. The black
  screen did not reproduce once the locking was fixed; `test_fill` was never
  needed.

State: EDID-less operation at 1024x768@60 preferred, VESA fallback list up to
1680x1050 offered (1920x1200 ceiling; the mode_valid PLL/bandwidth filter
trims the list). Resolution changeable from COSMIC Displays like any monitor.

Remaining loose ends, in rough priority:
- Controlled hot-unplug while the compositor is actively driving the output —
  the interface-parenting fix makes this the designed drm_dev_unplug pattern,
  but it has not been exercised deliberately since the fix.
- rmmod under compositor remains impossible by design (open fds pin the
  module); unload = unplug or reboot. Not a bug.
- The component/HDMI (it66121) path kept the old adapter-parented master and
  with it the disconnect deadlock; untestable without hardware, flagged for
  upstream.
- fbcon "Deferring console take-over" — fbdev emulation present but console
  never takes over; harmless for desktop use.
- The `^ 4` dword-swizzle in the RGB888 pack path is confirmed correct by the
  working picture.

## Hot-unplug incident #2 — semaphore deadlock froze every display

User test: unplug while COSMIC was actively driving the output, replug into a
different (USB-2) port. Result: all displays froze on the next display
operation; hard reboot. Previous-boot journal tells the whole story:

    17:02:04  usb 2-4: USB disconnect / Data URB error -19 / Unbinding
    17:02:05  WARN drm_atomic_helper_wait_for_vblanks: vblank wait timed out
    17:03:02  [drm] *ERROR* flip_done timed out; commit wait timed out
    17:05:22  hung task: fl2000_stream_disable -> fl2000_display_disable
    17:05:22  hung task: output_poll_execute blocked on ww_mutex

Chain: the unplug teardown runs an atomic shutdown holding ALL modeset
ww_mutexes; inside it stream_disable() called drain_workqueue() while the
stream worker sat parked in down_interruptible() on work_sem — whose only
up() sources were URB completions, dead with the cable. drain never returns,
the locks are never released, and every later commit (COSMIC) and the
connector poll worker pile up behind them. Screens frozen system-wide; the
user's disable click merely joined the queue. Note this trap was not
unplug-specific: an ordinary disable raced it whenever no completion was
pending at drain time.

Fix (commit "Fix system-wide display freeze on hot-unplug while streaming"):
stream_disable ups the semaphore after clearing 'enabled' and the worker
re-checks the flag on wake and exits, so drain always terminates. Plus
vblank_off before stream teardown in display_disable, since vblanks on this
device are URB completions — that removes the shutdown-path vblank WARN.

Unrelated to the driver: the replug into the other port never enumerated at
USB level ("Device not responding to setup address", error -71) — dongle
needs a working USB-3 port; nothing arrived for the driver to probe.

## 1080p shimmer — bandwidth-starved RGB888 (fixed with RGB565 fallback)

Symptom: at 1920x1080@60 the VGA monitor shows a steady tremble/shimmer; lower
modes (1600x900, 1280x720, 1024x768) are rock solid.

Mechanism: the driver picked bytes/pixel from a bandwidth table whose
SuperSpeed entry was `5000000000 * 100 / 80 / 8` = 781 MB/s — the 100/80 is
inverted (comment says 80% budget, arithmetic gives 125%). Every mode under
~260 MHz therefore got RGB888. At 1080p60 that drains 445 MB/s from the chip's
line buffer while real-world xHCI bulk fills at ~350-400 MB/s; the LBUF runs
permanently at its floor and the DAC timing gets perturbed — the tremble. The
FL2000DX was designed for 16-bit colour at 1080p (drain 297 MB/s), which is
what Windows uses.

Fix in fl2000_drm.c:

- `fl2000_get_bytes_pix()` now picks RGB888 only when `3 * pixclock` fits the
  per-speed budget, falls back to RGB565 when `2 * pixclock` fits, and rejects
  the mode otherwise. Budgets: 350 MB/s for SuperSpeed (realistic bulk
  ceiling), HS/SuperSpeed+ keep their old effective values so USB 2.0 mode
  filtering does not regress.
- New runtime param `force_bpp` (`/sys/module/fl2000/parameters/force_bpp`):
  0 = auto (default), 2 = RGB565 everywhere, 3 = RGB888 everywhere. Applies on
  the next modeset. Useful to A/B the shimmer against pixel format without
  rebuilding.
- `fl2000_output_mode_set()` logs the chosen format
  (`Pixel format: 2 bytes/pixel (RGB565)`).

Consequence: 1920x1080@60 (148.3 MHz) and 1680x1050/1920x1200 fall back to
RGB565; everything up to 1600x900/1280x1024 stays RGB888. Expect visible
gradient banding on the VGA monitor only at the modes that changed.

Build note: the Makefile used `M=$(PWD)`, which breaks any build invoked as
`make -C fl2000_driver` from another directory (the kernel kbuild then looks
for a Makefile in the wrong objdir). Switched to `M=$(CURDIR)`; also dropped
the `CHECK=/usr/bin/sparse` hook (sparse is not installed here). DKMS build
and install: `install.sh` at the repo root (creates fl2000_drm/1.0.2).

## 1080p still flashing — the RGB565 fallback above never fired (units bug)

Symptom: after the previous fix, 1920x1080@60 still showed bright flashes,
strongest at the bottom of the screen.

Cause found in `fl2000_get_bytes_pix()`: the bandwidth test compared
`(u64)pixclock * 3 <= max_bw` with `pixclock` in **kHz** (DRM convention,
148500 for 1080p60) against `max_bw` in **bytes/second** (350 000 000).
`148500 * 3 = 445 500 <= 350 000 000` is true for every mode the PLL filter
allows, so the function always returned 3 (RGB888) and the RGB565 fallback
never triggered — the dongle kept draining 445 MB/s from a ~350 MB/s bus, the
line buffer bottomed out during each frame, and the DAC flashed where the
buffer hit the floor (bottom of screen, frame end). The top of the frame is
immune because the buffer is fullest right after the frame boundary.

Fix: compare in consistent units — `(u64)pixclock * 3000` (RGB888) and
`(u64)pixclock * 2000` (RGB565) against the bytes/second budget. At SuperSpeed
1080p60 (148.5 MHz) now selects RGB565 (297 MB/s drain) as originally
intended; 1600x900/1280x1024@60 stay RGB888; 1680x1050/1920x1200 fall to
RGB565; modes whose RGB565 drain still exceeds 350 MB/s are now filtered out
of the mode list. USB 2.0 High Speed and SuperSpeed+ return RGB888
unconditionally (old behaviour: no honest HS budget allows any real mode, and
10 Gbps covers RGB888 at the pixel clock ceiling).

Verification after reload: `sudo dmesg | grep "Pixel format"` must show
`2 bytes/pixel (RGB565)` at 1920x1080. `force_bpp` still overrides everything
for A/B tests.

## 1080p missing from the mode list after reboot — dead htotal-adjustment loop

Symptom: after a reboot the VGA monitor no longer offered 1920x1080; the
kernel mode list topped out at 1680x1050 and GNOME fell back to 1280x720.

Cause: the monitor's EDID (`LG FULL HD`, one detailed timing) advertises
1920x1080@60 at exactly 148.5 MHz. The driver's `mode_valid` runs every mode
through the PLL synthesis check (500 ppm budget), and 148.5 MHz cannot be
synthesised with the 1 GHz VCO cap (best: 890/6 = 148.333 MHz, 1121 ppm).
The documented ±10 htotal-adjustment search was supposed to fix exactly this,
but the loop variable was dead: `for (int m = 0, s = 0, ...)` — `s` starts at
0, `-0 == 0`, so `d` never left 0 and only the raw pixel clock was ever
checked. Same bug as upstream. The rejected DTD made the mode list lose
1080p (and 1440x900/1400x1050/1280x800, whose clocks also fail unadjusted).

Fix: `s = 1` so `d` follows the intended sequence 0, -1, 1, -2, 2, ... .
1920x1080 now validates with htotal 2198 at 148.333 MHz (213 ppm) — the
adjusted timing is what `fl2000_output_mode_set()` programs, and analog VGA
tolerates the 0.1% htotal change. 1440x900/1400x1050/1280x800 come back too.

Verification after reinstall (`install.sh`, then unplug/replug or reboot):
`xrandr` lists 1920x1080 for VGA-1 and dmesg shows
`Mode configured: "1920x1080"` with clock 148333 and
`2 bytes/pixel (RGB565)`.

## 1080p still missing after reboot #2 — no-EDID fallback list never had it

Symptom: after a cold reboot VGA-1 again topped out at 1680x1050, even with the
fixed module loaded (srcversion of `/lib/modules/.../updates/dkms/fl2000.ko`
matched the local build). The earlier verification had been done in a session
where the EDID *was* read (`LG FULL HD` detailed timing → 1080p came from the
DTD); on this boot `card1-VGA-1/edid` was 0 bytes.

Cause: with no EDID the driver relies on `drm_add_modes_noedid(1920, 1200)`,
and the resulting list (10 modes: 640x480 … 1680x1050) simply does not carry
1920x1080@60. Hand-checking the PLL math confirmed the fixed `mode_valid`
would accept it (d = -2 → htotal 2198, 890/6 = 148.333 MHz, ~213 ppm; RGB565
drain 297 MB/s ≤ 350 MB/s budget) — the mode is rejected by absence, not
validation. Whether the fallback table carries 1080p varies by kernel version;
this dongle's DDC also reads nothing on many cold boots (documented above),
so EDID-dependent availability made 1080p disappear "at random".

Fix in fl2000_drm.c:

- `fl2000_connector_get_modes()` now injects an explicit CEA-861
  1920x1080@60 (148.5 MHz, htotal 2200) into the no-EDID mode list when the
  fallback table did not provide one. `mode_valid` still filters it.
- `fl2000_display_mode_valid()` logs rejections via `dev_dbg()`:
  `no PLL setting within tolerance` / `pixel drain exceeds USB bandwidth
  budget`. Enable with dyndbg to see exactly why a mode drops out.

Verification after reload: `xrandr` must list 1920x1080 under VGA-1 even with
`cat /sys/class/drm/card1-VGA-1/edid | wc -c` == 0.

## Debugging notes

- regmap debugfs exists at `/sys/kernel/debug/regmap/2-4/` but is of limited
  use: `registers` is read-only and dumping it would issue ~16384 USB control
  transfers (`max_register = 0xFFFF`, stride 4). There is no register-write
  file. Add `dev_dbg()` to the driver instead.
- The driver is almost entirely silent by default. Enable dynamic debug:
      echo 'module fl2000 +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
- `fl2000.ko` depends on `drm_dma_helper`; `modprobe drm_dma_helper` first when
  using `insmod`.

## Session log — 2026-09-04 (session 4): crash triage via kdump, unplug-panic fix, OOB fix, stall watchdog

Machine had hard-reset during the previous session; this session reconstructed
events from the journal and the kdump vmcores before touching code.

### Incident reconstruction (journal + /var/crash)

Three kernel crashes the same day, all in fl2000. Boots:

- kdump `202609040702` (uptime 85167 s): crash in a long-running boot.
- kdump `202609040716` (uptime 730 s): crash right after the first reboot.
- kdump `202609041234` (12:34): the one the user reported ("reinicou ao
  desplugar da última vez"). The 12:34 `dmesg` is root-only, but the boot
  signature is identical: vblank stall WARN at 12:32:07, then boot -2 ends
  abruptly at 12:34:20 with NO `USB disconnect` line ever reaching the journal,
  and boot -1 (44 s) is the kdump capture boot (`kdump-tools-dump.service`).
  I.e.: screen went black (stall) → user unplugged → panic in disconnect →
  kdump → automatic reboot. Same as 0716.

### Crash 1 — OOB write in fl2000_stream_compress (kdump 202609040702)

    fbcon restore path: drm_release → drm_client_dev_restore → commit
    → fl2000_display_update → fb2000_dirty → fl2000_stream_compress
    #PF supervisor WRITE, RIP fl2000_stream_compress+0x16e

Registers at fault: width 1920, height 1080, pitch 7680 (0x1e00) — while the
log right before shows `Mode configured: 1024x768 RGB888`. The fbdev client's
restore commit put its old 1920x1080 fb onto the 1024x768 preferred mode
(`drm_set_preferred_mode(1024,768)`); the atomic plane check clips silently,
but `fb2000_dirty()` passed `fb->height/fb->width` to compress — walking
6.2 MB into 2.25 MB buffers. OOB page fault, machine down.

Fix (fl2000_drm.c): `fb2000_dirty()` now takes clip dims from the caller;
`fl2000_display_update()` clips to `crtc->state->adjusted_mode`
hdisplay/vdisplay. Plus a defensive check in `fl2000_stream_compress()`
that drops the frame if `height*width*bytes_pix > buf_size` instead of
walking off the buffers.

### Crash 2 — hot-unplug panic: use-after-free in fl2000_stream_disable (kdump 202609040716, and the 12:34 one)

    hub_event → usb_disconnect → fl2000_disconnect → fl2000_drm_unbind
    → fl2000_drm_if_release → drm_atomic_helper_shutdown
    → drm_atomic_helper_disable_all → fl2000_display_disable
    → fl2000_stream_disable → up() → Oops (NULL deref in __up)

Root cause: `fl2000_drm_if_release()` destroyed the stream
(`fl2000_stream_destroy()` frees the devres) BEFORE
`drm_atomic_helper_shutdown()` ran. Shutdown runs a disable-all commit whose
`.disable` callback calls `fl2000_stream_disable(drm_if->stream)` — a
dangling pointer. The `!stream || IS_ERR(stream)` guards added in commit
974fe1d are useless against dangling: the freed devres node was reused/zeroed
and `up()` went to address 0x8. Every hot-unplug under an active stream was
a latent panic — this is the "reiniciou novamente" bug.

Fix (fl2000_drm.c): `fl2000_drm_if_release()` order is now

    component_unbind_all → drm_kms_helper_poll_fini
    → drm_atomic_helper_shutdown   (pipe callbacks still see a live stream)
    → drm_dev_unplug                (drm_dev_enter fails: no late commit/restore)
    → fl2000_stream_destroy + drm_if->stream = NULL
    → fl2000_intr_destroy  + drm_if->intr = NULL
    → drm_edid_free

The unplug-before-destroy also closes the fbdev-restore-after-teardown window
of crash 1: after `drm_dev_unplug()`, `fb2000_dirty()` fails fast via
`drm_dev_enter()`.

### The recurring black screen — evidence, not a guess any more

The journal shows `vblank wait timed out` on commit_work at 09:53, 11:01,
12:15 (boot -3) and 12:32 (boot -2). Vblanks on this device ARE URB
completions, so each WARN is a post-mortem of a silent streaming stall: the
monitor loses sync (black), the connector still reports connected (mouse
cursor travels across the dead output), and no driver error is logged
anywhere.

Host-side analysis: the transmit worker's semaphore/URB accounting is
balanced (every `down()` either submits a URB — which guarantees a future
`up()` from its completion — or `continue`s only while ≥1 URB is in flight),
and every worker exit path logs. So a silent stall means the chip or the
xHCI endpoint stopped completing bulk URBs. Wedged, in-flight, zero
completions: the driver had no way to even notice.

Fix (fl2000_streaming.c): a stall watchdog.

- `last_completion` jiffies stamped in the data completion.
- Watchdog `delayed_work` on its own workqueue (`fl2000_stall`, freezable —
  the transmit worker parks inside its work function on `work_sem`, so an
  ordered queue shared with it would never get to run). Poll 1 s, stall
  threshold 250 ms.
- On stall, a ladder (all steps log loudly — this is the evidence the
  previous occurrences never left):
  1. soft recovery ×2: scuttle the anchor (never poison — poison blocks
     until the HCD returns every URB, which never happens on a wedged port
     and would pin the watchdog worker) + kick the worker twice.
  2. hard recovery ×2: `fl2000_stream_reprogram()` in fl2000_drm.c —
     app-reset + reprogram the last mode (PLL/timings/pixfmt/transfers/
     interrupts/AFE, same sequence as output_mode_set; config stored in
     drm_if). Control transfers with bounded timeout, cannot hang. If the
     reset chip resumes ACKing, the scuttled in-flight URBs complete and
     their buffers return by themselves.
  3. give up: one loud error telling the user to replug; keeps watching and
     logs "stream resumed" if it ever comes back on its own.
- Armed in `fl2000_stream_enable()`, cancelled in `fl2000_stream_disable()`
  (first thing after `enabled = false`) and in release.

Known accepted race: the hard reprogram takes no modeset locks, so it could
interleave with a real modeset; a stalled output is already broken and the
next modeset rewrites everything anyway.

### Also fixed

`fl2000_submit_urb()` (fl2000.h): retried -ENXIO/-ENOMEM with `cond_resched()`
unconditionally — called from the interrupt URB's completion (hard IRQ) this
would BUG ("scheduling while atomic"). Retry now only when `!in_interrupt()`.

### Test procedure (needs sudo, which this session could not run)

The dongle is currently plugged and the OLD module is loaded (refcount 3,
compositor holds the card): rmmod is impossible until unplug.

1. `cd /home/xbral/fl2000_driver && make && sudo ./install.sh`
   (install.sh: unplug → `sudo rmmod fl2000` → install → replug+modprobe)
2. To avoid exercising the OLD module's unplug-panic path during the swap,
   disable the VGA output in the desktop display settings first (stream
   stopped ⇒ `drm_atomic_helper_shutdown` has no active CRTC to disable ⇒
   the UAF path is not reached), then unplug.
3. `sudo rmmod fl2000`, replug the dongle (USB-3 port!), confirm the new
   module binds: `dmesg | grep fl2000`, `cat /proc/sys/kernel/tainted` should
   stay without the W bit.
4. Re-enable the display. Then:
   - Unplug with the stream ACTIVE (the old panic scenario) — expect a clean
     unbind, no reboot, no kdump dir in /var/crash.
   - Normal use at 1920x1080. If the screen goes black again, the watchdog
     now says exactly what happened in dmesg (`stream stall: ... soft
     recovery`, `chip reset recovery`, or `giving up`). Collect that log —
     it decides whether the stall is chip-side (recovery helps) or xHCI-side
     (recovery fails, next step would be endpoint reset at the USB layer).
5. No dry-run trigger for the watchdog is known (the stall is spontaneous);
   just use the machine and watch dmesg if the screen ever goes black.

### Incident #3 — false disconnect killed the output (2026-09-08)

Different signature from the recurring stream stall (no `stream stall` logs
at all — the watchdog never fired, so the bulk stream was alive until the
end). Journal evidence, boot 18:32, 1920x1080@60 RGB565:

    19:02:39  i2c rd addr 0x50 off 0x00 timed out: post 0x0 (done 0 status 0x0
              ready 0 mon_det 0 edid_det 0)   <- chip detect circuit says NO monitor
    19:05:03  same i2c timeout again
    19:07:41  WARN drm_atomic_helper_wait_for_vblanks: vblank wait timed out (commit_work)
    19:20:15  usb 4-1: USB disconnect         <- user unplugged the dongle

The DDC fallback in `fl2000_connector_detect()` ran because the 0x805C sense
read itself failed (control-plane of the detect block went down mid-session).
`mon_det=0` + hung i2c with the cable still attached: the chip's detect block
died, the poll reported disconnected, the compositor disabled the output — the
~10 s vblank wait holding every modeset lock is the "froze, then went black"
the user saw. Replugging the dongle restores it every time.

Conclusion: sense==0 / sense-error is NOT proof the monitor is gone; only
sense==1 is trustworthy. Fix (commit `sticky-connect`): once the sense
register has seen a monitor in this session, `detect()` stays connected no
matter what it reports later (`monitor_seen` latch in fl2000_drm_if).
`sticky_connect=0` restores the old behavior. A genuinely unplugged monitor
just keeps receiving signal — harmless — while the false-disconnect class
(black screen needing a dongle replug) is eliminated.

### Loose ends / next session

- If the watchdog's hard recovery works (screen comes back), consider arming
  it permanently and lowering the thresholds (250 ms is conservative).
- If the stall persists through hard recovery, suspect the xHCI endpoint:
  a next step is `usb_reset_device()` from the watchdog (heavier, revokes
  all interfaces — needs care with the three-interface binding) or digging
  into xHCI event/TRB logs when it happens.
- /var/crash/202609041234/dmesg is root-only; confirm its trace matches
  crash 2 when convenient (`sudo cat /var/crash/202609041234/dmesg.202609041234 | grep -A30 BUG`).
- 3 kdumps ate ~1.2 GB in /var/crash; safe to delete the vmcores once the
  traces are confirmed.
- The 07:02 crash also proves the fbdev client restore path is exercised on
  every compositor crash/fd close; the clip fix covers the geometry mismatch,
  but `drm_set_preferred_mode(1024,768)` vs the 1080p fbdev fb mismatch is
  worth a look if restore glitches remain.

## Endpoint reset rung in the stall ladder (2026-09-08, session 5)

Incident at 20:12: the recurring spontaneous stall again, but with new
evidence: the watchdog's hard recovery ran twice and **succeeded** (app-reset
+ reprogram completed, no errors) yet the bulk stream never resumed — control
plane alive, bulk plane dead. That signature points at the xHCI bulk endpoint
state, not the chip.

Change (fl2000_streaming.c): inserted a host-side endpoint reset rung between
soft and hard recovery:

    recoveries 1-2   soft: scuttle anchor + kick worker (unchanged)
    recoveries 3-4   usb_reset_endpoint(usb_dev, 0x01)  <-- new
    recoveries 5-8   chip app-reset + mode reprogram (unchanged)
    recovery 9       give up (unchanged)

Notes:
- `usb_reset_endpoint()` returns void on kernel 7.0 (no error to log).
- It is a no-op on xHCI unless the endpoint is in HALTED/STOPPED state, so a
  device-side NAK wedge (endpoint still "Running") may not be cured by it —
  but it is cheap, bounded and the designated first host-side attempt. If it
  proves ineffective, the next hammer is `usb_queue_reset_device()` (deferred
  full re-enumeration; safe from the watchdog workqueue, but tears down and
  recreates all three interfaces — verify the component rebind path handles
  it before arming).
- Verification: next stall should show `endpoint reset recovery 3`/`4` lines
  between the soft and chip reset lines.

## Device reset rung: the stall recovery that actually works (2026-09-09, session 6)

Field evidence, today (boot Sep 8 20:29):

    38364  Mode configured 1920x1080 (streaming starts)
    54434  stream stall - 4.5 HOURS of clean streaming before the wedge
    54527  stream re-enable (modeset) - stall again within 1 s: chip still wedged
    54558  replug -> Mode configured, streaming resumes
    54897  stream stall - 5.6 MINUTES this time
    55031  user unplug; replug -> EDID fail at bind -> no modeset -> black screen

Every rung of the existing ladder failed. Why:

1. **Soft rung was a no-op.** Scuttling removes the stuck URBs from the anchor
   but the HCD still owns them; the two buffers pin `wait_list` at
   FL2000_MAX_INFLIGHT and the worker's throttle (`continue` while
   inflight >= 2) blocks every resubmission. Recovery literally could not
   submit anything.
2. **Endpoint reset refused.** `usb_reset_endpoint` is a no-op on xHCI while
   the ring has pending TDs: dmesg shows `EP not empty, refuse reset`.
3. **Chip app-reset is irrelevant.** Control transfers complete, the chip
   reprogram succeeds (no errors logged), but bulk completions never resume:
   the chip's *streaming engine* is wedged and its app-reset doesn't touch it.

Only a physical replug revived the display — a port reset. So the fix gives
the watchdog a software port reset:

    recoveries 1-2   soft: pop anchor + usb_unlink_urb each stuck URB (async
                     cancellation; givebacks return the buffers and the worker
                     resubmits). Unlink, not scuttle: scuttle lost the URB
                     references, unlink actually cancels them.
    recovery 3       usb_reset_endpoint (kept; now meaningful if the unlink
                     givebacks emptied the ring)
    recoveries 4-5   usb_lock_device_for_reset + usb_reset_device: park the
                     worker (enabled=false + drain) so no URB is submitted
                     across the reset, reset the port, reprogram the chip from
                     its post-reset state (reprogram callback now also
                     re-selects the streaming altsetting and runs
                     fl2000_usb_magic), restart the interrupt URB
                     (fl2000_intr_restart - the reset aborts it and the
                     completion no longer resubmits on aborts), re-init the
                     anchor/semaphore and restart the stream.
    recovery 6       give up

Side fixes:
- fl2000_intr_completion() marks the URB inactive on aborts instead of
  resubmitting into a resetting device (which always failed -ENODEV).
- Expected side effect of the hard rung: the monitor blinks ~1-2 s during the
  device reset (chip re-inits). Better than a permanent black screen.
- Open question for prevention: the wedge interval is wildly variable (4.5 h
  vs 5.6 min), so it is not a steady drift - looks like the chip's streaming
  firmware crashes on some rare race (maybe an underrun). The device reset is
  the only cure; a USB capture of the last traffic before a wedge could
  confirm, but the recovery now makes it self-healing.
