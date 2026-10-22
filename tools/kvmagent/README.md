# kvmagent — the program on the far machine that sends its screen

One Windows executable, no installer, no admin, no dependencies beyond what
ships with Windows. It captures the desktop, compresses the parts that changed,
and writes them down a serial port — the one the ArgonOS board presents when it
is plugged into that machine's USB.

```
kvmagent                       find the board and start
kvmagent --list                which serial ports look like the board
kvmagent --port COM7           a particular one
kvmagent --tcp 127.0.0.1:5563  the emulator instead of a board
kvmagent --fps 20 --tile 64
```

## What it is not in the path of

Keyboard and mouse. Those go the other way entirely — out of the board as USB
HID, into a machine that has never heard of us — so they keep working in a
BIOS, at a login screen, and after this program has crashed. This exists only
because the picture cannot come any other way: a PC does not send its screen
out over USB, there being no such thing as a USB display class. The reasoning
is in [`docs/14-usb-kvm.md`](../../docs/14-usb-kvm.md).

## How it works

- **Capture**: `BitBlt` of the desktop into a 32-bit DIB, with the mouse cursor
  drawn in afterwards (`BitBlt` does not include it, and a remote desktop with
  no pointer is unusable).
- **Change detection**: the screen is a grid of tiles, 64 pixels square by
  default, each hashed with FNV-1a. Only tiles whose hash moved are sent, so a
  still screen costs nothing on the wire.
- **Compression**: PNG, using the `miniz` already vendored in this tree.
  Lossless matters more than it looks like it should — this is a remote desktop
  for reading text on, and a JPEG of a terminal is unreadable at the ratios that
  would make JPEG worth using. A screen of text is what PNG is best at.
- **Splitting**: a tile whose PNG will not fit the board's buffer is quartered
  and each quarter sent on its own. Splitting rather than growing the board's
  buffer is the right way round: the board is the machine with the least memory
  in the chain, and a screen of photographs must not be able to make it ask for
  more.
- **Back-pressure**: writing to the port blocks when the board is behind, which
  is exactly what should happen. The chain regulates itself from the phone
  backwards.

Nothing decodes anything in the middle. The board finds message boundaries and
forwards bytes; the phone's browser decodes PNG for free. That is why the ESP32
needs no frame buffer, no codec and no measurable CPU for the picture.

## Back the other way

Two messages travel from the phone to here, and neither of them is input:

- **a request for a whole screen**, sent by a phone that has just connected. It
  also gets the screen's size resent, because it never saw the first one.
- **clipboard text**. This is the answer to "you cannot type Cyrillic": a HID
  report asks for a *key position*, and which letter that produces is the far
  machine's layout's business. Text that must arrive as itself goes on the
  clipboard and is pasted.

## Build

```
tools\kvmagent\build.cmd
```

mingw-w64 — the same compiler `ARGON_HOST_CC_BIN` names for the host tests.
Output is `build\kvmagent.exe`. Links `gdi32`, `user32`, `setupapi`, `ws2_32`,
`advapi32`, all of which are part of Windows.

## Testing it without a board

`argon kvmtest picture` builds this, boots the emulator, points the agent at
`USBCDCVIRT.SYS` over TCP, and checks that a whole screen arrives at the phone
with every message framed and every PNG matching its rectangle. It is the same
binary that would talk to a real board; only the last inch differs.

One thing to know when watching it by hand: the agent captures **this**
desktop, so a browser showing the page is showing the screen it is part of. The
picture feeds itself and the tile count runs away. That is the test rig, not the
system.
