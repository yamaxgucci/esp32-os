set(ARGON_KERNEL_SRCS
    "src/boot/kernel.c"
    "src/boot/platform.c"
    "src/core/path.c"
    "src/core/cfg.c"
    "src/core/cmdline.c"
    "src/console/screen.c"
    "src/console/vtout.c"
    "src/console/vtin.c"
    "src/console/console.c"
    "src/console/uart_console.c"
    "src/fs/vfs.c"
    "src/fs/ramfs.c"
    "src/fs/idfvfs.c"
    "src/fs/storage.c"
    "src/shell/lineedit.c"
    "src/shell/shell.c"
    "src/shell/cmd_fs.c")

idf_component_register(
    SRCS ${ARGON_KERNEL_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES argon_abi
    PRIV_REQUIRES esp_psram esp_timer esp_partition nvs_flash spi_flash
                  esp_system esp_hw_support esp_driver_uart log
                  fatfs wear_levelling vfs sdmmc esp_driver_sdmmc
                  esp_driver_sdspi)

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
    PRIVATE ${ARGON_TRACE_DEF} ARGON_BUILD_ID="${ARGON_BUILD_ID}")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror=implicit-function-declaration
    -Wno-unused-parameter)
