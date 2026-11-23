# Q10 V0.5: BBP9981 feature parity

Curve factory default is ON for both sensor addresses: full-gain speed6 counts/ms,
exponent1.48, minimum gain0.66, maximum gain1.35; pointer speed defaults to30 DPI.
Stored/user curve settings override detection; disabled UI folds all parameters.


Target `zitaotech_q10/nrf52840/zmk`. Version is provided by build-version.conf,
not hardware defconfig. build-local.py uses the existing local WSL toolchain.

The keymap is byte-identical to current BBP9981: five named layers and four
reserved Studio slots. Factory automatic touchpad layer is ON, layer4,800ms;
mouse-button timer extension is OFF,200ms. Bootmagic is position4. Runtime
sleep supports Studio settings, factory Never. Backlight defaults80, BLE+8dBm.
Touchpad curve,200ms touch-start gap/two discarded packets, subpixel pointer
scaling, direction/scroll/mode keys and four BLE-slot DPI/burst profiles match
BBP9981. USB edits the selected BLE slot; curve/scroll settings are shared.

## Preserved Q10 hardware

9x6 matrix/42 physical keys and transform retain the original Q10 wiring.
Diode-less open-source column scanning retains5us/20us settling times.
I2C SDA=P1.09,SCL=P0.08; touchpad LED=P1.13; keyboard LED=P1.06;
backlight state PWM=P1.04. Storage and UF2 bootloader partitions are unchanged.
Only runtime tap-dance and the retained default-layer behavior nodes are added to the device tree.

The old Q10 source confirms MOTION=P1.01,active-low/pull-up:
C:/Users/Dai/zmk/app/boards/arm/zitaotech_q10/custom_driver/a320.c.
V0.2 restores that IRQ rather than retaining the previous0.4 port's8ms polling.
The IRQ submits I2C work immediately; active-line continuation is1ms and read
errors retry after2ms. Startup checks an already-active line. Idle does not poll.
V0.4 retains swapped X/Y and reverses the resulting Y axis for both protocols, retaining address/read transactions:
0x37/register0x0A/7bytes: X=-buf[1],Y=buf[3];
0x3B/register0x82/3bytes: X=-buf[2],Y=-buf[1].

## Validation and use

Native tests cover matrix scanning, actual packet/probe/read functions,
IRQ callbacks/startup-active/retry behavior, curve/filter boundaries, independent
DPI profile persistence, timeout readback and single-channel LED retry.
The legacy test_polling.py filename now validates protocols; test_irq.py covers
IRQ behavior. Build and package checks compare preserved wiring and exact keymap,
compiled features, version and UF2 address boundaries. These are software checks.

Flash only the Q10 image. Saved settings override new factory defaults; edit
individual values/keys in Studio as needed. Build scripts never flash devices.
USB/BLE, actual interrupt wiring/axes, LED outputs, suspend/wakeup and power-cycle
persistence require a physical Q10 test. This is a trial build, not a public release.
Source bundles require the current locally customized ZMK app and feature modules.

## Reset recovery firmware

`build-reset.py` builds a separate Q10 recovery image using `reset.conf`.
It uses the same checked reset-to-bootloader code as BBP9981: erase the settings
partition at APPLICATION/96, check the erase result, select bootloader mode and
reboot at APPLICATION/99. Erase or boot-mode failure stops startup rather than
reporting success or entering the normal keyboard application.
This clears saved keymaps, pointing/backlight settings and Bluetooth bonds.
After it automatically returns to the UF2 drive, copy the normal Q10 firmware
to that drive. The recovery image is not the normal keyboard firmware.
`test_reset.py` checks the success/failure paths and initialization ordering;
actual flash erasure and automatic USB re-enumeration need a physical Q10 test.

## Fast scroll mode key

Q10 reports `fast_scroll_supported` to Studio. Assign the fast-scroll key on the
Touchpad page, then hold it during scrolling for 2x speed; release restores the
normal rate. It does not switch pointer motion into scrolling on its own.
Factory position is unassigned (-1), and the existing `ztp/fastscrollkey` setting
persists the selected position. `test_fast_scroll.py` checks both axes, smooth
and stepped scrolling, release/reassignment/unassign and duplicate-position
rejection. This uses the same multiplier as Sofle without changing pointer gain.
