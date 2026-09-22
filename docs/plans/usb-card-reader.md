# The board as a card reader: exposing `A:` over USB

*Feasibility study, `argon target esp32s3-lcd2`. Nothing here has been built or
flashed; every claim about this tree carries a path and a line, and every claim
about ESP-IDF carries a header or a file from `D:\Espressif\esp-idf` at
**v5.5.5** (`git -C D:\Espressif\esp-idf describe --tags` →
`v5.5.5`, `b774170ff46c393eeb5e495ea37936038d3f4f4f`).*

## The request, and what the answer has to be

> "if this is a full USB, make it so I can see the card on the computer"

The person has no card reader. The card is in the slot, mounted as `A:` by
ArgonOS, and the only cable in the world that reaches this board is the Type-C
socket. They want the card to appear as a drive on the PC, so files can be
copied both ways with no reader.

**The ask is reasonable and the feature is possible.** The hard part is not the
USB class — it is that the one physical USB port on this board is, today, the
console. That is a decision this tree already made, it is the thing that
flashing depends on, and it cannot be kept while a USB mass-storage device runs.
Sections 3 and 7 are the whole of that argument.

---

## 1. What this tree has today: no USB device stack at all

**There is no TinyUSB anywhere in the tree.** A search for `tinyusb`, `TUSB`,
`usb_msc`, `TUD_`, `CONFIG_TINYUSB` over the whole worktree finds no source, no
header, no `idf_component.yml` entry:

- `dependencies.lock` — the full dependency set is `espressif/esp_lcd_qemu_rgb`
  1.0.2, `idf` 5.5.5, `joltwallet/littlefs` 1.22.3. No `esp_tinyusb`, no
  `tinyusb`.
- `managed_components/` — two directories, `espressif__esp_lcd_qemu_rgb` and
  `joltwallet__littlefs`. That is the whole set.
- `sdkconfig` — no `CONFIG_TINYUSB_*` symbol exists at all.

The only "USB device" in the tree is the **console**, and it is the chip's
USB-Serial-JTAG peripheral, not the USB-OTG peripheral a TinyUSB device needs.

### The port layer is a console, and only a console

| Where | What it says |
|---|---|
| `components/argon_port/idf/argon/port/impl/uart.h:19-34` | `#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)` adds **one pseudo-port** past the real UARTs: `AG_PORT_UART_JTAG = SOC_UART_NUM`, `AG_PORT_UART_CONSOLE = AG_PORT_UART_JTAG`, pins `(-1)`. The comment at `:22-23` says it: "there is no USB-to-UART bridge on it, so UART0 talks to unconnected pins". |
| `components/argon_port/idf/src/uart_hw.c:31-45, 146-162` | Under `#ifdef AG_PORT_UART_JTAG`, the one port index forks to `usb_serial_jtag_driver_install()`; every other index stays a real UART. `is_jtag()`/`s_jtag_open` are the whole of it. |
| `components/argon_port/idf/src/uart_hw.c:184-187` | `ag_port_uart_config` returns `AG_OK` for that port: "USB-Serial-JTAG has no baud, parity or stop bits to set." |
| `components/argon_port/idf/src/uart_hw.c:298, 312` | Reads and writes go to `usb_serial_jtag_write_bytes` / `usb_serial_jtag_read_bytes`. |
| `components/argon_port/CMakeLists.txt:120` | The component depends on `esp_driver_uart esp_driver_usb_serial_jtag` — the console, nothing more. |

**So the port configures the native USB as a console and nothing else.** It is
not a device stack, not CDC-ACM in software, and not a UDC. There is no
`tud_*`, no `tinyusb_driver_install`, no descriptor of any kind.

### Where the console decision actually lives

- `sdkconfig.defaults.esp32s3:76` — `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, with
  the comment at `:72-75` that USB-Serial-JTAG becomes "an additional console
  endpoint at runtime once the console multiplexer exists".
- `sdkconfig.esp32s3.lcd2:37-59` — **the pack deliberately overrides nothing**,
  so the board inherits UART0. The comment at `:48-52` states this as a choice
  with a stated one-line escape hatch, and `:41-42` records the honest
  uncertainty: "Which of the two the socket carries is not written down anywhere
  I found".
- `boards/esp32s3-touch-lcd-2/BOARD.CFG:314-322` — the same, in the board file:
  "the console stays on UART0 ... and the one line that moves it to
  USB-Serial-JTAG is in `sdkconfig.esp32s3.lcd2`".

**But the generated `sdkconfig` says otherwise.** `sdkconfig:1342-1350`:

```
# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set
# CONFIG_ESP_CONSOLE_USB_CDC is not set
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
# CONFIG_ESP_CONSOLE_UART_CUSTOM is not set
# CONFIG_ESP_CONSOLE_NONE is not set
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=y
CONFIG_ESP_CONSOLE_UART_NUM=-1
CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM=4
```

`CONFIG_ESP_CONSOLE_UART_NUM=-1` is unambiguous: **there is no UART console in
the firmware as currently generated.** The board's console is the native USB.
This is consistent with the person's own report — they reach the board on
**COM9** — and with the fact that the S3's default in IDF's console choice is
USB-Serial-JTAG where the peripheral exists
(`D:\Espressif\esp-idf\components\esp_system\Kconfig:265`,
`config ESP_CONSOLE_USB_SERIAL_JTAG` at `:287`), so `sdkconfig.defaults.esp32s3:76`
is being lost to IDF's own default, not to the pack.

> **A correction the tree should carry, independent of this feature.** The pack
> believes the console is UART0 and the generated file says it is on the native
> USB. One of the two is wrong, and `impl/uart.h:19` picks the port model from
> the *generated* `CONFIG_` symbol, so the code follows the generated file. Any
> work in this area should settle that reading first: it decides whether the
> person's COM9 is the USB-Serial-JTAG console (it is, if the generated file is
> the built one) or a UART bridge nobody has documented.

For reference, the **vendor's** own generated config for this exact board keeps
both endpoints and lets whichever is wired print
(`D:\Work\waveshare-s3-lcd2\demo\ESP-IDF\01_sd_card_test\sdkconfig`):

```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set
# CONFIG_ESP_CONSOLE_SECONDARY_NONE is not set
CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED=y
```

That is the "second endpoint" option ArgonOS does not have yet, and it is worth
knowing it exists — but see §3: it does **not** buy the card reader.

### What *does* exist, and is a head start

The tree already runs the USB-OTG peripheral — **as a host**, for a wired
keyboard:

- `components/argon_port/idf/src/usb_hw.c:31-35` — guarded by
  `CONFIG_ARGON_USB_HOST && SOC_USB_OTG_SUPPORTED`; uses `usb/usb_host.h`.
- `components/argon_port/CMakeLists.txt:162-169` — the `usb` component is linked
  conditionally, exactly like `esp_adc`, so a build without the host does not
  put `usb_host.h` on the include path.
- `components/argon_kernel/Kconfig:533-553` — `config ARGON_USB_HOST`,
  `depends on SOC_USB_OTG_SUPPORTED`, `default n`. **Off by default**, and off
  in every `sdkconfig.*` in the tree.
- `components/argon_port/include/argon/port/usb.h:1-93` — the host contract:
  `ag_port_usb_start/stop/status/on_report`. Device-side is absent.

The relevant caps on this chip
(`components/soc/esp32s3/include/soc/soc_caps.h`): `SOC_USB_OTG_SUPPORTED 1`,
`SOC_USB_SERIAL_JTAG_SUPPORTED 1`, `SOC_USB_OTG_PERIPH_NUM (1U)`. **One OTG
peripheral and one Serial-JTAG peripheral, sharing one internal PHY.** That
"one" is the entire conflict.

---

## 2. What ESP-IDF 5.5.5 offers, from this machine

### The device stack is not in IDF — it is a managed component

IDF 5.5.5's own `components/usb/` contains **only the host stack and the PHY**:

```
components/usb/include/usb/usb_helpers.h
components/usb/include/usb/usb_host.h
components/usb/include/usb/usb_types_ch9.h
components/usb/include/usb/usb_types_ch11.h
components/usb/include/usb/usb_types_stack.h
components/usb/include/esp_private/usb_phy.h
```

There is **no `tud_*`, no MSC class, no `tinyusb` directory** anywhere under
`D:\Espressif\esp-idf`, and no `CONFIG_TINYUSB_*` symbol in its Kconfig. A
device stack must be added as a dependency. The maintained one is
`espressif/esp_tinyusb`, which pulls `espressif/tinyusb` with it
(`device/esp_tinyusb/idf_component.yml`: `idf: '>=5.0'`, `tinyusb: '>=0.17.0~2'`,
targets include `esp32s3`). **IDF 5.5.5 satisfies both constraints.** Latest is
2.3.0; 1.7.6~2 is the last of the 1.x line.

### The exact headers and functions

For a **custom** MSC backend — which is what §8 concludes this board needs — the
bare TinyUSB class driver is the right layer, and it needs nothing from
`esp_tinyusb` at all:

| Header | What is used |
|---|---|
| `tinyusb.h` | `tinyusb_driver_install(const tinyusb_config_t *)`, `tinyusb_driver_uninstall()` |
| `class/msc/msc_device.h` | `tud_msc_read10_cb(lun, lba, offset, buffer, bufsize)`, `tud_msc_write10_cb(...)`, `tud_msc_capacity_cb(lun, block_count, block_size)` (**required**), `tud_msc_scsi_cb`, `tud_msc_test_unit_ready_cb`, `tud_msc_start_stop_cb`, `tud_msc_is_writable_cb` |
| `tusb.h` | `tud_mount_cb` / `tud_umount_cb` for media-present reporting, `tud_task()` |

The media-present hook is **`tud_msc_test_unit_ready_cb`** — returning `false`
makes the host (and Windows Explorer) treat the LUN as "no media", which is
exactly right when there is no card in the slot. The capacity hook is
`tud_msc_capacity_cb`. There is no "start/stop" hook in the sense of a driver
lifecycle: `tud_msc_start_stop_cb` is the SCSI `START STOP UNIT` command (media
eject/load), and `tud_mount_cb`/`tud_umount_cb` are host-attach events.

The `esp_tinyusb` convenience API is the alternative, and its shape matters
(`device/esp_tinyusb/include/tinyusb_msc.h`, v2.3.0):

```c
esp_err_t tinyusb_msc_install_driver(const tinyusb_msc_driver_config_t *config);
esp_err_t tinyusb_msc_new_storage_spiflash(const tinyusb_msc_storage_config_t *, tinyusb_msc_storage_handle_t *);
esp_err_t tinyusb_msc_new_storage_sdmmc   (const tinyusb_msc_storage_config_t *, tinyusb_msc_storage_handle_t *);
esp_err_t tinyusb_msc_get_storage_capacity(handle, uint32_t *sector_count);
esp_err_t tinyusb_msc_get_storage_sector_size(handle, uint32_t *sector_size);
esp_err_t tinyusb_msc_set_storage_mount_point(handle, tinyusb_msc_mount_point_t);
```

**And its `medium` union is the show-stopper for this board.** From that header:

```c
typedef struct {
    union {
        wl_handle_t wl_handle;   /* Wear levelling handle for SPI flash storage. */
#if (SOC_SDMMC_HOST_SUPPORTED)
        sdmmc_card_t *card;      /* SD/MMC card descriptor. */
#endif
    } medium;
    ...
} tinyusb_msc_storage_config_t;
```

Two backends exist: **internal SPI flash** and **an `sdmmc_card_t`**. This
board's card is on **SPI**
(`boards/esp32s3-touch-lcd-2/BOARD.CFG:114-125`, `interface = spi`,
`spi_host = 2`, `max_khz = 20000`) — and the port mounts it through IDF's
`esp_driver_sdspi`, not `esp_driver_sdmmc`. `tinyusb_msc_new_storage_sdmmc`
wants a card handle produced by `sdmmc_host`; there is no `sdspi` entry point in
the API. In 1.x the MSC callbacks were user-overridable and a custom backend was
possible; **2.0.0 removed them** ("MSC: Removed dedicated callbacks; introduced
a single callback with an event ID for each storage", changelog v2.0.0), and the
event callback carries mount/format events only — no read/write hook.

> `SOC_SDMMC_HOST_SUPPORTED` is 1 on the S3, so the `card` arm *compiles*. It
> still cannot be handed a card the kernel mounted over SPI. This is the kind of
> mismatch that looks like a one-line fix and is not.

**Conclusion for §2: MSC device class is available, but only through TinyUSB's
own class driver with a hand-written backend, or through `esp_tinyusb` after
re-doing the card mount on SDMMC. The bare class driver is the honest choice**
(§8).

### What the port's own storage layer gives us

Everything a backend needs already exists behind the port contract
(`components/argon_port/include/argon/port/storage.h:97-112`):

```c
ag_err_t ag_port_sd_mount(const ag_port_sd_cfg_t *cfg, const char *base_path,
                          bool allow_format, ag_port_sd_t *out);
ag_err_t ag_port_sd_unmount(const char *base_path, ag_port_sd_t card);
uint32_t ag_port_sd_sector_size(ag_port_sd_t card);
uint64_t ag_port_sd_sectors(ag_port_sd_t card);
ag_err_t ag_port_sd_read_sectors(ag_port_sd_t card, uint64_t sector, void *buf,
                                 uint32_t count);
```

Three of the five exist. **Two do not, and they are the write path:**

- **`ag_port_sd_write_sectors` does not exist.** `storage_hw.c` implements
  `mount` (`:224`), `unmount` (`:245`), `sector_size` (`:255`), `sectors`
  (`:264`), `read_sectors` (`:270`), `media_space` (`:282`) — and nothing else.
  A card reader that can only read is not the feature requested.
- **There is no media-present call.** The port has no way to say "the card is
  gone now". `fs/storage.h:24-28` states the hardware reason plainly: "There is
  no line on this class of board that says a card has been removed, so the
  system cannot notice: it has to be told". For a card reader this becomes a
  live correctness problem — Windows caches the FAT and will write to a volume
  whose sectors have stopped answering — so a **presence probe**
  (`ag_port_sd_present()`, a cheap re-read of sector 0) has to be added too.

---

## 3. The conflict, settled: USB-Serial-JTAG and TinyUSB **cannot** coexist

**They cannot, on this chip, with this board's wiring. This is settled by
Espressif's own documentation and by IDF's source, not inference.**

IDF's own guide says it twice
([USB OTG Console](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/usb-otg-console.html)):

> "At the moment, this 'USB Console' feature is incompatible with TinyUSB stack."

> "By default, the USB_SERIAL_JTAG module is connected to the internal PHY of
> ESP32-S3, while the USB OTG peripheral can be used only if the external USB PHY
> is connected. Since the CDC console is provided via the USB OTG peripheral, it
> cannot be used through the internal PHY in this configuration."
> "You can permanently switch the internal USB PHY to work with USB OTG
> peripheral instead of USB_SERIAL_JTAG by burning `USB_PHY_SEL` eFuse."

And Espressif's FAQ states the general rule without qualification: the USB-OTG
peripheral and the USB-Serial-JTAG peripheral are mutually exclusive — *"USB-OTG
外设和 USB-Serial-JTAG 外设在同一时刻只有其一能工作"* ("only one of the USB-OTG
peripheral and the USB-Serial-JTAG peripheral can work at any one time").

**The mechanism, in IDF's source.** Both drivers reach for the same internal
PHY, through two different registers, and neither knows about the other:

| Owner | Code | Effect |
|---|---|---|
| USB-Serial-JTAG console | `components/esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:211-217` — `usb_serial_jtag_ll_phy_enable_external(false)` ("Use internal PHY"), `..._enable_pad(true)`, `..._set_defaults()` | Sets the USB_SERIAL_JTAG peripheral's PHY select to internal and drives the D+/D− pads from it |
| TinyUSB device | `esp_tinyusb/device/esp_tinyusb/tinyusb.c` — `usb_phy_config_t phy_conf = { .controller = USB_PHY_CTRL_OTG, .target = USB_PHY_TARGET_INT, .otg_mode = USB_OTG_MODE_DEVICE, .otg_speed = USB_PHY_SPEED_FULL }`, then `usb_new_phy(&phy_conf, &phy_hdl)` | Claims the internal ("FSLS") PHY for the OTG peripheral |

There is exactly one internal PHY on the S3
(`SOC_USB_OTG_PERIPH_NUM (1U)`, `SOC_USB_SERIAL_JTAG_SUPPORTED 1`), and IDF
tracks its ownership in a single ref-counted slot
(`components/usb/usb_phy.c:459-474`, `p_phy_ctrl_obj->fsls_phy`;
`usb_phy_get_phy_status(USB_PHY_TARGET_INT, &status)` at `:477-492` reports
`USB_PHY_STATUS_FREE` or `IN_USE`). **Whichever side is installed last wins, and
the other one goes silent** — the pads are reconfigured out from under it.

There is no "internal PHY for OTG *and* internal PHY for Serial-JTAG". There is
one PHY, two owners, and no arbiter.

### What breaks if both are attempted

- The console stops mid-session: writes to `usb_serial_jtag` go into a FIFO
  nobody drains to a host, and the person sees a dead COM9. No error is raised;
  `usb_serial_jtag_write_bytes` still "succeeds". This is the failure mode the
  IDF guide warns about at its limitation 6 — "USB functionality will not work or
  will not fully work at some moments during development ... using the UART
  console for flashing and monitoring provides a much better development
  experience".
- The card reader enumerates inconsistently, because installing TinyUSB on a PHY
  whose pads the console just reconfigured is a race, not a handover.
- The **kernel's own console task** (`console/uart_console.c:44-74`) keeps
  polling a port whose backing peripheral no longer answers the host, so the
  system looks alive and is unreachable.

### The options, and the recommendation

| # | Option | Verdict |
|---|---|---|
| **A** | **Move the console to UART0** (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n`, `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`), give the internal PHY to TinyUSB MSC permanently. | **Rejected as the default.** It frees the PHY cleanly and is exactly the one-line move `sdkconfig.esp32s3.lcd2:52-55` documents. But it deletes the console from the only socket the person has. Whether UART0 43/44 is *also* on that socket is written down nowhere (`BOARD.CFG:314-322` says so explicitly), and if it is not, the board becomes headless on the desk and the flashing conversation in §7 gets much worse. |
| **B** | **Keep the console on USB-Serial-JTAG; expose the card only on demand**, cycling the PHY: detach the console endpoint, uninstall the Serial-JTAG driver, install TinyUSB + MSC, run until told to stop, then uninstall TinyUSB and reinstall Serial-JTAG. | **Recommended.** It needs no reflash of the console decision, it changes nothing about the boot path, and the silent-window risk is confined to a deliberate operator command. The full mechanical argument is §7. |
| **C** | **Burn `USB_PHY_SEL` eFuse** to hand the internal PHY to OTG permanently. | **Rejected — irreversible.** It is a one-time eFuse write. If the socket is the Serial-JTAG path, the board loses its console and its ROM-loader USB port *for good*, with no way back that does not involve the UART pads. This is not a decision to take in a feature branch. |
| **D** | **Both endpoints, vendor style** (`ESP_CONSOLE_UART_DEFAULT=y` **and** `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`). | **Does not solve it.** The secondary console still means the Serial-JTAG peripheral owns the internal PHY, so TinyUSB still cannot start. It buys the *console* robustness the pack wanted — and it is a good change on its own merits, see §1 — but it is orthogonal to the card reader. Also note ArgonOS has no console multiplexer yet (`sdkconfig.defaults.esp32s3:72-75`), so it would need `uart_hw.c` to grow a second endpoint first. |
| **E** | **MSC as a separate boot mode** (a dedicated build, or an OTA-swapped image, that is a card reader and nothing else). | **Viable fallback, not the plan.** It avoids all arbitration but costs a flash cycle to enter and leave, and this tree's flasher story is already the delicate part (§7). Keep it in the back pocket for the day option B's silent window proves too annoying. |

**Recommendation: B.** The card reader becomes a shell command — `usbmsc
start` / `usbmsc stop` — that takes the USB peripheral for as long as it is
running. The console is off COM9 while it runs, and that is stated to the
operator before the change is made, because a console that silently stops is
the failure this project has already paid for twice.

---

## 4. How the card's blocks are reached from the kernel today

**Raw sectors are already reachable from the kernel. There is no block-device
class missing.** The ABI has had geometry since before this feature was
considered:

- `sdk/include/argon/abi.h:1105` — `AG_DEV_BLOCK` exists as a device class, and
  `:1113` — `AG_DEV_STORAGE` is the class actually used for media.
- `sdk/include/argon/abi.h:1159` — `AG_IOC_GEOMETRY = AG_IOC(AG_DEV_STORAGE, 1)`.
- `sdk/include/argon/abi.h:1180-1183` — `ag_geometry_t { uint32_t sector_size;
  uint64_t sectors; }`. **The block interface is already shaped like a disk.**
- `sdk/include/argon/abi.h:1197-1219` — `ag_dev_ops_t`: `open`, `close`, `read`,
  `write`, `ioctl`, `size`, plus optional `*_session` variants.
- `sdk/include/argon/abi.h:1464-1469` — a device "is a byte addressable object
  with a name, and the handle carries the position — so `fs->read`, `fs->write`
  and `fs->seek` work on `/dev/sd0` exactly as they do on a file".

And the card is already published as such a device, from the port's card handle:

`components/argon_kernel/src/fs/storage.c:316-320` —

```c
static const ag_dev_ops_t k_sd_ops = {
    .read = sd_dev_read,
    .ioctl = sd_dev_ioctl,
    .size = sd_dev_size,
};
```

registered at `:359-367` as `sd0`, class `AG_DEV_STORAGE`, driver `sdspi` or
`sdmmc` by the board's `kind` (`:361`), with `.flags = AG_DEVF_READONLY |
AG_DEVF_HOTPLUG`. `sd_dev_ioctl` (`:298-314`) answers `AG_IOC_GEOMETRY` with the
card's real sector size and count; `sd_dev_read` (`:260-296`) walks sectors
through the single DMA bounce buffer `s_sd_bounce` (`:197`, allocated at
`:351`), because `ag_port_sd_read_sectors` is sector-granular and a hexdump has
odd offsets.

**Two things stand between this and a card reader.**

1. **`write` is deliberately absent, and enforced in two places.**
   `storage.c:363` sets `AG_DEVF_READONLY`; `device.c:464-468` refuses an open
   for writing with `-AG_EROFS` and `device.c:630-633` refuses the write itself.
   The reason is written out at `storage.c:185-193` and it is a *correct*
   reason: "The filesystem above them is mounted and has its own cache; writing
   underneath it would corrupt what it thinks it wrote." **A USB card reader
   writes underneath the filesystem** — so the flag is not the obstacle, the
   mounted FAT is.
2. **The card is mounted as `A:` the whole time.**
   `storage.c:407-444` `mount_media()` calls `ag_port_sd_mount` and then
   `ag_idfvfs_create(..., "fat", ...)`; `:427` mounts it at `/sd` with
   `AG_MOUNT_REMOVABLE`. Nothing unmounts it for the duration of an MSC session.
   Letting a PC write sectors while ArgonOS holds a FAT driver with a dirty
   cache on the same card is a guaranteed corruption, and it is the single
   largest correctness risk in this feature.

**Is a new ABI class needed? No.** What is needed is (a) a write op behind the
existing `ag_dev_ops_t`, and (b) an ownership rule: while the MSC session is
live, the kernel must have **ejected `A:`** — which it already knows how to do.
`ag_storage_eject_media()` (`storage.h:32`, `storage.c:463-470`) tears the
mount down, revokes the device node and leaves the port's card handle alone if
we arrange it to. That is the honest smallest interface, and it is one function,
not a class.

**An important detail in the port's favour.** Because the port owns the card
through `ag_port_sd_read_sectors`, the MSC backend inherits the board's SPI
quirks for free — critically the measured panel-CS trap
(`BOARD.CFG:89-112`, `cs_hold = 45`) which `storage_hw.c` handles inside
`ag_port_sd_mount`. A backend that called IDF's `sdspi` driver directly would
have to re-learn that trap, and would collide with the panel's bus. **The MSC
backend must go through `argon_port`, not around it.**

---

## 5. What is missing on the kernel side, with sizes

Counted against the tree as it stands. Line counts are estimates for new code,
not measurements.

| # | Work | Where | Size |
|---|---|---|---|
| 1 | `ag_port_sd_write_sectors()` | `components/argon_port/include/argon/port/storage.h` (+1 decl), `idf/src/storage_hw.c` (+1 fn, `esp_driver_sdspi`'s `sdspi_host` write call), `idf/argon/port/impl/storage.h` if the impl splits | **~25 lines, 1 function** |
| 2 | `ag_port_sd_present()` — cheap sector-0 re-read, so "card pulled" is detectable | same two files | **~20 lines, 1 function** |
| 3 | **USB device port module**: `ag_port_usbd_*` — start/stop, an MSC backend wired to (1)/(2), `tud_msc_*` callbacks, a `tud_task` service loop | new `components/argon_port/idf/src/usbd_hw.c`; new `components/argon_port/include/argon/port/usbd.h`; `idf/argon/port/impl/usbd.h` | **~350–450 lines, ~12 functions.** The bulk: `tud_msc_capacity_cb`, `tud_msc_read10_cb`, `tud_msc_write10_cb`, `tud_msc_test_unit_ready_cb`, `tud_msc_start_stop_cb`, `tud_msc_is_writable_cb`, `tud_mount_cb`/`tud_umount_cb`, plus start/stop/status |
| 4 | **PHY arbitration between the console and TinyUSB** — this is the part that decides whether the feature is safe | `idf/src/usbd_hw.c`, with calls into `uart_hw.c`/`impl/uart.h`; reuse `usb_serial_jtag_driver_uninstall()` (`driver/usb_serial_jtag.h:92`) and `ag_uart_console_detach()` (`console/uart_console.h`, already used at runtime by `uartbridge`, `shell/shell.c:5392`) | **~120 lines, ~4 functions** (take, release, status, and a guard that refuses to start while something else holds the port) |
| 5 | **Kernel glue**: `usbmsc` shell command; eject/remount `A:` around a session; publish `usbmsc0` as `AG_DEV_STORAGE` | `components/argon_kernel/src/shell/shell.c` (command table at `:5499-5595` is where `uartbridge` and `reboot` live); `fs/storage.c` for the eject/remount | **~150 lines, ~4 functions** |
| 6 | **Kconfig + build**: `config ARGON_USB_MSC` (`depends on SOC_USB_OTG_SUPPORTED`, `default n`), modelled on `ARGON_USB_HOST` (`argon_kernel/Kconfig:533`); conditional `target_link_libraries` for `espressif__esp_tinyusb` / `espressif__tinyusb`, modelled on `argon_port/CMakeLists.txt:162-169` | `argon_kernel/Kconfig`, `argon_port/CMakeLists.txt`, `main/idf_component.yml` (or root `idf_component.yml`), `dependencies.lock` | **~40 lines + the dependency** |
| 7 | **Tests** | `host-tests/` for the geometry/ownership logic; a QEMU target has no USB device, so the board is the gate — say so in the plan rather than pretending otherwise | **~150 lines** |

**Total: roughly 850–1000 new lines, ~25 functions, 2 new files, ~8 touched
files, plus one new managed dependency.**

Two sizes worth stating separately because they are easy to miss:

- **Flash cost.** The firmware is 1 412 880 bytes today
  (`partitions_16mb_lcd2.csv:9-12`, measured for the S3-Zero). The `factory`
  slot is 3 MB (`:37`), so there is ~1.6 MB of headroom — but the 16 MB table's
  appfs runs to `0xAF0000` and the 8 MB default reserved roughly 848 KB of
  unallocated tail (`sdkconfig.defaults.esp32s3:9`,
  `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`). TinyUSB's device stack is tens of KB and
  will fit; the point is that **the table must be re-checked against a real
  build**, and the gate is shared (§9).
- **RAM cost.** TinyUSB's device stack plus a task (`CONFIG_TINYUSB_TASK_STACK`
  and the MSC bounce buffer, `CONFIG_TINYUSB_MSC_BUFSIZE`). The 192 KB code
  arena (`sdkconfig.defaults.esp32s3:41`) is reserved whether an app runs or
  not, so this comes out of what is left. The esp_tinyusb README's own S3
  throughput table is the other number that matters here: **0.57 MB/s read,
  0.24 MB/s write at a 512-byte FIFO; 0.93 / 0.93 MB/s at 8192** — so
  `CONFIG_TINYUSB_MSC_BUFSIZE` is not a tuning nicety, it is a 4× difference on
  a card holding a gigabyte.

---

## 6. The staged plan

**Stage 0 — settle the console question from §1, before writing any code.**
Read the generated `sdkconfig` for the build that is actually on the board and
confirm whether `CONFIG_ESP_CONSOLE_UART_NUM` is `-1`. Everything downstream
assumes the console is on the native USB. If it is instead on UART0, this
feature is *much* easier (option A becomes free) and the recommendation changes.
**One boot log settles it; nothing else can.**

**Stage 1 — read-only, and it is a real deliverable.**
Add (1) `ag_port_sd_write_sectors` is *not* needed here; add (3) the port's
`ag_port_usbd_*` with **only** `tud_msc_capacity_cb`, `tud_msc_read10_cb`,
`tud_msc_test_unit_ready_cb`; `tud_msc_write10_cb` returns `false` so the host
mounts the volume **read-only**. Add (4) the PHY handover and (5) `usbmsc
start|stop`. Add (2) the presence probe. At the end of Stage 1 the person can
plug the board in, run `usbmsc start`, and **copy files off the card in Explorer
with no reader** — which is the larger half of what they asked for, and it
cannot corrupt anything.

**Stage 2 — writes, and only after the ownership rule is enforced.**
Add `tud_msc_write10_cb` over (1). Before the LUN reports writable, the kernel
must have ejected `A:` (`ag_storage_eject_media`, `storage.c:463`) and must
refuse to remount it while the session lives; on `usbmsc stop`, remount.
`tud_msc_is_writable_cb` and the `AG_DEVF_READONLY` flag stop being enough here
— this is where the corruption risk actually lives, so it is a separate stage
with its own board test: write a file from Windows, `usbmsc stop`, `dir a:`, and
read it back on the board.

**Stage 3 — the niceties, only if Stage 2 holds.** A second LUN for internal
flash (`flash0` already exists at `storage.c:333-341` and is a real device);
auto-start on boot behind a `SYSTEM.CFG` line; a `usbmsc` status in `dev`. Each
is small and each is optional.

---

## 7. Risks — first of all the one that matters: **can this board still be flashed?**

This board is flashed over the same USB port, by the neighbour board
(git `b908237` "cluster: the board on the power bank, flashed by the one next to
it"; the helper is `apps/espflash/espflash.c`). A card reader that costs the
ability to reflash the board is not a feature, it is a brick with a nice
filesystem on it. Three separate mechanisms matter, and they fail differently.

**7.1 The ROM loader is independent of what the application does — this is the
safety net, and it holds.** On the S3 the download loader lives in mask ROM.
`reboot /download` (`shell/shell.c:1534-1571`, `shell.c:5595`) writes
`RTC_CNTL_FORCE_DOWNLOAD_BOOT` in `RTC_CNTL_OPTION1_REG` and restarts
(`components/argon_port/idf/src/dlboot_hw.c:54-65`), and `ag_port_download_boot_capable()`
(`:49-52`) is a *tested capability* — the register is present on the S3 and the
C3 and absent on the ESP32 and the C6 (`dlboot_hw.c:24-26`). The ROM then runs
before any application code, so **a running card reader cannot stop the board
from entering the loader.** What ROM listens on is decided by the strapping
pins (`USB_PHY_SEL` eFuse plus `EFUSE_STRAP_JTAG_SEL`, which is the
"pull GPIO3 low" row in the S3 TRM's USB-PHY table), not by the application.

**7.2 The dangerous window is real and must be stated to the operator.** While
TinyUSB owns the internal PHY, **the Serial-JTAG console is gone, and with it
the `reboot /download` command** — you cannot type a command into a port that
does not exist. That is why the session must be (a) operator-initiated,
(b) announced before it starts, with a stated way back, and (c) *stoppable
without the console*, which means the stop condition has to be something the
board itself can see: a host-initiated eject on the PC side, a timeout, or the
reset button. **This is the single design constraint that decides whether option
B is usable, and it should be settled in Stage 1, not after.**

**7.3 The silent-failure mode is the one this project has already paid for.**
`uart_hw.c:189-199` documents a boot log that went silent because a console was
reprogrammed while a byte was in flight, and it took an unrelated image-layout
change to make it reproducible. A PHY handover is the same class of race with a
worse symptom: the console "works" (`usb_serial_jtag_write_bytes` returns
success) while nothing arrives. So the handover needs the same treatment the
UART path got — drain first, and verify the other end is actually gone before
claiming the PHY (`usb_phy_get_phy_status(USB_PHY_TARGET_INT, &status)` is the
check, `usb_phy.c:477-492`) — and the operator needs a written recovery
procedure that assumes the console may not come back on its own.

**Recovery, stated plainly, because the person holding the board needs it:**
1. **Try the console.** If COM9 is alive, `usbmsc stop`.
2. **If the console is dead, power-cycle the board.** The PHY ownership is
   runtime state, not eFuse and not RTC — a clean boot re-installs the
   USB-Serial-JTAG console exactly as today, because `usbmsc` is started by a
   command and not by the boot path (Stage 1).
3. **If the board is in a boot loop with the console dead**, hold BOOT (GPIO0)
   and toggle reset into ROM download mode, then `idf.py flash` — the classic
   path. **This is the reason the feature must never be enabled from the boot
   path in Stage 1 or 2.**
4. **The option-C eFuse is what makes recovery 3 the *only* option.** That is
   the concrete reason C is rejected in §3.

**7.4 Second-order risks.**
- **Corruption of the card** if writes land while `A:` is mounted — §4 and
  Stage 2. The highest-probability real harm in the whole feature.
- **The panel and the card share one SPI bus** (`BOARD.CFG:46-75`), and the
  MSC session drives the card hard while the console may be repainting the
  panel. The port already arbitrates the bus
  (`storage_hw.c` `s_bus_up[]`, `io_hw.c` `ag_port_spi_open`), and the measured
  `cs_hold = 45` trap is handled there — so **the backend must not bypass the
  port**, or it will corrupt the card's replies exactly as
  `BOARD.CFG:97-104` records.
- **Throughput.** ~0.9 MB/s at best on the S3 through esp_tinyusb's own S3
  measurements. A 7.7 GB card (the card measured in `BOARD.CFG:101`: "SDHC,
  7690 MB") is not something to copy in full, and the plan should say so rather
  than let someone discover it.
- **Windows will offer to format.** No presence line means the host can be
  looking at a volume whose card was pulled. The presence probe (2) and
  `tud_msc_test_unit_ready_cb` are the answer, and they are cheap.
- **A new managed dependency** enters a lock file that is currently three
  entries and a hash (`dependencies.lock`). It should be added deliberately,
  pinned, and only for the target that needs it.
- **Verification.** No QEMU model of a USB device exists, so this feature cannot
  be gated the way everything else in this tree is gated — the board is the only
  gate. That is an honest reduction in the project's usual coverage and belongs
  in the plan as a cost, not a footnote.

---

## 8. One recommendation, in one paragraph

Add `espressif/esp_tinyusb` for its bare TinyUSB dependency and **write the MSC
backend by hand** against `tud_msc_capacity_cb` / `tud_msc_read10_cb` /
`tud_msc_write10_cb` / `tud_msc_test_unit_ready_cb`, reading and writing through
`ag_port_sd_read_sectors` and a new `ag_port_sd_write_sectors` — because
`esp_tinyusb`'s own MSC storage API accepts only a wear-levelling handle or an
`sdmmc_card_t`, and this board's card is on SPI behind the port's own mount with
a board-specific CS trap that only the port knows about. Gate it behind a new
`ARGON_USB_MSC` Kconfig option, default **n**, and expose it as an operator
command, never from the boot path. Ship **read-only first**: that alone satisfies
"let me see the card on the computer" and cannot corrupt anything. Take the USB
peripheral for the duration, accept that the console is off COM9 while it runs,
and write the recovery procedure into the documentation next to the command,
because a board that cannot be reflashed is the one outcome worth more care than
the feature itself.

## Решение заказчика: это приложение, а не ядро

Прямое указание: режим картридера должен быть **отдельным приложением .AXE**, а не частью
ядра ОС.  Это меняет раскладку работы так:

* **Приложение** (.AXE) владеет всей политикой: начать и закончить сеанс, выбрать, какое
  блочное устройство отдавать (sd0), только чтение или запись, размонтировать A: на время
  сеанса, условие остановки (таймаут или команда с хоста) и что печатать оператору.
* **Порт** делает только то, что не может приложение: TinyUSB живёт за rgon_port, потому что
  ядро и приложения не включают заголовки ESP-IDF, а весь чип-специфичный код — там.  Порт
  предоставляет это как устройство/класс, которым управляет приложение, — тот же приём, что у
  AG_DEV_LINK, AG_DEV_DISPLAY, AG_DEV_INPUT: чип-специфика внизу, политика наверху.
* **Ядра это не касается**: ни новой политики в ядре, ни запуска из загрузки.  Отсюда, кстати,
  автоматически решается главная опасность из §6: режим не может включиться сам при загрузке,
  потому что его запускает оператор приложением, и остановка по таймауту живёт в том же
  приложении.

Объём отчёта (§5) перераспределяется: порт — небольшой слой установки TinyUSB и передачи
колбэков в таблицу приложения, приложение — основная часть (чтение/запись секторов через
существующее блочное устройство, размонтирование A:, сессия и её завершение).

Порядок не меняется и остаётся двухэтапным: **сначала только чтение** (запись отклоняется, хост
монтирует том read-only), и лишь потом запись с размонтированием A:.
