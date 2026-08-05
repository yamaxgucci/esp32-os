# Host-side unit tests for the parts of the kernel that are pure logic.
#
#   cmake -S host-tests -B build-host
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# No ESP-IDF and no hardware required.
cmake_minimum_required(VERSION 3.16)
project(argon_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(ARGON_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

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
    "${ARGON_ROOT}/components/argon_kernel/src/core/path.c"
    "${ARGON_ROOT}/components/argon_kernel/src/core/cfg.c"
    "${ARGON_ROOT}/components/argon_kernel/src/core/cmdline.c"
    "${ARGON_ROOT}/components/argon_kernel/src/shell/lineedit.c"
    "${ARGON_ROOT}/components/argon_kernel/src/fs/vfs.c"
    "${ARGON_ROOT}/components/argon_kernel/src/fs/ramfs.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/screen.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/vtout.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/vtin.c")

target_include_directories(argon_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${ARGON_ROOT}/sdk/include"
    "${ARGON_ROOT}/components/argon_kernel/include")

if(MSVC)
    target_compile_definitions(argon_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(argon_tests PRIVATE /W4)
else()
    target_compile_options(argon_tests PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
endif()

add_test(NAME argon_tests COMMAND argon_tests)
