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

[[nodiscard]] Expected<std::uint32_t> resolve_index(const RtlSdrSourceConfig& config)
{
    const std::uint32_t attached = rtlsdr_get_device_count();
    if (attached == 0) {
        return fail("no RTL-SDR is attached. librtlsdr sees no device at all, which on Windows "
                    "usually means the dongle's USB interface is still bound to the DVB-T "
                    "driver rather than to WinUSB.");
    }

    if (!config.by_serial) {
        if (config.index >= attached) {
            return fail(std::format(
                "there is no RTL-SDR at index {}: {} attached, so the indices run 0 to {}",
                config.index, attached, attached - 1));
        }
        return config.index;
    }

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
    return fail(std::format("librtlsdr could not look up the serial '{}' (it returned {})",
                            config.serial, found),
                found);
}

[[nodiscard]] Expected<Device> open_device(std::uint32_t index)
{
    rtlsdr_dev_t* handle = nullptr;
    const int rc = rtlsdr_open(&handle, index);
    if (rc != 0 || handle == nullptr) {
        return fail(std::format(
            "could not open the RTL-SDR at index {}: librtlsdr returned {}. A device that "
            "enumerates and will not open is almost always held by another program, or has an "
            "interface that libwdi has not bound to WinUSB.",
            index, rc),
            rc);
    }
    return Device(handle);
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

    [[nodiscard]] Status open(const RtlSdrSourceConfig& config, Device device,
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
    [[nodiscard]] bool running() const override { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] Status seek(dsp::SampleIndex index) override;

    [[nodiscard]] SourceStats stats() const override;
    [[nodiscard]] ClockQuality clock() const override;

private:
    static void usb_callback(unsigned char* buffer, std::uint32_t bytes, void* context);

    void on_transfer(const std::uint8_t* data, std::uint32_t bytes);
    void note_dropped(std::size_t samples);
    void run_usb();
    void run_delivery();
    void note_stream_error(Error error);
    void join_locked();
    [[nodiscard]] Expected<ClockModel> make_clock_model() const;

    SourceCapabilities caps_{};
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
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> producer_done_{true};

    std::vector<Slot> slots_{};
    std::atomic<std::uint64_t> slot_head_{0};
    std::atomic<std::uint64_t> slot_tail_{0};

    // Callback thread only, both of them.
    dsp::SampleIndex produced_index_ = 0;
    std::uint64_t pending_gap_ = 0;

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

Status RtlSdrSource::open(const RtlSdrSourceConfig& config, Device device, SourceCapabilities caps,
                          std::vector<int> gain_steps, dsp::SampleRate rate, dsp::Hertz center)
{
    caps_ = std::move(caps);
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

    const auto requested = static_cast<std::uint32_t>(center);
    if (const int rc = rtlsdr_set_center_freq(device_.get(), requested); rc != 0) {
        return fail(std::format("the tuner refused {} Hz: librtlsdr returned {}", center, rc), rc);
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
        return fail(std::format("the device refused {} S/s: librtlsdr returned {}", rate, rc), rc);
    }

    // The RTL2832U resamples with a fractional divider off a 28.8 MHz
    // crystal, so a request that is not exactly representable lands nearby.
    // What comes back is what the samples were actually taken at.
    const std::uint32_t achieved = rtlsdr_get_sample_rate(device_.get());
    if (achieved == 0) {
        return fail(std::format("the device reported a sample rate of zero after being set to {} "
                                "S/s, which rtl-sdr.h documents as its error return",
                                rate));
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

    const auto wanted = static_cast<int>(std::llround(db * 10.0));
    const int landed = nearest_step(gain_steps_, wanted);

    if (const int rc = rtlsdr_set_tuner_gain_mode(device_.get(), 1); rc != 0) {
        return fail(std::format("could not put the tuner into manual gain mode: librtlsdr "
                                "returned {}",
                                rc),
                    rc);
    }
    if (const int rc = rtlsdr_set_tuner_gain(device_.get(), landed); rc != 0) {
        return fail(std::format("the tuner refused a gain of {} dB: librtlsdr returned {}",
                                static_cast<double>(landed) / 10.0, rc),
                    rc);
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

    // rtlsdr_set_tuner_gain_mode takes "manual", so the sense is inverted
    // here rather than at every call site.
    if (const int rc = rtlsdr_set_tuner_gain_mode(device_.get(), on ? 0 : 1); rc != 0) {
        return fail(std::format("could not switch the tuner to {} gain: librtlsdr returned {}",
                                on ? "automatic" : "manual", rc),
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
        return fail(std::format("could not reset the device's sample buffer: librtlsdr returned "
                                "{}",
                                rc),
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
        return std::unexpected(stop_error_);
    }
    return {};
}

void RtlSdrSource::join_locked()
{
    cancel_requested_.store(true, std::memory_order_release);

    if (usb_thread_.joinable()) {
        // Retried until it is accepted, and then not again.
        //
        // rtlsdr_cancel_async only arms the cancel once rtlsdr_read_async has
        // reached its running state. Called before that it returns -2 and
        // does nothing, and read_async then never returns; the window is the
        // few microseconds between spawning the thread and libusb's loop
        // being armed, which is exactly where a test that starts and
        // immediately stops lands. So it is retried.
        //
        // Retrying past the first acceptance is the bug that this shape
        // avoids. A second call arriving while the first cancel is still
        // draining takes librtlsdr's other branch, which forces the async
        // state straight to inactive: read_async's wait loop then exits with
        // transfers still submitted and frees them underneath libusb. What
        // that produces is a control transfer failing during close with a
        // pipe error, well after the capture, which reads as a device fault
        // rather than as this.
        while (!producer_done_.load(std::memory_order_acquire)) {
            if (rtlsdr_cancel_async(device_.get()) == 0) {
                break;
            }
            std::this_thread::sleep_for(kCancelPoll);
        }
        usb_thread_.join();
    }

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
    // it, which is what pins it to the cancel rather than to anything here.
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
                        rc),
            rc});
    }

    producer_done_.store(true, std::memory_order_release);
}

void RtlSdrSource::run_delivery()
{
    std::uint64_t sequence = 0;
    dsp::SampleIndex stream_index = 0;

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
        const bool producer_finished = producer_done_.load(std::memory_order_acquire);

        // Acquire, pairing with the callback thread's release store of head.
        // Without it the memcpy below can read bytes the writer has not
        // published.
        const std::uint64_t head = slot_head_.load(std::memory_order_acquire);

        if (tail == head) {
            if (producer_finished) {
                break;
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
                if (!cancel_requested_.load(std::memory_order_acquire)) {
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
            return fail(std::format("the device refused direct sampling mode {}: librtlsdr "
                                    "returned {}",
                                    static_cast<int>(config.direct), rc),
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
    // not locked" on the way to failing. The tuner is in a failed state from
    // that moment: every later rtlsdr_set_center_freq returns
    // LIBUSB_ERROR_PIPE, which reaches an operator as "the tuner refused
    // 435000000 Hz: librtlsdr returned -9" and points at the frequency they
    // asked for rather than at the one nobody asked for.
    //
    // Observed on 2026-09-21. The device picker omits a key whose box is empty,
    // which is right, and this was the one backend that could not take the
    // omission. Nothing in this tree had ever streamed from an rtlsdr without
    // freq=, because every documented example and every test supplies one, so a
    // silently under-specified open had no way to surface until a GUI produced
    // one.
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
            return fail(std::format("the tuner refused {} Hz: librtlsdr returned {}",
                                    config.center_hz, rc),
                        rc);
        }
    }

    if (const int rc = rtlsdr_set_sample_rate(device, static_cast<std::uint32_t>(config.rate));
        rc != 0) {
        return fail(std::format("the device refused {} S/s: librtlsdr returned {}", config.rate,
                                rc),
                    rc);
    }

    if (config.ppm_given) {
        // -2 is librtlsdr saying the device is already at that correction,
        // which is what ppm=0 asks for on a device nobody has corrected. It
        // is the requested state, so it is not a failure.
        const int rc = rtlsdr_set_freq_correction(device, config.ppm);
        if (rc != 0 && rc != -2) {
            return fail(std::format("the device refused a correction of {} ppm: librtlsdr "
                                    "returned {}",
                                    config.ppm, rc),
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
                "this dongle's tuner does not support offset tuning: librtlsdr returned {}. "
                "Offset tuning moves the tuner off the wanted frequency to get the ADC's DC "
                "spike out of the passband, and only a zero-IF tuner such as the E4000 has that "
                "problem to solve.",
                rc),
                rc);
        }
    }

    if (const int rc = rtlsdr_set_agc_mode(device, config.digital_agc ? 1 : 0); rc != 0) {
        return fail(std::format("the device refused to turn its digital AGC {}: librtlsdr "
                                "returned {}",
                                config.digital_agc ? "on" : "off", rc),
                    rc);
    }

    // Written every time, never left as found. The bias tee is a latch inside
    // the dongle rather than process state: it survives the program that set
    // it, so a default that does not assert itself is a default that does not
    // exist. See the header for why the assertion matters more than the
    // value.
    if (const int rc = rtlsdr_set_bias_tee(device, config.bias_tee ? 1 : 0); rc != 0) {
        return fail(std::format("the device refused to turn its bias tee {}: librtlsdr returned "
                                "{}",
                                config.bias_tee ? "on" : "off", rc),
                    rc);
    }

    if (config.gain_auto) {
        if (const int rc = rtlsdr_set_tuner_gain_mode(device, 0); rc != 0) {
            return fail(std::format("the device refused automatic tuner gain: librtlsdr returned "
                                    "{}",
                                    rc),
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
            return fail(std::format("could not put the tuner into manual gain mode: librtlsdr "
                                    "returned {}",
                                    rc),
                        rc);
        }
        if (const int rc = rtlsdr_set_tuner_gain(device, landed); rc != 0) {
            return fail(std::format("the tuner refused a gain of {} dB: librtlsdr returned {}",
                                    static_cast<double>(landed) / 10.0, rc),
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

    auto index = resolve_index(config);
    if (!index) {
        return std::unexpected(index.error());
    }

    auto device = open_device(*index);
    if (!device) {
        return std::unexpected(device.error());
    }

    const char* name = rtlsdr_get_device_name(*index);
    const rtlsdr_tuner tuner = rtlsdr_get_tuner_type(device->get());
    const std::vector<int> gain_steps = gain_steps_of(device->get());

    return capabilities_of(config, *index,
                           (name == nullptr || *name == '\0') ? "RTL-SDR" : name, tuner,
                           gain_steps);
}

Expected<std::unique_ptr<Source>> open_rtlsdr_source(const RtlSdrSourceConfig& config)
{
    if (auto ok = validate(config); !ok) {
        return std::unexpected(ok.error());
    }

    auto index = resolve_index(config);
    if (!index) {
        return std::unexpected(index.error());
    }

    auto device = open_device(*index);
    if (!device) {
        return std::unexpected(device.error());
    }

    const char* name = rtlsdr_get_device_name(*index);
    const rtlsdr_tuner tuner = rtlsdr_get_tuner_type(device->get());
    std::vector<int> gain_steps = gain_steps_of(device->get());

    SourceCapabilities caps = capabilities_of(
        config, *index, (name == nullptr || *name == '\0') ? "RTL-SDR" : name, tuner, gain_steps);

    auto applied = configure(device->get(), config, gain_steps, caps.tune_ranges);
    if (!applied) {
        return std::unexpected(with_context(applied.error(), caps.display_name));
    }

    auto source = std::make_unique<RtlSdrSource>();
    if (auto opened = source->open(config, std::move(*device), std::move(caps),
                                   std::move(gain_steps), applied->rate, applied->center);
        !opened) {
        return std::unexpected(opened.error());
    }
    return std::unique_ptr<Source>(std::move(source));
}

}  // namespace revenant::source
