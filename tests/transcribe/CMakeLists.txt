# Speech to text: core/transcribe.
#
# test_model_store.cpp needs no network and no GPU. It runs the size and hash
# logic over a three-byte "model" in a scratch directory, and the download's
# failure paths against loopback. Its one hidden case, [.download], fetches the
# real 1.6 GB model and is run by name.
#
# test_whisper.cpp needs the model and a GPU, and SKIPS saying so when the
# model is not on the machine; REVENANT_REQUIRE_WHISPER=1 makes that a failure.
# Its speech is made at test time by Windows' SAPI, spoken_clip.cpp, because
# the tree holds no audio; that file says why. It takes the engine's device
# through tests/reference/gpu_fixture.cpp so REVENANT_GPU_INDEX and
# REVENANT_REQUIRE_GPU mean what they mean in every other binary. Its hidden
# case, [.speed], is the latency measurement.
#
# The segmenter, resampler and queue cases are added below as their files
# land, under the same target.

add_executable(revenant_transcribe_tests
    ../support/no_modal_dialogs.cpp
    ../reference/gpu_fixture.cpp
    spoken_clip.cpp
    test_model_store.cpp
    test_whisper.cpp
)

foreach(source IN ITEMS test_segmenter.cpp test_resample.cpp test_transcriber.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        target_sources(revenant_transcribe_tests PRIVATE ${source})
    endif()
endforeach()

target_link_libraries(revenant_transcribe_tests PRIVATE
    revenant_transcribe
    Catch2::Catch2WithMain
)
target_include_directories(revenant_transcribe_tests PRIVATE "${CMAKE_SOURCE_DIR}")

# SAPI for the synthetic speech.
if(WIN32)
    target_link_libraries(revenant_transcribe_tests PRIVATE sapi ole32)
endif()

revenant_apply_warnings(revenant_transcribe_tests)

# Plain discovery, not revenant_discover_tests: nothing here opens the dongle,
# and that function's second pass lists only [dongle] cases, which here is an
# empty list. Catch2 exits non-zero on a test spec that matches nothing, so
# the pass fails and ctest is left holding a revenant_transcribe_tests_NOT_BUILT
# placeholder that reports as a failure on every run. Measured 2026-10-03.
catch_discover_tests(revenant_transcribe_tests
    PROPERTIES
        LABELS "transcribe"
        TIMEOUT 300
)
