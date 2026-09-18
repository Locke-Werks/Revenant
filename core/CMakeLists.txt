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

add_library(revenant_core STATIC
    dsp/complex_ops.cpp
    dsp/convert.cpp
    dsp/denormal_mode.cpp
    dsp/pfb_branch_reference.cpp
    dsp/pfb_design.cpp
    dsp/pfb_fft_reference.cpp
    dsp/vrx_reference.cpp
    dsp/synth/channel.cpp
    dsp/synth/modulators.cpp
    dsp/synth/wideband.cpp
    engine/audio_egress.cpp
    engine/device_ring.cpp
    engine/engine.cpp
    engine/graph.cpp
    engine/scheduler.cpp
    engine/vrx_place.cpp
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
    source/synthetic_source.cpp
)

target_include_directories(revenant_core PUBLIC "${CMAKE_SOURCE_DIR}")

target_link_libraries(revenant_core PUBLIC
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
)

revenant_add_shaders(revenant_core
    TARGET_ENV vulkan1.3
    SHADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/identity.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cmul.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pfb_branch.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/pfb_fft.comp"
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
