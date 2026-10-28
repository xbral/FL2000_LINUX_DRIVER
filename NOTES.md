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

## Hardening pass (2026-09-09, session 7)

Software-only improvements from the "what could be better" review. All build
clean (`make`, `make W=1`) and pass checkpatch on the diff; none touch the
chip register sequences, so no hardware validation was required to land them.

1. **System suspend/resume implemented** (`fl2000_drv.c`, `fl2000_drm.c`).
   The old callbacks were stubs with a swapped dev_dbg (suspend logged
   "resume"). They now drive `drm_mode_config_helper_suspend()/resume()` from
   the AVCONTROL interface only (the driver is bound to all three interfaces;
   calling it three times would race itself). Resume restores the streaming
   altsetting, re-programs the saved mode through the helper, re-applies
   `fl2000_usb_magic()` and restarts the interrupt URB. The freezable stream
   worker exits on the freezer's fake signal (`down_interruptible` -> -EINTR),
   so streaming is already stopped before the USB device suspends.

2. **Modeset vs. watchdog reprogram serialized** (`fl2000_drm.c`). The accepted
   race from session 4 (hard recovery reprogramming registers without modeset
   locks) is closed with a `mode_lock` mutex held around the register sequence
   in both `fl2000_output_mode_set()` and `fl2000_stream_reprogram()`. Both run
   in process context (commit worker vs. watchdog workqueue), so a mutex is
   sufficient and cannot deadlock against the stream worker.

3. **Bind failure now cleans up** (`fl2000_drm.c`, `fl2000_drv.c`). Every early
   return in `fl2000_drm_bind()` went to a single error path that releases the
   stream/interrupt devres (they live on `&usb_dev->dev` and would otherwise
   leak until replug), tears down the bridge/poll if already up and clears the
   master drvdata. `fl2000_probe()` frees the shared regmap/i2c resources when
   registration fails instead of leaking them (the old "TODO: Halt driver on
   initialization failure").

4. **Detect circuit re-arm instead of pure sticky** (`fl2000_drm.c`,
   `fl2000_registers.c`). On sense != 1 with the monitor already seen, the
   driver now rewrites the detect enable bits once per loss episode and re-reads
   `0x805C`. If the block comes back it reports connected; only then does it
   fall back to the sticky latch. A genuinely unplugged monitor costs one
   control write per episode, not one per 10 s poll.

5. **EDID read retry** (`fl2000_drm.c`). A failed first `drm_edid_read_custom()`
   is retried once after re-arming the detect block (replug / just-revived
   block) before falling back to the VESA mode list.

6. **Stall forensics** (`fl2000_streaming.c`). The watchdog now logs the last
   URB status plus per-session completion/error counters, and snapshots the
   chip status register (`fl2000_log_vga_status()`) at the first soft recovery.
   Control transfers still complete during the wedge, so this captures the
   chip's own view of the dead output - the evidence every previous occurrence
   never left. Root cause (why the streaming engine wedges) still needs the USB
   capture noted above.

7. **IT66121 DDC serialization** (`bridge/it66121_drv.c`). Added the mutex the
   old `XXX: lock` comments called for, around the DDC handshakes in the
   interrupt worker and the full EDID read sequence.

8. **`make check`** runs sparse (`C=1`) for static analysis; optional, not in CI.

## EPROTO flood slipped past the watchdog (2026-09-09, session 8)

User report: "o monitor parou de exibir imagem novamente, tive que retirar o
dongle para poder voltar a ter imagem". New signature, same wedge underneath.
Journal (boot Sep 9 13:26, 1920x1080@60 RGB565):

    17:53:36  usb 4-1: Nonzero urb status, -71   (first of a flood)
    ...       5308 x -71 over 74 s (EPROTO), monitor black
    17:54:50  usb 4-1: USB disconnect -> user unplugs to recover
    17:56:39  replug: Bind, Mode configured, picture back

No `stream stall` / `recovery` / `gave up` line was ever logged: the watchdog
never fired. Root cause in `fl2000_stream_data_completion()`: `last_completion`
was refreshed on *every* completion, including error ones. The wedge had
changed shape from the old silent one (zero completions) to an -EPROTO flood
(completions at frame rate, none of them carrying pixels), so the liveness
signal the watchdog keys on stayed fresh while the output was dead. The whole
recovery ladder - including the USB device reset that is the only rung that
actually revives this wedge - was never reached, forcing a physical replug.

Fix:

- `fl2000_stream_data_completion()` refreshes `last_completion` only when
  `urb->status == 0`. Any error completion leaves the timer alone, so a
  sustained error flood trips the 250 ms threshold and the existing ladder runs
  (soft unlink -> endpoint reset -> device reset), turning the manual replug
  into a ~4 s self-heal. A lone transient error followed by a success still
  clears the episode as before.
- `fl2000_urb_status()` logs the unexpected status with
  `dev_err_ratelimited()`. 5308 synchronous `dev_err()` calls in 74 s flooded
  the log; the watchdog's own message remains the forensic evidence.

Not changed: the ladder order. Soft/endpoint rungs are known no-ops for this
wedge (session 6), but they are cheap, bounded, and harmless on the EPROTO
path (the in-flight URBs give back -ENOENT on unlink), and keeping one generic
ladder avoids a second recovery code path.

Verification: next EPROTO flood must show `stream stall: no URB completion ...`
followed by `endpoint reset recovery` / `USB device reset recovery` in dmesg
within a few seconds, and the picture should return without unplugging.

### Install blocker found while testing: Xwayland pins deleted DRM nodes

With the dongle already unplugged, `rmmod fl2000` still failed with `in use`
(refcnt 6). Cause: **Xwayland (PID 6651) held three open fds to deleted DRM
nodes** - `card0`, `card2` and `card3`, the last being this dongle's card. Each
open fd holds `try_module_get(fl2000_drm_driver_fops.owner)` (the
`DEFINE_DRM_GEM_FOPS` owner is THIS_MODULE), so the compositor's X11 proxy kept
the module alive after the USB device was gone. Killing Xwayland released 3 of
the 6 references; the remaining 3 are a genuine refcount leak not tied to any
fd or sysfs device (`find /proc/*/fd`, `/sys/bus/usb/drivers/fl2000_usb`,
`/sys/class/drm` all clean), and `CONFIG_MODULE_FORCE_UNLOAD` is off, so only a
reboot clears them. Recorded as a separate pre-existing bug; it does not affect
the display fix but does break the unplug -> rmmod -> install loop.

Installed 1.0.4 to disk while the old module was still resident (safe: file
copy only) and rebooted to load it, since a fresh module load is required.

Investigated with kprobes (session 8, after the reboot). Conclusion: **there is
no driver-side refcount leak.** The refs are all userspace DRM opens.

- In this kernel a driver bound to a device does **not** hold a module
  reference: `usbhid` has 3 bound interfaces and `refcnt 0`. So the fl2000
  refcount comes only from explicit `try_module_get()` calls.
- The DRM and fbdev layers take one per user `open()` and drop it on `release`:
  `DEFINE_DRM_GEM_FOPS` sets `fops->owner = THIS_MODULE`;
  `drm_fbdev_shmem_fb_open()` does
  `try_module_get(fb_helper->dev->driver->fops->owner)`; `i2cdev_open()` /
  `i2c_get_adapter()` take one on `adapter->owner` (i2c-15 here, no openers).
- With the dongle plugged, `refcnt` matches the userspace clients:
  `/sys/kernel/debug/dri/2/clients` lists only `systemd-logind` and
  `gnome-shell`, and the fd scan adds Xwayland's stale `card0 (deleted)`. The
  fl2000 card is DRM minor 2 (`card2`) on this boot; `card0` is a dead minor
  from the pre-reboot session.
- No leaked DRM device in `/sys/kernel/debug/dri/`, no leftover `4-1:*`
  interfaces, no leaked fbdev - the kernel objects are all torn down.

The recurring blocker is therefore userspace: after hot-unplug the compositor
closes its own card fd (observed as a `module_put` from gnome-shell), but
**Xwayland keeps a stale fd to the deleted node**, pinning the module. Multiple
unplug/replug cycles can leave more than one. `rmmod` is blocked by design while
any such fd is open (`CONFIG_MODULE_FORCE_UNLOAD` off), so module swaps still
need `rmmod` after the fds close - i.e. restart Xwayland/log out, or reboot.
This is not a driver bug and needs no kernel change; a driver cannot force a
userspace fd closed.

Note for the kprobe technique: filter the `try_module_get`/`module_put`
arguments with `m=$arg1` (not `%di`) and remember to clear the `stacktrace`
trigger (`echo '!stacktrace' > events/kprobes/EV/trigger`) before deleting the
probe, otherwise `kprobe_events` stays busy.

## USB reset recovery self-deadlock froze the machine (2026-09-10, session 9)

User report: "o monitor que está ligado pelo dongle usb desligou novamente e o
computador todo travou" - and asked whether the PC or the monitor froze first.
The driver was the cause: the monitor going black was the known stream wedge,
but the *machine* freeze was the watchdog's own USB-reset recovery deadlocking on
itself. Boot `ec273cfd` (Sep 9 18:12 - Sep 10 11:18), 1920x1080:

    11:10:19  [CRTC:37:crtc-0] vblank wait timed out  (WARNING, kworker/u32:6)
    11:10:20  stream stall, soft recovery 1
    11:10:20  -EPROTO flood (5308 x -71 total)
    11:10:21  stream stall, soft recovery 2 (last status -71)
    11:10:22  stream stall persists, endpoint reset recovery 3
    11:10:23  stream stall persists, USB device reset recovery 4
    11:10:23  fl2000_usb 4-1:1.0: Unbinding FL2000 master   <- the trap
    11:11:25  USB disconnect
    11:12:33  INFO: task kworker/u32:6 blocked for more than 122 s
              Workqueue: fl2000_stall fl2000_stream_watchdog
              __flush_work <- cancel_delayed_work_sync <- fl2000_stream_disable
              <- fl2000_display_disable <- drm_atomic_helper_shutdown
              <- fl2000_drm_unbind <- fl2000_disconnect <- usb_unbind_interface
              <- usb_forced_unbind_intf <- usb_reset_device <- fl2000_stream_watchdog
    repeats 11:14:36 (245 s), 11:16:39 (368 s)
    11:14:36  INFO: kworker/7:0 (usb_hub_wq hub_event) blocked in usb_disconnect ...
              "blocked on a mutex likely owned by task kworker/u32:6"
    11:18:26  last journal entry; hard freeze, no panic, no kdump
    11:20:12  reboot

Root cause: `usb_reset_device()` force-unbinds every bound interface whose driver
does *not* provide both `pre_reset` and `post_reset` (`drivers/usb/core/hub.c`,
the `unbind = ...` / `usb_forced_unbind_intf()` loop before the reset). The
fl2000 USB driver provided neither, so recovery 4 - issued from *inside* the
`fl2000_stream_watchdog` work item - synchronously ran
`fl2000_disconnect -> fl2000_drm_unbind -> drm_atomic_helper_shutdown ->
fl2000_display_disable -> fl2000_stream_disable`, and that last function's
`cancel_delayed_work_sync(&stream->watchdog_work)` waited on the very work item
executing it. The worker never returned, held `usb_dev->mutex`, and `hub_event`
then blocked in `usb_disconnect()` on that mutex: the entire USB stack wedged and
the machine hard-froze. (Even without the deadlock, the unbind would have freed
`stream` via devres while the watchdog still used it - a use-after-free.)

Fix:

- **`pre_reset` / `post_reset` on `fl2000_driver`** (`fl2000_drv.c`,
  `fl2000_drm.c`). Both callbacks are now present, so `usb_reset_device()` resets
  the port *in place* instead of force-unbinding the interface. `pre_reset`
  quiesces the stream (`fl2000_stream_disable`, only on AVCONTROL); `post_reset`
  reprograms the chip from its wiped post-reset state (`fl2000_stream_reprogram`)
  and restarts the aborted interrupt URB. The watchdog then drops giveback
  stragglers and re-kicks the transmit session as before. Return 0 from both =
  "no unbind/rebind needed".
- **Self-cancel guard** (`fl2000_streaming.c`). `fl2000_watchdog_cancel()` skips
  `cancel_delayed_work_sync(&watchdog_work)` when `current_work()` *is* the
  watchdog, used by both `fl2000_stream_disable()` and `fl2000_stream_release()`.
  Required because `pre_reset` disables the stream from within the watchdog
  context; it also makes any future teardown path safe.
- Removed the now-dead `fl2000_stream_set_recovery()` reprogram callback plumbing
  (the DRM layer calls `fl2000_stream_reprogram()` directly from `post_reset`).

Verification: next stall must show `USB device reset recovery` followed by the
chip reprogram and `stream resumed` / fresh completions, with **no**
`Unbinding FL2000 master` and no hung-task lines. A `usbfs`-initiated reset
(rare here - the watchdog is the only caller) restores the hardware but leaves
the stream off until the next modeset; the watchdog path is unaffected.

## Chip self-reset escapes the watchdog; the "logout" was a mutter crash (2026-09-16, session 10)

User report: "depois de alguns dias funcionando normalmente, o monitor agora a
pouco, apagou a tela e não voltou, quando retirei o usb e reconectei, ele fez
logout na conta". Boot `e399581d` (Sep 15 18:36 - open), 1920x1080@60 RGB565,
module loaded at boot. Two USB re-enumerations on `4-1`; the user confirmed the
first one was NOT a physical unplug:

    07:31:14       last clean modeset (1920x1080)
    07:50:45.392   [CRTC:37:crtc-0] vblank wait timed out (WARNING, commit_work)
    07:50:45.732   usb 4-1: USB disconnect, device number 2      <- +339 ms
    07:50:45.734   usb 4-1: Data URB error -19
    07:50:45.955   usb 4-1: new SuperSpeed USB device number 3   <- re-enumerated
    07:50:45.992   [drm] Initialized fl2000_drm ... on minor 0   <- was minor 1
    07:50:45       gnome-shell: drmModeRmFB: No such device /
                   Zero-copy disabled for /dev/dri/card1, import failed /
                   Failed to acquire front buffer: Missing secondary GPU framebuffer /
                   Page flip failed: drmModeCreatePropertyBlob: No such device
    08:00:55       usb 4-1: USB disconnect, device number 3      <- user unplug
    08:00:57       usb 4-1: new SuperSpeed USB device number 4
    08:00:57       gnome-shell: Failed to hotplug secondary gpu '/dev/dri/card0':
                   device already present
    08:00:58       gnome-shell: GNOME Shell crashed with signal 11 (SIGSEGV)
    08:01:08       org.gnome.Shell@ubuntu.service: Failed with result 'core-dump'
    08:01:08       session shutdown, logout; relogin modesets 1920x1080 again

Not a driver regression, and the session-9 fix held:

- `boot -2` (Sep 12 07:44 - Sep 15 18:35, ~3 days) ran this same module
  (srcversion `89522E6B796C39ABFB45C5C`) with **zero** USB disconnects: the
  "few days working" were genuinely clean.
- The whole incident boot has no `stream stall` / `recovery` / `Unbinding` /
  hung-task line and taint is only `OE`. The pre_reset/post_reset path did not
  trigger and did not need to: the device left the bus before any watchdog rung
  ran.

Root cause of the black screen: the chip wedged its streaming engine and then
**self-reset at the USB level** (disconnect + re-enumeration) 339 ms after the
last successful completion. The host cannot prevent a device-initiated
disconnect: the watchdog (`STALL_TIMEOUT` 250 ms, `WATCHDOG_PERIOD` 1 s) cannot
fire inside that window, and even if it did, the recovery ladder takes ~4 s and
a host `usb_reset_device()` against a chip that is already resetting also ends
in re-enumeration. Re-registration landed on a **different DRM minor**
(`card1` -> `card0`, freed by simpledrm when i915 took over), so mutter, which
held `/dev/dri/card1`, lost its front buffer and left the output black.

Root cause of the logout: mutter 50.1 SIGSEGV in its secondary-GPU hotplug path
(`Failed to hotplug secondary gpu '/dev/dri/card0': device already present`),
not the driver. systemd logged `org.gnome.Shell@ubuntu.service: Failed with
result 'core-dump'` and tore the session down.

Frequency: intermittent, seen before on this dongle (`boot -4`/`boot -5` had
re-enumerations too; `boot -2` did not). The interval is wildly variable,
matching the long-standing "streaming firmware crashes on some rare race"
hypothesis from session 6.

Where to go from here (no driver fix proposed):

- Physical: test another USB-3 port (direct, no hub) or a powered hub. "Hours of
  clean streaming, then the chip resets under bulk load" is also consistent with
  a port power/brownout event, which the host driver cannot observe.
- Mutter: the SIGSEGV on secondary-GPU hotplug is reproducible and is worth a
  GNOME report, independent of fl2000.
- Driver: a faster watchdog (100 ms) would most likely lose the 339 ms race and
  risks resetting on transient hiccups; not worth it without a USB capture of
  the wedge moment.

Verification status: display recovered after the relogin (modeset at 08:01:08);
the machine did NOT freeze and there was no fault in fl2000 - this incident is
entirely outside the driver's reach.

## Device-reset rung completes, then the watchdog never runs again (2026-10-16, session 11)

User report: "o monitor parou de funcionar novamente e não voltou, ele ainda
está conectado via usb porém com a tela preta". Boot began Oct 13 07:50, module
`1E50DE90CD5049885E78440`, same USB path as every other occurrence (`4-1`).

Timeline (Oct 16). Unlike session 10, the host-side ladder DID run, and unlike
every occurrence before it, the device stayed enumerated on the same minor:

    09:02:04  Mode configured: 1920x1080@60 RGB565      <- last good modeset
    09:33:26  stream stall: no URB completion for 591 ms (2 URBs in flight),
              soft recovery 1 (last status 0, 112877 completions, 0 errors)
    09:33:26  VGA status 0x33fffee3: connected 1 ... pll_up 1 dac_up 1
    09:33:26  fl2000_urb_status: 208 callbacks suppressed
    09:33:26  Nonzero urb status, -71                    x73  (EPROTO)
    09:33:27  stream stall: ... soft recovery 2 (last status -71, 73 errors)
    09:33:28  stream stall persists, endpoint reset recovery 3
    09:33:29  stream stall persists, USB device reset recovery 4
    09:33:29  reset SuperSpeed USB device number 3 using xhci_hcd
    09:33:29  Stopping interrupts
    09:33:29  USB device reset: reprogramming chip mode
              <-- LAST LINE THE DRIVER EVER PRINTED. Silent for the next 1h45.

112877 completions since 09:02 is 60 fps exactly: the stream was healthy and the
chip stopped mid-frame. Same signature as every previous occurrence (bulk
completions stop, control transfers keep answering, `-71`/EPROTO floods the
endpoint), same escalation path - what is new is what happened *after* rung 4.

### What the reset left behind

Measured while the monitor was black:

- **No re-enumeration.** Still `Bus 004 Device 003`, `bConfigurationValue=1`,
  5000 Mb/s. The in-place `pre_reset`/`post_reset` design held: same DRM minor
  (`card0`), `card0-VGA-1` never left `/sys/class/drm`, connector still reads
  `connected` + `enabled`. Mutter therefore never noticed and never re-modeset.
- **The transmit worker is parked.** `/proc/2459297/stack` reads
  `fl2000_stream_work+0x75`, and `+0x75` is the instruction right after the
  `down_interruptible` call (confirmed in the disassembly of the loaded `.ko`).
  Stable across samples: no URB has completed since 09:33, so no pixel leaves.
- **The stall watchdog is gone.** The driver creates three workqueues;
  `fl2000_stream` and `fl2000_interrupt` both have live kworker threads,
  `fl2000_stall` has none. The ladder that revived the display three times in
  this boot cannot run again.
- **Nothing is wedged.** Every `/proc/*/task/*/stack` on the machine was
  scanned: no thread anywhere inside fl2000/usb/xhci code except the parked
  worker. This is a silent dead state, not a deadlock.
- **No userspace path back.** Mutter's `GetCurrentState` lists only `HDMI-1`
  and `eDP-1` - it lost the VGA output after the 00:34 re-enumeration
  (`drmModeRmFB: No such device`, `Page flip failed`). The CRTC is held by
  fbcon instead (`plane[35] fb=40 allocated by [fbcon]`, XR24). `xrandr` cannot
  see the output, `card0-VGA-1/dpms` is `-r--r--r--` (no DPMS handler
  registered), and writing 6.2 MB to `/dev/fb1` produced no log line, no worker
  CPU and no watchdog. A physical replug was the only recovery.

Note: ftrace is non-functional on this machine (0 entries from the `function`
tracer with no filter), so all of the above comes from `/proc` + disassembly.

### Two defects, both about the driver being unable to say what it is doing

**1. `fl2000_stream_watchdog()` returned unarmed (fl2000_streaming.c).** The
first statement was

```c
if (!stream->enabled)
        return;
```

- no log, no re-arm. One tick landing in a window where `enabled` is false kills
the recovery ladder for the rest of the session, permanently and silently. That
window exists on every rung-4 run: `fl2000_drm_pre_reset()` calls
`fl2000_stream_disable()`, which clears `enabled` (fl2000_streaming.c:834), and
the only place that sets it back is the tail of
`fl2000_stream_reset_device()`, i.e. *after* `usb_reset_device()` returns. A
tick anywhere in there ends the watchdog. The observable result is exactly what
was found: no `fl2000_stall` thread, no further logs, black screen, no give-up
line.

Fixed to `goto rearm` with a `dev_warn_ratelimited()`. Safe against teardown:
`fl2000_watchdog_cancel()` does `cancel_delayed_work_sync()` before
`destroy_workqueue()`, that call waits for pending *and* running items, and a
delayed work can only re-arm from inside a run - so the re-arm either loses the
race to the cancel or is caught by it.

**2. `fl2000_stream_reprogram()` was unobservable.** It printed one `dev_err()`
*before* touching any register, chained every step through `if (!ret)`, and
`fl2000_drm_post_reset()` discarded the return value entirely. A run that died
on the very first `usb_set_interface()` was indistinguishable in dmesg from one
that programmed the chip perfectly. Since "reprogramming chip mode" was the
last line the driver ever printed, this incident cannot even be attributed
today: the chip may have been dead, or the reprogram may have failed on the
first control transfer after the reset.

Fixed: each step is named, failures log the step and the errno, success logs the
replayed geometry, and `fl2000_stream_reset_device()` logs a closing line when
the stream is re-armed. A future occurrence will say which register write did
not answer, and whether the recovery completed.

### Status

Installed as `fl2000_drm/1.0.5` (srcversion `528025688BCB2607B3150BB`,
previously `1E50DE90CD5049885E78440`), initramfs refreshed. **Not yet loaded**:
the running module needs a reboot, since `rmmod` still fails on the per-plug
reference leak. Recovery of the current black screen is a physical replug.

Not fixed, and not fixed by this change: the underlying wedge is still the chip
losing its streaming engine under bulk load, and the session-10 case (chip
self-resets off the bus before any rung runs) is still outside the driver's
reach.

## Session 12 (2026-10-28): dongle drops off the bus again, and the Bluetooth mouse

Two complaints on the same day: the USB monitor went dark again, and the
Bluetooth mouse stutters. Boot 09:49, module 1.0.5 (`528025688BCB2607B3150BB`),
dongle on `4-1` (USB3), Bluetooth dongle `8087:0026` (AX201) on `3-10` - **both on
the same xHCI controller `0000:00:14.0`**, fl2000 on the USB3 bus, BT on the
USB2 companion bus.

### The monitor

    09:49:30  [drm] Initialized fl2000_drm on minor 1
    10:19:09  USB disconnect, device number 2
    10:19:10  new SuperSpeed USB device number 3
    11:23:15  Mode requested / Mode configured 1920x1080@60 RGB565
    11:23:36  USB disconnect, device number 3          <- device-initiated
    11:23:39  new SuperSpeed USB device number 4
    11:23:42  Mode requested / Mode configured
    11:24:46  Mode requested / Mode configured

**No `stream stall` line at all in this boot.** The ladder never ran: the chip
simply left the bus (the session-10 pattern, out of the driver's reach). And
after the re-enumeration the stream was dead again:

- transmit worker parked in `down_interruptible` (`fl2000_stream_work+0x75`,
  confirmed in the disassembly), 0 CPU over 30 s
- **the `fl2000_stall` watchdog worker did not exist**, even with the 1.0.5
  re-arm fix. Neither `kworker/R-fl2000_stall` nor `kworker/u32:N+fl2000_stall`.
- `card0-VGA-1` still `connected`, CRTC still `active=1`, fb owned by
  gnome-shell. Nothing in userspace noticed.

So the 1.0.5 fix did not fix it, because the watchdog was never *armed* in that
session - not "armed and died". That is not distinguishable from the outside
without instrumentation, which is what 1.0.6 adds.

Mutter also carries a stale key: it knows the monitor as **VGA-2** (from
`~/.config/monitors.xml`, which has accumulated VGA-1..VGA-5 entries from five
different sessions) while the kernel connector is **VGA-1** (the driver pins the
name at bind time). Mutter therefore applies a config whose connector key does
not exist, and the output stays black. The XML should be pruned.

### 1.0.6 changes

- **Watchdog lifecycle is now observable**: `fl2000_display_enable/disable`,
  `fl2000_stream_enable/disable`, the arm and the cancel all log. "The watchdog
  never ran" and "the watchdog ran and died" are now different log lines.
- **`watchdog_armed` flag** replaces the overloaded `enabled` test in the
  watchdog tick. 1.0.5 re-armed on `!enabled`, which cannot distinguish
  "transiently disabled by pre_reset" from "torn down", and a delayed work that
  re-arms itself from inside its own run survives `cancel_delayed_work_sync()`
  - so 1.0.5 could leave a disabled watchdog ticking forever, and could not
  tell a real teardown from a transient one.
- **Halved the driver's constant control traffic**: `fl2000_check_interrupt()`
  ran `regmap_write_bits(VGA_STATUS_REG, mask, ...)` on every interrupt poll
  even with `mask == 0`, i.e. a full USB control transfer that changes
  nothing, 125 times a second, forever. Now skipped when `mask == 0`. This is
  the only always-on USB cost the driver has, and it lands on the same xHCI as
  the Bluetooth dongle.

Installed as `fl2000_drm/1.0.6` (srcversion `71D092D5598D03C14DE9D27`);
needs a reboot to load.

### A/B protocol for the mouse (scripts/bt_stutter_ab.py)

Measuring the input layer alone is worthless here: the timestamp of an
`input_event` is when it reached the input core, so a period in which the user
simply stopped moving the mouse looks exactly like a stutter. The tool instead
captures btmon and the input device **simultaneously** and aligns them:

- a big delay between an HCI packet and its input event = the host delayed it
- a hole in the HCI timeline = the link delayed it
- a hole in the HCI timeline **followed by a burst** (several packets less than
  5 ms apart) = the reports were held somewhere and released all at once -
  this is the "grudinho" the user feels, and it is the one metric that does
  not depend on how much the hand moved

Baseline before the replug (driver idle, 1.0.5, mouse moved continuously):

    carga do fl2000: 0.000 s de CPU   (watchdog AUSENTE)
    HCI: 2201 pacotes (55.2/s)   input: 1970 eventos (56.8/s)
    buracos no HCI > 100ms: 34
      deles seguidos de rajada: 0
      maior buraco no HCI: 1800 ms
    atraso HCI -> input: p50=10.9 p90=12.2 p99=100.9 max=325  (>100ms: 20)
    gaps do input layer: 20-500ms=133  >500ms=6

To be run after the reboot + replug, with the dongle actually streaming:

    sudo scripts/bt_stutter_ab.py 40 "DEPOIS (fl2000 transmitindo, 1.0.6)"

### monitors.xml

Mutter keys a monitor by (connector, vendor, product, serial). The file had
accumulated one `<configuration>` per session - VGA-1 through VGA-5, from five
different sessions where the kernel had named the connector differently before
the pin was added. Mutter therefore applied a configuration whose connector
key did not exist and the output stayed black. Rewritten with a single
configuration and the connector name the driver actually pins
(`VGA-1`), matching the kernel's `/sys/class/drm/card0-VGA-1`. Backup at
`~/.config/monitors.xml.bak-2026-10-28`.

### ROOT CAUSE of the dead stall watchdog: devres_alloc() is not zeroed

Found on 2026-10-28, after the 1.0.6 instrumentation showed the watchdog being
armed and never running.

    12:06:44  display enable: arming the stream
    12:06:44  stream enabled: stall watchdog armed (tick every 1000 ms)
    ... and no fl2000_stall worker thread, ever, in any session

`fl2000_stream_create()` does:

```c
	stream = devres_alloc(&fl2000_stream_release, sizeof(*stream), GFP_KERNEL);
	...
	INIT_WORK(&stream->work, &fl2000_stream_work);
	INIT_DELAYED_WORK(&stream->watchdog_work, &fl2000_stream_watchdog);
```

`devres_alloc()` is `kmalloc()` - **the memory is not zeroed**. `INIT_WORK()`
sets `work->flags = 0`, so the plain work item was fine. But
`INIT_DELAYED_WORK()` expands to `__TIMER_INITIALIZER()`, which fills only the
timer's callback, flags and entry pointer - **it never touches the rest of the
timer, including its lock**. `timer_setup()` (the alternative) does
`memset(timer, 0, sizeof(*timer))` inside `timer_init_key()`; the static
initializer has no such memset and the kernel's documented contract for
`INIT_WORK`/`INIT_DELAYED_WORK` is "zero initialization followed by setting the
function pointer".

A `delayed_work` armed on garbage memory silently never fires. Which is
exactly what was observed: the watchdog was armed, logged, and never ran a
single tick - so the recovery ladder that revives a wedged chip never executed,
in any session, and the display stayed black until a physical replug.

Fixed in 1.0.7 by `memset(stream, 0, sizeof(*stream))` right after the
allocation, and the same for `fl2000_intr_create()`'s `struct fl2000_intr`.

This is the defect behind the whole "monitor goes dark and never comes back"
family of incidents: the chip wedges, the *host* side ladder that was supposed
to notice and fix it never ran, and the only thing that ever brought the
display back was the physical replug.

### Second defect: the watchdog re-armed itself with the wrong API

With 1.0.7 the watchdog finally existed (`kworker/u32:0-fl2000_stall`) - and
then it vanished again, with not a single line in dmesg:

    16:07  verify_after_reboot: 1 thread(s): kworker/u32:0-fl2000_stall
    16:08  bt_stutter_ab:         watchdog AUSENTE
    16:25  ps:                    fl2000_stall=0
    16:26  ps:                    fl2000_stall=1   (novo tid)

The workqueue worker came and went, and nothing was logged, because a healthy
watchdog logs nothing at all - the ladder only speaks when it has something to
recover.

The cause is the re-arm at the end of the watchdog function:

```c
rearm:
	queue_delayed_work(stream->watchdog_wq, &stream->watchdog_work, FL2000_WATCHDOG_PERIOD);
```

Re-arming a delayed work **from inside its own callback** is exactly the case
`queue_delayed_work()` cannot serve: the timer is detached while its callback
runs, so the internal `timer_delete_sync()` fails and the re-arm is silently
dropped. `mod_delayed_work()` is the API for that case.

Fixed in 1.0.8: `mod_delayed_work()` at the re-arm, the return value of both
the arm and the re-arm logged, and a heartbeat every 60 ticks
(`stall watchdog alive: N ticks, M completions, K errors`) so a healthy
watchdog stops being invisible.

Note on the return value: `mod_delayed_work()` returns **false when the work
was idle** - which is the normal case here, because the tick that just ran
consumed the work, so the call queues it fresh, i.e. success. Reading it the
other way round produced a "failed to re-arm itself" line every second. The
check is now the right way round (1.0.9).

### The Bluetooth mouse: the driver WAS the cause after all - CORRECTED, IT WAS NOT

The A/B was done the only way that controls for the confound: same session,
same window, the dongle's output switched off and on between the two
measurements, so the user's hand is the control and the dongle is the only
variable. The measurement aligns the btmon timeline with the input-event
timeline by the offset that maximises the number of input events landing on
an HCI packet - a metric that does not depend on how much the hand moved.

    A  fl2000 TRANSMITINDO (1.85 s de CPU na janela)
       57,0% dos eventos de input casam com um pacote HCI (+/-5 ms)
       atraso |evento - pacote|: p50=1,9  p90=2621  p99=3532  max=3599 ms
       37,9% dos eventos a mais de 100 ms do pacote HCI
       29,3% a mais de 500 ms - ate 3,6 SEGUNDOS de atraso

    B  fl2000 OCIOSO (0,000 s de CPU na janela)
       100,0% dos eventos de input casam com um pacote HCI (+/-5 ms)
       atraso |evento - pacote|: p50=0,0  p90=0,1  p99=0,2  max=4 ms
       NENHUM evento a mais de 5 ms

**THIS VERDICT WAS WRONG**, and the reason is the window densities: that "A"
window had **16,9 eventos/s** and the "B" window had **160,7**. The metric
needs a dense event stream - with few events, the offset that "matches" an
event to an HCI packet is ambiguous and the delay becomes noise.

The proof came from the run of 2026-10-28 17:05, where the **fl2000 idle**
window - the control, dongle doing nothing - measured p50 = 2173 ms and 784
late events per thousand, simply because that window had 19,8 eventos/s.

With dense movement (~170 eventos/s in all three windows of the same run):

    A1  chunks de 1 MB, transmitindo   p99= 13,9 ms   0,3 atrasados/1000
    A2  1 URB por frame, transmitindo  p99= 13,2 ms   0,0 atrasados/1000
    B   fl2000 ocioso                  p99= 13,9 ms   1,6 atrasados/1000

**All three are clean.** The fl2000's streaming load does **not** degrade the
Bluetooth link in steady state, and the chunking neither fixed nor broke
anything measurable there. The tool now flags any window with fewer than 60
eventos/s as not comparable.

So the mouse stutter is **not** the fl2000's fault - which is what the
measurements taken with the driver completely idle had already said:

- Input layer (`/dev/input/event20`, the BT mouse): 88 gaps of 20-500 ms in
  60 s, worst 416 ms.
- HCI layer (`btmon`, 30 s): 2622 ACL RX packets, p50 interval 11.0 ms,
  p99 22.9 ms, **max 169 ms**, 98.2 % of intervals below 15 ms.
- Aligned HCI -> input: delivery delay p50 10 ms, p90 12 ms, p99 22.9 ms;
  **5 events out of 3416** had more than 100 ms of host-side delay.
- bluetoothd is not starved either - 0,14 ms of runqueue wait over 10 s.

The host path (USB bulk -> btusb -> HCI -> BlueZ -> HIDP -> input core) adds
essentially nothing: the delay visible at the input layer is the delay already
present at the HCI socket.

What the capture does show is the link cycling between modes, with the host
only waking it every 30 seconds:

    < HCI Command: Exit Sniff Mode
    > HCI Event: Mode Change  Mode: Active (0x00)
    > HCI Event: Mode Change  Mode: Sniff (0x02) Interval: 11,250 ms
    > ACL Data RX                        <- 1,57 s de nada no meio

The two `Exit Sniff Mode` commands are exactly 30,03 s apart - a periodic
host task. In between, the mouse's report waits in the peripheral until the
host polls the link. **That is the stutter, and it is a Bluetooth-side
phenomenon**: the sniff policy of the AX201/MTK controller, the mouse's own
firmware, or the RF environment.

Things worth testing with dense movement, in this order:

1. **USB autosuspend of the Bluetooth dongle.** `btusb` runs with
   `enable_autosuspend=Y`; the AX201 controller going to sleep is a classic
   source of multi-hundred-millisecond mouse stalls.
   `echo on > /sys/bus/usb/devices/3-10/power/control`
2. **2,4 GHz interference.** The laptop's WiFi is on 5 GHz (channel 132), but
   the AX201 shares one antenna and one RF front end between WiFi and
   Bluetooth, and the WiFi link is weak (-74 dBm, 21 % of packets
   retransmitted). Heavy WiFi retransmission steals airtime from the
   Bluetooth link.
3. **The mouse's own RF and firmware.** A cheap Jieli mouse with an
   internal antenna, next to a USB 3 dongle radiating.
4. **The controller's link policy** - the sniff interval the AX201 negotiates
   is visible in the capture (`Mode Change ... Sniff (0x02) Interval:
   11,250 ms`).

### The Bluetooth side, measured (scripts/bt_side_test.sh)

    C1  baseline (autosuspend auto), fl2000 TRANSMITINDO a 5% de um core
        2278 pacotes HCI (73,4/s) | 3115 eventos (101,8/s)
        atraso p50=1,9  p90=5,8  p99=12,9  max=14 ms
        NENHUM evento a mais de 100 ms

The link is clean with the dongle streaming. The stutter did not reproduce
after the reboot, which fits the hypothesis that it was a symptom of the
degraded state the dongle was in during the previous session (chip
half-configured, output toggled repeatedly, driver in a broken state) and
not a steady-state property of the streaming load.

The tool now refuses to compare a window with fewer than 60 eventos/s and
says so explicitly - the sparse windows were the source of two wrong
verdicts in this session.

### 1.0.9

- the `mod_delayed_work()` return value checked the right way round (no more
  "failed to re-arm" spam)
- the heartbeat now names the thread doing the ticking, to settle the
  question of which workqueue worker runs the watchdog

### 1.0.10 - the frame is split into chunks

`fl2000_chunk_kb` (default 1024, 0 = one URB per frame) splits each frame
into bulk transfers of that size. At 1080p60 RGB565 a frame is 4,15 MB, which
went out as a single URB; now it goes out as 4 chunks of ~1 MB.

**Result: it did NOT fix the Bluetooth stutter.** With dense mouse movement
all three configurations measure identically clean (see the correction
above). The chunking is kept because it is correct, it preserves the frame
rate exactly (measured: 246 chunks/s = 61,5 frames/s, 0 errors) and it cuts
the bulk the controller holds queued for us from 8,3 MB to 2 MB - good
hygiene on a controller shared with a Bluetooth dongle. But it is not the
answer to the mouse, and the notes above have been corrected accordingly.

**What changed in the streaming path:**

- `struct fl2000_stream_buf` grew a table of per-chunk scatterlists
  (`chunk_sgt[]`), the chunk counters (`chunk_next`, `chunk_done`) and
  `chunk_bytes`. The pages and the vmap are unchanged - the chunks are views
  of the same buffer, built with `sg_alloc_table_from_pages()` per chunk, so
  the byte stream the chip sees is identical to before.
- The transmit worker sends one chunk at a time, keeping at most
  `FL2000_MAX_INFLIGHT` **chunks** in flight (was: that many *frames*, i.e.
  8,3 MB of queued bulk; now 2 MB). The throttle counts chunks, not frames.
- The chunks of a frame go out in order - the frame stays in `stream->cur_buf`
  until its last chunk is submitted - so the chip still receives whole frames
  in sequence.
- **Only the last chunk of a frame carries `URB_ZERO_PACKET`**: the ZLP is
  what closes a frame for the chip, and one in the middle of a frame would be
  read as the end of it.
- The completion counts chunk completions. The buffer goes back to the render
  list and the vblank fires only when the **last** chunk of a frame completes
  - the vblank rate is unchanged (one per frame), which matters: firing it per
  chunk would have made mutter render at 4x the rate.
- The USB device reset path now puts the half-sent frames back in the render
  list with their chunk counters cleared and zeroes the in-flight counter.
  Without that, the poisoned URBs (which never complete) would leave the
  counter stuck and the throttle would wedge the stream for good.

**Honest expectation:** the burst is 4x shorter and the queued bulk is 4x
smaller, but the byte rate is the same - 249 MB/s - and the chip's FIFO
paces the host, so the wire traffic is roughly continuous either way. The
interference with the Bluetooth dongle is therefore most likely dominated by
the *byte rate*, not by the burst shape. If the measurement after this change
shows no improvement, the effective fixes are the ones that cut bytes or cut
the contention: lower the VGA mode, or put the Bluetooth dongle on the
Thunderbolt controller (`0000:00:0d.0`), which shares nothing with the
fl2000.

`fl2000_chunk_kb=0` restores the old one-URB-per-frame behaviour, so the two
can be compared without rebuilding.

### The A/B protocol (scripts/bt_ab_full.sh)

The interference is measured by running btmon and the input device
**simultaneously** and aligning them. The alignment metric is the one that
does not depend on how much the hand moved: the offset that maximises the
number of input events landing on an HCI packet, and then the distribution of
the distance between an event and its packet.

    A  fl2000 TRANSMITINDO (1,85 s de CPU na janela)
       57,0% dos eventos de input casam com um pacote HCI (+/-5 ms)
       atraso |evento - pacote|: p50=1,9  p90=2621  p99=3532  max=3599 ms
       546,8 eventos a mais de 100 ms por mil eventos

    B  fl2000 OCIOSO (0,000 s de CPU na janela)
       100,0% dos eventos de input casam com um pacote HCI (+/-5 ms)
       atraso |evento - pacote|: p50=0,0  p90=0,1  p99=0,2  max=4 ms
       0,0 eventos a mais de 100 ms por mil eventos

All the big holes in the HCI timeline are innocent: every one of them has
zero input events inside, i.e. the mouse simply was not moving. The stutter
is in the *delivery delay*, not in the gaps.

`scripts/bt_ab_full.sh` runs three windows in one go - chunks of 1 MB, one
URB per frame (`chunk_kb=0`), and the dongle idle - changing `chunk_kb` and
forcing a modeset in between (the toggle triggers "Mode configured", and the
buffers with their per-chunk scatterlists are reallocated there, so the
parameter takes effect without a rebuild). `scripts/bt_ab_compare.py` prints
the comparison, normalising by the number of events so that windows with
different hand movement stay comparable.

Note on the watchdog check inside the A/B tool: the thread of the
`fl2000_stall` workqueue never shows up in /proc (session 12), so the tool
judges it by the driver's heartbeat instead.

### scripts/fl2000_toggle.py

`xrandr` cannot see the VGA output (it is not on the Xwayland screen) and the
DRM `dpms` attribute is read-only (no DPMS handler is registered), so the only
way to force `fl2000_display_disable()`/`enable()` - and with it a fresh
watchdog arming - is Mutter's DisplayConfig D-Bus API. The script does that,
re-packing the layout so Mutter's "logical monitors not adjacent" and "missing
primary logical" checks pass.

### scripts/verify_after_reboot.py

One-shot check of everything the dmesg alone cannot answer: module srcversion
installed vs. loaded, whether the `fl2000_stall` worker exists, whether the
watchdog was armed/cancelled, whether the stream is alive, whether the
recovery ladder ever had to run, and whether the kernel's connector name
matches the one Mutter knows.

Run it as the **user**, not as root: Mutter is only reachable through the
user's session bus. The privileged bits (kernel journal, `/proc/<tid>/stack`)
go through `sudo` from inside the script.

Note on the stream check: the transmit worker parks in `down_interruptible()`
between completions even on a healthy stream (it wakes 60 times a second at
60 fps), so the stack does not distinguish "dead" from "alive". What
distinguishes them is the watchdog: if it exists and has not logged a stall,
frames are arriving. The worker's CPU consumption is reported alongside as a
supporting number, not as the verdict.

### scripts/bt_stutter_ab.py

Captures btmon and the input device simultaneously and aligns them; see the
A/B protocol section above.
