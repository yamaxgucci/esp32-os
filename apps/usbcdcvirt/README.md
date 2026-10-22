# USBCDCVIRT — `/dev/usbcdc0` with a socket where the cable goes

Loadable `.SYS` that publishes **`/dev/usbcdc0`**, the serial pipe a board with
`CONFIG_ARGON_USB_DEVICE_CDC` gets from the kernel, and carries its bytes over
TCP **`:5563`** (QEMU hostfwd) instead of over USB.

The companion of [`USBHIDVIRT`](../usbhidvirt/README.md), and the same trade.
QEMU models no OTG peripheral, so the pipe cannot exist there; what can exist is
the same device with a socket in place of the wire. The agent that would run on
the far machine connects to it and behaves exactly as it would over USB — it is
the same binary, sending the same bytes — so the whole path above the port is
verified without a board, and the picture reaches the phone while the cable does
not exist.

```
drv install h:\usbcdcvirt.sys
run h:\kvm.axe
build\kvmagent.exe --tcp 127.0.0.1:5563
argon kvmtest picture      all of the above, automatically
```

Bytes both ways, no framing — that is what the real device is. With nothing
connected a read is `-AG_EAGAIN` and a write is `-AG_ENODEV`, which is what
happens on a desk when the board is plugged in and nobody has started the agent.

Canonical paths (mkaxe stages both): `build/apps/USBCDCVIRT.SYS`,
`build/sd_card/USBCDCVIRT.SYS`.

## Build

Normally just `argon apps` (or `argon apps --only USBCDCVIRT.SYS`) — the
authoritative build line lives in [`tools/apps.json`](../../tools/apps.json).
