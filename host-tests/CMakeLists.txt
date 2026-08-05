# Host-side build of the parts of the kernel that are pure logic: the unit
# tests, and the vtdump tool.
#
#   cmake -S host-tests -B build-host -G Ninja
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# No ESP-IDF and no hardware required.  Use `argon tests` to do all of that.
cmake_minimum_required(VERSION 3.16)
project(argon_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(ARGON_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# Everything in the kernel that has no dependency on the chip.  These build for
# the host and the target from the same sources.
set(ARGON_PORTABLE_SRCS
    "${ARGON_ROOT}/components/argon_kernel/src/core/path.c"
    "${ARGON_ROOT}/components/argon_kernel/src/core/cfg.c"
    "${ARGON_ROOT}/components/argon_kernel/src/core/cmdline.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/screen.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/vtout.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/vtin.c"
    "${ARGON_ROOT}/components/argon_kernel/src/shell/lineedit.c"
    "${ARGON_ROOT}/components/argon_kernel/src/fs/vfs.c"
    "${ARGON_ROOT}/components/argon_kernel/src/fs/ramfs.c"
    "${ARGON_ROOT}/components/argon_kernel/src/core/journal.c")

set(ARGON_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}"
    "${ARGON_ROOT}/sdk/include"
    "${ARGON_ROOT}/components/argon_kernel/include")

# Predictable paths for the scripts that invoke these.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(argon_tests
    main.c
    test_path.c
    test_cfg.c
    test_screen.c
    test_vtout.c
    test_vtin.c
    test_cmdline.c
    test_lineedit.c
    test_vfs.c
    test_journal.c
    ${ARGON_PORTABLE_SRCS})

target_include_directories(argon_tests PRIVATE ${ARGON_INCLUDES})

# Renders a raw serial transcript into the screen it produces, using the
# kernel's own screen module.
add_executable(vtdump
    "${ARGON_ROOT}/tools/vtdump/vtdump.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/screen.c")

target_include_directories(vtdump PRIVATE ${ARGON_INCLUDES})

foreach(target argon_tests vtdump)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wno-unused-parameter)
    endif()
endforeach()

add_test(NAME argon_tests COMMAND argon_tests)
