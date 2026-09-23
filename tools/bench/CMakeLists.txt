# The BER/SNR harness: a library so tests can drive a sweep directly, and a
# thin executable so CI can call it.

add_library(revenant_bench STATIC
    sweep.cpp
    curve.cpp
    throughput.cpp
    rds_subject.cpp
    mode_subjects.cpp
    fsk_subjects.cpp
    tone_subjects.cpp
    dv_subjects.cpp
)
target_link_libraries(revenant_bench PUBLIC revenant_core)
target_include_directories(revenant_bench PUBLIC ${CMAKE_SOURCE_DIR})
revenant_apply_warnings(revenant_bench)

add_executable(bench main.cpp)
target_link_libraries(bench PRIVATE revenant_bench)
revenant_apply_warnings(bench)
revenant_embed_version(bench DESCRIPTION "Revenant BER/SNR harness")

# The reference twins are in revenant_core, but the harness computes theory
# curves and impairment power of its own, and a sweep whose numbers shift with
# the optimiser is not a baseline anything can regress against.
revenant_apply_reference_fp(revenant_bench)
