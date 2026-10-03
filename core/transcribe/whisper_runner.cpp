// See whisper_runner.h. The decode settings are below, each with its reason,
// because every one of them departs from a default for something radio does
// that a dictation microphone does not.

#include "core/transcribe/whisper_runner.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>

#include <ggml-backend.h>
#include <ggml.h>
#include <whisper.h>

namespace revenant::transcribe {
namespace {

// WHERE WHISPER AND GGML WRITE, WHICH IS NOWHERE A USER SEES.
//
// Left alone, both print a page of model hyperparameters, buffer sizes and
// device capabilities to stderr on every load, into the console of a headless
// engine and the log of every test. whisper_log_set installs one callback for
// both, process-wide, since ggml has a single logger.
//
// The lines are kept rather than dropped, the last kLogLines of them, because
// a load that fails says why only in this stream: whisper_init returns a null
// pointer and nothing else. load() quotes the tail in its error. They also
// carry the one fact the API does not return, which device whisper.cpp
// actually initialised, and load() reads that back as a check on its own
// choice. REVENANT_WHISPER_LOG=1 echoes everything to stderr as it arrives,
// for a person debugging a load by hand.
//
// A mutex, in a file docs/conventions.md would forbid one in the sample path
// of. This is not that path: the callback runs on the transcription thread
// and on a load, never on a thread that moves samples.
constexpr std::size_t kLogLines = 64;

// whisper_backend_init_gpu's report of the device it took. Kept apart from
// the window above, because a load logs more lines after it than the window
// holds and the check in load() must not depend on the window's size.
constexpr std::string_view kBackendLine = "whisper_backend_init_gpu: ";

struct LogSink {
    std::mutex lock;
    std::deque<std::string> lines;
    std::string partial;
    std::uint64_t sequence = 0;
    std::string backend_line;
    std::uint64_t backend_sequence = 0;
    bool echo = false;
};

LogSink& log_sink()
{
    static LogSink sink;
    return sink;
}

void on_log(ggml_log_level level, const char* text, void*)
{
    if (text == nullptr) {
        return;
    }
    LogSink& sink = log_sink();
    const std::scoped_lock held(sink.lock);
    if (sink.echo) {
        std::fputs(text, stderr);
    }
    // ggml hands over fragments, GGML_LOG_LEVEL_CONT continuing the last, so
    // the line is assembled before it is kept. DEBUG is ggml's per-device
    // capability dump, which is the volume this exists to silence and holds
    // nothing a failure message needs.
    if (level == GGML_LOG_LEVEL_DEBUG) {
        return;
    }
    sink.partial.append(text);
    std::size_t newline = 0;
    while ((newline = sink.partial.find('\n')) != std::string::npos) {
        sink.lines.push_back(sink.partial.substr(0, newline));
        sink.partial.erase(0, newline + 1);
        ++sink.sequence;
        if (sink.lines.back().starts_with(kBackendLine)) {
            sink.backend_line = sink.lines.back();
            sink.backend_sequence = sink.sequence;
        }
        if (sink.lines.size() > kLogLines) {
            sink.lines.pop_front();
        }
    }
}

void install_log_sink()
{
    static std::once_flag once;
    std::call_once(once, [] {
        const char* echo = std::getenv("REVENANT_WHISPER_LOG");
        log_sink().echo = echo != nullptr && std::string_view(echo) == "1";
        whisper_log_set(on_log, nullptr);
    });
}

[[nodiscard]] std::uint64_t log_mark()
{
    LogSink& sink = log_sink();
    const std::scoped_lock held(sink.lock);
    return sink.sequence;
}

// The lines kept since mark, oldest first. Lines that have already scrolled
// out of the window are gone; kLogLines is sized so a single load's warnings
// and errors fit.
[[nodiscard]] std::vector<std::string> log_since(std::uint64_t mark)
{
    LogSink& sink = log_sink();
    const std::scoped_lock held(sink.lock);
    const std::uint64_t fresh = sink.sequence - mark;
    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(fresh, sink.lines.size()));
    return {sink.lines.end() - static_cast<std::ptrdiff_t>(take), sink.lines.end()};
}

// The last backend line logged after mark, or empty.
[[nodiscard]] std::string backend_line_since(std::uint64_t mark)
{
    LogSink& sink = log_sink();
    const std::scoped_lock held(sink.lock);
    return sink.backend_sequence > mark ? sink.backend_line : std::string{};
}

[[nodiscard]] std::string quote_log(const std::vector<std::string>& lines)
{
    std::string out;
    constexpr std::size_t kQuoted = 6;
    const std::size_t from = lines.size() > kQuoted ? lines.size() - kQuoted : 0;
    for (std::size_t i = from; i < lines.size(); ++i) {
        out += "\n  ";
        out += lines[i];
    }
    return out;
}

[[nodiscard]] std::string lowered(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] std::string trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

// One GPU in ggml's registry, with its position among the GPUs, which is the
// number whisper_context_params::gpu_device counts in. whisper.cpp's
// whisper_backend_init_gpu walks ggml_backend_dev_get in order and counts GPU
// and IGPU devices alike, so the ordinal has to be counted the same way.
struct GgmlGpu {
    int ordinal = 0;
    std::string name;
    std::string description;
    bool integrated = false;
};

[[nodiscard]] std::vector<GgmlGpu> ggml_gpus()
{
    std::vector<GgmlGpu> gpus;
    int ordinal = 0;
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t device = ggml_backend_dev_get(i);
        // "enum" because ggml names the type and its getter the same.
        const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU) {
            continue;
        }
        GgmlGpu gpu;
        gpu.ordinal = ordinal++;
        gpu.name = ggml_backend_dev_name(device);
        gpu.description = ggml_backend_dev_description(device);
        gpu.integrated = type == GGML_BACKEND_DEVICE_TYPE_IGPU;
        gpus.push_back(std::move(gpu));
    }
    return gpus;
}

[[nodiscard]] std::string describe(const GgmlGpu& gpu)
{
    return std::format("{}: {} ({})", gpu.name, gpu.description, gpu.integrated ? "integrated" : "discrete");
}

[[nodiscard]] std::string list_gpus(const std::vector<GgmlGpu>& gpus)
{
    if (gpus.empty()) {
        return "ggml sees no Vulkan GPU at all";
    }
    std::string out = "ggml sees";
    for (const GgmlGpu& gpu : gpus) {
        out += "\n  ";
        out += describe(gpu);
    }
    return out;
}

// The choice whisper_runner.h promises: the hint as a case-insensitive
// substring of the description, or with no hint the first discrete device.
// An integrated part is taken only when a hint names it, which is a person
// asking for it, never as a fallback.
[[nodiscard]] Expected<GgmlGpu> choose_gpu(const std::string& hint)
{
    const std::vector<GgmlGpu> gpus = ggml_gpus();
    if (!hint.empty()) {
        const std::string wanted = lowered(hint);
        for (const GgmlGpu& gpu : gpus) {
            if (lowered(gpu.description).find(wanted) != std::string::npos) {
                return gpu;
            }
        }
        return fail(std::format("no Vulkan device ggml can use matches '{}'; {}", hint, list_gpus(gpus)));
    }
    for (const GgmlGpu& gpu : gpus) {
        if (!gpu.integrated) {
            return gpu;
        }
    }
    return fail(std::format(
        "ggml sees no discrete GPU, and an integrated one is not taken unless named, because the "
        "one on the development machine computes wrong results (docs/building.md, \"Choosing a "
        "GPU\"); {}",
        list_gpus(gpus)));
}

}  // namespace

struct Whisper::Impl {
    whisper_context* context = nullptr;
    std::string backend;

    ~Impl()
    {
        if (context != nullptr) {
            whisper_free(context);
        }
    }
};

Whisper::Whisper(std::unique_ptr<Impl> impl, WhisperOptions options)
    : impl_(std::move(impl)), options_(std::move(options))
{
}

Whisper::~Whisper() = default;

std::vector<std::string> whisper_gpu_devices()
{
    install_log_sink();
    std::vector<std::string> out;
    for (const GgmlGpu& gpu : ggml_gpus()) {
        out.push_back(describe(gpu));
    }
    return out;
}

Expected<std::unique_ptr<Whisper>> Whisper::load(const WhisperOptions& options)
{
    install_log_sink();

    std::error_code status;
    if (!std::filesystem::is_regular_file(options.model_path, status)) {
        return fail(std::format("the Whisper model is not at {}", options.model_path.string()));
    }
    if (options.threads < 1) {
        return fail(std::format("Whisper needs at least one CPU thread, and was given {}", options.threads));
    }

    whisper_context_params params = whisper_context_default_params();
    params.use_gpu = options.use_gpu;
    // Flash attention is whisper.cpp 1.8.3's default and stays on: ggml's
    // Vulkan backend implements it, and it is the lower-memory path on a card
    // the radio chain is also using.
    params.flash_attn = true;
    // DTW token timestamps want a per-model alignment-head preset and a
    // separate pass. word_timestamps uses whisper's ordinary token timestamps,
    // which come from the timestamp tokens it already decodes.
    params.dtw_token_timestamps = false;

    std::string backend = "CPU";
    std::string expected_name;
    if (options.use_gpu) {
        auto chosen = choose_gpu(options.gpu_hint);
        if (!chosen) {
            return std::unexpected(with_context(chosen.error(), "choosing the Whisper device"));
        }
        params.gpu_device = chosen->ordinal;
        backend = std::format("{}: {}", chosen->name, chosen->description);
        expected_name = chosen->name;
    }

    const std::uint64_t mark = log_mark();
    // UTF-8, which whisper_init_from_file_with_params widens again under MSVC
    // before opening, so a model under a profile path with a non-ASCII user
    // name opens.
    const std::u8string path8 = options.model_path.u8string();
    const std::string path(path8.begin(), path8.end());

    auto impl = std::make_unique<Impl>();
    impl->context = whisper_init_from_file_with_params(path.c_str(), params);
    const std::vector<std::string> said = log_since(mark);
    if (impl->context == nullptr) {
        return fail(std::format("whisper.cpp could not load {}. What it said last:{}",
                                options.model_path.string(), quote_log(said)));
    }

    // whisper_backend_init_gpu falls back to the CPU without failing, logging
    // "no GPU found", and a 1.6 GB model on the CPU is a few times slower than
    // realtime and takes every thread it is given. That is not a degraded mode
    // worth running in silently beside a radio, so it is a refusal. Its log
    // line naming the backend it initialised is the only report of which
    // device it really took, and it is checked against the choice above.
    if (options.use_gpu) {
        // The same line reports each device it considers, "found GPU device",
        // and then the one it takes, "using Vulkan0 backend", which is the
        // last of them; "no GPU found" ends it when it took none.
        const std::string reported = backend_line_since(mark);
        const std::string wanted = std::format("using {} backend", expected_name);
        if (reported.find(wanted) == std::string::npos) {
            return fail(std::format("whisper.cpp did not initialise {} as asked; it reported \"{}\". "
                                    "What it said last:{}",
                                    backend, reported, quote_log(said)));
        }
    }

    impl->backend = std::move(backend);
    return std::unique_ptr<Whisper>(new Whisper(std::move(impl), options));
}

std::string Whisper::backend_description() const
{
    return impl_->backend;
}

Expected<std::vector<WhisperSegment>> Whisper::transcribe(std::span<const float> pcm16k)
{
    std::vector<WhisperSegment> segments;
    if (pcm16k.empty()) {
        return segments;
    }
    const double seconds = static_cast<double>(pcm16k.size()) / kWhisperSampleRate;
    if (seconds > kWhisperMaxSeconds) {
        return fail(std::format("{:.1f} s of audio is more than one Whisper window of {:.0f} s; "
                                "split it before transcribing",
                                seconds, kWhisperMaxSeconds));
    }
    // A NaN reaches the log-mel transform and comes back as confident text
    // about nothing. Cheap to refuse at 16 kS/s.
    if (!std::ranges::all_of(pcm16k, [](float sample) { return std::isfinite(sample); })) {
        return fail("the audio holds a sample that is not a finite number");
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = options_.threads;

    // Every call starts clean. whisper.cpp by default feeds the previous
    // call's text back in as the prompt for the next, which suits one speaker
    // dictating and is wrong for radio, where consecutive transmissions are
    // unrelated: one hallucinated line over noise becomes the prompt for the
    // next transmission and is repeated into it. OracleX hit exactly this and
    // recovered only by detecting the loop after the fact.
    params.no_context = true;

    // Temperature fallback off. With the default 0.2 step, a decode that fails
    // the entropy or log-probability check is redone at 0.2, 0.4 and so on up
    // to 1.0, six decodes in all, and radio noise fails those checks routinely.
    // That turns the worst audio into the most GPU time and the most invented
    // text. One greedy decode at temperature zero, and the caller rejects on
    // the returned no_speech_prob and avg_logprob instead.
    params.temperature = 0.0f;
    params.temperature_inc = 0.0f;
    params.greedy.best_of = 1;

    // Blank and non-speech tokens suppressed: "[BLANK_AUDIO]", "(music)",
    // "*static*" and the rest of the annotations whisper writes for audio it
    // does not hear as words. suppress_blank is the default; suppress_nst is
    // not, and it is the one that removes them.
    params.suppress_blank = true;
    params.suppress_nst = true;

    params.language = options_.language.empty() ? "en" : options_.language.c_str();
    params.detect_language = false;
    params.translate = false;

    params.initial_prompt = options_.initial_prompt.empty() ? nullptr : options_.initial_prompt.c_str();
    params.carry_initial_prompt = false;

    params.token_timestamps = options_.word_timestamps;

    // Nothing to the console.
    params.print_special = false;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;

    const std::uint64_t mark = log_mark();
    const int rc = whisper_full(impl_->context, params, pcm16k.data(), static_cast<int>(pcm16k.size()));
    if (rc != 0) {
        return fail(std::format("whisper_full failed. What it said last:{}", quote_log(log_since(mark))),
                    rc);
    }

    const whisper_token eot = whisper_token_eot(impl_->context);
    const int n_segments = whisper_full_n_segments(impl_->context);
    segments.reserve(static_cast<std::size_t>(std::max(n_segments, 0)));
    for (int s = 0; s < n_segments; ++s) {
        const char* raw = whisper_full_get_segment_text(impl_->context, s);
        WhisperSegment segment;
        segment.text = trimmed(raw != nullptr ? raw : "");
        // Whisper's times are in units of 10 ms.
        segment.t0 = static_cast<float>(whisper_full_get_segment_t0(impl_->context, s)) / 100.0f;
        segment.t1 = static_cast<float>(whisper_full_get_segment_t1(impl_->context, s)) / 100.0f;
        segment.no_speech_prob = whisper_full_get_segment_no_speech_prob(impl_->context, s);

        double logprob_sum = 0.0;
        int text_tokens = 0;
        WhisperWord* word = nullptr;
        double word_p = 0.0;
        int word_tokens = 0;
        const auto close_word = [&] {
            if (word != nullptr && word_tokens > 0) {
                word->p = static_cast<float>(word_p / word_tokens);
            }
        };

        const int n_tokens = whisper_full_n_tokens(impl_->context, s);
        for (int t = 0; t < n_tokens; ++t) {
            const whisper_token_data data = whisper_full_get_token_data(impl_->context, s, t);
            // Ids from end-of-text upward are the special tokens: timestamps,
            // start of transcript, language, task. None of them is text and
            // none of their probabilities says anything about the words.
            if (data.id >= eot) {
                continue;
            }
            logprob_sum += data.plog;
            ++text_tokens;

            if (!options_.word_timestamps) {
                continue;
            }
            const char* piece_raw = whisper_full_get_token_text(impl_->context, s, t);
            const std::string_view piece = piece_raw != nullptr ? piece_raw : "";
            if (piece.empty()) {
                continue;
            }
            // A byte-pair piece with no leading space continues the word
            // before it, so "un" + "likely" is one word with one timing.
            if (piece.front() == ' ' || word == nullptr) {
                close_word();
                std::string text = trimmed(piece);
                if (text.empty()) {
                    continue;
                }
                segment.words.push_back(WhisperWord{std::move(text), static_cast<float>(data.t0) / 100.0f,
                                                    static_cast<float>(data.t1) / 100.0f, 0.0f});
                word = &segment.words.back();
                word_p = data.p;
                word_tokens = 1;
            } else {
                word->text.append(piece);
                word->t1 = static_cast<float>(data.t1) / 100.0f;
                word_p += data.p;
                ++word_tokens;
            }
        }
        close_word();
        segment.avg_logprob = text_tokens > 0 ? static_cast<float>(logprob_sum / text_tokens) : 0.0f;

        if (!segment.text.empty()) {
            segments.push_back(std::move(segment));
        }
    }
    return segments;
}

}  // namespace revenant::transcribe
