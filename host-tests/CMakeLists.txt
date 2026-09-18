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
    "${ARGON_ROOT}/components/argon_kernel/src/core/power.c"
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
    "${ARGON_ROOT}/apps/cktbench/ckt_circuits.c"
    # The one-dimensional valve model: static curves baked from the same solver,
    # with the linear network moved out into sections around them.
    "${ARGON_ROOT}/apps/common/tube/ag_tube.c"
    "${ARGON_ROOT}/apps/common/tube/ag_biq.c"
    "${ARGON_ROOT}/apps/common/tube/ag_os.c"
    "${ARGON_ROOT}/apps/common/tube/ag_tone.c"
    "${ARGON_ROOT}/apps/common/tube/ag_amp.c")

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
    "${ARGON_ROOT}/apps/common/tube"
    "${ARGON_ROOT}/apps/common/pixband"
    "${ARGON_ROOT}/apps/common/ws"
    "${ARGON_ROOT}/apps/cktbench")

# The NAM player has to agree with the reference implementation to the last bit -
# host tests check exactly that - so wherever it is compiled in, the compiler is
# told not to contract a multiply and an add into one rounding step.  On x86-64
# without FMA there is nothing to contract and this changes nothing; on a machine
# where there is, it is the difference between a passing test and a mystery.
if(NOT MSVC)
    set_source_files_properties("${ARGON_ROOT}/tools/nam.c" PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off")
endif()

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
    test_power.c
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
    test_tube.c
    test_nam.c
    test_netmsg.c
    test_rempix.c
    test_desktop.c
    test_fsops.c
    "${ARGON_ROOT}/apps/desktop/dsk_rect.c"
    "${ARGON_ROOT}/apps/desktop/dsk_layout.c"
    "${ARGON_ROOT}/apps/desktop/dsk_paint.c"
    "${ARGON_ROOT}/apps/desktop/dsk_wm.c"
    "${ARGON_ROOT}/apps/desktop/dsk_kbd.c"
    "${ARGON_ROOT}/apps/desktop/dsk_menu.c"
    "${ARGON_ROOT}/apps/desktop/dsk_dlg.c"
    "${ARGON_ROOT}/apps/common/fsops/fsops.c"
    test_rlink.c
    "${ARGON_ROOT}/apps/common/rlink/ag_rlink.c"
    test_atproto.c
    "${ARGON_ROOT}/apps/common/atproto/ag_atproto.c"
    # The link to a phone.  Unlike test_rempix.c, which mirrors REMDISP's codec
    # in a copy because a .SYS for Xtensa will not link here, these two are the
    # shipped files: what the test exercises is what PHONE.SYS runs.
    test_phonelink.c
    "${ARGON_ROOT}/apps/common/pixband/ag_pixband.c"
    "${ARGON_ROOT}/apps/common/ws/ag_ws.c"
    test_espldr.c
    "${ARGON_ROOT}/apps/common/esp8266/ag_espldr.c"
    test_stm32ldr.c
    "${ARGON_ROOT}/apps/common/stm32/ag_stm32ldr.c"
    "${ARGON_ROOT}/tools/nam.c"
    "${ARGON_ROOT}/apps/cc/cc_compile.c"
    "${ARGON_ROOT}/components/argon_kernel/src/dev/draw.c"
    ${ARGON_DSP_SRCS}
    ${ARGON_PORTABLE_SRCS})

# host_stub first, for the same reason as the smoke tools below: ag_ir asks
# the kernel for memory, and there is no kernel here.
target_include_directories(argon_tests BEFORE PRIVATE
    "${ARGON_ROOT}/tools/host_stub")
target_include_directories(argon_tests PRIVATE
    "${ARGON_ROOT}/tools"
    ${ARGON_INCLUDES}
    "${ARGON_ROOT}/apps/common/ir"
    "${ARGON_ROOT}/apps/common")
if(NOT MSVC)
    target_link_libraries(argon_tests PRIVATE m)
endif()

# Compiled in rather than guessed: the working directory differs between running
# the executable directly and running it through ctest.
target_compile_definitions(argon_tests PRIVATE
    AG_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures"
    AG_TREE_ROOT="${ARGON_ROOT}"
    AG_HOST=1)

# Renders a raw serial transcript into the screen it produces, using the
# kernel's own screen module.
add_executable(vtdump
    "${ARGON_ROOT}/tools/vtdump/vtdump.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/codepage.c"
    "${ARGON_ROOT}/components/argon_kernel/src/console/screen.c")

target_include_directories(vtdump PRIVATE ${ARGON_INCLUDES})

# A picture encoded by the real encoder, for tools/phonepage.py to replay at a
# browser.  The page's three decoders are the half of the phone link that the
# two-board rig cannot judge, and writing the encoder a second time in Python
# to test them would be a second thing to be wrong - see ag_pixband.h.
add_executable(pixband_sample
    "${ARGON_ROOT}/tools/pixband_sample.c"
    "${ARGON_ROOT}/apps/common/pixband/ag_pixband.c")

target_include_directories(pixband_sample PRIVATE
    "${ARGON_ROOT}/apps/common/pixband")

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
add_executable(wavrate
    "${ARGON_ROOT}/tools/wavrate.c"
    "${ARGON_ROOT}/tools/wavrate_core.c")
target_include_directories(wavrate PRIVATE "${ARGON_ROOT}/tools")
if(NOT MSVC)
    target_link_libraries(wavrate PRIVATE m)
endif()

# The one-dimensional valve model: prints the baked curves, the three filter
# responses and what oversampling is worth, and renders a take through it.
# It also plays the NAM captures itself now - the reference a fit is measured
# against is made in memory rather than by three passes through files.
add_executable(tube_render
    "${ARGON_ROOT}/tools/tube_render.c"
    "${ARGON_ROOT}/tools/nam.c"
    "${ARGON_ROOT}/tools/wavrate_core.c"
    "${ARGON_ROOT}/apps/common/tube/ag_tube.c"
    "${ARGON_ROOT}/apps/common/tube/ag_biq.c"
    "${ARGON_ROOT}/apps/common/tube/ag_os.c"
    "${ARGON_ROOT}/apps/common/tube/ag_tone.c"
    "${ARGON_ROOT}/apps/common/tube/ag_amp.c"
    "${ARGON_ROOT}/apps/common/ckt/ag_ckt.c"
    "${ARGON_ROOT}/apps/common/ckt/ag_mathf.c"
    # The cabinet, because the chain ends there and the int16 boundary in front
    # of it is a real place for a level to go wrong.
    "${ARGON_ROOT}/apps/common/ir/ag_ir.c"
    "${ARGON_ROOT}/apps/common/ir/ag_fft.c")
# host_stub first: ag_ir asks the kernel for memory and there is no kernel here.
target_include_directories(tube_render BEFORE PRIVATE
    "${ARGON_ROOT}/tools/host_stub")
target_include_directories(tube_render PRIVATE ${ARGON_INCLUDES}
    "${ARGON_ROOT}/tools"
    "${ARGON_ROOT}/apps/common/ir")
if(NOT MSVC)
    target_link_libraries(tube_render PRIVATE m)
endif()

# The same chain, but played at the sound card in a loop with the knobs live.
# Windows only, because it talks to winmm; everything it drives is portable.
if(WIN32)
    add_executable(tube_live
        "${ARGON_ROOT}/tools/tube_live.c"
        # The capture, for the live reference: see g_ref_src in tube_live.c.
        "${ARGON_ROOT}/tools/nam.c"
        "${ARGON_ROOT}/apps/common/tube/ag_tube.c"
        "${ARGON_ROOT}/apps/common/tube/ag_biq.c"
        "${ARGON_ROOT}/apps/common/tube/ag_os.c"
        "${ARGON_ROOT}/apps/common/tube/ag_tone.c"
        "${ARGON_ROOT}/apps/common/tube/ag_amp.c"
        "${ARGON_ROOT}/apps/common/ckt/ag_ckt.c"
        "${ARGON_ROOT}/apps/common/ckt/ag_mathf.c"
        "${ARGON_ROOT}/apps/common/ir/ag_ir.c"
        "${ARGON_ROOT}/apps/common/ir/ag_fft.c")
    target_include_directories(tube_live BEFORE PRIVATE
        "${ARGON_ROOT}/tools/host_stub")
    target_include_directories(tube_live PRIVATE ${ARGON_INCLUDES}
        "${ARGON_ROOT}/apps/common/ir"
        "${ARGON_ROOT}/tools")
    # winmm for the old path, ole32/uuid for WASAPI - see the note above wa_open.
    target_link_libraries(tube_live PRIVATE winmm ole32 uuid avrt)
    # Optimised even though nothing else here is: the float cabinet is a direct
    # 4410-tap convolution, about 100 M multiplies a second, and it has to keep
    # up with the sound card rather than with a progress bar.  The tool prints
    # its measured cost at startup, so this is checked rather than hoped.
    if(MSVC)
        target_compile_definitions(tube_live PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(tube_live PRIVATE /W4 /O2)
    else()
        target_link_libraries(tube_live PRIVATE m)
        target_compile_options(tube_live PRIVATE
            -O2 -Wall -Wextra -Wno-unused-parameter)
    endif()
endif()

# The NAM/TONE3000 captures every fitted voicing is measured against, played in
# this tree rather than by a Go binary in a temporary directory.  Optimised for
# the same reason tube_live is: it is 10 000 multiplies a sample and a take is
# eight seconds long.
add_executable(nam_run
    "${ARGON_ROOT}/tools/nam_run.c"
    "${ARGON_ROOT}/tools/nam.c")
target_include_directories(nam_run PRIVATE "${ARGON_ROOT}/tools")
if(MSVC)
    target_compile_definitions(nam_run PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(nam_run PRIVATE /W4 /O2)
else()
    target_link_libraries(nam_run PRIVATE m)
    # No fast math and no FMA contraction, on purpose: this has to agree with a
    # reference implementation to the last bit, and both of those change the
    # order and the width of the arithmetic.  See the note in tools/nam.h.
    target_compile_options(nam_run PRIVATE
        -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter)
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

foreach(target argon_tests vtdump pixband_sample engine_smoke synth_smoke
        ckt_smoke ckt_play ir_check wavdiff wavrate ckt_exact tube_render)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wno-unused-parameter)
    endif()
endforeach()

add_test(NAME argon_tests COMMAND argon_tests)
