# The engine. No Qt anywhere in this tree, enforced by the headless preset and
# by the source scan in the root CMakeLists.
#
# dsp/synth holds the modulators, the wideband scene synthesizer and the
# channel simulator. They began under tools/siggen, as the handoff document's
# layout has them, and moved here when the synthetic source made them an engine
# component rather than a test instrument: core must not depend on tools, and
# the engine now opens a synthetic scene as an ordinary Source. tools/siggen is
# still the command-line tool, now a thin front end over these.

find_package(Threads REQUIRED)

# librtlsdr, GPL-2.0-or-later, which is why Revenant is GPL-3.0-or-later. See
# docs/clean-room.md: clean-room is still the default and this is the recorded
# exception, taken because the RTL2832U's datasheet specifies the USB
# transport and no raw IQ mode at all, so there was no document to implement
# against. docs/rtlsdr-provenance.md has the finding.
find_package(rtlsdr CONFIG REQUIRED)

# The decoders. rds_bits.cpp is the RDS physical layer, subcarrier to bits;
# rds_groups.cpp is the block and group layer above it and is landing on a
# separate branch in the same session.
#
# The EXISTS guard is there so the two branches build independently and merge
# without either one having to edit the other's line. tests/CMakeLists.txt
# already uses the same guard on rpc/ for the same reason. Once both have
# landed the guard can go, and a missing file would then be a broken build
# rather than a silently absent decoder, which is the right end state.
set(REVENANT_DECODE_SOURCES
    decode/rds_bits.cpp
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/decode/rds_groups.cpp")
    list(APPEND REVENANT_DECODE_SOURCES decode/rds_groups.cpp)
endif()

add_library(revenant_core STATIC
    ${REVENANT_DECODE_SOURCES}
    detect/detector.cpp
    dsp/complex_ops.cpp
    dsp/convert.cpp
    dsp/denormal_mode.cpp
    dsp/pfb_branch_reference.cpp
    dsp/pfb_design.cpp
    dsp/pfb_fft_reference.cpp
    dsp/spectrum_levels_reference.cpp
    dsp/spectrum_reference.cpp
    dsp/vrx_reference.cpp
    dsp/synth/channel.cpp
    dsp/synth/modulators.cpp
    dsp/synth/rds_mod.cpp
    dsp/synth/wideband.cpp
    engine/audio_egress.cpp
    engine/audio_wasapi.cpp
    engine/device_ring.cpp
    engine/engine.cpp
    engine/graph.cpp
    engine/scheduler.cpp
    engine/spectrum_scale.cpp
    engine/vrx_place.cpp
    engine/vrx_stage.cpp
    engine/wav_writer.cpp
    gpu/buffer.cpp
    gpu/context.cpp
    gpu/kernel.cpp
    gpu/shaders.cpp
    gpu/vma_impl.cpp
    source/capabilities.cpp
    source/clock_model.cpp
    source/file_source.cpp
    source/registry.cpp
    source/rtlsdr_source.cpp
    source/synthetic_source.cpp
)

target_include_directories(revenant_core PUBLIC "${CMAKE_SOURCE_DIR}")

target_link_libraries(revenant_core PUBLIC
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

# WIN32 only, because the Linux and macOS audio backends arrive later and
# engine/audio_wasapi.cpp compiles to a "not supported on this platform"
# failure there rather than dropping out of the build. ole32 carries COM and
# the PROPVARIANT helpers; avrt carries MMCSS, which is what gets the render
# thread scheduled as Pro Audio.
if(WIN32)
    target_link_libraries(revenant_core PRIVATE ole32 avrt)
endif()

# PRIVATE: rtl-sdr.h is included by source/rtlsdr_source.cpp and by nothing
# else, so nothing that links the engine has to see it. The generator
# expression is what the port's own usage file prescribes, because a static
# triplet exports rtlsdr::rtlsdr_static and a shared one exports
# rtlsdr::rtlsdr.
target_link_libraries(revenant_core PRIVATE
    $<IF:$<TARGET_EXISTS:rtlsdr::rtlsdr>,rtlsdr::rtlsdr,rtlsdr::rtlsdr_static>
)

revenant_add_shaders(revenant_core
    TARGET_ENV vulkan1.3
    SHADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/identity.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cmul.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pfb_branch.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pfb_fft.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spectrum.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spectrum_levels.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_cu8_cf32.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_cs8_cf32.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_cs16_cf32.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vrx_fine.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vrx_demod.comp"
)

revenant_apply_warnings(revenant_core)

# Applied to the whole engine, not only to the reference twins that strictly
# need it. An SDR engine has no business being built with fast math: a
# reassociated filter is a different filter. Making it explicit here means the
# setting is a decision on the record rather than a compiler default nobody
# checked.
revenant_apply_reference_fp(revenant_core)

# The wire. Two libraries, and the comment in rpc/CMakeLists.txt says why.
add_subdirectory(rpc)
