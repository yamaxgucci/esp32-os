firmware size with and without BLE, esp32s3, 21-22 October 2026

The question the plan asks (D4): what does CONFIG_ARGON_BLE_PERIPHERAL cost the
firmware?  On this tree's canonical build the answer is not a number of bytes:
it does not link.  Measured three times, twice on a hand-made sdkconfig and once
on the one `argon target esp32s3` generates from the committed defaults.

=== 1. as it is committed ===

    > argon target esp32s3          (regenerates sdkconfig from sdkconfig.defaults)
    > argon build
    argonos.bin binary size 0x12e510 bytes. Smallest app partition is 0x300000
    bytes. 0x1d1af0 bytes (61%) free.
    file size: 1 238 288 bytes
    partition table: partitions.csv (factory 0x300000, sysfs at 0x310000)

=== 2. with BLE on ===

    sdkconfig, added:
        CONFIG_ARGON_ENABLE_BLE=y

    The first attempt stopped in the compiler:

        components/argon_port/idf/src/bt_hw.c:42:10: fatal error:
        host/ble_gap.h: No such file or directory

    because ARGON_ENABLE_BLE `select`s BT_NIMBLE_ENABLED but a `select` does not
    move the host `choice`: sdkconfig still said CONFIG_BT_BLUEDROID_ENABLED=y.
    Setting the host stack explicitly:

        # CONFIG_BT_BLUEDROID_ENABLED is not set
        CONFIG_BT_NIMBLE_ENABLED=y
        CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
        CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
        CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
        CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
        CONFIG_ARGON_ENABLE_BLE=y

    Then it compiled and did not link:

        ld.exe: region `dram0_0_seg' overflowed by 38224 bytes
        collect2.exe: error: ld returned 1 exit status

=== 3. with the host stack's memory in PSRAM ===

    added CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y:
        dram0_0_seg overflowed by 38224 bytes      (no change)

    added CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y:
        dram0_0_seg overflowed by 35840 bytes      (2 384 bytes better)

    So the deficit is internal-DRAM .data/.bss that these knobs do not move:
    in this tree the BLE peripheral is not a bare peripheral - it comes with the
    input stack (NimBLE host plus esp_hid for a keyboard).  The Kconfig says the
    same about the two radios: "What does not fit is running both at once."

=== 4. restored ===

    > argon target esp32s3
    > argon build
    argonos.bin binary size 0x12e510 bytes   (1 238 288 bytes, the same figure)

Nothing was committed to sdkconfig: it is generated, and `argon target esp32s3`
is what makes it, so the tree is as a fresh checkout would build it.

=== 5. 23 Oct 2026: it fits, and section 3 was measured on the wrong target ===

    Sections 1-3 above answered "does the QEMU image grow with BLE", and that
    image is `argon target esp32s3`: CONFIG_ARGON_APP_ARENA_KB=192, an internal
    arena of a hundred and ninety-two kilobytes.  The board on the desk is
    `argon target esp32s3-cam` -> sdkconfig.esp32s3.board, which says
    CONFIG_ARGON_APP_ARENA_KB=32 and CONFIG_ARGON_APP_ARENA_PSRAM=y - the
    comment in that file says why: "on the S3 instruction RAM and the
    DMA-capable data RAM are one physical SRAM".  So the 38 KB deficit in
    section 3 was not BLE's appetite, it was 160 KB of arena the QEMU target
    reserves and the CAM target does not.  The conclusion drawn there ("in this
    tree the BLE peripheral comes with esp_hid and does not fit") was wrong, and
    a Kconfig default that has said `CONFIG_ARGON_ENABLE_BLE=y` all along was
    silently ignored because the local sdkconfig had been hand-edited to `is not
    set` - the trap argon.ps1 warns about at Warn-StaleSdkconfig.

    From the defaults, on the CAM target, BLE + Wi-Fi + the built-in camera:

        argon target esp32s3-cam && argon build
        argonos.bin 0x1fdd20, DIRAM 79.51% used, 70 033 bytes free
        (NimBLE, esp_hid, coexist and the Wi-Fi stack in the image)

    It links.  What it did on the board was assert:

        BLE assert emi.c 164, param 00000000 00007800
        Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)

    0x7800 is 30 720 bytes, and `r_emi_em_base_init` is the controller's
    exchange memory: one contiguous block of internal, DMA-capable RAM that it
    asks for when the radio starts, through `heap_caps_malloc(size,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)`.  On the board with Wi-Fi joined:

        mem     internal 185 KB total, 36 KB free, largest 25 KB

    36 KB free and 25 KB in one piece: the allocation returned NULL, the
    address stayed 0, and the assert fired.  Nothing about it was Wi-Fi versus
    Bluetooth - it was 30 KB that has to be next to itself.

    So the kernel's static tables went to PSRAM, which is what the board pack
    already has a switch for:

        boards/esp32s3-cam/BOARD.CFG:  [memory] kernel.static = psram
        -> CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
           CONFIG_ARGON_PLACE_KERNEL_STATIC_PSRAM=y

        .bss  100 712 -> 69 128   (31 584 bytes to PSRAM)
        DIRAM 271 727 -> 240 223   (79.51% -> 70.29%)
        free                     70 033 -> 101 537

    That is the number that matters for the radio, and it is the answer to
    "the S3 has PSRAM, so put it there": the memory the controller needs is
    DMA-capable and cannot live in PSRAM, but almost everything the kernel
    keeps can, and moving it is what leaves the 30 KB next to itself.
