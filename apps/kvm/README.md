# KVM — the board is a keyboard and mouse for the PC it is plugged into

A phone drives the computer at the other end of the OTG cable. That computer
runs **nothing**: no agent, no driver, no port open, no account — it sees a
keyboard. So this works at the BIOS, at the boot menu, at the login screen, at a
blue screen and at a desktop that has stopped answering, which is the whole list
of moments when a remote desktop is what you actually needed and cannot have.

What it does not do is show you a picture. A PC does not send its screen out
over USB — there is no USB display class, and everything sold as one carries a
driver for the far machine. Why, and what it would take, is in
[`docs/14-usb-kvm.md`](../../docs/14-usb-kvm.md).

```
run kvm.axe            serve on port 80
run kvm.axe 8080       somewhere else
```

It prints the address to open. One page, one WebSocket, one phone at a time —
the newest connection wins, so a page left open on a laptop cannot lock out the
phone in somebody's hand.

## The page

- **trackpad**: drag to move, tap to click, two fingers to scroll
- **Left / Middle / Right**, and **Drag**, which holds the left button down
  because a touchscreen cannot hold one finger on a button and move another
- **Esc, Tab, Ctrl, Shift, Alt, Win**: the modifiers are sticky — tap one on,
  it folds into the next key, then it is off
- arrows, Enter, Backspace, **C-A-Del**
- a text box that types as you type into it

On a laptop browser the physical keyboard drives the far machine key for key:
`code` in a keydown event is a *position* on the keyboard, which is exactly what
a HID report carries. On a phone there are no key positions — a mobile keyboard
reports what it inserted, not what was pressed — so the text box is the keyboard
there and the keys with no character are buttons.

Non-ASCII cannot be typed and that is the protocol, not a shortcut: a report
asks for a key position, and which letter that produces is decided by the layout
the *far* machine is set to. See the note in `apps/common/kvm/ag_kvm.h`.

## What it needs

A build whose OTG port is a device rather than a host:

```
argon target esp32s3-kvm
argon build
argon flash -port COM3
```

That target is `esp32s3-board` with `CONFIG_ARGON_USB_HOST=n` and
`CONFIG_ARGON_USB_DEVICE=y`. The two are mutually exclusive and not by choice:
the S3 has one OTG controller behind one internal PHY. On the CAM board the
console stays on the CH343 bridge and nothing collides; on a board whose only
socket is the native one (the S3-Zero) this image takes the console with it.

In the emulator there is no OTG peripheral at all, so the device comes from a
driver instead:

```
drv install h:\usbhidvirt.sys
run h:\kvm.axe
python tools/usbhidvirt.py        watch what the far machine would receive
argon kvmtest                     19 checks, both ends played automatically
```

## Build

Normally just `argon apps` (or `argon apps --only KVM.AXE`) — the authoritative
build line lives in [`tools/apps.json`](../../tools/apps.json).

Canonical paths (mkaxe stages both): `build/apps/KVM.AXE`,
`build/sd_card/KVM.AXE`.

## Where the parts are

| | |
|---|---|
| `kvm.c` | the server, the socket loop, the device |
| `kvm_page.h` | the page, one string in flash |
| `../common/kvm/ag_kvm.[ch]` | keyboard state, the ASCII map, the wire format |
| `../common/kvm/ag_ws.[ch]` | WebSocket: SHA-1, base64, frames |

The two `common/kvm` files are compiled into the host tests
(`host-tests/test_kvm.c`, `test_ws.c`) on purpose: what they get wrong is
invisible on the far machine and looks like a bad cable.
