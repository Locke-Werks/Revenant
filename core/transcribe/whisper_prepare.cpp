// The production Prepare: the model store and whisper.cpp behind the
// transcriber's Recogniser seam.
//
// The glue only. What the model is and how it is fetched and pinned is
// model_store.h's; what runs it is whisper_runner.h's; when it is asked for
// and what is done with its output is transcriber.h's.

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/transcribe/model_store.h"
#include "core/transcribe/transcriber.h"
#include "core/transcribe/whisper_runner.h"

namespace revenant::transcribe {

namespace {

class WhisperRecogniser final : public Recogniser {
public:
    explicit WhisperRecogniser(std::unique_ptr<Whisper> whisper) : whisper_(std::move(whisper)) {}

    Expected<std::vector<RecognisedSegment>> recognise(std::span<const float> pcm16k) override
    {
        auto segments = whisper_->transcribe(pcm16k);
        if (!segments) {
            return std::unexpected(with_context(std::move(segments.error()), "whisper"));
        }
        std::vector<RecognisedSegment> out;
        out.reserve(segments->size());
        for (WhisperSegment& segment : *segments) {
            out.push_back(RecognisedSegment{
                std::move(segment.text),
                segment.no_speech_prob,
                segment.avg_logprob,
            });
        }
        return out;
    }

    std::string backend() const override { return whisper_->backend_description(); }

private:
    std::unique_ptr<Whisper> whisper_;
};

}  // namespace

Prepare whisper_prepare(WhisperOptions options)
{
    return [options = std::move(options)](const PrepareReport& report,
                                          const std::atomic<bool>& cancel)
               -> Expected<std::unique_ptr<Recogniser>> {
        const std::uint64_t size = kDefaultModel.size;

        // model_ready hashes a file it has no current sidecar for, which is
        // seconds on 1.6 GB, so this is reported as verifying.
        report(ModelState::Verifying, 0, size);
        auto ready = model_ready(kDefaultModel);
        if (!ready) {
            return std::unexpected(with_context(std::move(ready.error()), "checking the speech model"));
        }
        if (cancel.load(std::memory_order_relaxed)) {
            return fail("cancelled before the speech model was fetched");
        }

        if (!*ready) {
            report(ModelState::Downloading, 0, size);
            // download_model hashes as the bytes arrive, so once the last of
            // them is in, what remains is the comparison and the rename:
            // verifying, not downloading.
            const auto progress = [&report, &cancel](const DownloadProgress& at) {
                const bool complete = at.total != 0 && at.done >= at.total;
                report(complete ? ModelState::Verifying : ModelState::Downloading, at.done, at.total);
                return !cancel.load(std::memory_order_relaxed);
            };
            if (auto fetched = download_model(kDefaultModel, progress); !fetched) {
                return std::unexpected(
                    with_context(std::move(fetched.error()), "downloading the speech model"));
            }
        }
        if (cancel.load(std::memory_order_relaxed)) {
            return fail("cancelled before the speech model was loaded");
        }

        // Not cancellable: whisper_init_from_file has no hook for it. Seconds,
        // which the destructor waits out.
        report(ModelState::Loading, size, size);
        WhisperOptions load_options = options;
        load_options.model_path = model_path(kDefaultModel);
        auto whisper = Whisper::load(load_options);
        if (!whisper) {
            return std::unexpected(with_context(std::move(whisper.error()), "loading the speech model"));
        }
        return std::make_unique<WhisperRecogniser>(std::move(*whisper));
    };
}

}  // namespace revenant::transcribe
