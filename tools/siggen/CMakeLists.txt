# The signal generator command line.
#
# The synthesis itself lives in core/dsp/synth now: the engine opens a
# synthetic scene as an ordinary Source, so the modulators, the wideband
# synthesizer and the channel simulator are engine components rather than
# tooling, and core cannot depend on tools. This is the front end over them.

add_executable(siggen main.cpp)
target_link_libraries(siggen PRIVATE revenant_core)
revenant_apply_warnings(siggen)
revenant_embed_version(siggen DESCRIPTION "Revenant signal generator")
