set(ARGON_KERNEL_SRCS
    "src/boot/kernel.c"
    "src/boot/platform.c"
    "src/boot/board.c"
    "src/core/path.c"
    "src/core/cfg.c"
    "src/core/cmdline.c"
    "src/core/sysconfig.c"
    "src/core/journal.c"
    "src/core/log.c"
    "src/core/reslist.c"
    "src/core/arena.c"
    "src/console/codepage.c"
    "src/console/screen.c"
    "src/console/vtout.c"
    "src/console/vtin.c"
    "src/console/console.c"
    "src/console/uart_console.c"
    "src/dev/device.c"
    "src/dev/devfs.c"
    "src/dev/devices.c"
    "src/dev/ioclaim.c"
    "src/dev/io.c"
    "src/fs/vfs.c"
    "src/fs/ramfs.c"
    "src/fs/idfvfs.c"
    "src/fs/storage.c"
    "src/loader/axeload.c"
    "src/loader/loader.c"
    "src/loader/api.c"
    "src/proc/proc.c"
    "src/proc/threads.c"
    "src/proc/fault.c"
    "src/proc/supervisor.c"
    "src/shell/lineedit.c"
    "src/shell/shell.c"
    "src/shell/cmd_fs.c"
    # The file manager is part of the system, not something to be copied onto a
    # board before it is useful.  Same sources as the .AXE in apps/fm, built with
    # AG_BUILTIN: see apps/fm/fm.h.
    "../../apps/fm/fm.c"
    "../../apps/fm/fmops.c")

idf_component_register(
    SRCS ${ARGON_KERNEL_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES argon_abi
    PRIV_REQUIRES esp_psram esp_timer esp_partition nvs_flash spi_flash
                  esp_system esp_hw_support esp_driver_uart log
                  fatfs wear_levelling vfs sdmmc esp_driver_sdmmc
                  esp_driver_sdspi esp_driver_spi esp_driver_gpio
                  # api->io: buses and pins straight through to the chip.
                  # esp_adc is deliberately absent - see ARGON_ENABLE_ADC.
                  esp_driver_i2c esp_driver_ledc
                  # xtensa: the exception table the fault handler installs into
                  xtensa)

# Analogue input, off by default: linking esp_adc runs a constructor that stops
# the boot under QEMU, and QEMU is how this system is verified.  See Kconfig.
if(CONFIG_ARGON_ENABLE_ADC)
    idf_component_get_property(adc_lib esp_adc COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${adc_lib})
endif()

# Stamp the build with a git description when one is available.
find_package(Git QUIET)
set(ARGON_BUILD_ID "dev")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE ARGON_GIT_DESC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(ARGON_GIT_DESC)
        set(ARGON_BUILD_ID "${ARGON_GIT_DESC}")
    endif()
endif()

# Set ARGON_BOOT_TRACE=1 in the environment to bring up a new board: early boot
# messages then go straight to the UART, and ESP-IDF log output stays there too
# instead of being routed through the screen.
if(DEFINED ENV{ARGON_BOOT_TRACE})
    set(ARGON_TRACE_DEF AG_BOOT_TRACE=$ENV{ARGON_BOOT_TRACE})
else()
    set(ARGON_TRACE_DEF AG_BOOT_TRACE=0)
endif()

target_compile_definitions(${COMPONENT_LIB}
    PRIVATE ${ARGON_TRACE_DEF} ARGON_BUILD_ID="${ARGON_BUILD_ID}"
            # For the sources shared with apps/: inside the kernel there is a
            # real C library, and this code is a built-in rather than an image.
            AG_BUILTIN=1 AG_HAVE_LIBC=1)

# The manager's own header sits next to its sources.
target_include_directories(${COMPONENT_LIB}
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../apps/fm")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror=implicit-function-declaration
    -Wno-unused-parameter)
