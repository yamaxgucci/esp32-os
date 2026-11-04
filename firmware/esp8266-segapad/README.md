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

`POST /ota` with the application image as the body is meant to update the
module without the jumper:
`curl --data-binary @build\segapad8266.bin http://<module>/ota`. It does not
work yet: on 4 November 2026 it answered 500 after the whole image, with the
1 MB layout in `partitions.csv` flashed. The build after that names the step
that failed in the answer; it has not been on the module.

## The bus speed

Run the CYD's I2C0 at 50 kHz for this stand-in (`khz = 50` under `[i2c0]` in
`C:\SYSTEM.CFG`): it loses 0.06 % of the transfers there against 0.24 % at
100 kHz, and its losses come at the moments it is being talked to over Wi-Fi,
which is when a button is pressed and let go. A real PCF8574 runs at 100.
