# The signal generator: modulators, the wideband scene synthesizer, and the
# channel simulator, plus the CLI that drives them.
#
# This is a test instrument, not part of the sample path, so it is built with
# the reference floating point settings rather than for speed. See
# cmake/CompilerFlags.cmake: contraction is off so the generator produces the
# same bits on every host compiler, which is the whole point of generating a
# signal whose ground truth is known.

# wideband.cpp renders a block across several workers. MSVC needs nothing for
# that, but a GCC or Clang build fails at link without -pthread, and
# CompilerFlags.cmake already contemplates both.
find_package(Threads REQUIRED)

add_library(revenant_siggen STATIC
    modulators.cpp
    wideband.cpp
    channel.cpp
)

target_link_libraries(revenant_siggen PUBLIC revenant_core Threads::Threads)
target_include_directories(revenant_siggen PUBLIC ${CMAKE_SOURCE_DIR})
revenant_apply_warnings(revenant_siggen)
revenant_apply_reference_fp(revenant_siggen)

add_executable(siggen main.cpp)
target_link_libraries(siggen PRIVATE revenant_siggen)
revenant_apply_warnings(siggen)
revenant_embed_version(siggen DESCRIPTION "Revenant signal generator")
