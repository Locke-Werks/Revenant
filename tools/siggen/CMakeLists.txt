# The signal generator command line.
#
# The synthesis itself lives in core/dsp/synth now: the engine opens a
# synthetic scene as an ordinary Source, so the modulators, the wideband
# synthesizer and the channel simulator are engine components rather than
# tooling, and core cannot depend on tools. This is the front end over them.

add_executable(siggen main.cpp)

# revenant_bench for the bench trials: `siggen rtty` and the rest write a trial
# through tools/bench's own generators, so the file is what the sweep fed its
# decoder rather than a second copy of the transmitter settings that could
# drift from it. Defined after this directory; CMake resolves it at generate.
target_link_libraries(siggen PRIVATE revenant_core revenant_bench)
revenant_apply_warnings(siggen)
revenant_embed_version(siggen DESCRIPTION "Revenant signal generator")
