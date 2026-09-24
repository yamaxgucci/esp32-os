# The layer between ArgonOS and whatever it is running on.
#
# One line decides which machine this firmware is built for.  Everything the
# kernel needs from below arrives through include/argon/port/*.h, and the
# directory named here is what answers it.
#
#   idf    ESP-IDF 5.5 - what the system runs on today
#   host   the host OS, for `argon tests` (built by host-tests/, not here)
#   bare   the skeleton: #error where the work is
set(ARGON_PORT idf)

if(NOT IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/${ARGON_PORT}")
    message(FATAL_ERROR "argon_port: no such port '${ARGON_PORT}'")
endif()

# Sources belonging to the chosen port.  The header part of the contract costs
# nothing (see README: static inline, or it does not belong here); these are the
# subsystems that are genuinely machine-specific.
#
# CONFIGURE_DEPENDS, and it is worth the re-glob it costs.  A plain GLOB is
# evaluated once and cached, so a NEW file in that directory does not build and
# the failure arrives as a linker error naming functions that are sitting right
# there in the file you just wrote.  Ten minutes each time, and the cure is not
# discoverable from the message.
#
# NOT IN SCRIPT MODE.  ESP-IDF runs every component's CMakeLists a second time
# under `cmake -P` to collect its requirements, and CONFIGURE_DEPENDS is
# refused outright there ("invalid for script and find package modes") - which
# fails the whole configure, some way from this line.  The requirements pass
# does not care which sources exist, so the plain glob is the right answer to
# give it.
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB ARGON_PORT_SRCS "${CMAKE_CURRENT_LIST_DIR}/${ARGON_PORT}/src/*.c")
else()
    file(GLOB ARGON_PORT_SRCS CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_LIST_DIR}/${ARGON_PORT}/src/*.c")
endif()

# The architecture component - `xtensa` on one side, `riscv` on the other - is
# NOT named here, and that is a build failure rather than a preference.
#
# It was `ARGON_ARCH_REQUIRES`, decided by `if(CONFIG_IDF_TARGET_ARCH_XTENSA)`,
# and a REQUIREMENTS list may not be conditional on a CONFIG_* value.  ESP-IDF
# evaluates every component's CMakeLists a FIRST time in CMake scripting mode to
# collect its requirements, and that pass runs before the configuration exists
# (`build.cmake`: "Perform early expansion of component CMakeLists.txt in CMake
# scripting mode", called above __kconfig_generate_config).  The variable is
# therefore undefined there, the else() branch is taken on every target, and
# configuring an ESP32 - the CYD, the S3-Zero - died with
#
#   CMake Error: Failed to resolve component 'riscv' required by component
#   'argon_port'
#
# which is what made every board except the C3 unbuildable.  On the C3 it hid
# itself: there `riscv` is a real component, so the wrong branch gave the right
# answer.
#
# Nothing is lost by dropping it.  ESP-IDF appends the target's architecture
# component to __COMPONENT_REQUIRES_COMMON and adds the common requirements to
# every component of the build, so `riscv` with riscv/rvruntime-frames.h and
# `xtensa` with xtensa/corebits.h are already required, include directories and
# all, by the component system itself.  What is NOT a common requirement is
# esp_app_format, which is why it is named in the list below instead: without it
# esp_app_desc.h, the running image's ELF SHA256, is not reachable.

# HOW THE PANIC PATH IS ENTERED ON RISC-V.
#
# The trap entry in components/riscv/vectors.S calls xt_unhandled_exception()
# and, if that returns, restores the frame and mrets - so routing that one call
# to fault_hw.c is all that is needed to get in ahead of the panic handler, and
# all that is needed to send a faulting application somewhere else instead.
# --wrap does exactly that and nothing else: it rewrites the reference, leaving
# ESP-IDF's own symbol in place as __real_xt_unhandled_exception.
#
# esp_panic_handler is wrapped as well, because a watchdog, a cache error or an
# abort reaches the panicking code without passing through that first call.  The
# record written there is what survives the reboot.
#
# Both only on RISC-V: the Xtensa half of fault_hw.c keeps no record across a
# reboot, so wrapping either symbol there would break the link for a behaviour
# nothing would use.
#
# NOT IN SCRIPT MODE, like the glob above: ESP-IDF re-runs this file under
# `cmake -P` to read its requirements, and build properties do not exist there.
if(NOT CMAKE_SCRIPT_MODE_FILE AND NOT CONFIG_IDF_TARGET_ARCH_XTENSA)
    idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=esp_panic_handler" APPEND)
    idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=xt_unhandled_exception" APPEND)
endif()

idf_component_register(
    SRCS ${ARGON_PORT_SRCS}
    INCLUDE_DIRS "include" "${ARGON_PORT}"
    # Public, not private: the port headers are inline and pull these in
    # wherever the kernel includes them.
    REQUIRES argon_abi heap esp_timer freertos log esp_system esp_hw_support
             esp_common esp_psram
             # spi_flash_cache_enabled(): the RISC-V fault handler is in IRAM and
             # has to know whether the code that decides may be fetched at all
             # (fault_hw.c).  The header is private to spi_flash, so it has to be
             # named here rather than reached through another component.
             spi_flash
             # esp_mmu_map / esp_cache_msync: mapping PSRAM into the instruction
             # window for the code arena (execmem_hw.c, S-1)
             esp_mm
             # the DVP camera transport (camera_hw.c); IDF core, small, linked
             # only when CONFIG_ARGON_ENABLE_CAMERA references it
             esp_driver_cam
             # the clock, for argon/port/power.h.  Present whether or not the
             # build enables power management: with it off, esp_pm_configure
             # is still there and still answers, which is how the port says
             # this image cannot scale.
             esp_pm
             # the running image's ELF SHA256, for the panic record (fault_hw.c).
             # The one thing that used to travel in ARGON_ARCH_REQUIRES and is
             # not a common requirement of its own - see the note above.
             esp_app_format
             # the serial ports everything talks over before there is anything
             # else - and, on a part whose Type-C socket goes straight to the
             # chip, the USB device that is the console instead of a UART
             esp_driver_uart esp_driver_usb_serial_jtag
             # flash partitions, and the two filesystems a board is born with
             esp_partition fatfs wear_levelling vfs sdmmc esp_driver_sdmmc
             esp_driver_sdspi esp_driver_spi joltwallet__littlefs
             # pins and buses straight through to the chip.  esp_adc is
             # deliberately absent - see ARGON_ENABLE_ADC in argon_kernel.
             esp_driver_gpio esp_driver_i2c esp_driver_ledc
             # the panel interface; the virtual RGB device itself is linked
             # below, only when there is an emulator to show it (ARGON_PANEL_QEMU)
             esp_lcd
             # the link, whichever it is, and lwIP sockets above it:
             # esp_eth is OpenEth under QEMU, esp_wifi is the radio on silicon
             esp_eth esp_wifi esp_netif esp_event nvs_flash lwip
             # BLE, for the keyboard a chip with no USB cannot have any other
             # way.  Mutually exclusive with Wi-Fi at run time on this part;
             # see ARGON_ENABLE_BLE in argon_kernel.
             bt esp_hid)

# Analogue input, off by default: linking esp_adc runs a constructor that stops
# the boot under QEMU, and QEMU is how this system is verified.
# The UART's receive by DMA without stopping (uartdma_hw.c, AG_UART_DMA_RING)
# takes over the receive channel of IDF's UHCI controller, whose struct is
# private to the driver: its header, and nothing else of it, from there.
if(EXISTS "${IDF_PATH}/components/esp_driver_uart/src/uhci_private.h")
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${IDF_PATH}/components/esp_driver_uart/src")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AG_UHCI_PRIVATE=1)
endif()

if(CONFIG_ARGON_ENABLE_ADC)
    idf_component_get_property(adc_lib esp_adc COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${adc_lib})
endif()

# Sound, off by default for the same shape of reason: the converters this uses
# exist on the original ESP32 only, and QEMU models neither them nor their DMA.
# See ARGON_ENABLE_AUDIO in argon_kernel.
if(CONFIG_ARGON_ENABLE_AUDIO)
    idf_component_get_property(dac_lib esp_driver_dac COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${dac_lib})
endif()

# The virtual RGB panel (CONFIG_ARGON_PANEL_QEMU), on the same terms as the
# camera below: named here rather than in REQUIRES so that a target with no
# emulator neither fetches the managed component nor tries to compile it.  It
# does not build for RISC-V - see the Kconfig entry.
if(CONFIG_ARGON_PANEL_QEMU)
    idf_component_get_property(qemu_rgb_lib espressif__esp_lcd_qemu_rgb COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${qemu_rgb_lib})
endif()

# USB host (CONFIG_ARGON_USB_HOST): the OTG peripheral as a host, for a wired
# HID keyboard (usb_hw.c).  Linked conditionally like esp_adc so a build without
# it neither references the component nor puts usb/usb_host.h on the path; the
# Kconfig option depends on SOC_USB_OTG_SUPPORTED, so it is off on chips (and in
# QEMU) that have no host to bring up.
if(CONFIG_ARGON_USB_HOST)
    idf_component_get_property(usb_lib usb COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${usb_lib})
endif()

# USB device (CONFIG_ARGON_USB_MSC): the OTG peripheral as a mass-storage DEVICE
# for the card-reader application (usbd_hw.c).  Bare TinyUSB, not esp_tinyusb -
# see main/idf_component.yml for why.  The `usb` component supplies the PHY API
# (esp_private/usb_phy.h) and the tinyusb component supplies the device stack.
# tinyusb ships no tusb_config.h of its own, so the port owns one
# (idf/tusb/tusb_config.h) and injects it onto both its own compile - usbd_hw.c
# includes tusb.h - and the tinyusb library's, the same mechanism esp_tinyusb
# uses to inject its own.
if(CONFIG_ARGON_USB_MSC)
    idf_component_get_property(usb_phy_lib usb COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${usb_phy_lib})
    idf_component_get_property(tinyusb_lib espressif__tinyusb COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${tinyusb_lib})
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/idf/tusb")
    target_include_directories(${tinyusb_lib} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/idf/tusb")
endif()

# The built-in camera (CONFIG_ARGON_CAMERA_BUILTIN): Espressif's esp32-camera,
# pulled in only when the switch is set (main/idf_component.yml gates the
# dependency on the same Kconfig).  Linking it here also puts esp_camera.h on
# camera_builtin_hw.c's include path; that file compiles to nothing otherwise,
# so the managed component is neither fetched nor referenced in a default build.
if(CONFIG_ARGON_CAMERA_BUILTIN)
    idf_component_get_property(cam_builtin_lib espressif__esp32-camera COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${cam_builtin_lib})
endif()

# mbedTLS: the crypto both TLS and SSH stand on.  esp_tls (the https client in
# tls_hw.c) is only needed for TLS; mbedtls itself (curves, ciphers, hashes -
# used by tls_hw.c and crypto_hw.c) is linked whenever either is on.  Linked
# conditionally so a build with neither pays for none of it; both build under
# QEMU.  esp_crt_bundle, the root-certificate bundle, is part of mbedtls.
if(CONFIG_ARGON_NET_TLS)
    idf_component_get_property(tls_lib esp-tls COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${tls_lib})
endif()
if(CONFIG_ARGON_NET_TLS OR CONFIG_ARGON_NET_SSH)
    idf_component_get_property(mbed_lib mbedtls COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${mbed_lib})
endif()

# Over-the-air update (CONFIG_ARGON_NET_OTA): esp_https_ota streams an image
# into the spare app slot (ota_hw.c).  It pulls in esp_http_client and
# app_update (esp_ota_ops); linked conditionally so a build without OTA carries
# none of it.
if(CONFIG_ARGON_NET_OTA)
    idf_component_get_property(ota_lib esp_https_ota COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${ota_lib})
    idf_component_get_property(httpc_lib esp_http_client COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${httpc_lib})
    idf_component_get_property(appupd_lib app_update COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${appupd_lib})
endif()

# CAN (CONFIG_ARGON_CAN): the TWAI controller driver (can_hw.c).  Linked
# conditionally so a build without CAN neither references it nor puts
# driver/twai.h on the path.
if(CONFIG_ARGON_CAN)
    idf_component_get_property(twai_lib esp_driver_twai COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${twai_lib})
endif()

# RMT (CONFIG_ARGON_RMT): the pulse generator that clocks out a WS2812 chain
# (rmt_hw.c).  Linked conditionally so a build without it neither references the
# driver nor puts driver/rmt_tx.h on the path.
if(CONFIG_ARGON_RMT)
    idf_component_get_property(rmt_lib esp_driver_rmt COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${rmt_lib})
endif()

# MCPWM (CONFIG_ARGON_MCPWM): the motor-control PWM, for a complementary pair
# with dead-time (mcpwm_hw.c).  Linked conditionally like the drivers above.
if(CONFIG_ARGON_MCPWM)
    idf_component_get_property(mcpwm_lib esp_driver_mcpwm COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${mcpwm_lib})
endif()

if(ARGON_PORT_SRCS)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Werror=implicit-function-declaration
        -Wno-unused-parameter)
endif()
