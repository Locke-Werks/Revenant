// Speech to text on a receiver's audio, through whisper.cpp on ggml's Vulkan
// backend.
//
// A THIN WRAPPER, AND THE ONLY FILE THAT KNOWS whisper.h EXISTS. Everything
// above it, the segmenter, the transcription queue, the wire, sees these
// types and nothing of ggml's, so a whisper.cpp upgrade that renames a field
// is a change to whisper_runner.cpp and to nothing else.
//
// VULKAN ONLY, NEVER CUDA. The owner's decision of 2026-10-03: the engine is a
// Vulkan program and stays one, and CUDA's runtime is a set of DLLs the static
// build cannot carry. vcpkg-overlays/ggml/portfile.cmake builds ggml with the
// Vulkan backend and the CPU backend and nothing else.
//
// THE DEVICE IS CHOSEN, NOT LEFT TO GGML. ggml registers every discrete and
// integrated GPU it finds, in Vulkan's enumeration order, and whisper.cpp takes
// "the gpu_device'th GPU" from that list. On the development machine the list
// holds the RTX 4090 and the Ryzen's integrated Radeon, whose driver computes
// wrong results and which docs/building.md "Choosing a GPU" records as dropped
// from support on 2026-09-22. So WhisperOptions::gpu_hint names the device by
// a substring of its name, the same string gpu::DeviceInfo::name carries for
// the device the engine opened, and load() refuses rather than guesses when
// nothing matches. backend_description() then says which device is running it.
//
// DESTROY IT BEFORE main RETURNS. ggml holds its Vulkan instance and devices
// in function-local statics created during the first load, and static
// destruction at exit tears those down before any static that was created
// earlier, so a Whisper held in a static and freed at exit reaches into a
// destroyed backend and faults. tests/transcribe/test_whisper.cpp met exactly
// that; whoever owns the Whisper releases it on the way out.
//
// ONE CALLER AT A TIME. whisper_full is not reentrant on one context, and this
// class does not lock: the transcription queue above it is the one consumer,
// which is the arrangement docs/conventions.md asks for rather than a mutex
// every caller has to know about.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/error.h"

namespace revenant::transcribe {

// Whisper's input rate. Fixed by the model: the log-mel front end is defined
// at 16 kHz and whisper_full assumes it without being told.
inline constexpr std::uint32_t kWhisperSampleRate = 16'000;

// The longest input one call takes. Whisper's encoder sees a 30 s window, and
// whisper_full on more than that walks the window forward and stitches, which
// is the place a hallucination from one window seeds the next. A receiver's
// transmission longer than this is the segmenter's to split, so it is refused
// here rather than quietly stitched.
inline constexpr double kWhisperMaxSeconds = 30.0;

struct WhisperOptions {
    std::filesystem::path model_path;

    bool use_gpu = true;

    // A substring of the Vulkan device name to run on, matched against
    // ggml's device descriptions, which are the driver's deviceName: the
    // engine's own gpu::DeviceInfo::name is the intended value. Empty takes
    // ggml's first discrete GPU, never an integrated one.
    std::string gpu_hint;

    // An ISO 639-1 code, or "auto" for whisper's own detection.
    std::string language = "en";

    // CPU threads for the parts that stay on the host: the log-mel transform
    // and whatever the CPU backend is handed.
    int threads = 4;

    // Biases the decoder's vocabulary on every call, call signs and place
    // names. It is NOT a carried context: each call starts from it afresh,
    // see no_context in whisper_runner.cpp.
    std::string initial_prompt;

    // Per-word timings and probabilities, from whisper's token timestamps.
    bool word_timestamps = false;
};

struct WhisperWord {
    std::string text;
    float t0 = 0;
    float t1 = 0;
    // The word's tokens' mean probability, 0 to 1.
    float p = 0;
};

struct WhisperSegment {
    std::string text;
    // Seconds from the start of the input.
    float t0 = 0;
    float t1 = 0;
    // Whisper's own estimate that the window held no speech, 0 to 1, taken
    // from the no-speech token at the first decoding step.
    float no_speech_prob = 0;
    // The mean natural log-probability of the segment's text tokens, timestamp
    // and special tokens excluded. Near zero is a confident decode; radio
    // noise decoded into words tends to sit below -1, which is the threshold
    // OpenAI's own transcribe.py uses beside no_speech_prob.
    float avg_logprob = 0;
    std::vector<WhisperWord> words;
};

class Whisper {
public:
    // Loads the model onto the chosen device. Seconds for the 1.6 GB default
    // model, so call it off any thread with a deadline.
    [[nodiscard]] static Expected<std::unique_ptr<Whisper>> load(const WhisperOptions& options);

    ~Whisper();
    Whisper(const Whisper&) = delete;
    Whisper& operator=(const Whisper&) = delete;
    Whisper(Whisper&&) = delete;
    Whisper& operator=(Whisper&&) = delete;

    // 16000 S/s mono float in [-1, 1], at most kWhisperMaxSeconds. Empty input
    // is an empty result. One caller at a time; the caller serialises.
    [[nodiscard]] Expected<std::vector<WhisperSegment>> transcribe(std::span<const float> pcm16k);

    // Which device actually runs it, as "Vulkan0: NVIDIA GeForce RTX 4090",
    // or "CPU" when use_gpu was off.
    [[nodiscard]] std::string backend_description() const;

    [[nodiscard]] const WhisperOptions& options() const { return options_; }

private:
    struct Impl;
    explicit Whisper(std::unique_ptr<Impl> impl, WhisperOptions options);

    std::unique_ptr<Impl> impl_;
    WhisperOptions options_;
};

// Every GPU ggml can see, as "Vulkan0: NVIDIA GeForce RTX 4090 (discrete)",
// one per entry, in ggml's order. For a diagnostic, and for the refusal load()
// writes when a hint matches nothing.
[[nodiscard]] std::vector<std::string> whisper_gpu_devices();

}  // namespace revenant::transcribe
