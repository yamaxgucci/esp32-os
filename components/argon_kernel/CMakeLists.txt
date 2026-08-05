set(ARGON_KERNEL_SRCS
    "src/boot/kernel.c"
    "src/boot/platform.c")

idf_component_register(
    SRCS ${ARGON_KERNEL_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES argon_abi
    PRIV_REQUIRES esp_psram esp_timer esp_partition nvs_flash spi_flash
                  esp_system esp_hw_support)

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

target_compile_definitions(${COMPONENT_LIB}
    PRIVATE ARGON_BUILD_ID="${ARGON_BUILD_ID}")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror=implicit-function-declaration
    -Wno-unused-parameter)
