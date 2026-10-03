// See spoken_clip.h.

#include "tests/transcribe/spoken_clip.h"

#include <cstdint>
#include <cstring>
#include <format>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which they assume.
#include <mmreg.h>
#include <objbase.h>
#include <sapi.h>
#endif

namespace revenant::test {

#if defined(_WIN32)
namespace {

// Releases a COM pointer on scope exit. The suite has no ATL, and a whole
// smart pointer for four interfaces in one function is more than this needs.
template <class T>
struct ComRef {
    T* p = nullptr;
    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ~ComRef()
    {
        if (p != nullptr) {
            p->Release();
        }
    }
    T* operator->() const { return p; }
};

}  // namespace

Expected<std::vector<float>> speak_16k(const std::wstring& text)
{
    // An apartment, because SAPI's voice is apartment-threaded. A thread that
    // already has the other kind is told so, and the call still works there
    // through COM's marshalling, so that answer is not a failure.
    const HRESULT joined = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool must_leave = SUCCEEDED(joined);
    struct Leave {
        bool active;
        ~Leave()
        {
            if (active) {
                CoUninitialize();
            }
        }
    } leave{must_leave};

    ComRef<ISpVoice> voice;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                reinterpret_cast<void**>(&voice.p)))) {
        return fail("Windows text-to-speech (SAPI) is not available on this machine");
    }

    ComRef<IStream> memory;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memory.p))) {
        return fail("could not create a memory stream for the synthesised speech");
    }
    ComRef<ISpStream> stream;
    if (FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream,
                                reinterpret_cast<void**>(&stream.p)))) {
        return fail("SAPI could not create an audio stream");
    }

    // Whisper's own rate and format, so the clip needs no resampling and the
    // test exercises the recogniser rather than a resampler.
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 16'000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 32'000;
    if (FAILED(stream->SetBaseStream(memory.p, SPDFID_WaveFormatEx, &format))) {
        return fail("SAPI refused 16 kHz 16-bit mono output");
    }
    if (FAILED(voice->SetOutput(stream.p, TRUE))) {
        return fail("SAPI could not direct its voice into the stream");
    }
    const HRESULT spoke = voice->Speak(text.c_str(), SPF_DEFAULT, nullptr);
    if (FAILED(spoke)) {
        return fail(std::format("SAPI could not speak (HRESULT 0x{:08X}); is a voice installed?",
                                static_cast<std::uint32_t>(spoke)));
    }
    voice->SetOutput(nullptr, FALSE);

    STATSTG stat{};
    if (FAILED(memory->Stat(&stat, STATFLAG_NONAME))) {
        return fail("could not size the synthesised speech");
    }
    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(memory.p, &global))) {
        return fail("could not reach the synthesised speech");
    }
    const auto bytes = static_cast<std::size_t>(stat.cbSize.QuadPart);
    const void* locked = GlobalLock(global);
    if (locked == nullptr) {
        return fail("could not lock the synthesised speech");
    }
    std::vector<std::int16_t> pcm(bytes / sizeof(std::int16_t));
    std::memcpy(pcm.data(), locked, pcm.size() * sizeof(std::int16_t));
    GlobalUnlock(global);

    if (pcm.empty()) {
        return fail("SAPI produced no audio; is a voice installed?");
    }
    std::vector<float> out(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        out[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    return out;
}

#else

Expected<std::vector<float>> speak_16k(const std::wstring&)
{
    return fail("synthesised speech needs Windows' SAPI, and this is not Windows");
}

#endif

}  // namespace revenant::test
