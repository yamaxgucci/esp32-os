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
             # xtensa: the exception table fault_hw.c installs into
             xtensa
             # the serial ports everything talks over before there is anything else
             esp_driver_uart
             # flash partitions, and the two filesystems a board is born with
             esp_partition fatfs wear_levelling vfs sdmmc esp_driver_sdmmc
             esp_driver_sdspi esp_driver_spi joltwallet__littlefs
             # pins and buses straight through to the chip.  esp_adc is
             # deliberately absent - see ARGON_ENABLE_ADC in argon_kernel.
             esp_driver_gpio esp_driver_i2c esp_driver_ledc
             # the virtual RGB panel under Espressif QEMU (absent on hardware)
             esp_lcd espressif__esp_lcd_qemu_rgb
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

# The built-in camera (CONFIG_ARGON_CAMERA_BUILTIN): Espressif's esp32-camera,
# pulled in only when the switch is set (main/idf_component.yml gates the
# dependency on the same Kconfig).  Linking it here also puts esp_camera.h on
# camera_builtin_hw.c's include path; that file compiles to nothing otherwise,
# so the managed component is neither fetched nor referenced in a default build.
if(CONFIG_ARGON_CAMERA_BUILTIN)
    idf_component_get_property(cam_builtin_lib espressif__esp32-camera COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${cam_builtin_lib})
endif()

# TLS client (CONFIG_ARGON_NET_TLS): esp_tls + mbedtls, for https in wget
# (tls_hw.c).  esp_crt_bundle (the root-certificate bundle) is part of the
# mbedtls component.  Linked conditionally so a build without TLS neither pulls
# mbedTLS's RAM nor puts its headers on the path; both build fine under QEMU.
if(CONFIG_ARGON_NET_TLS)
    idf_component_get_property(tls_lib esp-tls COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${tls_lib})
    idf_component_get_property(mbed_lib mbedtls COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${mbed_lib})
endif()

if(ARGON_PORT_SRCS)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Werror=implicit-function-declaration
        -Wno-unused-parameter)
endif()
