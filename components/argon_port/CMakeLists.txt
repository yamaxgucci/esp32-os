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
file(GLOB ARGON_PORT_SRCS "${CMAKE_CURRENT_LIST_DIR}/${ARGON_PORT}/src/*.c")

# The exception table fault_hw.c installs into is the one thing here that is
# named after the instruction set rather than after the chip.  `xtensa` is a
# component on the Xtensa parts and does not exist at all on the RISC-V ones
# (C3, C6, H2, P4), where naming it fails the configure rather than the link.
# RISC-V has no equivalent table, so fault_hw.c compiles there to a port that
# answers -AG_ENOTSUP - which argon/port/fault.h allows in so many words.
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    set(ARGON_ARCH_REQUIRES xtensa)
else()
    set(ARGON_ARCH_REQUIRES "")
endif()

idf_component_register(
    SRCS ${ARGON_PORT_SRCS}
    INCLUDE_DIRS "include" "${ARGON_PORT}"
    # Public, not private: the port headers are inline and pull these in
    # wherever the kernel includes them.
    REQUIRES argon_abi heap esp_timer freertos log esp_system esp_hw_support
             esp_common esp_psram
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
             ${ARGON_ARCH_REQUIRES}
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

if(ARGON_PORT_SRCS)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Werror=implicit-function-declaration
        -Wno-unused-parameter)
endif()
