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
             # OpenEth + lwIP sockets (QEMU today)
             esp_eth esp_netif esp_event nvs_flash lwip)

# Analogue input, off by default: linking esp_adc runs a constructor that stops
# the boot under QEMU, and QEMU is how this system is verified.
if(CONFIG_ARGON_ENABLE_ADC)
    idf_component_get_property(adc_lib esp_adc COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${adc_lib})
endif()

if(ARGON_PORT_SRCS)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Werror=implicit-function-declaration
        -Wno-unused-parameter)
endif()
