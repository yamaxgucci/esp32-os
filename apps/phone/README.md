# PHONE — the board's screen and keyboard, on a phone

Loadable `.SYS` that publishes **`/dev/phone0`** (a display) and
**`/dev/pkbd0`** (an input device), and serves its own client page on
**:8765**. A phone joins the board's Wi-Fi — or the board joins the house's —
opens `http://<board>:8765/`, and has the screen and the keyboard.

Nothing to install on the phone: the page is in this image. Add it to the home
screen and it opens without browser chrome.

Design, protocol and what is not done yet: [`docs/plans/phone.md`](../../docs/plans/phone.md).

## Install

```
drv install a:\phone.sys
```

Persists via `[modules]` in `C:\SYSTEM.CFG`. This session only: `drv load a:\phone.sys`.

The address is printed to the log when the link comes up:

```
phone: http://192.168.4.1:8765/
```

`SYSTEM.CFG`:

```ini
[phone]
port = 8765        ; 80 is HTTPD.AXE's, 8080 is too common
text = yes         ; offer the console as characters (default)
password = secret  ; leave it out and anybody who reaches the port is in
```

## The password

Without one, whoever reaches the port has the screen and the keyboard — the
same standing `telnet on` has here. With one, the board challenges and the
browser answers: the board sends a nonce, both sides hash it with the password
(SHA-1), and only the digest crosses. **The password itself is never on the
wire**, and a digest somebody copied is worth nothing on the next connection.
Until the answer arrives the board sends nothing at all — not the geometry, not
a row of the console — and listens to nothing but the answer.

The page asks once and keeps the password in the browser's own storage, per
board, so the phone is not asked again. A wrong answer costs a second and a
half and a hung-up connection, which makes guessing over a network a week's
work rather than an afternoon's.

What it is **not**: encryption. Everything after the answer — the screen, the
keys — crosses in the clear, because a board this size cannot carry TLS under a
video link. It keeps strangers out; it does not keep a listener from watching.

Leaving the line out keeps the old behaviour, deliberately: a board that
suddenly refused its owner after an update would be worse than one that never
asked. The driver says which it is at load:

```
PHONE: open http://<board>:8765/ - 80x25 cells, NO PASSWORD ([phone] password to set one)
```

## Build

`argon apps --only PHONE.SYS`. The authoritative line lives in
[`tools/apps.json`](../../tools/apps.json), which also runs
[`tools/mkpage.py`](../../tools/mkpage.py) first — that gzips
[`web/index.html`](web/index.html) into `build/apps/PHONE.GZ`, so a page edited
and not regenerated cannot ship.

**PHONE.GZ has to be on the board, next to the driver.** It is a file on C: and
not an array inside the .SYS, because a .SYS keeps its data in RAM: as an array
the driver's data segment was 50 KB, and the CYD has about thirty free with its
radio up. As a file it costs a one-kilobyte read buffer. Put it there with the
rest:

```
python tools/mksysfs.py --board boards/esp32-cyd --add build/apps/PHONE.GZ=PHONE.GZ ...
```

`[phone] page` names it if it should live somewhere else; the default is
`C:\PHONE.GZ`. Without it the board answers 500 and says which file is
missing.

Edit the page as HTML. It is served verbatim (gzipped, not minified), comments
and all: "view source" on the phone is the only debugger on that side of the
link.

## Checking it without a board

The client half is the half neither QEMU nor the board can check — that a
palette band comes out as the right picture, that CP437 box drawing draws, that
a tap lands where a finger was. [`tools/phonefixture.py`](../../tools/phonefixture.py)
speaks the board's side of the protocol on a PC and prints everything it gets
back:

```
python tools/phonefixture.py
```

then open `http://127.0.0.1:8765/` in a browser. `--picture` sends a picture
with flat panels, a gradient and noise in it, so all three encodings are used
on one screen.

The codec and the WebSocket framing are checked in `argon tests`
(`host-tests/test_phonelink.c`) against the shipped sources, bit for bit.

## What it looks like from the system

| | |
|---|---|
| `phone0` | `AG_DEV_DISPLAY`. `blit_rect` takes pixels, `text_row`/`text_cursor` take the console. No `acquire`: there is no surface here, the picture is somebody else's memory. |
| `pkbd0` | `AG_DEV_INPUT`. Keys and taps are injected into the console queue, so every application gets them without knowing. `read()` also hands out KBDVIRT's eight-byte packets, for an application that takes its input from a handle (DOOM does). |

The driver owns a task (`sys->module_task`, ABI 0.48), on the system core at
priority 5. `blit_rect` copies the rectangle and marks the damage and does
nothing else — encoding and the network are the task's, so a game does not pay
for a screen it did not ask to be remote.
