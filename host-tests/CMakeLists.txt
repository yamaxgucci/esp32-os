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
    "${ARGON_ROOT}/components/argon_kernel/src/console/codepage.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/screen.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/vtout.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/vtin.c"
    "${ARGON_ROOT}/components/argon_kernel/src/shell/lineedit.c"
    "${ARGON_ROOT}/components/argon_kernel/src/shell/shell_path.c"
    "${ARGON_ROOT}/components/argon_kernel/src/fs/vfs.c"
    "${ARGON_ROOT}/components/argon_kernel/src/fs/ramfs.c"
    "${ARGON_ROOT}/components/argon_kernel/src/dev/device.c"
    "${ARGON_ROOT}/components/argon_kernel/src/dev/devfs.c"
    "${ARGON_ROOT}/components/argon_kernel/src/dev/ioclaim.c"
    "${ARGON_ROOT}/components/argon_kernel/src/core/journal.c"
    "${ARGON_ROOT}/components/argon_kernel/src/core/reslist.c"
    "${ARGON_ROOT}/components/argon_kernel/src/core/arena.c"
    "${ARGON_ROOT}/components/argon_kernel/src/loader/axeload.c"
    "${ARGON_ROOT}/components/argon_kernel/src/loader/axesig.c"
    "${ARGON_ROOT}/components/argon_kernel/src/loader/probe.c")

# The shared audio DSP layer is userspace, not kernel, but it is plain
# fixed-point C with no device I/O - so it runs on the host, and it is the
# part of the tree where a wrong constant is silent until somebody listens.
set(ARGON_DSP_SRCS
    "${ARGON_ROOT}/apps/common/dsp/ag_dsp.c"
    "${ARGON_ROOT}/apps/common/synth/ag_osc.c"
    "${ARGON_ROOT}/apps/common/synth/ag_filt.c"
    "${ARGON_ROOT}/apps/common/synth/ag_dist.c"
    "${ARGON_ROOT}/apps/common/synth/ag_fmx.c"
    "${ARGON_ROOT}/apps/common/synth/ag_synth.c"
    "${ARGON_ROOT}/apps/common/smp/ag_smp.c"
    "${ARGON_ROOT}/apps/common/grain/ag_grain.c"
    "${ARGON_ROOT}/apps/common/ir/ag_ir.c"
    "${ARGON_ROOT}/apps/common/ir/ag_fft.c")

set(ARGON_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}"
    "${ARGON_ROOT}/sdk/include"
    "${ARGON_ROOT}/components/argon_kernel/include"
    "${ARGON_ROOT}/apps/cc"
    "${ARGON_ROOT}/apps/common/dsp"
    "${ARGON_ROOT}/apps/common/synth"
    "${ARGON_ROOT}/apps/common/smp"
    "${ARGON_ROOT}/apps/common/grain")

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
    test_shell_path.c
    test_vfs.c
    test_device.c
    test_ioclaim.c
    test_journal.c
    test_reslist.c
    test_arena.c
    test_axeload.c
    test_axesig.c
    test_codepage.c
    test_probe.c
    test_cc.c
    test_draw.c
    test_dsp.c
    "${ARGON_ROOT}/apps/cc/cc_compile.c"
    "${ARGON_ROOT}/components/argon_kernel/src/dev/draw.c"
    ${ARGON_DSP_SRCS}
    ${ARGON_PORTABLE_SRCS})

# host_stub first, for the same reason as the smoke tools below: ag_ir asks
# the kernel for memory, and there is no kernel here.
target_include_directories(argon_tests BEFORE PRIVATE
    "${ARGON_ROOT}/tools/host_stub")
target_include_directories(argon_tests PRIVATE
    ${ARGON_INCLUDES}
    "${ARGON_ROOT}/apps/common/ir")
if(NOT MSVC)
    target_link_libraries(argon_tests PRIVATE m)
endif()

# Compiled in rather than guessed: the working directory differs between running
# the executable directly and running it through ctest.
target_compile_definitions(argon_tests PRIVATE
    AG_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures"
    AG_HOST=1)

# Renders a raw serial transcript into the screen it produces, using the
# kernel's own screen module.
add_executable(vtdump
    "${ARGON_ROOT}/tools/vtdump/vtdump.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/codepage.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/screen.c")

target_include_directories(vtdump PRIVATE ${ARGON_INCLUDES})

# Listening files.  Not tests - ears catch things numbers do not - but they
# are built here so the engines they render cannot quietly stop compiling.
# Run build-host/engine_smoke to write build/listen/*.wav.
add_executable(engine_smoke
    "${ARGON_ROOT}/tools/engine_smoke.c"
    "${ARGON_ROOT}/apps/common/dx7/ag_dx7.c"
    "${ARGON_ROOT}/apps/common/fm/ag_fm.c"
    ${ARGON_DSP_SRCS})

add_executable(synth_smoke
    "${ARGON_ROOT}/tools/synth_smoke.c"
    ${ARGON_DSP_SRCS})

foreach(smoke engine_smoke synth_smoke)
    # host_stub first: it supplies the ag_malloc/ag_free that ag_ir and ag_fx
    # would otherwise take from the real syscall table, which has no kernel
    # behind it here.
    target_include_directories(${smoke} BEFORE PRIVATE
        "${ARGON_ROOT}/tools/host_stub")
    target_include_directories(${smoke} PRIVATE
        ${ARGON_INCLUDES}
        "${ARGON_ROOT}/apps/common/dx7"
        "${ARGON_ROOT}/apps/common/fm"
        "${ARGON_ROOT}/apps/common/fx"
        "${ARGON_ROOT}/apps/common/ir")
endforeach()

foreach(target argon_tests vtdump engine_smoke synth_smoke)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wno-unused-parameter)
    endif()
endforeach()

add_test(NAME argon_tests COMMAND argon_tests)
