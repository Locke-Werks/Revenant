# The wideband detector.
#
# No GPU, and not because a GPU would be inconvenient here. The detector is
# host code by design, for the reason core/detect/detector.h states: the
# spectrum frame has already crossed the bus before it sees one. Running the
# device to produce these frames would test the channelizer, the FFT kernel
# and the readback path as well, and a failure would then have three possible
# causes.
#
# Two ways of producing a frame, for two different questions.
#
# test_detector.cpp builds each frame bin by bin from a chosen mean, so a case
# asserts against arithmetic: a known centre, a known bandwidth and a known
# SNR in the reference bandwidth, none of them measured.
#
# test_detector_scene.cpp runs a real modulated scene through the channelizer
# and spectrum TWINS, which tests/reference proves bit-exact against the
# kernels. The frames are the ones the device would produce and no device is
# involved. That is the only way to ask what the detector does with interior
# structure, because a rectangle has none.
#
# The end-to-end path, engine to radio to track list, is covered by
# revenant-cli --detect rather than here.

add_executable(revenant_detect_tests
    ../support/no_modal_dialogs.cpp
    scene_frames.cpp
    test_detector.cpp
    test_detector_overlap.cpp
    test_detector_scene.cpp
    test_fold_emitters.cpp
    test_front_end.cpp
    test_groups.cpp
    test_label.cpp
    test_shape.cpp
)

target_link_libraries(revenant_detect_tests PRIVATE
    revenant_core
    Catch2::Catch2WithMain
)
target_include_directories(revenant_detect_tests PRIVATE "${CMAKE_SOURCE_DIR}")

# The voice survey renders tools/siggen/voice.h's scene and runs it through the
# engine, so it needs the GPU and the scene library from tools/siggen. It is
# hidden, so ctest never runs it, and it is built only where the tools are.
# Guarding it drops no case ctest counts: catch_discover_tests lists no hidden
# case either way.
if(TARGET revenant_siggen_scenes)
    target_sources(revenant_detect_tests PRIVATE test_voice_survey.cpp)
    target_link_libraries(revenant_detect_tests PRIVATE revenant_siggen_scenes)
endif()

revenant_apply_warnings(revenant_detect_tests)
revenant_apply_reference_fp(revenant_detect_tests)

catch_discover_tests(revenant_detect_tests
    PROPERTIES
        LABELS "detect"
        TIMEOUT 300
)
