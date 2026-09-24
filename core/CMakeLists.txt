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

# characterise/ is the waveform characteriser: what a signal IS, without
# decoding it. It reads complex baseband rather than the power spectrum,
# because symbol rate, modulation order, tone structure and frame period are
# not in a magnitude spectrum at all, and it is a pure library over a span of
# samples with no engine seam of its own yet. core/characterise/characterise.h
# says why that is deliberate and what wiring it later will cost.
#
# The decoders. rds_bits.cpp is the RDS physical layer, subcarrier to bits;
# rds_groups.cpp is the block and group layer above it. imbe.cpp is the P25
# Phase 1 voice codec, decode side, channel bits to 8 kHz PCM.
#
# dv_phy.cpp, dv_codes.cpp, p25p1.cpp, dstar.cpp and tetra.cpp are the three
# digital voice physical layers and the machinery under them. dv_phy.cpp is
# the part with no standard in it: matched filtering, symbol timing recovery
# and sync correlation, which all three share and none of them owns.
# dv_codes.cpp is the error control codes the three standards specify, each
# citing its clause. The three mode files then contain their standards and
# little else, which is what makes them checkable against those standards.
#
# D-STAR and TETRA stop at the bits. P25 goes through to audio: p25p1.cpp
# cuts the IMBE frames out of each LDU and hands them to imbe.cpp. D-STAR's
# voice is AMBE with no published algorithm, and TETRA's is ACELP and
# unimplemented. docs/modes.md has the scope and the reasoning.
#
# dmr.cpp and dmr_codes.cpp are the fourth, DMR's framing and metadata from
# ETSI TS 102 361-1 and -2, with its Annex B codes in a file of their own
# because they are more than the other three standards' together. It shares
# dv_phy.cpp's discriminator and filter and not its symbol timing, because a
# TDMA burst carries its own; core/decode/dmr.h says why. Its voice is
# AMBE+2 and stops at the bits like D-STAR's.
#
# WHAT THE END OF THE PARAGRAPH ABOVE USED TO SAY: "including why DMR is
# absent from a list naming its three neighbours". DMR was excluded on
# 2026-09-21 over a live patent and put back in scope by the owner on
# 2026-09-23; docs/modes.md has both.
#
# vocoder.cpp is the voice-codec seam every implementation writes against. It
# carries no codec and not one frame size; core/decode/vocoder.h says why a
# constant here would have no citation to give. vocoder_plugin.cpp is the
# loader that turns a third-party DLL into one of those implementations, and
# decode/vocoder_abi.h is the C contract such a DLL implements.
#
# Both were behind an EXISTS guard while they landed on separate branches in
# the same session. Both have landed, and the guard is gone rather than left
# in place: it was configure-time only, so a file that went missing or got
# renamed would drop a whole decoder out of the library and configure clean,
# and the first sign of it would be a link error somewhere unrelated or,
# worse, nothing at all. A missing source is now a broken build, which is
# what it is.
# The machine-wide device lock, source/device_lock.h. Its own small library
# rather than a file in revenant_core so a tool that opens hardware without the
# engine, tools/devicespike, takes the same lock without linking Vulkan and the
# rest of the engine to get it. advapi32 carries the security descriptor calls.
add_library(revenant_device_lock STATIC
    source/device_lock.cpp
)
target_include_directories(revenant_device_lock PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(revenant_device_lock PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(revenant_device_lock PRIVATE advapi32)
endif()
revenant_apply_warnings(revenant_device_lock)

add_library(revenant_core STATIC
    characterise/catalogue.cpp
    characterise/characterise.cpp
    characterise/cyclostationary.cpp
    characterise/periodicity.cpp
    characterise/tones.cpp
    characterise/transform.cpp
    decode/aprs.cpp
    decode/ax25.cpp
    decode/cw.cpp
    decode/dmr.cpp
    decode/dmr_codes.cpp
    decode/dstar.cpp
    decode/dv_codes.cpp
    decode/dv_phy.cpp
    decode/fsk.cpp
    decode/imbe.cpp
    decode/navtex.cpp
    decode/m17.cpp
    decode/p25p1.cpp
    decode/pocsag.cpp
    decode/psk31.cpp
    decode/rds_bits.cpp
    decode/rds_groups.cpp
    decode/rtty.cpp
    decode/sitor_b.cpp
    decode/tetra.cpp
    decode/tone_frontend.cpp
    decode/varicode.cpp
    decode/vocoder.cpp
    decode/vocoder_plugin.cpp
    detect/detector.cpp
    detect/front_end.cpp
    detect/groups.cpp
    detect/label.cpp
    detect/tier_two.cpp
    identify/identify.cpp
    dsp/complex_ops.cpp
    dsp/convert.cpp
    dsp/denormal_mode.cpp
    dsp/front_end_correction.cpp
    dsp/front_end_correction_reference.cpp
    dsp/noise_reference.cpp
    dsp/pfb_branch_reference.cpp
    dsp/pfb_design.cpp
    dsp/pfb_fft_reference.cpp
    dsp/spectrum_levels_reference.cpp
    dsp/spectrum_reference.cpp
    dsp/vrx_reference.cpp
    dsp/synth/channel.cpp
    dsp/synth/cw_mod.cpp
    dsp/synth/dmr_mod.cpp
    dsp/synth/dv_mod.cpp
    dsp/synth/fsk_mod.cpp
    dsp/synth/m17_mod.cpp
    dsp/synth/modulators.cpp
    dsp/synth/psk31_mod.cpp
    dsp/synth/rds_mod.cpp
    dsp/synth/wfm_mod.cpp
    dsp/synth/wideband.cpp
    engine/audio_egress.cpp
    engine/audio_wasapi.cpp
    engine/device_ring.cpp
    engine/engine.cpp
    engine/graph.cpp
    engine/noise_stage.cpp
    engine/probe.cpp
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
    source/calibration.cpp
    source/capabilities.cpp
    source/clock_model.cpp
    source/corrected_source.cpp
    source/file_source.cpp
    source/registry.cpp
    source/rtlsdr_source.cpp
    source/synthetic_source.cpp
    thread_role.cpp
)

target_include_directories(revenant_core PUBLIC "${CMAKE_SOURCE_DIR}")

target_link_libraries(revenant_core PUBLIC
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
    Threads::Threads
    revenant_device_lock
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
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_cs24_cf32.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vrx_fine.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vrx_demod.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vrx_carrier.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/iq_moments.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/iq_correct.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/noise_blank.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/noise_line.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/noise_spectral.comp"
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
