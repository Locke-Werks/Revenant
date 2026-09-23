# The engine spine: the device ring's host logic and the source abstraction.
#
# test_engine.cpp does need a GPU, and takes the shared fixture from
# tests/reference so that REVENANT_GPU_INDEX and REVENANT_REQUIRE_GPU mean the
# same thing in every binary the conformance matrix runs.
#
# test_audio_wasapi.cpp needs no GPU and no sound card. The one case that does
# touch an endpoint skips itself when there is none, and feeds silence when
# there is one, so the suite stays runnable on a headless box and stays quiet
# on a desk.
#
# No GPU is needed for the ring or the source cases. Most of the risk in a ring is in the sizing, the
# retirement floor and the wrap arithmetic, and most of the risk in a source is
# in the URI layer, the seek and the flow control. All of that is host logic
# and all of it fails silently when it fails.
#
# test_rtlsdr_source.cpp needs no GPU either, and needs a dongle for only some
# of its cases. The URI cases run everywhere; the ones that touch hardware
# skip themselves with a reason when librtlsdr sees no device, so this binary
# stays green on a build machine with nothing plugged in without pretending it
# covered the radio.
#
# test_engine_passband.cpp is the per-receiver transform, which needs a GPU
# for the same reasons and one more: the passband runs two passes of the
# spectrum kernel and reassembles them, so its correctness is in a copy
# region as much as in any arithmetic. Its last case needs no device and is
# in this file rather than in tests/reference because it is refereeing the
# graph's reassembly rule rather than a kernel.
#
# test_engine_spectrum.cpp is the spectrum path's half of test_engine.cpp and
# needs a GPU for the same reason: the transform is on the device and the
# frame comes back through a readback buffer, so nothing about the axis, the
# window gate or the sink can be dispatched without one. It is a separate file
# rather than more cases in test_engine.cpp because it needs a different
# engine configuration, a small block so the window gate is exercised at all,
# and because the two would otherwise share a fixture that suited neither.

# test_audio_egress.cpp needs neither either. What it covers is the failure
# paths nothing else reaches: a backend that returns an error and a backend
# that throws. The second used to end the process, because the drain thread is
# a std::thread and an exception out of a caller-supplied write() reaches the
# top of it, so that case is a crash rather than an assertion when it
# regresses.

# test_signal_meter.cpp needs no GPU either. The meter used to live inside
# Graph::complete behind a readback, where the only way to check it was to
# run a receiver on a device; core/engine/signal_meter.h exists so the
# divisor a buffer of interleaved floats takes is assertable on its own.

# test_vrx_tune.cpp is click-to-tune: which demodulator a measured signal
# gets, and what happens when the grid cannot carry the receiver that choice
# implies. Most of it is pure and needs no device, because the decision rule
# and the grid arithmetic are integer functions. Its last two cases do need
# one, because "a receiver that can actually hear it" is a claim about a
# receiver that is running rather than about a placement struct.

# test_vrx_reanchor.cpp needs a GPU and no radio. It drives one DemodStage
# directly rather than an engine, because the only engine-level way to a
# receiver whose inputs were overwritten is a dongle retune: the synthetic
# scene declares no gap and a Demand file source blocks rather than dropping.
# The stage holds the three cursors the branch is arithmetic over, so handing
# it a first_block that has jumped past its ring reaches the branch on a build
# machine.

# test_engine_dv.cpp is the three digital voice modes through the engine. It
# needs a GPU because the receiver under test is a fine stage on the device,
# and one of its cases does not: which truncated channels place() refuses is
# integer arithmetic over the grid.

# test_audio_fanout.cpp needs neither a GPU nor a sound card. AudioFanout is
# the one part of core/engine/engine.h that is pure host code, and the wire
# cases in tests/rpc rest on it, so its ordering and its failure semantics are
# refereed here where a failure names the property rather than a subscription
# that went quiet.

# test_engine_probe.cpp is tier two through the engine: the probe pool's
# receivers on emitters of known modulation, and on an empty slot. It needs a
# GPU because a probe is a fine stage on the device. It borrows
# tests/characterise/signal_lab.cpp for the OFDM burst, which nothing in
# core/dsp/synth renders, rather than keeping a second copy of it here.

# test_engine_retune.cpp is a front-end retune through the real engine with no
# radio. It needs a GPU because the receivers it moves are fine stages on the
# device. The source is a synthetic scene wrapped so that tune() moves its
# centre label, handed over through engine::open_built_source, because every
# backend that runs without hardware refuses a retune and is right to.

# test_channel_ring.cpp needs a GPU. It is the channel ring's depth against a
# receiver's display tap with frames in flight counted: one case builds the
# stage directly, as test_vrx_reanchor.cpp does, and one runs a stationary tone
# through the engine with no spectrum stage to make the ring larger than the
# tap alone asks for.

add_executable(revenant_engine_tests
    ../support/no_modal_dialogs.cpp
    ../reference/gpu_fixture.cpp
    ../characterise/signal_lab.cpp
    test_audio_egress.cpp
    test_audio_fanout.cpp
    test_audio_wasapi.cpp
    test_channel_ring.cpp
    test_engine.cpp
    test_engine_dv.cpp
    test_engine_passband.cpp
    test_engine_probe.cpp
    test_engine_retune.cpp
    test_engine_spectrum.cpp
    test_ring.cpp
    test_rtlsdr_source.cpp
    test_signal_meter.cpp
    test_sources.cpp
    test_spectrum_scale.cpp
    test_vrx_reanchor.cpp
    test_vrx_tune.cpp
)

target_link_libraries(revenant_engine_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)
target_include_directories(revenant_engine_tests PRIVATE "${CMAKE_SOURCE_DIR}")

revenant_apply_warnings(revenant_engine_tests)
revenant_apply_reference_fp(revenant_engine_tests)

catch_discover_tests(revenant_engine_tests
    PROPERTIES
        LABELS "engine"
        TIMEOUT 300
)
