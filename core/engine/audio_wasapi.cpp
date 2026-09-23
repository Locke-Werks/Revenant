// The WASAPI render backend.
//
// core/engine/audio_wasapi.h states the four decisions and what close() has to
// guarantee. This file holds them up. Three things about the structure are not
// visible from the header.
//
// EVERY COM CALL HAPPENS ON THE RENDER THREAD. Not just GetBuffer: the
// enumerator, the endpoint, IAudioClient::Initialize and the notification
// client are all created, used and released there. The control thread never
// touches a COM pointer. That is why open() starts the thread and then blocks
// on a promise for its result rather than opening the device itself and
// handing the objects over: a client activated in whatever apartment the
// caller happens to be in, then driven from an MTA thread, works on every
// machine anyone tests it on and is still an apartment violation. Blocking is
// free here, because open() is a control-thread call that is allowed to block
// and the egress layer calls it once.
//
// THE THREAD IS STARTED AT open() AND RELEASED AT attach(). The device has to
// be opened during open(), because the header requires a device that cannot be
// had to fail there rather than degrade, and the render loop must not run
// before there is a tap to read. So the thread opens the device, reports, and
// then waits on begin_event_ until attach() has installed the tap. close()
// signals stop_event_ instead, which is what unblocks a backend that was
// opened and then abandoned.
//
// THE WATCHER OWNS ITS OWN COPY OF THE WAKE HANDLE. An IMMNotificationClient
// can be called on a device-enumerator thread concurrently with
// UnregisterEndpointNotificationCallback, so a callback can be in flight while
// this object is tearing down. It therefore duplicates the event handle and
// closes its copy in its own destructor, and touches nothing the backend owns.
// That is the difference between a headset unplug during shutdown being a
// no-op and it being a write to a closed handle.

#include "core/engine/audio_wasapi.h"
#include "core/thread_role.h"

#ifdef _WIN32

#include <windows.h>

#include <mmreg.h>
#include <objbase.h>

// The include order here is load-bearing and is not alphabetical.
//
// initguid.h first, because the PKEY_ constants below are declared by macros
// that emit an extern declaration without it and a definition with it. Nothing
// in the Windows libraries exports them, so leaving it out is an unresolved
// external at link time, not a missing value.
//
// mmdeviceapi.h before functiondiscoverykeys_devpkey.h, because the latter
// uses the DEFINE_PROPERTYKEY macro and does not define it: the include that
// would have is commented out in the SDK header.
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <audioclient.h>
#include <avrt.h>
#include <propidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <format>
#include <future>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace revenant::engine {
namespace {

// How long the render thread waits for the device to ask for a buffer before
// calling it dead, as a multiple of the buffer it was given, with a floor. A
// healthy event-driven client is woken once per period; a device that has gone
// several whole buffers without asking is not going to ask again, and the
// thread has to stop rather than sit in an infinite wait holding a tap the
// egress layer is about to free.
constexpr double kWaitTimeoutPeriods = 4.0;
constexpr DWORD kMinWaitTimeoutMs = 200;

// A default endpoint that has just changed is not always openable on the
// instant the notification arrives, so a first failure is not yet a fault.
constexpr int kReopenAttempts = 10;
constexpr DWORD kReopenRetryMs = 100;

[[nodiscard]] std::unexpected<Error> com_fail(HRESULT hr, std::string_view attempted)
{
    return fail(std::format("could not {}: HRESULT 0x{:08X}", attempted,
                            static_cast<std::uint32_t>(hr)),
                static_cast<long long>(hr));
}

[[nodiscard]] std::unexpected<Error> last_error_fail(std::string_view attempted)
{
    return com_fail(HRESULT_FROM_WIN32(GetLastError()), attempted);
}

[[nodiscard]] std::string to_utf8(const wchar_t* text)
{
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }
    const auto length = static_cast<int>(std::wcslen(text));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
    return out;
}

[[nodiscard]] std::wstring to_wide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const auto length = static_cast<int>(text.size());
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (chars <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), length, out.data(), chars);
    return out;
}

// A minimum COM smart pointer. WRL and ATL both do this and both drag in a
// header set an engine tree has no other use for.
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    // For a COM out-parameter. Releases first, so retrying through the same
    // pointer cannot leak the previous attempt.
    T** put()
    {
        reset();
        return &ptr_;
    }

    void** put_void()
    {
        reset();
        return reinterpret_cast<void**>(&ptr_);
    }

    // Takes ownership of a reference the caller already holds.
    void adopt(T* raw)
    {
        reset();
        ptr_ = raw;
    }

    void reset()
    {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

// CoInitializeEx and its matching CoUninitialize, scoped.
//
// RPC_E_CHANGED_MODE means the thread is already in a single-threaded
// apartment. The endpoint objects still work, so that is usable; what must not
// happen is this scope uninitialising an apartment it did not create.
class ComScope {
public:
    ComScope()
    {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = SUCCEEDED(result_);
    }

    ~ComScope()
    {
        if (owned_) {
            CoUninitialize();
        }
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;

    [[nodiscard]] bool usable() const { return owned_ || result_ == RPC_E_CHANGED_MODE; }
    [[nodiscard]] HRESULT result() const { return result_; }

private:
    HRESULT result_ = S_OK;
    bool owned_ = false;
};

[[nodiscard]] std::string endpoint_id(IMMDevice* device)
{
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || raw == nullptr) {
        return {};
    }
    std::string id = to_utf8(raw);
    CoTaskMemFree(raw);
    return id;
}

// Reads the friendly name and the engine's format straight out of the property
// store. Nothing here activates an IAudioClient, which is what lets
// enumerate_audio_devices() answer on a machine where opening would fail.
void describe_endpoint(IMMDevice* device, AudioDeviceInfo& info)
{
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
        return;
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        info.name = to_utf8(value.pwszVal);
    }
    PropVariantClear(&value);

    PropVariantInit(&value);
    if (SUCCEEDED(properties->GetValue(PKEY_AudioEngine_DeviceFormat, &value)) &&
        value.vt == VT_BLOB && value.blob.pBlobData != nullptr &&
        value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        // Copied out rather than read through a cast, because the blob is a
        // byte buffer with no alignment promise attached to it.
        WAVEFORMATEX format{};
        std::memcpy(&format, value.blob.pBlobData, sizeof(format));
        info.mix_rate = static_cast<dsp::SampleRate>(format.nSamplesPerSec);
        info.mix_channels = format.nChannels;
    }
    PropVariantClear(&value);
}

// Watches for the system default render endpoint moving.
//
// Sets a flag by signalling an event and returns. Everything else, including
// deciding whether the change is worth reopening for, happens on the render
// thread: this is called on a thread owned by the device enumerator, and doing
// real work here blocks the audio subsystem for every process on the machine.
class DefaultRenderWatcher final : public IMMNotificationClient {
public:
    // Takes ownership of `wake`, which must be a handle duplicated for this
    // object alone. See the note at the top of the file for why it cannot
    // borrow the backend's.
    explicit DefaultRenderWatcher(HANDLE wake) : wake_(wake) {}

    DefaultRenderWatcher(const DefaultRenderWatcher&) = delete;
    DefaultRenderWatcher& operator=(const DefaultRenderWatcher&) = delete;
    DefaultRenderWatcher(DefaultRenderWatcher&&) = delete;
    DefaultRenderWatcher& operator=(DefaultRenderWatcher&&) = delete;

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        // eCommunications is deliberately ignored. It moves when a headset is
        // plugged in for a call and is not what ordinary playback follows, so
        // acting on it would tear the stream down for a change that did not
        // affect us.
        if (flow == eRender && (role == eConsole || role == eMultimedia)) {
            // Under the lock, because unregistering does not wait for a
            // callback already running: without it the render thread can
            // close this handle between the test above and the signal, and
            // SetEvent on a closed handle is either a no-op or a signal to
            // whatever inherited the value.
            const std::lock_guard<std::mutex> guard(lock_);
            if (wake_ != nullptr) {
                SetEvent(wake_);
            }
        }
        return S_OK;
    }

    // Closes the handle and stops signalling, before the last reference goes.
    // Called by the owner after UnregisterEndpointNotificationCallback, which
    // is documented not to drain in-flight callbacks, so this is what makes
    // the handle's lifetime definite rather than probable.
    void detach()
    {
        const std::lock_guard<std::mutex> guard(lock_);
        if (wake_ != nullptr) {
            CloseHandle(wake_);
            wake_ = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return S_OK;
    }

private:
    // Private because the lifetime is the reference count's, not a scope's.
    ~DefaultRenderWatcher()
    {
        if (wake_ != nullptr) {
            CloseHandle(wake_);
        }
    }

    std::atomic<ULONG> references_{1};
    std::mutex lock_;
    HANDLE wake_ = nullptr;
};

// MMCSS "Pro Audio" scheduling, for as long as the scope lasts.
//
// Scoped to the render loop rather than the thread, because the boost is for a
// thread running on the device's clock and the same thread spends its first
// moments waiting for a tap. AvSetMmThreadCharacteristics returning null is
// not a failure worth reporting: it means MMCSS is disabled on the machine,
// the loop still runs, and the consequence is a thread more likely to be
// descheduled, which is exactly what the tap's underrun counter is for.
class MmcssScope {
public:
    explicit MmcssScope(const wchar_t* task)
    {
        DWORD index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(task, &index);
    }

    ~MmcssScope()
    {
        if (handle_ != nullptr) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }

    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;
    MmcssScope(MmcssScope&&) = delete;
    MmcssScope& operator=(MmcssScope&&) = delete;

private:
    HANDLE handle_ = nullptr;
};

void close_handle(HANDLE& handle)
{
    if (handle != nullptr) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

class WasapiBackend final : public AudioBackend {
public:
    explicit WasapiBackend(const WasapiOptions& options) : options_(options) {}

    ~WasapiBackend() override { teardown(); }

    [[nodiscard]] AudioBackendKind kind() const override { return AudioBackendKind::Pull; }

    Status open(const AudioStreamInfo& info) override;
    Status attach(const AudioTap& tap) override;
    Status close() override
    {
        teardown();
        const std::scoped_lock lock(fault_mutex_);
        return fault_;
    }

private:
    // --- the control thread -------------------------------------------------

    // The whole of close() except reporting. Separate so the failure paths in
    // open() and the destructor can stop the thread without discarding a
    // Status they have nothing to do with.
    void teardown();

    // --- the render thread --------------------------------------------------

    void render_main();
    void render_body();
    void announce(const Status& status);
    void record_fault(const Error& error);

    Status device_open();
    Status bind_endpoint();
    Status watch_default_endpoint();
    Status start_stream();
    void stop_stream();
    void device_close();

    Status run_loop();
    void prime();
    Status serve_one_period();
    Status reopen_on_default();

    WasapiOptions options_;
    AudioStreamInfo info_{};
    AudioTap tap_;

    // Auto-reset: WASAPI signals it once per period and the loop consumes it.
    HANDLE audio_event_ = nullptr;

    // Manual-reset, all three: each is a latched state rather than a pulse, so
    // a thread that is not waiting at the moment it is set still sees it.
    HANDLE stop_event_ = nullptr;
    HANDLE begin_event_ = nullptr;
    HANDLE device_changed_event_ = nullptr;

    std::thread thread_;
    std::promise<Status> ready_;
    std::atomic<bool> announced_{false};

    bool opened_ = false;
    bool attached_ = false;

    mutable std::mutex fault_mutex_;
    Status fault_;

    // Render thread only, from the moment the thread starts until it exits.
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    ComPtr<DefaultRenderWatcher> watcher_;
    UINT32 buffer_frames_ = 0;
    DWORD wait_timeout_ms_ = kMinWaitTimeoutMs;
    std::size_t max_backlog_frames_ = 0;
    std::uint32_t channels_ = 1;
    bool started_ = false;
};

Status WasapiBackend::open(const AudioStreamInfo& info)
{
    if (opened_) {
        return fail("the WASAPI backend is already open");
    }
    if (info.rate <= 0) {
        return fail(std::format("the WASAPI backend was given a sample rate of {}, which is not a "
                                "rate a device can be asked for",
                                info.rate));
    }
    if (info.channels == 0 || info.channels > kMaxAudioChannels) {
        return fail(std::format("the WASAPI backend was given {} channels; it carries 1 or {}",
                                info.channels, kMaxAudioChannels));
    }
    // Finiteness first, and on both, because the comparisons below are all
    // false against a NaN. An infinite buffer reaches a floating-to-integer
    // cast that is undefined behaviour, and a NaN backlog cap passes the
    // "not zero" test and then fails the "greater than zero" test at the
    // trim, so the cap is silently off while the header says it is on.
    if (!std::isfinite(options_.buffer_ms) || !(options_.buffer_ms > 0.0)) {
        return fail(std::format("the WASAPI backend was asked for a buffer of {} ms",
                                options_.buffer_ms));
    }
    if (!std::isfinite(options_.max_backlog_ms) || options_.max_backlog_ms < 0.0) {
        return fail(std::format("the WASAPI backend was given a backlog cap of {} ms",
                                options_.max_backlog_ms));
    }
    if (options_.max_backlog_ms != 0.0 && options_.max_backlog_ms < options_.buffer_ms) {
        // The header's rule, enforced rather than documented. A backlog cap
        // below the device buffer trims away the samples the device is about
        // to ask for, so every period underruns by construction.
        return fail(std::format(
            "the WASAPI backend was given a {} ms backlog cap under a {} ms buffer; the cap must "
            "be at least the buffer or every period trims the samples the device is about to ask "
            "for",
            options_.max_backlog_ms, options_.buffer_ms));
    }
    if (!std::isfinite(options_.gain) || options_.gain < 0.0F) {
        return fail(std::format("the WASAPI backend was given a gain of {}", options_.gain));
    }

    info_ = info;
    channels_ = info.channels;

    audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    begin_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    device_changed_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (audio_event_ == nullptr || stop_event_ == nullptr || begin_event_ == nullptr ||
        device_changed_event_ == nullptr) {
        auto error = last_error_fail("create the WASAPI render thread's events");
        teardown();
        return error;
    }

    ready_ = std::promise<Status>{};
    announced_.store(false, std::memory_order_relaxed);
    std::future<Status> ready = ready_.get_future();

    try {
        thread_ = std::thread([this] { render_main(); });
    } catch (const std::system_error& spawn) {
        auto error = fail(std::format("could not start the WASAPI render thread: {}", spawn.what()));
        teardown();
        return error;
    }

    // The thread reports what happened to the device before it will render
    // anything, which is what makes an unopenable device a failure of open()
    // rather than a backend that plays nothing.
    Status device = ready.get();
    if (!device) {
        teardown();
        return device;
    }

    opened_ = true;
    return {};
}

Status WasapiBackend::attach(const AudioTap& tap)
{
    if (!opened_) {
        return fail("attach() on a WASAPI backend that has not been opened");
    }
    if (attached_) {
        return fail("the WASAPI backend already has a tap");
    }
    if (!tap.valid()) {
        return fail("the WASAPI backend was attached to a tap with no ring behind it");
    }
    if (tap.rate() != info_.rate) {
        return fail(std::format("the tap runs at {} Hz and the WASAPI backend was opened at {} Hz",
                                tap.rate(), info_.rate));
    }
    if (tap.channels() != info_.channels) {
        return fail(std::format("the tap has {} channels and the WASAPI backend was opened with {}",
                                tap.channels(), info_.channels));
    }

    tap_ = tap;
    attached_ = true;

    // Publishes the tap to the render thread: it is waiting on this handle and
    // reads tap_ only after the wait returns.
    if (SetEvent(begin_event_) == 0) {
        return last_error_fail("release the WASAPI render thread");
    }
    return {};
}

void WasapiBackend::teardown()
{
    if (stop_event_ != nullptr) {
        SetEvent(stop_event_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }

    // Only after the join. The render thread waits on all four handles, and
    // closing one out from under a wait is how this turns into a hang that
    // reproduces on somebody else's machine.
    close_handle(audio_event_);
    close_handle(stop_event_);
    close_handle(begin_event_);
    close_handle(device_changed_event_);

    opened_ = false;
    attached_ = false;
    tap_ = AudioTap{};
}

void WasapiBackend::announce(const Status& status)
{
    if (!announced_.exchange(true, std::memory_order_acq_rel)) {
        ready_.set_value(status);
    }
}

void WasapiBackend::record_fault(const Error& error)
{
    const std::scoped_lock lock(fault_mutex_);

    // Keeps the first fault. A device that has gone away produces a cascade,
    // and the one that names the cause is the one that arrived first.
    if (fault_) {
        fault_ = std::unexpected(error);
    }
}

void WasapiBackend::render_main()
{
    name_this_thread(L"revenant wasapi render");
    render_body();

    // A thread that got here without reporting would leave open() blocked on a
    // broken promise, which surfaces as an exception on the control thread in
    // the one place this file promises not to throw.
    announce(fail("the WASAPI render thread stopped before it could report on the device"));
}

void WasapiBackend::render_body()
{
    ComScope com;
    if (!com.usable()) {
        announce(com_fail(com.result(), "initialise COM on the WASAPI render thread"));
        return;
    }

    try {
        Status opened = device_open();
        announce(opened);

        if (opened) {
            const HANDLE waits[] = {stop_event_, begin_event_};
            const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (woke == WAIT_OBJECT_0 + 1) {
                const MmcssScope priority(L"Pro Audio");
                Status ran = run_loop();
                if (!ran) {
                    record_fault(ran.error());
                }
            }
        }
    } catch (...) {
        // std::format and std::string are the only things below that can throw
        // and only on allocation failure. Recording it beats terminating the
        // process, and the fault comes back out of close().
        try {
            record_fault(Error{"the WASAPI render thread stopped on an exception"});
        } catch (...) {
        }
    }

    device_close();
}

Status WasapiBackend::device_open()
{
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), enumerator_.put_void());
    if (FAILED(hr)) {
        return com_fail(hr, "create the multimedia device enumerator");
    }

    Status bound = bind_endpoint();
    if (!bound) {
        return bound;
    }

    // Only meaningful for the default endpoint: a named device that has
    // disappeared has not been replaced by anything.
    if (options_.follow_default && options_.device_id.empty()) {
        return watch_default_endpoint();
    }
    return {};
}

Status WasapiBackend::bind_endpoint()
{
    stop_stream();

    HRESULT hr = S_OK;
    if (options_.device_id.empty()) {
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, device_.put());
        if (FAILED(hr)) {
            return com_fail(hr, "open the system default audio render endpoint");
        }
    } else {
        const std::wstring wide = to_wide(options_.device_id);
        hr = enumerator_->GetDevice(wide.c_str(), device_.put());
        if (FAILED(hr)) {
            return com_fail(
                hr, std::format("open the audio render endpoint '{}'", options_.device_id));
        }
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.put_void());
    if (FAILED(hr)) {
        return com_fail(hr, "activate an audio client on the render endpoint");
    }

    // The engine's own format, not the device's. The convert flags below are
    // what make that acceptable whatever the machine's mix format is; see the
    // second decision in audio_wasapi.h.
    //
    // The channel count is the stream's rather than a hardcoded one. The
    // header describes a mono engine and that is the ordinary case, but
    // AudioStreamInfo carries a count and WFM registers two, and a stereo
    // stream declared as mono plays back at twice the speed.
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(channels_);
    format.nSamplesPerSec = static_cast<DWORD>(info_.rate);
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * (format.wBitsPerSample / 8));
    format.nAvgBytesPerSec = format.nSamplesPerSec * static_cast<DWORD>(format.nBlockAlign);
    format.cbSize = 0;

    const auto duration = static_cast<REFERENCE_TIME>(options_.buffer_ms * 10'000.0);
    constexpr DWORD kStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                   AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamFlags, duration, 0, &format, nullptr);
    if (FAILED(hr)) {
        return com_fail(hr, std::format("initialise a shared-mode stream of {} channel(s) of "
                                        "float32 at {} Hz",
                                        channels_, info_.rate));
    }

    hr = client_->SetEventHandle(audio_event_);
    if (FAILED(hr)) {
        return com_fail(hr, "give the audio client its render event");
    }

    hr = client_->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) {
        return com_fail(hr, "read back the size of the buffer the device granted");
    }
    if (buffer_frames_ == 0) {
        return fail("the audio device granted a buffer of zero frames");
    }

    hr = client_->GetService(__uuidof(IAudioRenderClient), render_.put_void());
    if (FAILED(hr)) {
        return com_fail(hr, "obtain the render client for the stream");
    }

    // Shared mode rounds the request up to the engine's own period, so the
    // timeout is derived from what was granted rather than from what was
    // asked for.
    const double period_ms =
        static_cast<double>(buffer_frames_) * 1000.0 / static_cast<double>(info_.rate);
    wait_timeout_ms_ = static_cast<DWORD>(
        std::max(static_cast<double>(kMinWaitTimeoutMs), period_ms * kWaitTimeoutPeriods));

    max_backlog_frames_ = 0;
    if (options_.max_backlog_ms > 0.0) {
        const double frames = options_.max_backlog_ms * static_cast<double>(info_.rate) / 1000.0;
        max_backlog_frames_ = static_cast<std::size_t>(frames);

        // open() already refused a cap below the requested buffer, but the
        // device is free to round the buffer up past it. The floor keeps the
        // header's invariant against what the device actually granted.
        max_backlog_frames_ = std::max(max_backlog_frames_, static_cast<std::size_t>(buffer_frames_));
    }

    // Prime the whole buffer with silence before the first Start(), so the
    // stream does not open on whatever the last client left in it.
    BYTE* data = nullptr;
    hr = render_->GetBuffer(buffer_frames_, &data);
    if (SUCCEEDED(hr)) {
        hr = render_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (FAILED(hr)) {
        return com_fail(hr, "prime the device buffer with silence");
    }

    return {};
}

Status WasapiBackend::watch_default_endpoint()
{
    HANDLE wake = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), device_changed_event_, GetCurrentProcess(), &wake, 0,
                        FALSE, DUPLICATE_SAME_ACCESS) == 0) {
        return last_error_fail("duplicate the wake handle for the default endpoint watcher");
    }

    auto* watcher = new (std::nothrow) DefaultRenderWatcher(wake);
    if (watcher == nullptr) {
        CloseHandle(wake);
        return fail("could not allocate the default audio endpoint watcher");
    }
    watcher_.adopt(watcher);

    const HRESULT hr = enumerator_->RegisterEndpointNotificationCallback(watcher_.get());
    if (FAILED(hr)) {
        watcher_.reset();
        return com_fail(hr, "register for default audio endpoint changes");
    }
    return {};
}

Status WasapiBackend::start_stream()
{
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        return com_fail(hr, "start the audio render stream");
    }
    started_ = true;
    return {};
}

void WasapiBackend::stop_stream()
{
    if (client_ && started_) {
        client_->Stop();
        client_->Reset();
    }
    started_ = false;
    render_.reset();
    client_.reset();
    device_.reset();
}

void WasapiBackend::device_close()
{
    if (watcher_ && enumerator_) {
        enumerator_->UnregisterEndpointNotificationCallback(watcher_.get());
    }
    if (watcher_) {
        watcher_->detach();
    }
    watcher_.reset();
    stop_stream();
    enumerator_.reset();
}

Status WasapiBackend::run_loop()
{
    prime();

    Status started = start_stream();
    if (!started) {
        return started;
    }

    // Order matters: WaitForMultipleObjects returns the lowest signalled index,
    // so a close that lands in the same instant as a device event wins.
    const HANDLE waits[] = {stop_event_, device_changed_event_, audio_event_};

    for (;;) {
        const DWORD woke = WaitForMultipleObjects(3, waits, FALSE, wait_timeout_ms_);

        if (woke == WAIT_OBJECT_0) {
            return {};
        }

        if (woke == WAIT_OBJECT_0 + 1) {
            ResetEvent(device_changed_event_);
            Status moved = reopen_on_default();
            if (!moved) {
                return moved;
            }
            continue;
        }

        // WAIT_TIMEOUT is a signed literal in winerror.h, so the cast is what
        // keeps the comparison from being a signed/unsigned mismatch under /WX.
        if (woke == static_cast<DWORD>(WAIT_TIMEOUT)) {
            // A device that has stopped asking is not coming back on its own,
            // and waiting forever holds a tap the egress layer wants to free.
            return fail(std::format(
                "the audio device stopped asking for buffers for {} ms and is treated as gone",
                wait_timeout_ms_));
        }

        if (woke != WAIT_OBJECT_0 + 2) {
            return last_error_fail("wait on the audio device's render event");
        }

        Status served = serve_one_period();
        if (!served) {
            const bool invalidated =
                served.error().code == static_cast<long long>(AUDCLNT_E_DEVICE_INVALIDATED);
            if (invalidated && options_.follow_default && options_.device_id.empty()) {
                Status moved = reopen_on_default();
                if (!moved) {
                    return moved;
                }
                continue;
            }
            return served;
        }
    }
}

void WasapiBackend::prime()
{
    // The device asks for its first buffer the instant the stream starts, and
    // the engine has not produced a sample yet: the graph has to fill a block,
    // channelize it and get the audio back before there is anything to play.
    // Starting anyway means the first period or two are counted silence, so
    // every monitored run ends by reporting an underrun and the operator
    // learns to ignore the one counter that matters.
    //
    // So wait for one device buffer's worth before starting, and give up
    // after a bounded time rather than never playing: a squelched receiver,
    // or one whose source has not begun, legitimately has nothing, and
    // silence that starts late is worse than silence that starts on time.
    constexpr auto kSlice = std::chrono::milliseconds(2);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

    while (tap_.readable_frames() < buffer_frames_) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return;
        }
        if (WaitForSingleObject(stop_event_, static_cast<DWORD>(kSlice.count())) ==
            WAIT_OBJECT_0) {
            return;
        }
    }
}

Status WasapiBackend::serve_one_period()
{
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        return com_fail(hr, "read how much of the device buffer is still unplayed");
    }
    if (padding >= buffer_frames_) {
        return {};
    }

    const UINT32 frames = buffer_frames_ - padding;

    // Before the read, not after: trimming afterwards discards audio this
    // period was entitled to and leaves the late audio in place.
    if (max_backlog_frames_ != 0) {
        tap_.trim_to_frames(max_backlog_frames_);
    }

    BYTE* data = nullptr;
    hr = render_->GetBuffer(frames, &data);
    if (FAILED(hr)) {
        return com_fail(hr, "claim a buffer from the device");
    }

    const std::size_t samples = static_cast<std::size_t>(frames) * channels_;
    auto* out = reinterpret_cast<float*>(data);

    // read_or_fill rather than read: a shortfall becomes counted silence
    // instead of whatever the device played last time round the buffer.
    tap_.read_or_fill(std::span<float>(out, samples));

    if (options_.gain != 1.0F) {
        for (std::size_t i = 0; i < samples; ++i) {
            out[i] *= options_.gain;
        }
    }

    hr = render_->ReleaseBuffer(frames, 0);
    if (FAILED(hr)) {
        return com_fail(hr, "hand the filled buffer back to the device");
    }
    return {};
}

Status WasapiBackend::reopen_on_default()
{
    stop_stream();

    Status bound = fail("the default audio endpoint was never reopened");
    for (int attempt = 0; attempt < kReopenAttempts; ++attempt) {
        bound = bind_endpoint();
        if (bound) {
            break;
        }
        if (WaitForSingleObject(stop_event_, kReopenRetryMs) == WAIT_OBJECT_0) {
            // Closing while the endpoint is still settling. Not a fault: the
            // caller asked for the stream to end and it has.
            return {};
        }
    }

    if (!bound) {
        return std::unexpected(with_context(
            bound.error(), "the default audio endpoint changed and the new one would not open"));
    }
    return start_stream();
}

}  // namespace

Expected<std::vector<AudioDeviceInfo>> enumerate_audio_devices()
{
    ComScope com;
    if (!com.usable()) {
        return com_fail(com.result(), "initialise COM to enumerate the audio devices");
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), enumerator.put_void());
    if (FAILED(hr)) {
        return com_fail(hr, "create the multimedia device enumerator");
    }

    // A machine with no render endpoint at all is an empty list rather than an
    // error: "there is nowhere to play this" is an answer, and the caller is
    // usually asking precisely because something is wrong.
    std::string default_id;
    ComPtr<IMMDevice> fallback;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, fallback.put()))) {
        default_id = endpoint_id(fallback.get());
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put());
    if (FAILED(hr)) {
        return com_fail(hr, "enumerate the active audio render endpoints");
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        return com_fail(hr, "count the active audio render endpoints");
    }

    std::vector<AudioDeviceInfo> devices;
    devices.reserve(count);

    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, device.put()))) {
            continue;
        }

        AudioDeviceInfo info;
        info.id = endpoint_id(device.get());
        if (info.id.empty()) {
            // An endpoint with no id cannot be named on a command line or in a
            // session file, so listing it would offer the caller something it
            // could not then ask for.
            continue;
        }
        describe_endpoint(device.get(), info);
        if (info.name.empty()) {
            info.name = info.id;
        }
        info.is_default = !default_id.empty() && info.id == default_id;
        devices.push_back(std::move(info));
    }

    return devices;
}

Expected<std::unique_ptr<AudioBackend>> make_wasapi_backend(const WasapiOptions& options)
{
    auto backend = std::make_unique<WasapiBackend>(options);
    return std::unique_ptr<AudioBackend>(std::move(backend));
}

}  // namespace revenant::engine

#else  // _WIN32

namespace revenant::engine {

// The Linux and macOS legs arrive later. Failing here, rather than leaving the
// symbols out, is what keeps a non-Windows build linking and turns "no audio"
// into a sentence instead of an unresolved external.

Expected<std::vector<AudioDeviceInfo>> enumerate_audio_devices()
{
    return fail("audio device enumeration is not supported on this platform: the WASAPI backend "
                "is Windows only, and no ALSA or CoreAudio backend exists yet");
}

Expected<std::unique_ptr<AudioBackend>> make_wasapi_backend(const WasapiOptions&)
{
    return fail("the WASAPI audio backend is not supported on this platform: it is Windows only, "
                "and no ALSA or CoreAudio backend exists yet");
}

}  // namespace revenant::engine

#endif  // _WIN32
