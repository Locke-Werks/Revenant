// Kept in its own translation unit: the Media Foundation headers include
// <windows.h>, and nothing that includes http.h may see that first.

#include "tools/webgate/audio_hls.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

namespace revenant::webgate {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kTargetBytesPerSecond = 8000;  // 64 kbit/s
// HEAACWAVEINFO's fixed fields precede the AudioSpecificConfig in
// MF_MT_USER_DATA.
constexpr UINT32 kHeaacInfoBytes = 12;

[[nodiscard]] std::string hr_text(const char* what, HRESULT hr) {
    return std::format("{}: HRESULT 0x{:08X}", what, static_cast<unsigned>(hr));
}

// Sampling frequency index from ISO 14496-3 table 1.16, for the two rates the
// encoder offers.
[[nodiscard]] int frequency_index(std::uint32_t rate) {
    switch (rate) {
        case 48000: return 3;
        case 44100: return 4;
        default: return -1;
    }
}

}  // namespace

struct AacEncoder::Impl {
    ~Impl() {
        mft.Reset();
        if (mf_started) {
            MFShutdown();
        }
    }

    [[nodiscard]] Status init(std::uint32_t rate) {
        sample_rate = rate;
        const int index = frequency_index(rate);
        if (index < 0) {
            return fail(std::format("the AAC encoder does not offer {} Hz", rate));
        }

        // Left initialised for the life of the thread: balancing it would mean
        // uninitialising on whichever thread destroys the encoder, which need
        // not be this one. A second call on a thread already in an apartment
        // is harmless, and a different apartment type is fine for this
        // in-process MFT.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
            return fail(hr_text("CoInitializeEx", com));
        }
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            return fail(hr_text("MFStartup", hr));
        }
        mf_started = true;

        hr = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&mft));
        if (FAILED(hr)) {
            return fail(hr_text("creating the AAC encoder", hr));
        }

        // Output first: the AAC encoder only lists the PCM inputs it accepts
        // once it knows what it is producing.
        ComPtr<IMFMediaType> chosen;
        std::uint32_t best_distance = UINT32_MAX;
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> candidate;
            if (mft->GetOutputAvailableType(0, i, &candidate) != S_OK) {
                break;
            }
            UINT32 candidate_rate = 0;
            UINT32 channels = 0;
            UINT32 bytes = 0;
            candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &candidate_rate);
            candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            candidate->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytes);
            if (candidate_rate != rate || channels != 1) {
                continue;
            }
            const std::uint32_t distance =
                bytes > kTargetBytesPerSecond ? bytes - kTargetBytesPerSecond
                                              : kTargetBytesPerSecond - bytes;
            if (distance < best_distance) {
                best_distance = distance;
                chosen = candidate;
            }
        }
        if (!chosen) {
            return fail(std::format("the AAC encoder offers no mono output at {} Hz", rate));
        }
        ComPtr<IMFMediaType> output;
        hr = MFCreateMediaType(&output);
        if (SUCCEEDED(hr)) {
            hr = chosen->CopyAllItems(output.Get());
        }
        if (SUCCEEDED(hr)) {
            // Raw access units, not ADTS: an mp4 sample is one bare frame.
            hr = output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        }
        if (SUCCEEDED(hr)) {
            hr = mft->SetOutputType(0, output.Get(), 0);
        }
        if (FAILED(hr)) {
            return fail(hr_text("setting the AAC output type", hr));
        }

        // The MFT takes 16-bit PCM only; push() converts from float.
        ComPtr<IMFMediaType> input;
        hr = MFCreateMediaType(&input);
        if (SUCCEEDED(hr)) {
            input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
            input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
            input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 2);
            hr = mft->SetInputType(0, input.Get(), 0);
        }
        if (FAILED(hr)) {
            return fail(hr_text("setting the PCM input type", hr));
        }

        // AAC-LC: object type 2 in five bits, frequency index in four, channel
        // configuration in four, then three zero flag bits. The encoder's own
        // value, when it reports one, wins.
        config = {static_cast<std::uint8_t>((2 << 3) | (index >> 1)),
                  static_cast<std::uint8_t>(((index & 1) << 7) | (1 << 3))};
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(mft->GetOutputCurrentType(0, &current))) {
            UINT32 size = 0;
            if (SUCCEEDED(current->GetBlobSize(MF_MT_USER_DATA, &size)) && size > kHeaacInfoBytes) {
                std::vector<std::uint8_t> blob(size);
                if (SUCCEEDED(current->GetBlob(MF_MT_USER_DATA, blob.data(), size, &size))) {
                    config.assign(blob.begin() + kHeaacInfoBytes, blob.begin() + size);
                }
            }
        }

        MFT_OUTPUT_STREAM_INFO info{};
        hr = mft->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) {
            return fail(hr_text("reading the encoder's output stream info", hr));
        }
        provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                            MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        output_buffer_bytes = std::max<DWORD>(info.cbSize, 4096);

        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return {};
    }

    // Collects everything the encoder has finished. Returns an error only for a
    // genuine failure; "need more input" is the normal way out.
    [[nodiscard]] Status drain() {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER out{};
            ComPtr<IMFSample> own;
            if (!provides_samples) {
                ComPtr<IMFMediaBuffer> buffer;
                HRESULT hr = MFCreateMemoryBuffer(output_buffer_bytes, &buffer);
                if (SUCCEEDED(hr)) {
                    hr = MFCreateSample(&own);
                }
                if (SUCCEEDED(hr)) {
                    hr = own->AddBuffer(buffer.Get());
                }
                if (FAILED(hr)) {
                    return fail(hr_text("allocating an output sample", hr));
                }
                out.pSample = own.Get();
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
            // The MFT hands back a reference it provided itself, and any events
            // it queued; both are ours to release.
            ComPtr<IMFSample> produced;
            produced.Attach(provides_samples ? out.pSample : nullptr);
            if (out.pEvents != nullptr) {
                out.pEvents->Release();
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return {};
            }
            if (FAILED(hr)) {
                return fail(hr_text("encoding", hr));
            }
            IMFSample* sample = provides_samples ? produced.Get() : own.Get();
            if (sample == nullptr) {
                return {};
            }
            ComPtr<IMFMediaBuffer> flat;
            if (FAILED(sample->ConvertToContiguousBuffer(&flat))) {
                return fail("could not read an encoded frame");
            }
            BYTE* data = nullptr;
            DWORD length = 0;
            if (FAILED(flat->Lock(&data, nullptr, &length))) {
                return fail("could not lock an encoded frame");
            }
            if (length > 0) {
                ready.emplace_back(data, data + length);
            }
            flat->Unlock();
        }
    }

    [[nodiscard]] Status feed(std::span<const float> samples) {
        const DWORD bytes = static_cast<DWORD>(samples.size() * sizeof(std::int16_t));
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
        if (FAILED(hr)) {
            return fail(hr_text("allocating an input buffer", hr));
        }
        BYTE* data = nullptr;
        hr = buffer->Lock(&data, nullptr, nullptr);
        if (FAILED(hr)) {
            return fail(hr_text("locking an input buffer", hr));
        }
        auto* pcm = reinterpret_cast<std::int16_t*>(data);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            // NaN compares false both ways and would reach the cast; map it to
            // silence rather than undefined behaviour.
            const float s = std::isnan(samples[i]) ? 0.0F : std::clamp(samples[i], -1.0F, 1.0F);
            pcm[i] = static_cast<std::int16_t>(std::lrintf(s * 32767.0F));
        }
        buffer->Unlock();
        buffer->SetCurrentLength(bytes);

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) {
            hr = sample->AddBuffer(buffer.Get());
        }
        if (FAILED(hr)) {
            return fail(hr_text("building an input sample", hr));
        }
        // 100 ns units. The encoder does not need the stamps to produce frames
        // but a continuous clock keeps it from guessing about gaps.
        const LONGLONG duration =
            static_cast<LONGLONG>(samples.size()) * 10'000'000LL / sample_rate;
        sample->SetSampleTime(clock);
        sample->SetSampleDuration(duration);
        clock += duration;

        hr = mft->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (auto status = drain(); !status) {
                return status;
            }
            hr = mft->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            return fail(hr_text("feeding the encoder", hr));
        }
        return drain();
    }

    ComPtr<IMFTransform> mft;
    bool mf_started = false;
    bool provides_samples = false;
    DWORD output_buffer_bytes = 4096;
    std::uint32_t sample_rate = 0;
    LONGLONG clock = 0;
    std::vector<std::uint8_t> config;
    std::vector<std::vector<std::uint8_t>> ready;
};

AacEncoder::AacEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AacEncoder::~AacEncoder() = default;

Expected<std::unique_ptr<AacEncoder>> AacEncoder::create(std::uint32_t sample_rate) {
    auto impl = std::make_unique<Impl>();
    if (auto status = impl->init(sample_rate); !status) {
        return std::unexpected(status.error());
    }
    return std::unique_ptr<AacEncoder>(new AacEncoder(std::move(impl)));
}

Status AacEncoder::push(std::span<const float> samples) {
    if (samples.empty()) {
        return {};
    }
    return impl_->feed(samples);
}

std::vector<std::vector<std::uint8_t>> AacEncoder::take_access_units() {
    return std::exchange(impl_->ready, {});
}

std::span<const std::uint8_t> AacEncoder::audio_specific_config() const {
    return impl_->config;
}

std::uint32_t AacEncoder::sample_rate() const {
    return impl_->sample_rate;
}

}  // namespace revenant::webgate
