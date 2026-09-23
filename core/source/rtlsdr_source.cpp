// The RTL-SDR backend.
//
// THE SHAPE, AND WHY IT IS TWO THREADS AND A SLOT QUEUE
//
// rtlsdr_read_async does not return until it is cancelled, and it calls its
// callback on the thread that called it, from inside libusb's event loop. The
// buffer handed to that callback is the URB's own memory, and libusb
// resubmits it the moment the callback returns. Two consequences fall out and
// they pull in opposite directions:
//
//   1. The callback must return promptly. A callback that takes longer than
//      the queued transfers cover leaves nothing submitted, and the RTL2832U
//      then overruns inside the dongle where there is no counter and no
//      notification. Loss that cannot be counted is the one kind this engine
//      refuses to have, so the sink cannot be called from here.
//   2. The bytes cannot outlive the callback, so deferring delivery means
//      copying them once.
//
// So: the callback memcpys into one slot of a small fixed queue and returns,
// and a second thread owned by this source drains the queue and calls the
// sink. One host-side copy, which is the floor given librtlsdr's ownership
// rule, and delivery then hands the sink a span straight into the slot, so
// nothing is copied again before the GPU reads it.
//
// THE QUEUE, documented as docs/conventions.md requires of every ring here:
//
//   writer     the librtlsdr callback thread, one only. It claims the slot at
//              head, fills it, and publishes with a release store of head.
//   reader     this source's delivery thread, one only. It reads the slot at
//              tail, calls the sink for each block inside it, and releases
//              the slot with a release store of tail.
//   ownership  the writer owns slots_[head] from the moment it observes room
//              until its store of head. The reader owns slots_[tail] from its
//              acquire load of head until its store of tail. Never both.
//   behind     a writer that finds no room DROPS THE WHOLE TRANSFER. It is a
//              Paced source: the crystal does not wait, there is nowhere to
//              put samples that keep arriving, and buffering further only
//              moves the same loss later and makes it larger. The drop is
//              counted in overrun_events and samples_lost, the index it
//              happened at is recorded in last_loss_index, and the gap is
//              carried forward so the next block's stamp.start and
//              dropped_before both describe it exactly. Nothing is
//              overwritten and nothing is silently skipped.
//
// The queue holds kSlotCount transfers, which at the default rate and
// transfer size is about a tenth of a second. That is the whole of the
// host-side buffering, deliberately: a deeper queue does not stop a consumer
// that is too slow, it only delays the discovery and lengthens the burst that
// is eventually lost.
//
// WHAT THE GAP ARITHMETIC GUARANTEES
//
// produced_index_ counts every sample the device delivered, kept or dropped,
// and only the callback thread touches it. Each published slot carries the
// number of samples dropped immediately before it, so the delivery thread can
// reconstruct the stream index without knowing when the drop happened in real
// time. The identity that holds at all times is:
//
//     samples_delivered + samples_lost == samples the device handed us
//
// and stamp.start is always the true index of the block's first sample rather
// than a count of what survived. A recording with a hole in it therefore has
// the hole in the right place, which is the only thing that makes it
// salvageable.

#include "core/source/rtlsdr_source.h"

#include <rtl-sdr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/source/clock_model.h"

namespace revenant::source {
namespace {

constexpr std::size_t kBytesPerSample = 2;
static_assert(bytes_per_sample(SampleFormat::Cu8) == kBytesPerSample,
              "the transfer arithmetic below is written in samples and converts with this "
              "constant, so it has to agree with the format table");

// The only gain control librtlsdr exposes on this family. On an R820T it
// drives the LNA, the mixer and the VGA together from one table, so naming it
// after any one of them would describe a stage that cannot be set on its own.
constexpr std::string_view kTunerStage = "tuner";

// 32768 samples is 13.6 ms at the default rate, which is short enough that a
// receiver monitoring on a loudspeaker is not listening to a backlog and long
// enough that the per-dispatch overhead is nowhere near the sample cost.
constexpr std::size_t kPreferredBlockSamples = 32'768;

// A block past this is an arithmetic mistake in the caller rather than a
// block anybody wants. Sixteen million samples is seven seconds of latency at
// the default rate.
constexpr std::size_t kMaxBlockSamples = 1u << 24;

// The USB request block size the RTL2832U's bulk endpoint is driven at.
// rtl-sdr.h requires a transfer length that is a multiple of 512 and asks for
// a multiple of this.
constexpr std::uint64_t kUrbBytes = 16'384;

// A transfer past this buys nothing: it is already 109 ms at the default rate
// and the queue below holds several of them.
constexpr std::uint64_t kMaxTransferBytes = 524'288;

// Transfers left outstanding with libusb. This is how long the host can be
// away before the dongle has nowhere to put samples, so it is the number that
// covers a scheduling hiccup: sixteen 64 KiB transfers is about 218 ms at the
// default rate.
constexpr std::uint32_t kTransferCount = 16;

// Power of two so the slot index is one AND. See the queue note in the header
// comment for what the depth is chosen against.
constexpr std::uint64_t kSlotCount = 8;
constexpr std::uint64_t kSlotMask = kSlotCount - 1;

// How long the delivery thread sleeps when the queue is empty. Short against
// a transfer's duration so it adds no measurable latency, long enough that an
// idle source is not a spinning core. A condition variable would be a mutex
// the callback thread has to take on the sample path, which is the thing
// docs/conventions.md forbids and the thing the slot queue exists to avoid.
constexpr auto kIdlePoll = std::chrono::microseconds(250);

// Attempts at rtlsdr_set_center_freq when the stream has just been paused for
// it.
//
// Four rather than one because, with librtlsdr v2.0.2, the first one was
// EXPECTED to fail, not because a retry might help. join_locked documents the
// reason: on Windows the first control transfer issued after the bulk transfers
// had been cancelled came back LIBUSB_ERROR_PIPE. Measured over six consecutive
// pause-and-retune rounds on an R820T, the first attempt failed and the second
// succeeded every single time, so one attempt would have turned a working
// retune into a refusal on every use. The remaining two are headroom for a
// dongle that needs a moment more, and four total still reports a genuinely
// unreachable frequency in the same breath rather than after a wait.
//
// WHAT CHANGED, 2026-09-23. The stall came from v2.0.2's cancel returning with
// transfers still in flight. With vcpkg-overlays/rtlsdr, whose cancel waits for
// every transfer, tools/rtlsdr-cancel-trial found the first control call after
// a cancel landing 600 times in 600, against 190 in 600 on v2.0.2. Four stay:
// they cost nothing when the first lands, and a different librtlsdr or dongle
// is exactly how the stall would come back.
constexpr int kRetunePipeRetries = 4;

// Between repeats of rtlsdr_cancel_async. See join_locked for why it repeats.
constexpr auto kCancelPoll = std::chrono::milliseconds(2);

// One sigma on placing sample zero of the stream on the wall clock.
//
// The anchor is read at start, immediately after rtlsdr_reset_buffer has
// flushed the dongle's FIFO and immediately before the first transfers are
// submitted, so the error is the USB submission and host scheduling latency
// between those two moments. That is not measured, and it is one-sided: the
// clock read happens before the device begins filling, so the anchor is early
// by that amount rather than scattered around it. Two milliseconds covers it
// on a loaded machine.
//
// This stops being the dominant term quickly. An undisciplined crystal at the
// tolerance below has drifted further than this inside two minutes, which is
// the honest reason not to spend effort tightening it.
constexpr std::int64_t kAnchorAccuracyNs = 2'000'000;

// Fractional frequency tolerance of the sample clock, one sigma, parts per
// million.
//
// A generic dongle runs on a plain crystal specified at tens of ppm and
// moving several more as it warms up. An RTL-SDR v3 has a 1 ppm TCXO, but
// nothing on the USB bus distinguishes one from a clone that copied the
// product string, so the model states the worse case.
//
// A caller who supplied ppm= has measured this device against a known carrier
// and librtlsdr applies the correction to the resampler as well as the tuner,
// so the residual is their calibration error rather than the part's
// tolerance.
constexpr double kUncalibratedTolerancePpm = 20.0;
constexpr double kCalibratedTolerancePpm = 2.0;

// The RTL2832U's crystal, which is what clocks the ADC. Under direct sampling
// the tuner is out of circuit and the DDC tunes inside the first Nyquist zone
// of this.
constexpr dsp::Hertz kRtl2832XtalHz = 28'800'000;

[[nodiscard]] std::int64_t unix_now_ns()
{
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
}

// A librtlsdr return code, with libusb's name for it where it has one.
//
// librtlsdr hands back the code of the libusb call that failed, so -9 at a
// retune is libusb's LIBUSB_ERROR_PIPE, a stalled control transfer, and -3 at
// an open is LIBUSB_ERROR_ACCESS, the device held by another process. The
// values are the ones libusb.h publishes. Some librtlsdr calls also return
// small negatives of their own, -1 for no device and -2 for a call made in the
// wrong state, and those collide with libusb's -1 and -2, so the name is given
// as what libusb would mean rather than as a certainty.
[[nodiscard]] std::string rc_text(int rc)
{
    std::string_view name;
    switch (rc) {
        case -1: name = "LIBUSB_ERROR_IO"; break;
        case -2: name = "LIBUSB_ERROR_INVALID_PARAM"; break;
        case -3: name = "LIBUSB_ERROR_ACCESS"; break;
        case -4: name = "LIBUSB_ERROR_NO_DEVICE"; break;
        case -5: name = "LIBUSB_ERROR_NOT_FOUND"; break;
        case -6: name = "LIBUSB_ERROR_BUSY"; break;
        case -7: name = "LIBUSB_ERROR_TIMEOUT"; break;
        case -8: name = "LIBUSB_ERROR_OVERFLOW"; break;
        case -9: name = "LIBUSB_ERROR_PIPE"; break;
        case -10: name = "LIBUSB_ERROR_INTERRUPTED"; break;
        case -11: name = "LIBUSB_ERROR_NO_MEM"; break;
        case -12: name = "LIBUSB_ERROR_NOT_SUPPORTED"; break;
        case -99: name = "LIBUSB_ERROR_OTHER"; break;
        default: break;
    }
    if (name.empty()) {
        return std::format("{}", rc);
    }
    return std::format("{} ({} if libusb produced it)", rc, name);
}

// Said when a control transfer was retried, so a refusal after a pause reads
// as the last of several rather than as the first.
[[nodiscard]] std::string attempts_text(int attempts)
{
    if (attempts <= 1) {
        return {};
    }
    return std::format(" on each of {} attempts", attempts);
}

[[nodiscard]] std::string rate_ranges_text()
{
    return std::format("{} to {} S/s and {} to {} S/s", kRtlSdrLowRateMin, kRtlSdrLowRateMax,
                       kRtlSdrHighRateMin, kRtlSdrHighRateMax);
}

[[nodiscard]] std::string_view tuner_name(rtlsdr_tuner tuner)
{
    switch (tuner) {
        case RTLSDR_TUNER_E4000: return "E4000";
        case RTLSDR_TUNER_FC0012: return "FC0012";
        case RTLSDR_TUNER_FC0013: return "FC0013";
        case RTLSDR_TUNER_FC2580: return "FC2580";
        // librtlsdr has no separate identifier for the R820T2: the part
        // answers the R820T's register probe and is reported as one.
        case RTLSDR_TUNER_R820T: return "R820T/R820T2";
        case RTLSDR_TUNER_R828D: return "R828D";
        case RTLSDR_TUNER_UNKNOWN: break;
    }
    return "unknown";
}

// What the tuner IC can reach, per part.
//
// Step is left at zero throughout, which TuneRange documents as continuous
// within whatever the synthesiser can resolve. That is the honest answer for
// every tuner here: they are fractional-N parts whose step depends on the
// divider the PLL happens to land in, so a single figure would be wrong in
// both directions. tune() reads the achieved frequency back from the device
// rather than predicting it, which is what the exactness actually rests on.
[[nodiscard]] std::vector<TuneRange> tune_ranges_for(rtlsdr_tuner tuner, DirectSampling direct)
{
    if (direct != DirectSampling::Off) {
        // The tuner is bypassed and rtlsdr_set_center_freq drives the
        // RTL2832U's DDC instead. The first Nyquist zone of the 28.8 MHz ADC
        // is what can be received without relying on an alias, so that is
        // what is claimed.
        return {TuneRange{0, kRtl2832XtalHz / 2, 0}};
    }

    switch (tuner) {
        case RTLSDR_TUNER_E4000:
            // Two ranges because the part has a genuine hole around the
            // 1.1 GHz PLL crossover. Where exactly it falls varies between
            // samples, so the claimed edges are the conservative ones.
            return {TuneRange{52'000'000, 1'100'000'000, 0},
                    TuneRange{1'250'000'000, 2'200'000'000, 0}};
        case RTLSDR_TUNER_FC0012:
            return {TuneRange{22'000'000, 948'600'000, 0}};
        case RTLSDR_TUNER_FC0013:
            return {TuneRange{22'000'000, 1'100'000'000, 0}};
        case RTLSDR_TUNER_FC2580:
            return {TuneRange{146'000'000, 308'000'000, 0},
                    TuneRange{438'000'000, 924'000'000, 0}};
        case RTLSDR_TUNER_R820T:
        case RTLSDR_TUNER_R828D:
            return {TuneRange{24'000'000, 1'766'000'000, 0}};
        case RTLSDR_TUNER_UNKNOWN: break;
    }

    // No ranges rather than a guess. can_tune then answers no for everything,
    // which is correct: librtlsdr has no driver for a tuner it did not
    // recognise, so nothing can be tuned on it at all.
    return {};
}

[[nodiscard]] std::string tune_ranges_text(const std::vector<TuneRange>& ranges)
{
    if (ranges.empty()) {
        return "nothing: librtlsdr did not recognise the tuner in this dongle, so it has no "
               "driver for it";
    }
    std::string out;
    for (const TuneRange& range : ranges) {
        if (!out.empty()) {
            out += " and ";
        }
        out += std::format("{} to {} Hz", range.low, range.high);
    }
    return out;
}

// RAII over rtlsdr_open. There is no close() on Source by design, so the
// handle has to be owned by something whose destructor is the close.
class Device {
public:
    Device() = default;
    explicit Device(rtlsdr_dev_t* handle) : handle_(handle) {}

    ~Device() { reset(); }

    Device(Device&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Device& operator=(Device&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] rtlsdr_dev_t* get() const { return handle_; }

    void reset()
    {
        if (handle_ != nullptr) {
            rtlsdr_close(handle_);
            handle_ = nullptr;
        }
    }

private:
    rtlsdr_dev_t* handle_ = nullptr;
};

[[nodiscard]] Status validate(const RtlSdrSourceConfig& config)
{
    if (!rtlsdr_rate_supported(config.rate)) {
        return fail(std::format(
            "{} S/s is not a rate an RTL-SDR can take. The RTL2832U's divider chain reaches {}, "
            "and nothing between {} and {} S/s. Above about {} S/s the USB host controller on "
            "most machines cannot keep up and the dongle drops samples inside itself where "
            "nothing can count them, so {} is the default.",
            config.rate, rate_ranges_text(), kRtlSdrLowRateMax + 1, kRtlSdrHighRateMin - 1,
            kRtlSdrReliableRateMax, kRtlSdrDefaultRate));
    }
    if (config.center_given) {
        if (config.center_hz < 0) {
            return fail(std::format("a centre frequency of {} Hz is below DC", config.center_hz));
        }
        if (config.center_hz > static_cast<dsp::Hertz>(std::numeric_limits<std::uint32_t>::max())) {
            return fail(std::format(
                "a centre frequency of {} Hz is past the {} Hz ceiling rtlsdr_set_center_freq "
                "takes, which is an unsigned 32-bit count of hertz",
                config.center_hz,
                static_cast<dsp::Hertz>(std::numeric_limits<std::uint32_t>::max())));
        }
    }
    if (!config.gain_auto && !std::isfinite(config.gain_db)) {
        return fail("gain= must be 'auto' or a finite number of decibels");
    }
    if (config.ppm_given && (config.ppm < -1'000 || config.ppm > 1'000)) {
        return fail(std::format(
            "a frequency correction of {} ppm is not a crystal error, it is a different radio. "
            "The correction is applied to the tuner and to the resampler, so a wrong one moves "
            "the declared sample rate too.",
            config.ppm));
    }
    return {};
}

// Everything about finding the device that can be answered without opening
// one, which is why it runs before the machine-wide lock is taken: a URI naming
// a dongle that is not there fails at once with the reason, rather than after
// waiting out somebody else's stream to be told the same thing.
[[nodiscard]] Status check_attached(const RtlSdrSourceConfig& config)
{
    const std::uint32_t attached = rtlsdr_get_device_count();
    if (attached == 0) {
        return fail("no RTL-SDR is attached. librtlsdr sees no device at all, which on Windows "
                    "usually means the dongle's USB interface is still bound to the DVB-T "
                    "driver rather than to WinUSB.");
    }

    if (!config.by_serial && config.index >= attached) {
        return fail(std::format(
            "there is no RTL-SDR at index {}: {} attached, so the indices run 0 to {}",
            config.index, attached, attached - 1));
    }
    return {};
}

// UNDER THE LOCK ONLY. A serial is looked up by rtlsdr_get_index_by_serial,
// which opens every attached dongle in turn to read its string descriptors, so
// it is as much an open as rtlsdr_open is.
[[nodiscard]] Expected<std::uint32_t> resolve_index(const RtlSdrSourceConfig& config)
{
    if (!config.by_serial) {
        return config.index;
    }

    const std::uint32_t attached = rtlsdr_get_device_count();
    const int found = rtlsdr_get_index_by_serial(config.serial.c_str());
    if (found >= 0) {
        return static_cast<std::uint32_t>(found);
    }
    if (found == -3) {
        return fail(std::format(
            "no attached RTL-SDR carries the serial '{}'. {} attached; rtlsdr://<index> opens "
            "one by position instead.",
            config.serial, attached));
    }
    return fail(std::format("librtlsdr could not look up the serial '{}': "
                            "rtlsdr_get_index_by_serial returned {}",
                            config.serial, rc_text(found)),
                found);
}

[[nodiscard]] Expected<Device> open_device(std::uint32_t index)
{
    rtlsdr_dev_t* handle = nullptr;
    const int rc = rtlsdr_open(&handle, index);
    if (rc != 0 || handle == nullptr) {
        return fail(std::format(
            "could not open the RTL-SDR at index {}: rtlsdr_open returned {}. A device that "
            "enumerates and will not open is almost always held by another program, or has an "
            "interface that libwdi has not bound to WinUSB.",
            index, rc_text(rc)),
            rc);
    }
    return Device(handle);
}

struct OpenedDevice {
    std::uint32_t index = 0;
    Device device{};
};

// The part of an open or a describe that touches the device. Always called
// through open_under_lock or probe_under_lock, which is what makes the lock
// come first on every path here.
[[nodiscard]] Expected<OpenedDevice> find_and_open(const RtlSdrSourceConfig& config)
{
    auto index = resolve_index(config);
    if (!index) {
        return std::unexpected(std::move(index.error()));
    }
    auto device = open_device(*index);
    if (!device) {
        return std::unexpected(std::move(device.error()));
    }
    return OpenedDevice{*index, std::move(*device)};
}

// The tuner's gain table, in tenths of a decibel, ascending.
//
// An empty table is not treated as a failure here. rtl-sdr.h documents a
// non-positive return as an error, but an unrecognised tuner legitimately has
// no table, and the two are indistinguishable through this interface. The
// consequence is confined: set_gain refuses by name when the table is empty,
// rather than anything proceeding as though a gain had been applied.
[[nodiscard]] std::vector<int> gain_steps_of(rtlsdr_dev_t* device)
{
    const int count = rtlsdr_get_tuner_gains(device, nullptr);
    if (count <= 0) {
        return {};
    }

    std::vector<int> steps(static_cast<std::size_t>(count), 0);
    const int filled = rtlsdr_get_tuner_gains(device, steps.data());
    if (filled != count) {
        return {};
    }
    std::sort(steps.begin(), steps.end());
    return steps;
}

// The dongle's USB serial string, read off the handle that is already open.
//
// rtlsdr_get_usb_strings on the handle rather than rtlsdr_get_device_usb_strings
// on the index, because the index form opens the device a second time, and
// that is refused while this process holds it. Empty when the device will not
// say, which leaves the calibration unkeyed rather than failing an open that
// is otherwise fine. rtl-sdr.h documents each buffer as 256 bytes.
[[nodiscard]] std::string serial_of(rtlsdr_dev_t* device)
{
    char manufacturer[256] = {};
    char product[256] = {};
    char serial[256] = {};
    if (rtlsdr_get_usb_strings(device, manufacturer, product, serial) != 0) {
        return {};
    }
    serial[sizeof(serial) - 1] = '\0';
    return std::string(serial);
}

[[nodiscard]] int nearest_step(const std::vector<int>& steps, int wanted)
{
    int best = steps.front();
    long long best_distance = std::llabs(static_cast<long long>(wanted) - best);
    for (const int step : steps) {
        const long long distance = std::llabs(static_cast<long long>(wanted) - step);
        if (distance < best_distance) {
            best = step;
            best_distance = distance;
        }
    }
    return best;
}

[[nodiscard]] SourceCapabilities capabilities_of(const RtlSdrSourceConfig& config,
                                                 std::uint32_t index,
                                                 std::string_view device_name,
                                                 rtlsdr_tuner tuner,
                                                 const std::vector<int>& gain_steps)
{
    SourceCapabilities caps;
    caps.uri = config.uri;
    caps.backend = "rtlsdr";
    caps.display_name =
        std::format("{} ({} tuner) at index {}", device_name, tuner_name(tuner), index);

    caps.tune_ranges = tune_ranges_for(tuner, config.direct);

    // THE ONE PLACE THIS DESCRIPTION IS LOOSER THAN THE HARDWARE.
    //
    // The RTL2832U takes two disjoint windows and refuses everything between
    // them. SourceCapabilities can express one continuous span or an
    // exhaustive list of discrete rates, and the device is neither: it
    // accepts any integer rate inside either window. Declaring the outer span
    // makes supports_rate answer yes for the 300 kS/s to 900 kS/s hole, and
    // declaring only the upper window makes it answer no for the lower one
    // that works.
    //
    // The outer span is chosen because its failure is loud. A caller that
    // acts on the over-claim gets refused at open by validate() above, with a
    // message naming both windows. The under-claim would instead make a rate
    // the hardware supports simply never appear in a rate list, with nothing
    // anywhere saying why.
    caps.sample_rates.clear();
    caps.min_rate = kRtlSdrLowRateMin;
    caps.max_rate = kRtlSdrHighRateMax;

    caps.native_format = SampleFormat::Cu8;
    caps.bits_per_component = 8;

    if (!gain_steps.empty()) {
        GainStage stage;
        stage.name = std::string(kTunerStage);
        stage.min_db = static_cast<double>(gain_steps.front()) / 10.0;
        stage.max_db = static_cast<double>(gain_steps.back()) / 10.0;
        stage.steps_db.reserve(gain_steps.size());
        for (const int step : gain_steps) {
            stage.steps_db.push_back(static_cast<double>(step) / 10.0);
        }
        stage.has_auto = true;
        caps.gain_stages.push_back(std::move(stage));
    }

    // ppm= goes to librtlsdr, which moves the tuner and the resampler itself,
    // so the engine must not add a stored correction on top of it.
    caps.device_corrects_frequency = config.ppm_given;

    caps.clock_sources = {ClockSource::Internal};
    caps.flow = FlowControl::Paced;
    caps.seekable = false;
    caps.length_samples = 0;
    caps.preferred_block_samples = kPreferredBlockSamples;
    caps.timestamp_accuracy_ns = kAnchorAccuracyNs;
    return caps;
}

// What a slot holds. One librtlsdr transfer, plus what is needed to place it
// in the stream.
struct Slot {
    std::vector<std::uint8_t> bytes;
    std::size_t samples = 0;

    // Samples the device produced and the queue could not take, immediately
    // before this slot's first sample. Carried here rather than in a counter
    // so the gap lands between the right two blocks however far behind the
    // reader is.
    std::uint64_t gap_before = 0;
};

class RtlSdrSource final : public Source {
public:
    RtlSdrSource() = default;
    ~RtlSdrSource() override;

    [[nodiscard]] Status open(const RtlSdrSourceConfig& config, DeviceLock lock, Device device,
                              SourceCapabilities caps, std::vector<int> gain_steps,
                              dsp::SampleRate rate, dsp::Hertz center);

    [[nodiscard]] const SourceCapabilities& capabilities() const override { return caps_; }

    [[nodiscard]] Expected<dsp::Hertz> tune(dsp::Hertz center) override;
    [[nodiscard]] dsp::Hertz center() const override
    {
        return center_hz_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) override;
    [[nodiscard]] dsp::SampleRate sample_rate() const override { return rate_; }

    [[nodiscard]] Expected<double> set_gain(std::string_view stage, double db) override;
    [[nodiscard]] Status set_gain_auto(std::string_view stage, bool on) override;

    [[nodiscard]] Status start(const StreamOptions& options, BlockSink sink) override;
    [[nodiscard]] Status stop() override;

    // TRUE THROUGH A RETUNE, AND BELT AND BRACES RATHER THAN THE MECHANISM.
    //
    // run_delivery is what clears running_, and since it no longer exits for a
    // retune it no longer clears it for one either: pause_producer_locked takes
    // down the USB thread alone. This still reads retuning_ because the cost of
    // being wrong is out of proportion to the cost of the check. Engine::run
    // polls exactly this in `while (!stop_requested && source_->running())`, so
    // a moment of false during a retune ends the run: the engine stops the
    // source, flushes the graph and returns, and an operator changing frequency
    // loses the stream instead of moving it.
    //
    // A caller cannot distinguish the two states and should not have to. What
    // running() means to every caller in the tree is "this source intends to
    // keep delivering", and through a retune it does.
    [[nodiscard]] bool running() const override
    {
        return running_.load(std::memory_order_acquire) ||
               retuning_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Status seek(dsp::SampleIndex index) override;

    [[nodiscard]] SourceStats stats() const override;
    [[nodiscard]] ClockQuality clock() const override;

private:
    // Every failure that leaves through a public call names the device, so a
    // refusal read in a log or a window says which dongle refused as well as
    // which librtlsdr call did. The steps below name the call; this names the
    // radio, once, at the edge.
    template <typename T>
    [[nodiscard]] Expected<T> named(Expected<T> result) const
    {
        if (!result) {
            return std::unexpected(with_context(std::move(result.error()), caps_.display_name));
        }
        return result;
    }

    static void usb_callback(unsigned char* buffer, std::uint32_t bytes, void* context);

    void on_transfer(const std::uint8_t* data, std::uint32_t bytes);
    void note_dropped(std::size_t samples);
    void run_usb();
    void run_delivery();
    void note_stream_error(Error error);
    void join_locked();
    [[nodiscard]] Status pause_producer_locked();
    void stop_transfers_locked();
    [[nodiscard]] Status resume_producer_locked();

    // EVERY CONTROL TRANSFER TO A STREAMING DONGLE GOES THROUGH HERE.
    //
    // Not just the retune. rtlsdr_set_center_freq, rtlsdr_set_tuner_gain_mode
    // and rtlsdr_set_tuner_gain are all vendor control transfers through the
    // same I2C repeater, and the platform stalls all of them once
    // rtlsdr_read_async has been running for about half a second. Only tune
    // paused the transfers at first, so switching the tuner to automatic gain
    // from the window took the stall instead and the operator's window locked
    // up waiting on it.
    //
    // `work` is called with the transfers stopped, the caller already holding
    // control_, and the delivery thread still running. It should retry its own
    // transfer, because with librtlsdr v2.0.2 the first one after a cancel
    // usually failed; see kRetunePipeRetries.
    //
    // THE STREAM IS RESTARTED WHATEVER `work` DID, including throwing its hands
    // up, because a source that was running when a control call arrived has to
    // be running when it returns. A resume that itself fails is reported over
    // whatever `work` said, since a source that cannot resume is the larger
    // fact.
    template <typename Work>
    [[nodiscard]] auto with_transfers_paused(Work&& work) -> decltype(work())
    {
        using Result = decltype(work());

        retuning_.store(true, std::memory_order_release);
        pause_epoch_.fetch_add(1, std::memory_order_acq_rel);
        struct ClearOnExit {
            std::atomic<bool>& flag;
            std::atomic<std::uint64_t>& epoch;
            ~ClearOnExit()
            {
                epoch.fetch_add(1, std::memory_order_acq_rel);
                flag.store(false, std::memory_order_release);
            }
        } clear_retuning{retuning_, pause_epoch_};

        const auto paused_at = std::chrono::steady_clock::now();
        const dsp::SampleIndex before_join = produced_index_;

        // NOT RESTARTED WHEN THE TRANSFERS DID NOT STOP CLEANLY. See run_usb:
        // a read_async that returns an error while being cancelled may have
        // freed a transfer libusb still holds, and a restart submits sixteen
        // more onto the list that freed one is still threaded through. The
        // stream ends instead, with the reason, and the control change is not
        // attempted on a device in that state.
        if (auto paused = pause_producer_locked(); !paused) {
            return std::unexpected(with_context(
                paused.error(), "the stream was stopped for a control change and not restarted"));
        }

        // produced_index_ and pending_gap_ are the callback thread's, and the
        // gap accounting below reads and writes both. Safe because the callback
        // thread is the one just joined: the delivery thread still running
        // beside this touches neither, it reads the slots and their gap_before.
        const dsp::SampleIndex during_join = produced_index_ - before_join;

        Result result = work();

        // Flushed whether or not `work` succeeded. The device's buffer has been
        // sitting unread for the length of the pause either way, so what is in
        // it is stale, and on a retune it was digitised at the old centre.
        std::optional<Error> flush_failed;
        if (const int rc = rtlsdr_reset_buffer(device_.get()); rc != 0) {
            flush_failed = Error{
                std::format("the device's sample buffer could not be flushed after a control "
                            "change, so the stream resumes with up to a buffer of samples "
                            "digitised before it: rtlsdr_reset_buffer returned {}",
                            rc_text(rc)),
                rc};
        }

        const double paused_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - paused_at).count();
        const auto produced_while_down =
            static_cast<std::uint64_t>(paused_seconds * static_cast<double>(rate_));
        if (produced_while_down > during_join) {
            note_dropped(static_cast<std::size_t>(produced_while_down - during_join));
        }

        if (auto resumed = resume_producer_locked(); !resumed) {
            return std::unexpected(with_context(
                resumed.error(), "the stream could not be restarted after a control change"));
        }
        if (!result) {
            return result;
        }
        if (flush_failed) {
            return std::unexpected(*flush_failed);
        }
        return result;
    }
    // The device half of each control call, split out so the same body serves a
    // stopped dongle and a streaming one. `attempts` is one when the dongle is
    // not streaming, so a genuine refusal is reported once rather than four
    // times over, and kRetunePipeRetries when it is, because with librtlsdr
    // v2.0.2 the first transfer after a cancel usually failed.
    [[nodiscard]] Expected<dsp::Hertz> tune_locked(dsp::Hertz center, int attempts);
    [[nodiscard]] Expected<double> set_gain_locked(double db, int attempts);
    [[nodiscard]] Status set_gain_auto_locked(bool on, int attempts);
    [[nodiscard]] Expected<dsp::Hertz> retune_streaming_locked(dsp::Hertz center);
    [[nodiscard]] Expected<ClockModel> make_clock_model() const;

    SourceCapabilities caps_{};

    // The machine-wide lock, held for as long as this source exists, which is
    // the whole of the device's open life. Declared before device_ so it is
    // destroyed after it: the handle closes first and only then may another
    // process open the dongle.
    DeviceLock lock_{};
    Device device_{};
    std::vector<int> gain_steps_{};
    bool ppm_given_ = false;

    dsp::SampleRate rate_ = 0;
    std::atomic<dsp::Hertz> center_hz_{0};

    // Guards every control-plane call and the clock model. Never taken by the
    // callback thread and never by the delivery thread, so a stop that is
    // holding it while joining cannot be waiting on a thread that is waiting
    // for it.
    mutable std::mutex control_{};
    ClockModel clock_model_{};

    std::thread usb_thread_{};
    std::thread deliver_thread_{};
    BlockSink sink_{};
    std::size_t block_samples_ = 0;
    std::uint32_t transfer_bytes_ = 0;
    std::int64_t anchor_ns_ = 0;

    std::atomic<bool> running_{false};

    // Held up across the join-and-restart a retune needs, and read by running().
    // Written only under control_, so a second retune cannot overlap the first.
    std::atomic<bool> retuning_{false};

    // Odd while the transfers are paused for a control change, and moved on at
    // both ends of every pause. run_delivery reads it on either side of its
    // exit check, so a pause that began or ended between its reads cannot be
    // taken for the stream ending. retuning_ alone cannot say that: a
    // delivery thread that reads producer_done_ during a pause and retuning_
    // after it has ended sees a finished producer and no retune, and would
    // leave with the restarted transfers still filling the queue. Found by
    // reading, not observed; the window is two loads wide.
    std::atomic<std::uint64_t> pause_epoch_{0};

    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> producer_done_{true};

    // What the last rtlsdr_read_async returned. Written by the USB thread
    // before it sets producer_done_ and read only after that thread has been
    // joined.
    int read_async_rc_ = 0;

    std::vector<Slot> slots_{};
    std::atomic<std::uint64_t> slot_head_{0};
    std::atomic<std::uint64_t> slot_tail_{0};

    // Callback thread only, both of them.
    dsp::SampleIndex produced_index_ = 0;
    std::uint64_t pending_gap_ = 0;

    // The delivery thread's own two counters. Owned by that thread while it
    // runs and only touched here by start(), which is the one point where no
    // delivery thread exists. See run_delivery for why they are not locals.
    std::uint64_t sequence_ = 0;
    dsp::SampleIndex stream_index_ = 0;

    std::mutex error_lock_{};
    Error stop_error_{};
    bool has_stop_error_ = false;

    std::atomic<std::uint64_t> blocks_delivered_{0};
    std::atomic<std::uint64_t> samples_delivered_{0};
    std::atomic<std::uint64_t> overrun_events_{0};
    std::atomic<std::uint64_t> samples_lost_{0};
    std::atomic<dsp::SampleIndex> last_loss_index_{0};
    std::atomic<dsp::SampleIndex> write_index_{0};
};

RtlSdrSource::~RtlSdrSource()
{
    std::scoped_lock lock(control_);
    join_locked();
}

Status RtlSdrSource::open(const RtlSdrSourceConfig& config, DeviceLock lock, Device device,
                          SourceCapabilities caps, std::vector<int> gain_steps,
                          dsp::SampleRate rate, dsp::Hertz center)
{
    caps_ = std::move(caps);
    lock_ = std::move(lock);
    device_ = std::move(device);
    gain_steps_ = std::move(gain_steps);
    ppm_given_ = config.ppm_given;
    rate_ = rate;
    center_hz_.store(center, std::memory_order_release);

    auto model = make_clock_model();
    if (!model) {
        return std::unexpected(with_context(model.error(), "RTL-SDR clock"));
    }
    clock_model_ = std::move(*model);
    return {};
}

Expected<ClockModel> RtlSdrSource::make_clock_model() const
{
    ClockModelConfig clock_config;
    clock_config.source = ClockSource::Internal;
    clock_config.rate = rate_;
    clock_config.anchor_accuracy_ns = kAnchorAccuracyNs;
    clock_config.oscillator_tolerance_ppm =
        ppm_given_ ? kCalibratedTolerancePpm : kUncalibratedTolerancePpm;
    return ClockModel::create(clock_config);
}

Expected<dsp::Hertz> RtlSdrSource::tune(dsp::Hertz center)
{
    if (center < 0) {
        return fail(std::format("a centre frequency of {} Hz is below DC", center));
    }
    if (center > static_cast<dsp::Hertz>(std::numeric_limits<std::uint32_t>::max())) {
        return fail(std::format("{} Hz is past the unsigned 32-bit hertz count "
                                "rtlsdr_set_center_freq takes",
                                center));
    }
    if (!caps_.can_tune(center)) {
        return fail(std::format("this dongle cannot tune {} Hz. It reaches {}.", center,
                                tune_ranges_text(caps_.tune_ranges)));
    }

    std::scoped_lock lock(control_);

    // A STREAMING DONGLE HAS TO BE PAUSED TO BE RETUNED, AND THIS IS NOT OURS.
    //
    // rtlsdr_set_center_freq needs the RTL2832U's I2C repeater, which is a
    // vendor control transfer, and on this platform that transfer stalls with
    // LIBUSB_ERROR_PIPE once rtlsdr_read_async has been running for about half a
    // second. Measured on an R820T on 2026-09-21, one retune per run so nothing
    // could be blamed on a previous one: a retune at 252 ms lands, one at 522 ms
    // and every one after it is refused, and the URB length does not move that
    // boundary in either direction (16 KiB, 64 KiB and 512 KiB transfers all
    // fail at 330 ms). librtlsdr with nothing of ours in the picture at all,
    // rtlsdr_open through rtlsdr_read_async on a bare thread, does exactly the
    // same thing: 0 at 310 ms, -9 at 1026 ms. So this is librtlsdr, libusb or
    // the WinUSB binding, and no amount of care on this side makes that transfer
    // go through.
    //
    // Issuing it from the USB thread instead is not the way out: from inside the
    // read_async callback the same call returns LIBUSB_ERROR_BUSY, because a
    // synchronous transfer submitted from within libusb's own event handling
    // cannot complete.
    //
    // What does work, six rounds out of six, is stopping the transfers around
    // it. tests/engine/test_rtlsdr_source.cpp carries all of that as [.probe]
    // cases, which is where the numbers above come from and how a later
    // librtlsdr, or a dongle that does not have this problem, gets checked
    // rather than assumed.
    return named(running_.load(std::memory_order_acquire) ? retune_streaming_locked(center)
                                                          : tune_locked(center, 1));
}

// One attempt for a dongle that is not streaming and several for one that has
// just been paused. See kRetunePipeRetries for why the first transfer after a
// cancel was expected to fail on librtlsdr v2.0.2, and note that a stopped
// dongle gets a single attempt so that a real refusal is reported as one rather
// than four times over.
Expected<dsp::Hertz> RtlSdrSource::tune_locked(dsp::Hertz center, int attempts)
{
    const auto requested = static_cast<std::uint32_t>(center);

    int rc = 0;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        rc = rtlsdr_set_center_freq(device_.get(), requested);
        if (rc == 0) {
            break;
        }
    }
    if (rc != 0) {
        return fail(std::format("the tuner refused {} Hz: rtlsdr_set_center_freq returned {}{}",
                                center, rc_text(rc), attempts_text(attempts)),
                    rc);
    }

    // Read back rather than echoed, and what comes back is the device's own
    // statement of where it is tuned rather than this backend's opinion.
    //
    // Measured on an R820T2 at 100 MHz, librtlsdr returns the request
    // unchanged: this tuner's driver records the frequency it was asked for
    // and does not report the PLL's actual landing. So the difference is zero
    // today, on this part. It is still read back rather than echoed, because
    // the difference is not zero on every tuner, is not zero under a ppm
    // correction, and is not zero under direct sampling, and a caller
    // subtracting these two integers has to be reading the device's answer
    // for that subtraction to mean anything. Both ends are integer hertz, so
    // whatever the difference is, it is exact.
    const std::uint32_t achieved = rtlsdr_get_center_freq(device_.get());
    if (achieved == 0 && requested != 0) {
        return fail(std::format(
            "the tuner reported a centre of zero after being asked for {} Hz, which rtl-sdr.h "
            "documents as its error return",
            center));
    }

    const auto landed = static_cast<dsp::Hertz>(achieved);
    center_hz_.store(landed, std::memory_order_release);
    return landed;
}

Expected<dsp::Hertz> RtlSdrSource::retune_streaming_locked(dsp::Hertz center)
{
    // THE TRANSFERS STOP, THE TUNER MOVES, THE TRANSFERS RESTART, AND THE
    // SAMPLES THAT WENT MISSING ARE REPORTED AS MISSING. The delivery thread
    // stays up throughout; see pause_producer_locked for why that is not
    // optional.
    //
    // What must not change across this is the sample index and the clock
    // anchor. A Paced source's timestamp is the anchor plus the index over the
    // rate, so if the roughly 790,000 samples the device produced during the
    // pause were simply not counted, every timestamp after the retune would be
    // a third of a second early and stay that way for the life of the stream.
    // note_dropped is what keeps that honest: it advances produced_index_ past
    // them, carries the count into the next block's dropped_before, and files
    // an overrun event, which is the same treatment a consumer too slow to keep
    // up already gets. So the gap is visible in SourceStats::samples_lost, on
    // the block boundary, and in the window, rather than being smoothed over.
    //
    // THE STREAM IS RESTARTED ON EVERY PATH OUT OF HERE, WHICH IS WHY THERE IS
    // ONLY ONE. Everything between the join and the resume records what went
    // wrong instead of returning, because a source that was running when this
    // was called has to be running when it returns however badly the retune
    // went: a client that typed a frequency this dongle cannot reach, or a
    // device that refuses to flush its buffer, must not cost the operator the
    // radio they were already listening to. An early return anywhere in the
    // middle would do exactly that, and it would do it silently, because
    // running() is held true across the pause.
    return with_transfers_paused([this, center] { return tune_locked(center, kRetunePipeRetries); });
}

// THE TRANSFERS STOP AND THE DELIVERY THREAD DOES NOT.
//
// Only the USB side has to go for the control transfer to get through, and only
// the USB side may go: Graph::on_block is documented "the source thread only",
// this source's delivery thread is that thread, and handing on_block to a
// freshly spawned one silently killed a receiver's audio while leaving every
// other sign of life intact. So a retune joins usb_thread_ and leaves
// deliver_thread_ running, draining whatever is already queued and then idling
// on an empty queue until the transfers come back. run_delivery's exit check is
// what makes that safe, and pause_epoch_ is what it reads.
//
// Fails when read_async did not come back cleanly, which the caller takes as
// the end of the stream rather than restarting it. See run_usb for what that
// return means.
Status RtlSdrSource::pause_producer_locked()
{
    stop_transfers_locked();

    if (read_async_rc_ != 0) {
        return fail(std::format("rtlsdr_read_async returned {} when its transfers were stopped",
                                rc_text(read_async_rc_)),
                    read_async_rc_);
    }
    return {};
}

// Cancels the transfers and joins the USB thread, and does nothing else. Shared
// by a pause and a stop so the one rule about cancelling lives in one place.
void RtlSdrSource::stop_transfers_locked()
{
    cancel_requested_.store(true, std::memory_order_release);

    if (!usb_thread_.joinable()) {
        return;
    }

    // Retried until it is accepted, and then not again.
    //
    // rtlsdr_cancel_async only arms the cancel once rtlsdr_read_async has
    // reached its running state. Called before that it returns -2 and does
    // nothing, and read_async then never returns; the window is the few
    // milliseconds between spawning the thread and libusb's loop being armed,
    // which is exactly where a stop right after a start or a second control
    // change right after a first one lands. Measured: with no gap between
    // control changes, 750 pauses out of 750 needed a second call.
    //
    // WHAT THIS PARAGRAPH USED TO SAY: that retrying past the first acceptance
    // is dangerous because "a second call arriving while the first cancel is
    // still draining takes librtlsdr's other branch, which forces the async
    // state straight to inactive". Not on the librtlsdr this tree links. A
    // probe calling rtlsdr_cancel_async twice back to back on a streaming
    // dongle got 0 then -2 in four rounds out of four, and read_async returned
    // 0 each time. Stopping at the first acceptance is still right, because a
    // second call has nothing left to do.
    while (!producer_done_.load(std::memory_order_acquire)) {
        if (rtlsdr_cancel_async(device_.get()) == 0) {
            break;
        }
        std::this_thread::sleep_for(kCancelPoll);
    }
    usb_thread_.join();
}

Status RtlSdrSource::resume_producer_locked()
{
    cancel_requested_.store(false, std::memory_order_relaxed);
    producer_done_.store(false, std::memory_order_release);

    try {
        usb_thread_ = std::thread([this] { run_usb(); });
    } catch (const std::system_error& error) {
        // producer_done_ back to true, so the delivery thread still running
        // beside this finishes once retuning_ clears rather than idling for
        // ever on a queue nothing will fill again.
        producer_done_.store(true, std::memory_order_release);
        return fail(std::format("could not restart the RTL-SDR transfer thread: {}", error.what()),
                    error.code().value());
    }

    return {};
}

Expected<dsp::SampleRate> RtlSdrSource::set_sample_rate(dsp::SampleRate rate)
{
    std::scoped_lock lock(control_);

    if (running_.load(std::memory_order_acquire)) {
        return fail(std::format(
            "the RTL-SDR is streaming at {} S/s and the rate is fixed for the life of a stream. "
            "Every block already delivered is indexed against it and the channel grid above was "
            "built from it, so changing it now would relabel a stream mid-flight. Stop, set {} "
            "S/s, start again.",
            rate_, rate));
    }
    if (!rtlsdr_rate_supported(rate)) {
        return fail(std::format("{} S/s is not a rate an RTL-SDR can take. It reaches {}.", rate,
                                rate_ranges_text()));
    }

    if (const int rc = rtlsdr_set_sample_rate(device_.get(), static_cast<std::uint32_t>(rate));
        rc != 0) {
        return fail(std::format("{}: the device refused {} S/s: rtlsdr_set_sample_rate "
                                "returned {}",
                                caps_.display_name, rate, rc_text(rc)),
                    rc);
    }

    // The RTL2832U resamples with a fractional divider off a 28.8 MHz
    // crystal, so a request that is not exactly representable lands nearby.
    // What comes back is what the samples were actually taken at.
    const std::uint32_t achieved = rtlsdr_get_sample_rate(device_.get());
    if (achieved == 0) {
        return fail(std::format("{}: the device reported a sample rate of zero after being set "
                                "to {} S/s, which rtl-sdr.h documents as its error return",
                                caps_.display_name, rate));
    }

    rate_ = static_cast<dsp::SampleRate>(achieved);
    return rate_;
}

Expected<double> RtlSdrSource::set_gain(std::string_view stage, double db)
{
    if (stage != kTunerStage) {
        return fail(std::format(
            "an RTL-SDR has no gain stage called '{}'. It has one, '{}', which on an R820T "
            "drives the LNA, the mixer and the VGA together from a single table.",
            stage, kTunerStage));
    }
    if (!std::isfinite(db)) {
        return fail("a gain has to be a finite number of decibels");
    }
    if (gain_steps_.empty()) {
        return fail(std::format(
            "this dongle reports no tuner gain table, so '{}' cannot be set manually. Put it "
            "into automatic mode instead.",
            kTunerStage));
    }

    std::scoped_lock lock(control_);

    // Paused around it while streaming, for the reason with_transfers_paused
    // gives: these are the same I2C-repeater transfers a retune uses and the
    // platform stalls them the same way.
    if (running_.load(std::memory_order_acquire)) {
        return named(with_transfers_paused(
            [this, db] { return set_gain_locked(db, kRetunePipeRetries); }));
    }
    return named(set_gain_locked(db, 1));
}

Expected<double> RtlSdrSource::set_gain_locked(double db, int attempts)
{
    const auto wanted = static_cast<int>(std::llround(db * 10.0));
    const int landed = nearest_step(gain_steps_, wanted);

    int mode_rc = 0;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        mode_rc = rtlsdr_set_tuner_gain_mode(device_.get(), 1);
        if (mode_rc == 0) {
            break;
        }
    }
    if (mode_rc != 0) {
        return fail(std::format("could not put the tuner into manual gain mode: "
                                "rtlsdr_set_tuner_gain_mode returned {}{}",
                                rc_text(mode_rc), attempts_text(attempts)),
                    mode_rc);
    }

    int gain_rc = 0;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        gain_rc = rtlsdr_set_tuner_gain(device_.get(), landed);
        if (gain_rc == 0) {
            break;
        }
    }
    if (gain_rc != 0) {
        return fail(std::format("the tuner refused a gain of {} dB: rtlsdr_set_tuner_gain "
                                "returned {}{}",
                                static_cast<double>(landed) / 10.0, rc_text(gain_rc),
                                attempts_text(attempts)),
                    gain_rc);
    }

    // Zero is both a valid gain in the R820T's table and the documented error
    // return, so it is only a failure when zero is not what was asked for.
    const int achieved = rtlsdr_get_tuner_gain(device_.get());
    if (achieved == 0 && landed != 0) {
        return fail(std::format("the tuner reported no gain after being set to {} dB, which "
                                "rtl-sdr.h documents as its error return",
                                static_cast<double>(landed) / 10.0));
    }
    return static_cast<double>(achieved) / 10.0;
}

Status RtlSdrSource::set_gain_auto(std::string_view stage, bool on)
{
    if (stage != kTunerStage) {
        return fail(std::format("an RTL-SDR has no gain stage called '{}' to put into automatic "
                                "mode. It has one, '{}'.",
                                stage, kTunerStage));
    }

    std::scoped_lock lock(control_);

    // THE CALL THAT FROZE THE WINDOW. Switching the tuner's gain mode is the
    // same class of transfer as a retune, and it was going straight at a
    // streaming dongle: see with_transfers_paused.
    if (running_.load(std::memory_order_acquire)) {
        return named(with_transfers_paused(
            [this, on] { return set_gain_auto_locked(on, kRetunePipeRetries); }));
    }
    return named(set_gain_auto_locked(on, 1));
}

Status RtlSdrSource::set_gain_auto_locked(bool on, int attempts)
{
    // rtlsdr_set_tuner_gain_mode takes "manual", so the sense is inverted
    // here rather than at every call site.
    int rc = 0;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        rc = rtlsdr_set_tuner_gain_mode(device_.get(), on ? 0 : 1);
        if (rc == 0) {
            break;
        }
    }
    if (rc != 0) {
        return fail(std::format("could not switch the tuner to {} gain: "
                                "rtlsdr_set_tuner_gain_mode returned {}{}",
                                on ? "automatic" : "manual", rc_text(rc),
                                attempts_text(attempts)),
                    rc);
    }
    return {};
}

Status RtlSdrSource::seek(dsp::SampleIndex index)
{
    return fail(std::format(
        "an RTL-SDR cannot seek to sample {}: it is a Paced source and its samples do not exist "
        "until the device produces them. Seeking is a Demand capability, and "
        "SourceCapabilities::seekable says so before anything tries. Record to a file and open "
        "that to move around inside a capture.",
        index));
}

Status RtlSdrSource::start(const StreamOptions& options, BlockSink sink)
{
    std::scoped_lock lock(control_);

    if (running_.load(std::memory_order_acquire) || usb_thread_.joinable() ||
        deliver_thread_.joinable()) {
        return fail("the RTL-SDR is already streaming");
    }
    if (!sink) {
        return fail("a source cannot be started without a sink");
    }
    if (options.start_index != 0) {
        return fail(std::format(
            "an RTL-SDR cannot start at sample {}. A Paced source's sample zero is whatever the "
            "device hands over first; start_index belongs to a Demand source reading something "
            "that already exists.",
            options.start_index));
    }

    // options.pace is ignored on purpose and it is not an error to set it.
    // The crystal is already holding the stopwatch, so there is nothing for a
    // pace multiplier to do, and refusing it would make Engine::run fail for
    // every caller that asked for realtime monitoring without knowing which
    // backend it had.

    std::size_t block = options.block_samples;
    if (block == 0) {
        block = caps_.preferred_block_samples;
    }
    if (block == 0) {
        block = kPreferredBlockSamples;
    }
    if (block > kMaxBlockSamples) {
        // Same ceiling and the same reason as the synthetic source, and it
        // also keeps the byte arithmetic below inside 64 bits for any value a
        // caller can put in a size_t.
        return fail(std::format("a block of {} samples is past the {} sample ceiling; that is a "
                                "buffer, not a block",
                                block, kMaxBlockSamples));
    }

    // The transfer is sized from the block so that one URB buffer carries a
    // whole number of blocks wherever it can. A block larger than the
    // transfer ceiling is honoured as an upper bound rather than exactly:
    // SourceBlock carries its own sample_count and a consumer that asked for
    // at most N is never handed more.
    const std::uint64_t wanted_bytes = static_cast<std::uint64_t>(block) * kBytesPerSample;
    const std::uint64_t rounded = ((wanted_bytes + kUrbBytes - 1) / kUrbBytes) * kUrbBytes;
    transfer_bytes_ =
        static_cast<std::uint32_t>(std::clamp(rounded, kUrbBytes, kMaxTransferBytes));

    slots_.assign(static_cast<std::size_t>(kSlotCount), Slot{});
    for (Slot& slot : slots_) {
        slot.bytes.assign(static_cast<std::size_t>(transfer_bytes_), 0);
        slot.samples = 0;
        slot.gap_before = 0;
    }

    slot_head_.store(0, std::memory_order_relaxed);
    slot_tail_.store(0, std::memory_order_relaxed);
    produced_index_ = 0;
    pending_gap_ = 0;
    sequence_ = 0;
    stream_index_ = 0;

    blocks_delivered_.store(0, std::memory_order_relaxed);
    samples_delivered_.store(0, std::memory_order_relaxed);
    overrun_events_.store(0, std::memory_order_relaxed);
    samples_lost_.store(0, std::memory_order_relaxed);
    last_loss_index_.store(0, std::memory_order_relaxed);
    write_index_.store(0, std::memory_order_relaxed);

    {
        std::scoped_lock errors(error_lock_);
        stop_error_ = Error{};
        has_stop_error_ = false;
    }

    // Flushes whatever the dongle's FIFO accumulated while it was configured
    // but not being read. Without it the first block is stale by however long
    // the setup took, and the anchor below would be attached to samples that
    // were digitised before it was read.
    if (const int rc = rtlsdr_reset_buffer(device_.get()); rc != 0) {
        return fail(std::format("{}: could not reset the device's sample buffer: "
                                "rtlsdr_reset_buffer returned {}",
                                caps_.display_name, rc_text(rc)),
                    rc);
    }

    auto model = make_clock_model();
    if (!model) {
        return std::unexpected(with_context(model.error(), "RTL-SDR clock"));
    }
    clock_model_ = std::move(*model);

    // Once, here, between the FIFO flush and the first transfer going out.
    // Every later timestamp is derived from the sample index against this,
    // never from another clock read. See kAnchorAccuracyNs for what the
    // remaining error is and why it is not worth chasing.
    const std::int64_t anchor = unix_now_ns();
    if (auto anchored = clock_model_.set_anchor(anchor); !anchored) {
        return std::unexpected(with_context(anchored.error(), "RTL-SDR clock"));
    }
    anchor_ns_ = anchor;

    block_samples_ = block;
    sink_ = std::move(sink);
    cancel_requested_.store(false, std::memory_order_relaxed);
    producer_done_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    try {
        deliver_thread_ = std::thread([this] { run_delivery(); });
    } catch (const std::system_error& error) {
        producer_done_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        return fail(std::format("could not start the RTL-SDR delivery thread: {}", error.what()),
                    error.code().value());
    }

    try {
        usb_thread_ = std::thread([this] { run_usb(); });
    } catch (const std::system_error& error) {
        producer_done_.store(true, std::memory_order_release);
        deliver_thread_.join();
        running_.store(false, std::memory_order_release);
        return fail(std::format("could not start the RTL-SDR transfer thread: {}", error.what()),
                    error.code().value());
    }

    return {};
}

Status RtlSdrSource::stop()
{
    std::scoped_lock lock(control_);
    join_locked();

    std::scoped_lock errors(error_lock_);
    if (has_stop_error_) {
        return std::unexpected(with_context(stop_error_, caps_.display_name));
    }
    return {};
}

void RtlSdrSource::join_locked()
{
    stop_transfers_locked();

    // Joined after the producer, never before. The delivery thread finishes
    // draining once producer_done_ is set, so the queue is empty when this
    // returns and no sample the device handed over has gone unaccounted.
    if (deliver_thread_.joinable()) {
        deliver_thread_.join();
    }

    // A NOTE ON THE MESSAGE THAT SHOWS UP AFTER THIS, so it is not
    // rediagnosed as a device fault.
    //
    // On Windows, the first control transfer issued after the bulk transfers
    // have been cancelled sometimes comes back LIBUSB_ERROR_PIPE, and
    // librtlsdr prints it on its own way out:
    //
    //   rtlsdr_demod_write_reg failed with -9
    //   r82xx_write: i2c wr failed=-9 reg=06 len=1
    //
    // That pair is rtlsdr_close enabling the I2C repeater to put the tuner
    // into standby. It is one transfer: everything after it in the same close
    // succeeds, the device reopens cleanly, and it appears on maybe two runs
    // in three. It is also entirely after the last sample, so no capture is
    // affected by it. Opening and closing without streaming never produces
    // it.
    //
    // WHAT THIS PARAGRAPH USED TO SAY after that: "which is what pins it to
    // the cancel rather than to anything here". The streaming is what it
    // follows, and the cancel is not needed. On 2026-09-23 a probe that read
    // with rtlsdr_read_sync, so that no transfer was ever cancelled, printed
    // the same pair at rtlsdr_close on two runs out of two.
    //
    // It cannot be absorbed by issuing a throwaway transfer first, because
    // every register accessor in librtlsdr prints its own failure: that would
    // relabel the line, not remove it. Suppressing the library's stderr to
    // hide it would hide real faults with it.

    running_.store(false, std::memory_order_release);
}

SourceStats RtlSdrSource::stats() const
{
    SourceStats out;
    out.blocks_delivered = blocks_delivered_.load(std::memory_order_relaxed);
    out.samples_delivered = samples_delivered_.load(std::memory_order_relaxed);
    out.overrun_events = overrun_events_.load(std::memory_order_relaxed);
    out.samples_lost = samples_lost_.load(std::memory_order_relaxed);
    out.last_loss_index = last_loss_index_.load(std::memory_order_relaxed);
    out.write_index = write_index_.load(std::memory_order_relaxed);
    return out;
}

ClockQuality RtlSdrSource::clock() const
{
    std::scoped_lock lock(control_);

    ClockQuality quality = clock_model_.quality();
    if (!clock_model_.anchored()) {
        // Nothing has been placed on the wall clock yet. Reporting the
        // model's zero here would claim perfect knowledge of an anchor that
        // does not exist, so the answer is the accuracy the anchor will have
        // when it is taken.
        quality.accuracy_ns = kAnchorAccuracyNs;
        return quality;
    }
    quality.accuracy_ns =
        clock_model_.accuracy_ns_at(write_index_.load(std::memory_order_relaxed));
    return quality;
}

void RtlSdrSource::usb_callback(unsigned char* buffer, std::uint32_t bytes, void* context)
{
    auto* self = static_cast<RtlSdrSource*>(context);
    if (self == nullptr || buffer == nullptr) {
        return;
    }
    self->on_transfer(reinterpret_cast<const std::uint8_t*>(buffer), bytes);
}

void RtlSdrSource::note_dropped(std::size_t samples)
{
    if (samples == 0) {
        return;
    }
    overrun_events_.fetch_add(1, std::memory_order_relaxed);
    samples_lost_.fetch_add(samples, std::memory_order_relaxed);
    last_loss_index_.store(produced_index_, std::memory_order_relaxed);
    produced_index_ += samples;
    pending_gap_ += samples;
}

void RtlSdrSource::on_transfer(const std::uint8_t* data, std::uint32_t bytes)
{
    const std::size_t samples = static_cast<std::size_t>(bytes) / kBytesPerSample;
    if (samples == 0) {
        return;
    }

    const std::size_t room = static_cast<std::size_t>(transfer_bytes_) / kBytesPerSample;
    if (samples > room) {
        // librtlsdr hands back a buffer of the length it was given, so this
        // cannot fire. It is here because the alternative to one comparison
        // per transfer is a heap overwrite driven by a USB device.
        note_dropped(samples);
        return;
    }

    const std::uint64_t head = slot_head_.load(std::memory_order_relaxed);

    // Acquire, pairing with the delivery thread's release store of tail. It
    // is what guarantees the reader has finished with the slot this call is
    // about to fill.
    const std::uint64_t tail = slot_tail_.load(std::memory_order_acquire);

    if (head - tail >= kSlotCount) {
        note_dropped(samples);
        return;
    }

    Slot& slot = slots_[static_cast<std::size_t>(head & kSlotMask)];
    std::memcpy(slot.bytes.data(), data, samples * kBytesPerSample);
    slot.samples = samples;
    slot.gap_before = pending_gap_;
    pending_gap_ = 0;
    produced_index_ += samples;

    // Release: every byte of the memcpy happens before the reader observes
    // the new head.
    slot_head_.store(head + 1, std::memory_order_release);
}

void RtlSdrSource::run_usb()
{
    const int rc = rtlsdr_read_async(device_.get(), &RtlSdrSource::usb_callback, this,
                                     kTransferCount, transfer_bytes_);

    read_async_rc_ = rc;

    if (!cancel_requested_.load(std::memory_order_acquire)) {
        // read_async returning without anybody asking it to means the
        // transfers stopped, which in practice means the dongle was unplugged
        // or reset. Recorded rather than swallowed: without it the stream
        // simply ends and a recording that stops early looks the same as one
        // that finished.
        note_stream_error(Error{
            std::format("the RTL-SDR stopped delivering samples: rtlsdr_read_async returned {} "
                        "without a cancel having been asked for, which usually means the device "
                        "was unplugged or reset",
                        rc_text(rc)),
            rc});
    } else if (rc != 0) {
        // AN ERROR FROM A CANCELLED read_async IS THE CRASH IN CI, ARRIVING.
        //
        // Measured on 2026-09-23 with full page heap on the test binary and a
        // probe that makes control changes on a streaming dongle back to back:
        // 3 of 708 cancelled read_async calls returned -5, LIBUSB_ERROR_NOT_FOUND,
        // where every other one returned 0, and two of those three processes
        // died within milliseconds on an access to freed memory. The faulting
        // frames were libusb's own: once in windows_iocp_thread unlinking a
        // completed transfer, once in add_to_flying_list under the next
        // rtlsdr_demod_write_reg, walking libusb's list of transfers in flight.
        // In both the transfer being touched had been freed. NOT_FOUND is what
        // libusb_cancel_transfer answers for a transfer already being
        // cancelled, so read_async is reporting that it cancelled one twice
        // and returned before its completion arrived, and the transfer it then
        // freed was still in flight. The two SegFaults in CI on 2026-09-23 fit
        // it and left no dump to confirm it: one died inside a retune after
        // its first control transfer, the other after the stop that followed
        // three retunes.
        //
        // HOW OFTEN, without page heap, same probe: 2 of 553 cancels with
        // sixteen 64 KiB transfers and 2 of 943 with four 256 KiB ones, so the
        // transfer size is not the lever. A cancel does not stop the
        // transfers already queued: every one of 518 cancels was followed by
        // 15 to 56 more completed transfers carrying samples before read_async
        // returned. Holding the callback 5 ms per transfer after a cancel, to
        // see whether the teardown was racing the callback, made it worse,
        // 5 of 89, so this callback stays as short as it is.
        //
        // Nothing reachable through rtl-sdr.h stops librtlsdr doing that, and
        // nothing afterwards can make the device's libusb state safe again.
        // What this side can do is refuse to build on it: the pause that
        // asked for the cancel does not restart the transfers, the stream
        // ends, and the reason reaches whoever stops it. With that in place,
        // 3 of 518 cancels returned -5 and two of the three processes carried
        // on with the named error rather than dying; the third still died, so
        // this narrows the crash and does not close it.
        //
        // WHAT CLOSED IT, 2026-09-23: the librtlsdr this tree links is no
        // longer the registry's v2.0.2. vcpkg-overlays/rtlsdr builds it with
        // cancel-waits-for-transfers.diff, which makes read_async wait for
        // libusb to hand back every transfer before freeing any. Over 3000
        // cancels each with tools/rtlsdr-cancel-trial, v2.0.2 returned -5 27
        // times and 22 processes died; the patched library returned -5 none
        // of 3000 and none died, and none of 3000 more under full page heap.
        // docs/rtlsdr-provenance.md has the table. This branch stays: it is
        // the right answer to any error from a cancelled read, and a library
        // that returns one has left libusb in a state nobody should build on.
        note_stream_error(Error{
            std::format("rtlsdr_read_async returned {} as its transfers were cancelled, where a "
                        "clean stop returns 0. librtlsdr has returned before libusb finished "
                        "cancelling a transfer, and the stream has been ended rather than "
                        "restarted on top of it",
                        rc_text(rc)),
            rc});
    }

    producer_done_.store(true, std::memory_order_release);
}

void RtlSdrSource::run_delivery()
{
    // MEMBERS, NOT LOCALS, because this thread is joined and restarted by a
    // retune while the stream it is delivering carries on.
    //
    // They were locals until 2026-09-21, which was correct while the only way
    // this thread ended was the stream ending. retune_streaming_locked has to
    // stop the transfers to move the tuner, and restarting delivery with a
    // fresh local zero would send the consumer a second block sequence 0 at
    // sample 0, so the block after a retune would claim the timestamp of the
    // first block of the capture. That is not a cosmetic slip: every consumer
    // in the tree keys off stream_index, so the retune would silently rewind
    // the whole recording by however long it had been running.
    std::uint64_t& sequence = sequence_;
    dsp::SampleIndex& stream_index = stream_index_;

    while (true) {
        const std::uint64_t tail = slot_tail_.load(std::memory_order_relaxed);

        // producer_done_ BEFORE head, and the order is the whole correctness
        // of the shutdown.
        //
        // Read the other way round, a transfer published between the two
        // loads is lost: the delivery thread sees an empty queue, the USB
        // thread then publishes its last slot and sets done, and the check
        // breaks the loop with that slot unread. Up to one whole transfer
        // disappears, counted neither in samples_delivered nor in
        // samples_lost, so the invariant this file's header states is false
        // for that run with every counter reading zero and the recording
        // quietly short.
        //
        // Loading done first makes the pair safe by construction. If done was
        // already true, the producer has finished and the head read below
        // sees everything it will ever publish. If it was false, a queue that
        // reads empty is genuinely empty at this instant and the loop sleeps
        // and looks again.
        const std::uint64_t epoch_before = pause_epoch_.load(std::memory_order_acquire);
        const bool producer_finished = producer_done_.load(std::memory_order_acquire);

        // Acquire, pairing with the callback thread's release store of head.
        // Without it the memcpy below can read bytes the writer has not
        // published.
        const std::uint64_t head = slot_head_.load(std::memory_order_acquire);

        if (tail == head) {
            // A RETUNE IS NOT THE PRODUCER FINISHING, and this thread must not
            // treat it as one.
            //
            // retune_streaming_locked stops the USB transfers to move the
            // tuner, which sets producer_done_ exactly as the end of a stream
            // does. Exiting here would end delivery mid-stream, and the whole
            // point of retuning without a source change is that the stream
            // carries on: Graph::on_block is documented "the source thread
            // only", and this thread is that thread. Measured before this
            // check existed: three retunes and a receiver's audio stopped at 20
            // chunks and never resumed, while the receiver id, the spectrum
            // frames and the engine all stayed up, because the graph waits for
            // each block's frame on whichever thread called on_block and that
            // thread had gone.
            //
            // The epoch is read on both sides of producer_done_, because a
            // single read of a flag cannot tell "no pause" from "a pause that
            // ended between my two loads". Odd at either read, or different
            // between them, is a pause, and a pause is never the end.
            if (producer_finished) {
                const std::uint64_t epoch_after = pause_epoch_.load(std::memory_order_acquire);
                const bool paused = (epoch_before & 1U) != 0 || epoch_after != epoch_before;
                if (!paused) {
                    break;
                }
            }
            std::this_thread::sleep_for(kIdlePoll);
            continue;
        }

        Slot& slot = slots_[static_cast<std::size_t>(tail & kSlotMask)];

        // The gap belongs to the first block after it and to no other, which
        // is why dropped_before is zeroed once it has been reported.
        stream_index += slot.gap_before;
        std::uint64_t gap = slot.gap_before;

        bool finished = false;
        std::size_t offset = 0;
        while (offset < slot.samples) {
            const std::size_t count = std::min(block_samples_, slot.samples - offset);

            SourceBlock block;
            block.stamp = dsp::BlockTimestamp{stream_index, anchor_ns_, rate_};
            block.format = SampleFormat::Cu8;
            block.sample_count = count;
            block.bytes = std::as_bytes(std::span<const std::uint8_t>(
                slot.bytes.data() + offset * kBytesPerSample, count * kBytesPerSample));
            block.dropped_before = gap;
            block.sequence = sequence;

            if (Status delivered = sink_(block); !delivered) {
                // A sink that fails after a stop was asked for is the stop
                // arriving, not a fault. Graph::cancel makes on_block refuse
                // precisely so a parked source wakes up, and reporting that
                // as the reason the stream ended would turn every ordinary
                // Ctrl-C into an error.
                //
                // A RETUNE IS NOT A STOP, THOUGH IT ASKS FOR A CANCEL.
                // pause_producer_locked sets cancel_requested_ to take the USB
                // thread down, so without this a sink failure anywhere in the
                // third of a second the tuner is moving was swallowed AND
                // ended delivery: the stream stopped dead, samples_delivered
                // froze, audio stopped, and stop() reported success because
                // the error had been discarded on the way past. Reported and
                // not swallowed, so whatever the sink is refusing says so.
                const bool stopping = cancel_requested_.load(std::memory_order_acquire) &&
                                      !retuning_.load(std::memory_order_acquire);
                if (!stopping) {
                    note_stream_error(delivered.error());
                }
                finished = true;
                break;
            }

            gap = 0;
            offset += count;
            stream_index += count;
            ++sequence;
            blocks_delivered_.fetch_add(1, std::memory_order_relaxed);
            samples_delivered_.fetch_add(count, std::memory_order_relaxed);
            write_index_.store(stream_index, std::memory_order_relaxed);
        }

        // Release: the reads above happen before the writer observes the new
        // tail and starts overwriting the slot.
        slot_tail_.store(tail + 1, std::memory_order_release);

        if (finished) {
            // Whatever is still queued is abandoned rather than counted as
            // lost. It reached this source but never reached the consumer,
            // and the consumer is the thing that just refused to take any
            // more, so calling it an overrun would report a device fault
            // where there was a deliberate stop. The delivered stream is
            // still self-consistent: write_index is where delivery actually
            // reached, and no gap inside it goes unreported.
            break;
        }
    }

    running_.store(false, std::memory_order_release);
}

void RtlSdrSource::note_stream_error(Error error)
{
    std::scoped_lock lock(error_lock_);
    if (has_stop_error_) {
        // The first failure is the one that explains the rest.
        return;
    }
    stop_error_ = std::move(error);
    has_stop_error_ = true;
}

// Writes the whole configuration onto an open device and reports what each
// part actually achieved.
//
// THE ORDER IS NOT ARBITRARY, and the reason is not only dependency.
// librtlsdr re-tunes the device as a side effect of several of these calls:
// set_direct_sampling ends by re-applying the handle's current frequency,
// set_sample_rate does the same through the R820T's bandwidth setter, and
// set_freq_correction re-applies both the rate and the frequency because the
// correction scales the crystal figure they are derived from. A freshly
// opened handle's frequency is zero, so any of those running before the
// frequency has been set makes the tuner try to lock DC and print
// "PLL not locked" on its way to failing. It is harmless and it is also a
// line of noise in front of an operator who then has to find out it means
// nothing.
//
// So the frequency goes on early, the rate after it, and the correction
// after both, and the achieved values are read back at the end rather than
// beside the call that set them, because a later step can move an earlier
// one.
struct Applied {
    dsp::SampleRate rate = 0;
    dsp::Hertz center = 0;
};

[[nodiscard]] Expected<Applied> configure(rtlsdr_dev_t* device, const RtlSdrSourceConfig& config,
                                          const std::vector<int>& gain_steps,
                                          const std::vector<TuneRange>& tune_ranges)
{
    // First, because it changes what a frequency means: with direct sampling
    // on, rtlsdr_set_center_freq drives the RTL2832U's DDC rather than the
    // tuner.
    //
    // Written only when it differs from what the device reports, which for
    // the ordinary case of direct=off at open means not written at all.
    // Unlike the bias tee this is safe to leave alone: rtlsdr_open resets the
    // demodulator, so the mode is known rather than inherited from whatever
    // ran last.
    if (rtlsdr_get_direct_sampling(device) != static_cast<int>(config.direct)) {
        if (const int rc = rtlsdr_set_direct_sampling(device, static_cast<int>(config.direct));
            rc != 0) {
            return fail(std::format("the device refused direct sampling mode {}: "
                                    "rtlsdr_set_direct_sampling returned {}",
                                    static_cast<int>(config.direct), rc_text(rc)),
                        rc);
        }
    }

    // A TUNABLE DONGLE OPENED WITH NO CENTRE IS REFUSED, AND THE REASON IS THAT
    // LEAVING IT ALONE WEDGES THE TUNER.
    //
    // With no freq= this function used to skip the block below entirely, so
    // rtlsdr_set_center_freq was never called and the tuner stayed where
    // rtlsdr_open left it, which is 0 Hz. That is not a quiet no-op. Setting the
    // sample rate re-tunes the handle's current frequency as a side effect, so
    // the R820T is then asked to lock DC, its PLL does not, and it prints "PLL
    // not locked" on the way to failing. An operator gets a dongle that opened
    // and streamed, pointed at nothing they asked for.
    //
    // Observed on 2026-09-21. The device picker omits a key whose box is empty,
    // which is right, and this was the one backend that could not take the
    // omission. Nothing in this tree had ever streamed from an rtlsdr without
    // freq=, because every documented example and every test supplies one, so a
    // silently under-specified open had no way to surface until a GUI produced
    // one.
    //
    // WHAT THIS GUARD IS NOT. It used to say that the wedged tuner was why
    // retuning was refused afterwards, and that is now false: a retune is
    // refused on a dongle opened correctly at 98.1 MHz as well, for the reason
    // retune_streaming_locked documents, and the two faults are independent.
    // This one is still worth refusing on its own terms, because an open with
    // no centre is an open nobody can have meant.
    //
    // NOT REFUSED WHEN THE DEVICE CAN LEGITIMATELY SIT AT DC, which is direct
    // sampling: tune_ranges_for reports {0, xtal/2} there, so the test is
    // whether zero is reachable rather than whether a mode was named. A caller
    // who wants HF through the direct-sampling branch is asking for something
    // real and gets it.
    if (!config.center_given) {
        const bool dc_reachable =
            std::any_of(tune_ranges.begin(), tune_ranges.end(),
                        [](const TuneRange& range) { return range.contains(0); });
        if (!dc_reachable) {
            return fail(std::format(
                "this dongle needs a centre frequency: freq= was not given, and leaving the "
                "tuner where opening it left it means asking an {} to lock DC, which fails and "
                "leaves it unable to tune at all afterwards. It reaches {}.",
                tuner_name(rtlsdr_get_tuner_type(device)), tune_ranges_text(tune_ranges)));
        }
    }

    if (config.center_given) {
        const bool reachable = std::any_of(
            tune_ranges.begin(), tune_ranges.end(),
            [&config](const TuneRange& range) { return range.contains(config.center_hz); });
        if (!reachable) {
            return fail(std::format("this dongle cannot tune {} Hz. It reaches {}.",
                                    config.center_hz, tune_ranges_text(tune_ranges)));
        }
        if (const int rc =
                rtlsdr_set_center_freq(device, static_cast<std::uint32_t>(config.center_hz));
            rc != 0) {
            return fail(std::format("the tuner refused {} Hz: rtlsdr_set_center_freq returned {}",
                                    config.center_hz, rc_text(rc)),
                        rc);
        }
    }

    if (const int rc = rtlsdr_set_sample_rate(device, static_cast<std::uint32_t>(config.rate));
        rc != 0) {
        return fail(std::format("the device refused {} S/s: rtlsdr_set_sample_rate returned {}",
                                config.rate, rc_text(rc)),
                    rc);
    }

    if (config.ppm_given) {
        // -2 is librtlsdr saying the device is already at that correction,
        // which is what ppm=0 asks for on a device nobody has corrected. It
        // is the requested state, so it is not a failure.
        const int rc = rtlsdr_set_freq_correction(device, config.ppm);
        if (rc != 0 && rc != -2) {
            return fail(std::format("the device refused a correction of {} ppm: "
                                    "rtlsdr_set_freq_correction returned {}",
                                    config.ppm, rc_text(rc)),
                        rc);
        }
    }

    // Only when it was asked for. rtlsdr_set_offset_tuning fails on the R820T
    // family whichever value it is handed, because the part is not zero-IF
    // and has nothing to offset, so writing the default would turn the most
    // common dongle in the world into an error at open.
    if (config.offset_tuning) {
        if (const int rc = rtlsdr_set_offset_tuning(device, 1); rc != 0) {
            return fail(std::format(
                "this dongle's tuner does not support offset tuning: rtlsdr_set_offset_tuning "
                "returned {}. Offset tuning moves the tuner off the wanted frequency to get the "
                "ADC's DC spike out of the passband, and only a zero-IF tuner such as the E4000 "
                "has that problem to solve.",
                rc_text(rc)),
                rc);
        }
    }

    if (const int rc = rtlsdr_set_agc_mode(device, config.digital_agc ? 1 : 0); rc != 0) {
        return fail(std::format("the device refused to turn its digital AGC {}: "
                                "rtlsdr_set_agc_mode returned {}",
                                config.digital_agc ? "on" : "off", rc_text(rc)),
                    rc);
    }

    // Written every time, never left as found. The bias tee is a latch inside
    // the dongle rather than process state: it survives the program that set
    // it, so a default that does not assert itself is a default that does not
    // exist. See the header for why the assertion matters more than the
    // value.
    if (const int rc = rtlsdr_set_bias_tee(device, config.bias_tee ? 1 : 0); rc != 0) {
        return fail(std::format("the device refused to turn its bias tee {}: "
                                "rtlsdr_set_bias_tee returned {}",
                                config.bias_tee ? "on" : "off", rc_text(rc)),
                    rc);
    }

    if (config.gain_auto) {
        if (const int rc = rtlsdr_set_tuner_gain_mode(device, 0); rc != 0) {
            return fail(std::format("the device refused automatic tuner gain: "
                                    "rtlsdr_set_tuner_gain_mode returned {}",
                                    rc_text(rc)),
                        rc);
        }
    } else {
        if (gain_steps.empty()) {
            return fail("this dongle reports no tuner gain table, so gain= cannot be set to a "
                        "number. Use gain=auto.");
        }
        const auto wanted = static_cast<int>(std::llround(config.gain_db * 10.0));
        const int landed = nearest_step(gain_steps, wanted);
        if (const int rc = rtlsdr_set_tuner_gain_mode(device, 1); rc != 0) {
            return fail(std::format("could not put the tuner into manual gain mode: "
                                    "rtlsdr_set_tuner_gain_mode returned {}",
                                    rc_text(rc)),
                        rc);
        }
        if (const int rc = rtlsdr_set_tuner_gain(device, landed); rc != 0) {
            return fail(std::format("the tuner refused a gain of {} dB: rtlsdr_set_tuner_gain "
                                    "returned {}",
                                    static_cast<double>(landed) / 10.0, rc_text(rc)),
                        rc);
        }
    }

    Applied applied;

    const std::uint32_t achieved_rate = rtlsdr_get_sample_rate(device);
    if (achieved_rate == 0) {
        return fail(std::format("the device reported a sample rate of zero after being set to {} "
                                "S/s, which rtl-sdr.h documents as its error return",
                                config.rate));
    }
    applied.rate = static_cast<dsp::SampleRate>(achieved_rate);

    if (config.center_given) {
        const std::uint32_t achieved = rtlsdr_get_center_freq(device);
        if (achieved == 0 && config.center_hz != 0) {
            return fail(std::format(
                "the tuner reported a centre of zero after being asked for {} Hz, which "
                "rtl-sdr.h documents as its error return",
                config.center_hz));
        }
        applied.center = static_cast<dsp::Hertz>(achieved);
    }

    return applied;
}

}  // namespace

Expected<std::vector<RtlSdrDevice>> enumerate_rtlsdr_devices()
{
    const std::uint32_t attached = rtlsdr_get_device_count();

    std::vector<RtlSdrDevice> out;
    out.reserve(attached);
    for (std::uint32_t index = 0; index < attached; ++index) {
        RtlSdrDevice device;
        device.index = index;

        const char* name = rtlsdr_get_device_name(index);
        device.name = (name == nullptr || *name == '\0') ? "RTL-SDR" : name;

        out.push_back(std::move(device));
    }
    return out;
}

Expected<SourceCapabilities> describe_rtlsdr_source(const RtlSdrSourceConfig& config)
{
    if (auto ok = validate(config); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto present = check_attached(config); !present) {
        return std::unexpected(present.error());
    }

    // Released when this returns, the device first and then the lock.
    auto opened =
        probe_under_lock(rtlsdr_lock_policy(), [&config] { return find_and_open(config); });
    if (!opened) {
        return std::unexpected(opened.error());
    }
    const std::uint32_t index = opened->handle.index;
    rtlsdr_dev_t* const device = opened->handle.device.get();

    const char* name = rtlsdr_get_device_name(index);
    const rtlsdr_tuner tuner = rtlsdr_get_tuner_type(device);
    const std::vector<int> gain_steps = gain_steps_of(device);

    SourceCapabilities caps = capabilities_of(
        config, index, (name == nullptr || *name == '\0') ? "RTL-SDR" : name, tuner, gain_steps);
    caps.serial = serial_of(device);
    return caps;
}

Expected<std::unique_ptr<Source>> open_rtlsdr_source(const RtlSdrSourceConfig& config)
{
    if (auto ok = validate(config); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto present = check_attached(config); !present) {
        return std::unexpected(present.error());
    }

    // Every return below that is not the source itself drops `opened`, which
    // closes the device and then lets go of the lock, in that order.
    auto opened =
        open_under_lock(rtlsdr_lock_policy(), [&config] { return find_and_open(config); });
    if (!opened) {
        return std::unexpected(opened.error());
    }
    const std::uint32_t index = opened->handle.index;
    rtlsdr_dev_t* const device = opened->handle.device.get();

    const char* name = rtlsdr_get_device_name(index);
    const rtlsdr_tuner tuner = rtlsdr_get_tuner_type(device);
    std::vector<int> gain_steps = gain_steps_of(device);

    SourceCapabilities caps = capabilities_of(
        config, index, (name == nullptr || *name == '\0') ? "RTL-SDR" : name, tuner, gain_steps);
    caps.serial = serial_of(device);

    auto applied = configure(device, config, gain_steps, caps.tune_ranges);
    if (!applied) {
        return std::unexpected(with_context(applied.error(), caps.display_name));
    }

    auto source = std::make_unique<RtlSdrSource>();
    if (auto started = source->open(config, std::move(opened->lock),
                                    std::move(opened->handle.device), std::move(caps),
                                    std::move(gain_steps), applied->rate, applied->center);
        !started) {
        return std::unexpected(started.error());
    }
    return std::unique_ptr<Source>(std::move(source));
}

}  // namespace revenant::source
