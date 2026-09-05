# USBHIDVIRT — `/dev/usbhid0` with a socket where the cable goes

Loadable `.SYS` that publishes **`/dev/usbhid0`**, the same device a board with
`CONFIG_ARGON_USB_DEVICE` gets from the kernel, and forwards every HID report
written to it over TCP **`:5562`** (QEMU hostfwd). The host side is
[`tools/usbhidvirt.py`](../../tools/usbhidvirt.py), which decodes the reports
and prints what a real computer would have understood by them.

This exists because QEMU models no OTG peripheral at all, so the one thing that
can never be verified in the emulator is the gadget itself. Everything above it
can: the application, the page it serves, the wire format the phone speaks, the
frames that come out and the order they come out in.

With no host tool connected a write fails with `-AG_ENODEV` — exactly what the
real device answers with the cable out — so the application's unplugged path is
exercised here too rather than only on a desk.

```
drv install h:\usbhidvirt.sys
run h:\kvm.axe
python tools/usbhidvirt.py                  watch
python tools/usbhidvirt.py --expect hello   watch, and exit 0 only if that was typed
python tools/usbhidvirt.py --leds 2         tell the guest Caps Lock is lit
```

`argon kvmtest` drives all of this automatically; see
[`docs/14-usb-kvm.md`](../../docs/14-usb-kvm.md).

## The frame

Sixteen bytes, the ABI's `ag_usbhid_frame_t`:

```
[0]    kind: 1 keyboard, 2 relative mouse, 3 absolute pointer
[1]    length of the report
[2..]  the report
```

Upstream the host tool sends one byte whenever it likes: the keyboard's lock
lamps, which is the only thing a real host ever sends this device.

Canonical paths (mkaxe stages both): `build/apps/USBHIDVIRT.SYS`,
`build/sd_card/USBHIDVIRT.SYS`.

## Build

Normally just `argon apps` (or `argon apps --only USBHIDVIRT.SYS`) — the
authoritative build line lives in [`tools/apps.json`](../../tools/apps.json).
