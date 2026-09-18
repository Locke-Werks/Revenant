# The signal generator and the channel simulator.
#
# A library so tests and the BER harness can drive it directly, and a thin CLI
# so a person can produce a capture by hand.
#
# This is also the wideband source Revenant is actually developed against. The
# only radio available sustains about 2.4 MS/s of 8-bit IQ; the channelizer,
# the fifty-VRX exit criterion and the 40 MB/s capture path all assume roughly
# twenty megahertz. The generator is what covers that gap, so it is a first
# class part of the build rather than a test fixture.

add_library(revenant_siggen STATIC
    modulators.cpp
    wideband.cpp
    channel.cpp
)

target_link_libraries(revenant_siggen PUBLIC revenant_core)
target_include_directories(revenant_siggen PUBLIC "${CMAKE_SOURCE_DIR}")

revenant_apply_warnings(revenant_siggen)

# Generated signals and calibrated impairments are the reference the decoders
# are scored against. A curve that shifts because the optimiser reassociated
# the noise generator is a curve that cannot be regressed against.
revenant_apply_reference_fp(revenant_siggen)

add_executable(siggen main.cpp)
target_link_libraries(siggen PRIVATE revenant_siggen)
revenant_apply_warnings(siggen)
revenant_embed_version(siggen DESCRIPTION "Revenant signal generator")
