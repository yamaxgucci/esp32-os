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
    "${ARGON_ROOT}/components/argon_kernel/src/loader/probe.c"
    "${ARGON_ROOT}/components/argon_kernel/src/net/netmsg.c")

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
    "${ARGON_ROOT}/apps/common/ir/ag_fft.c"
    "${ARGON_ROOT}/apps/common/ckt/ag_mathf.c"
    "${ARGON_ROOT}/apps/common/ckt/ag_ckt.c"
    "${ARGON_ROOT}/apps/common/ckt/ag_stage.c"
    # The benchmark's netlists: the host tests check that they are circuits
    # before the guest bench reports what they cost.
    "${ARGON_ROOT}/apps/cktbench/ckt_circuits.c")

set(ARGON_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}"
    "${ARGON_ROOT}/sdk/include"
    "${ARGON_ROOT}/components/argon_kernel/include"
    # The port contract, answered by the `bare` port.  These sources are the
    # ones that need nothing from the machine, so the only headers they reach
    # for are the ones `bare` already implements in full; anything that needed
    # a scheduler or an allocator would stop here with a readable #error rather
    # than by failing to link.
    "${ARGON_ROOT}/components/argon_port/include"
    "${ARGON_ROOT}/components/argon_port/bare"
    "${ARGON_ROOT}/apps/cc"
    "${ARGON_ROOT}/apps/common/dsp"
    "${ARGON_ROOT}/apps/common/synth"
    "${ARGON_ROOT}/apps/common/smp"
    "${ARGON_ROOT}/apps/common/grain"
    "${ARGON_ROOT}/apps/common/ckt"
    "${ARGON_ROOT}/apps/cktbench")

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
    test_ckt.c
    test_netmsg.c
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

# The circuit solver, rendered so it can be heard - including the same preamp
# with and without oversampling, which is the one difference no peak or RMS
# measurement in test_ckt.c can show.
add_executable(ckt_smoke
    "${ARGON_ROOT}/tools/ckt_smoke.c"
    ${ARGON_DSP_SRCS})

# Runs a real recording through the amplifier model and the reverb.  Takes a
# wav on the command line, so it is a tool rather than a fixed render.
add_executable(ckt_play
    "${ARGON_ROOT}/tools/ckt_play.c"
    ${ARGON_DSP_SRCS})

# Measures the convolution engine against the same convolution in double
# precision.  A tool rather than a test because it needs a recording, but the
# number it prints is the one that says whether the IR is clean.
add_executable(ir_check
    "${ARGON_ROOT}/tools/ir_check.c"
    ${ARGON_DSP_SRCS})
# Add AG_IR_TRACE=1 here to make ag_ir print each block's working scales.  Off
# by default because it is pages of output; on when a number in ir_check moves
# and the question is which shift moved it.

# The same amplifier with every compromise removed: double precision, no
# tables, no antialiasing trick, Newton to 1e-12, long filters.  Slow on
# purpose - it exists to say whether an artefact belongs to the circuit or to
# the arithmetic that runs it cheaply.
add_executable(ckt_exact "${ARGON_ROOT}/tools/ckt_exact.c")
if(NOT MSVC)
    target_link_libraries(ckt_exact PRIVATE m)
endif()

# The NAM captures are 48 kHz and everything else here is 44.1.  A resampler
# that coloured the top octave would ruin a test about a high-frequency buzz,
# so this one is a ten-thousand-tap polyphase sinc.
add_executable(wavrate "${ARGON_ROOT}/tools/wavrate.c")
if(NOT MSVC)
    target_link_libraries(wavrate PRIVATE m)
endif()

# Two renders, one knob apart: says whether a change is audible before anyone
# has to listen to it.
add_executable(wavdiff "${ARGON_ROOT}/tools/wavdiff.c")
target_include_directories(wavdiff PRIVATE ${ARGON_INCLUDES})
if(NOT MSVC)
    target_link_libraries(wavdiff PRIVATE m)
endif()

foreach(smoke engine_smoke synth_smoke ckt_smoke ckt_play ir_check)
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

foreach(target argon_tests vtdump engine_smoke synth_smoke ckt_smoke ckt_play
        ir_check wavdiff wavrate ckt_exact)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wno-unused-parameter)
    endif()
endforeach()

add_test(NAME argon_tests COMMAND argon_tests)
