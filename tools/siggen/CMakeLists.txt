# The signal generator command line.
#
# The synthesis itself lives in core/dsp/synth now: the engine opens a
# synthetic scene as an ordinary Source, so the modulators, the wideband
# synthesizer and the channel simulator are engine components rather than
# tooling, and core cannot depend on tools. This is the front end over them.

# The scenes siggen writes that are built out of core/dsp/synth rather than
# being part of it: the labelled scene, the voice scene, the speech that
# feeds both, and the lift that puts emitters rendered at their own rate onto
# one wideband capture. A library rather than siggen's own sources so the
# voice survey in tests/detect renders exactly the scene `siggen voice`
# writes, not a copy of it.
add_library(revenant_siggen_scenes STATIC labelled.cpp lift.cpp speech.cpp voice.cpp)
target_link_libraries(revenant_siggen_scenes PUBLIC revenant_core)
target_include_directories(revenant_siggen_scenes PUBLIC "${CMAKE_SOURCE_DIR}")
revenant_apply_warnings(revenant_siggen_scenes)

add_executable(siggen main.cpp)

# revenant_bench for the bench trials: `siggen rtty` and the rest write a trial
# through tools/bench's own generators, so the file is what the sweep fed its
# decoder rather than a second copy of the transmitter settings that could
# drift from it. Defined after this directory; CMake resolves it at generate.
target_link_libraries(siggen PRIVATE revenant_core revenant_bench revenant_siggen_scenes)
revenant_apply_warnings(siggen)
revenant_embed_version(siggen DESCRIPTION "Revenant signal generator")
