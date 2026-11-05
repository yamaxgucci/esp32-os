# ESP8266 stand-in for a pad on a PCF8574

An ESP-07 that answers the CYD's I2C as a PCF8574 at 0x20 with a 6-button
Mega Drive pad on it, so `SEGAPAD.SYS` can be tried before the expander and the
pad are wired. The buttons come from a phone: the module serves a pad page on
port 80.

- **Access point** `SEGAPAD`, open, page at `http://192.168.4.1/`.
- **Home network** as well, when `main/wifi_secret.h` (gitignored) defines
  `SP_SSID` and `SP_PASS`. The DHCP hostname is `segapad`.

The top line of the page is the module's answer: what it holds and how many
writes and reads the CYD made of it. If those two counts grow, the bus is
alive.

## Wires

The UART wires, so the CYD that flashes the module over UART1 reads it over
I2C afterwards with nothing moved:

| CYD (CN1) | ESP-07 |
|---|---|
| IO21 (SDA) | TX, GPIO1 |
| IO22 (SCL) | RX, GPIO3 |
| 3.3V | VCC |
| GND | GND |

Power it from the CYD only. Do not connect a USB adapter's 3.3 V to it at the
same time.

## How it answers

The ESP8266 has no I2C slave, so the bus is answered in software: a START
raises an interrupt, and the whole transaction runs in that interrupt by
polling the pins. The radio's NMI cannot be held off, and one that lands in the
middle of a byte loses a bit. The CYD sees that as a NACK or a garbled read,
and `SEGAPAD.SYS` treats it as one bad read among many. The pad model is
`apps/common/segapad`, the same one the host tests run. So this proves the
driver, the bus and the keys into a program, but not how a real pad behaves.

## Build and flash

```
.\firmware\esp8266-segapad\build.ps1
```

This builds in `D:\sp8266fw`, because the SDK cannot build from a path with a
space in it. Flash from the CYD with `ESPFLASH.AXE`, the four images on the
card (C: on a CYD has no room for them): GPIO0 to GND, power-cycle the CYD,
then one command at a time, each after the one before has said `returned` -
two at once left the ESP8266's loader deaf until the next power-cycle:

```
run a:\espflash.axe write a:\pboot.bin -a 0x0     -keep
run a:\espflash.axe write a:\ppart.bin -a 0x8000  -keep
run a:\espflash.axe write a:\potad.bin -a 0xd000  -keep
run a:\espflash.axe write a:\pad.bin   -a 0x10000
```

`pboot.bin`, `ppart.bin` and `pad.bin` are `build\bootloader\bootloader.bin`,
`build\partition_table\partition-table.bin` and `build\segapad8266.bin`;
`potad.bin` is 8 KB of 0xff, an empty OTA record, so the module boots the
first slot. "flash end ... input/output error" after the last block is the
ESP8266 loader's usual answer and the data is written. Then GPIO0 back to
VCC and power-cycle the CYD again.

UART1 must receive on IO21 while flashing (`[uart1] rx = 21`). The board pack
has it on IO35.

That is for a bare module or a broken one. Once it runs, update it over
Wi-Fi, no jumper:

```
.\firmware\esp8266-segapad\build.ps1
.\firmware\esp8266-segapad\ota.ps1
```

On a 1 MB ESP8266 an image runs only from the slot it was linked for, and the
SDK links for the first app slot in the table. So `build.ps1` builds the
application twice: in `D:\sp8266fw` for the first slot (0x10000) and in
`D:\sp8266fw1` with `partitions_slot1.csv`, a table that has only the second
slot (0x80000). `ota.ps1` asks the module which slot it runs (`GET /ota`),
sends the other image to `POST /ota`, and checks that the module came back
in the other slot. The first try, 4 November 2026, sent the first slot's image
to the second slot and `esp_ota_end` refused it (0x1503,
ESP_ERR_OTA_VALIDATE_FAILED: the entry point was outside the slot). On
5 November it went 0 to 1, 1 to 0 and 0 to 1 again.

## The page and the module talk in states

`/p?m=HEX&e=HEX,...`: `m` is what is held now, `e` every state the buttons went
through since the last request, and the module plays each in turn before it
settles on `m`. Only what is held, one request at a time, lost quick taps: a
tap that began and ended while a request was out never went out. Measured on
4 November 2026 with the page against a stand-in module and a CYD that reads
like `SEGAPAD.SYS`, and then on the board through MD: 20 taps of 60 ms with
80 ms between them reached MD as 20 presses (the page before: 12 of 20 at a
100 ms module, 6 of 20 at 250 ms).

A state plays until the CYD has read it three times whole, at least 50 ms and
at most 500 ms. Whole means the eight steps with no lost answer and with the
step where a 6-button pad shows Z Y X Mode. Played by the clock it lost taps of
X: 7 of 10 at 48 ms, 8 of 10 at 100 ms; at three reads of eight steps, 7 of 10
again (5 November 2026, Sonic in MD, and the driver saw the same presses as
MD). The module drops few answers - 140 of 82666 reads - but while it handles
the HTTP request that carries a tap its answers come late, a late step resets
the pad's count as a real pad's would, and that read has no X in it: of 2814
whole reads, 1295 reached the 6-button step. Counting only those, MD saw
10 of 10 taps 400 ms apart, 20 of 20 at 150 ms, and Mode 20 of 20 at 80 ms. At
nine taps a second (30 ms, 80 ms apart) it is 16 to 20 of 20: the queue fills
faster than the module plays it. `/p` answers with both counts (`scans`,
`six`). A real PCF8574 has none of this.

## The bus speed

Run the CYD's I2C0 at 50 kHz for this stand-in (`khz = 50` under `[i2c0]` in
`C:\SYSTEM.CFG`): it loses 0.06 % of the transfers there against 0.24 % at
100 kHz, and its losses come at the moments it is being talked to over Wi-Fi,
which is when a button is pressed and let go. A real PCF8574 runs at 100.
