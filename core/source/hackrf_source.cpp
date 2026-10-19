// The HackRF backend.
//
// THE SHAPE is the RTL-SDR backend's, for the same reason: libhackrf calls its
// receive callback on its own transfer thread with a buffer libusb resubmits
// the moment the callback returns, so the callback must be quick and the bytes
// cannot outlive it. The callback copies into one slot of a small fixed queue
// and returns; a delivery thread owned by this source drains the queue and
// calls the sink. rtlsdr_source.cpp's header documents the queue's ownership
// rules and the gap arithmetic, and both hold here unchanged:
//
//     samples_delivered + samples_lost == samples the device handed us
//
// with stamp.start always the true index of a block's first sample.
//
// What is simpler than the RTL-SDR: libhackrf owns its transfer thread, so
// there is no second thread of ours blocking in a read call, and a retune is
// one hackrf_set_freq on a streaming device rather than a pause and restart.

#include "core/source/hackrf_source.h"

#include <libhackrf/hackrf.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
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
#include "core/thread_role.h"

namespace revenant::source {
namespace {

constexpr std::size_t kBytesPerSample = 2;
static_assert(bytes_per_sample(SampleFormat::Cs8) == kBytesPerSample,
              "the transfer arithmetic below is in samples and converts with this constant");

constexpr std::string_view kCombinedStage = "gain";
constexpr std::string_view kLnaStage = "lna";
constexpr std::string_view kVgaStage = "vga";
constexpr std::string_view kAmpStage = "amp";

// One libhackrf transfer at the default 256 KiB, 13 ms at 10 MS/s.
constexpr std::size_t kPreferredBlockSamples = 131'072;
constexpr std::size_t kMaxBlockSamples = 1u << 24;

// Power of two so the slot index is one AND. Eight 256 KiB transfers is about
// 105 ms at the default rate, the same order as the RTL-SDR's queue and for
// the same reason: deeper only delays discovering a consumer that is too slow.
constexpr std::uint64_t kSlotCount = 8;
constexpr std::uint64_t kSlotMask = kSlotCount - 1;

constexpr auto kIdlePoll = std::chrono::microseconds(250);

// The anchor is taken just before hackrf_start_rx, so its error is the
// latency from there to the first transfer landing, a few transfer durations.
constexpr std::int64_t kAnchorAccuracyNs = 2'000'000;

// The HackRF One's clock is a 20 ppm crystal unless a TCXO or an external
// 10 MHz reference is fitted, and nothing on the bus says which.
constexpr double kTolerancePpm = 20.0;

[[nodiscard]] std::int64_t unix_now_ns()
{
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
}

[[nodiscard]] std::string rc_text(int rc)
{
    return std::format("{} ({})", hackrf_error_name(static_cast<hackrf_error>(rc)), rc);
}

// hackrf_init once for the life of the process, and hackrf_exit never.
//
// hackrf_exit refuses while any device is open and tears down the libusb
// context every other open shares, so pairing it with each source would fail
// whenever two overlap and race whenever they do not. The context is a few
// kilobytes and the OS reclaims it at exit.
[[nodiscard]] Status ensure_library()
{
    static std::once_flag once;
    static int result = HACKRF_SUCCESS;
    std::call_once(once, [] { result = hackrf_init(); });
    if (result != HACKRF_SUCCESS) {
        return fail(std::format("libhackrf could not initialise: hackrf_init returned {}",
                                rc_text(result)),
                    result);
    }
    return {};
}

[[nodiscard]] std::vector<double> steps(double max_db, double step_db)
{
    std::vector<double> out;
    for (double db = 0.0; db <= max_db; db += step_db) {
        out.push_back(db);
    }
    return out;
}

[[nodiscard]] SourceCapabilities capabilities_of(const HackrfSourceConfig& config,
                                                 const std::string& name)
{
    SourceCapabilities caps;
    caps.uri = config.uri;
    caps.backend = "hackrf";
    caps.display_name = std::format("{} {}", name, config.serial);
    caps.serial = config.serial;
    caps.tune_ranges = {TuneRange{kHackrfMinFrequency, kHackrfMaxFrequency, 0}};
    caps.min_rate = kHackrfMinRate;
    caps.max_rate = kHackrfMaxRate;
    caps.native_format = SampleFormat::Cs8;
    caps.bits_per_component = 8;

    // The combined stage first, because the client draws a control for the
    // first stage only. hackrf_split_gain has the rule it follows.
    GainStage combined;
    combined.name = std::string(kCombinedStage);
    combined.max_db = kHackrfCombinedMaxDb;
    combined.steps_db = steps(kHackrfCombinedMaxDb, kHackrfVgaStepDb);
    caps.gain_stages.push_back(std::move(combined));

    GainStage lna;
    lna.name = std::string(kLnaStage);
    lna.max_db = kHackrfLnaMaxDb;
    lna.steps_db = steps(kHackrfLnaMaxDb, kHackrfLnaStepDb);
    caps.gain_stages.push_back(std::move(lna));

    GainStage vga;
    vga.name = std::string(kVgaStage);
    vga.max_db = kHackrfVgaMaxDb;
    vga.steps_db = steps(kHackrfVgaMaxDb, kHackrfVgaStepDb);
    caps.gain_stages.push_back(std::move(vga));

    GainStage amp;
    amp.name = std::string(kAmpStage);
    amp.max_db = kHackrfAmpDb;
    amp.steps_db = {0.0, kHackrfAmpDb};
    caps.gain_stages.push_back(std::move(amp));

    caps.clock_sources = {ClockSource::Internal};
    caps.flow = FlowControl::Paced;
    caps.seekable = false;
    caps.preferred_block_samples = kPreferredBlockSamples;
    caps.timestamp_accuracy_ns = kAnchorAccuracyNs;
    return caps;
}

// The name of the attached device with this serial, from the USB product id.
[[nodiscard]] Expected<std::string> find_attached(const std::string& serial)
{
    auto attached = enumerate_hackrf_devices();
    if (!attached) {
        return std::unexpected(attached.error());
    }
    for (const HackrfDevice& device : *attached) {
        if (device.serial == serial) {
            return device.name;
        }
    }
    if (attached->empty()) {
        return fail(std::format("no HackRF is attached, so serial {} cannot be opened", serial));
    }
    std::string listed;
    for (const HackrfDevice& device : *attached) {
        listed += listed.empty() ? "" : ", ";
        listed += device.serial;
    }
    return fail(std::format("no attached HackRF has serial {}. Attached: {}", serial, listed));
}

struct DeviceCloser {
    void operator()(hackrf_device* device) const
    {
        if (device != nullptr) {
            static_cast<void>(hackrf_close(device));
        }
    }
};
using Device = std::unique_ptr<hackrf_device, DeviceCloser>;

struct Slot {
    std::vector<std::uint8_t> bytes;
    std::size_t samples = 0;
    std::uint64_t gap_before = 0;
};

class HackrfSource final : public Source {
public:
    HackrfSource(Device device, SourceCapabilities caps, dsp::SampleRate rate,
                 dsp::Hertz center, HackrfGains gains, bool amp)
        : device_(std::move(device)),
          caps_(std::move(caps)),
          rate_(rate),
          center_hz_(center),
          gains_(gains),
          amp_(amp)
    {
    }

    ~HackrfSource() override
    {
        std::scoped_lock lock(control_);
        join_locked();
    }

    [[nodiscard]] const SourceCapabilities& capabilities() const override { return caps_; }

    [[nodiscard]] Expected<dsp::Hertz> tune(dsp::Hertz center) override
    {
        std::scoped_lock lock(control_);
        if (!hackrf_frequency_supported(center)) {
            return fail(std::format("the HackRF cannot tune {} Hz. It reaches {} to {} Hz.",
                                    center, kHackrfMinFrequency, kHackrfMaxFrequency));
        }
        if (const int rc = hackrf_set_freq(device_.get(), static_cast<std::uint64_t>(center));
            rc != HACKRF_SUCCESS) {
            return fail(std::format("the HackRF refused {} Hz: hackrf_set_freq returned {}",
                                    center, rc_text(rc)),
                        rc);
        }
        center_hz_.store(center, std::memory_order_release);
        return center;
    }

    [[nodiscard]] dsp::Hertz center() const override
    {
        return center_hz_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) override
    {
        std::scoped_lock lock(control_);
        if (running_.load(std::memory_order_acquire)) {
            return fail("the HackRF's sample rate cannot change while it streams. Stop the "
                        "source first: the stream's timestamps are derived from one rate.");
        }
        if (auto applied = apply_rate(device_.get(), rate); !applied) {
            return std::unexpected(applied.error());
        }
        rate_ = rate;
        return rate;
    }

    [[nodiscard]] dsp::SampleRate sample_rate() const override { return rate_; }

    [[nodiscard]] Expected<double> set_gain(std::string_view stage, double db) override
    {
        std::scoped_lock lock(control_);
        HackrfGains wanted = gains_;
        bool amp = amp_;
        if (stage == kCombinedStage) {
            wanted = hackrf_split_gain(db);
        } else if (stage == kLnaStage) {
            wanted.lna_db = hackrf_snap_lna(db);
        } else if (stage == kVgaStage) {
            wanted.vga_db = hackrf_snap_vga(db);
        } else if (stage == kAmpStage) {
            amp = std::isfinite(db) && db >= kHackrfAmpDb / 2.0;
        } else {
            return fail(std::format("the HackRF has no gain stage '{}'. Its stages are {}, {}, {} "
                                    "and {}.",
                                    stage, kCombinedStage, kLnaStage, kVgaStage, kAmpStage));
        }
        if (auto applied = apply_gains(device_.get(), wanted, amp); !applied) {
            return std::unexpected(applied.error());
        }
        gains_ = wanted;
        amp_ = amp;
        record_gains_locked();

        if (stage == kLnaStage) {
            return gains_.lna_db;
        }
        if (stage == kVgaStage) {
            return gains_.vga_db;
        }
        if (stage == kAmpStage) {
            return amp_ ? kHackrfAmpDb : 0.0;
        }
        return gains_.total();
    }

    [[nodiscard]] Status set_gain_auto(std::string_view stage, bool on) override
    {
        if (!on) {
            return {};
        }
        return fail(std::format("the HackRF has no automatic gain on stage '{}'. Set a number.",
                                stage));
    }

    [[nodiscard]] Status start(const StreamOptions& options, BlockSink sink) override;
    [[nodiscard]] Status stop() override
    {
        std::scoped_lock lock(control_);
        join_locked();
        std::scoped_lock errors(error_lock_);
        if (has_stop_error_) {
            return std::unexpected(with_context(stop_error_, caps_.display_name));
        }
        return {};
    }

    [[nodiscard]] bool running() const override
    {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Status seek(dsp::SampleIndex index) override
    {
        static_cast<void>(index);
        return fail("a HackRF is a Paced source and cannot seek: there is no sample it has not "
                    "yet produced to move to, and none it has already produced to go back to");
    }

    [[nodiscard]] SourceStats stats() const override
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

    [[nodiscard]] ClockQuality clock() const override
    {
        std::scoped_lock lock(control_);
        ClockQuality quality = clock_model_ ? clock_model_->quality() : ClockQuality{};
        if (!clock_model_ || !clock_model_->anchored()) {
            quality.accuracy_ns = kAnchorAccuracyNs;
            return quality;
        }
        quality.accuracy_ns =
            clock_model_->accuracy_ns_at(write_index_.load(std::memory_order_relaxed));
        return quality;
    }

    [[nodiscard]] static Status apply_rate(hackrf_device* device, dsp::SampleRate rate)
    {
        if (!hackrf_rate_supported(rate)) {
            return fail(std::format("the HackRF takes {} to {} S/s, not {}", kHackrfMinRate,
                                    kHackrfMaxRate, rate));
        }
        if (const int rc = hackrf_set_sample_rate(device, static_cast<double>(rate));
            rc != HACKRF_SUCCESS) {
            return fail(std::format("the HackRF refused {} S/s: hackrf_set_sample_rate returned {}",
                                    rate, rc_text(rc)),
                        rc);
        }
        // The baseband filter follows the rate at three quarters of it, which
        // is what hackrf_transfer does: wide enough to keep the span, narrow
        // enough that what lies just outside it is attenuated before the ADC
        // rather than aliased in.
        const std::uint32_t bandwidth = hackrf_compute_baseband_filter_bw(
            static_cast<std::uint32_t>(static_cast<double>(rate) * 0.75));
        if (const int rc = hackrf_set_baseband_filter_bandwidth(device, bandwidth);
            rc != HACKRF_SUCCESS) {
            return fail(std::format("the HackRF refused a baseband filter of {} Hz: "
                                    "hackrf_set_baseband_filter_bandwidth returned {}",
                                    bandwidth, rc_text(rc)),
                        rc);
        }
        return {};
    }

    [[nodiscard]] static Status apply_gains(hackrf_device* device, HackrfGains gains, bool amp)
    {
        if (const int rc = hackrf_set_lna_gain(device, static_cast<std::uint32_t>(gains.lna_db));
            rc != HACKRF_SUCCESS) {
            return fail(std::format("the HackRF refused an LNA gain of {} dB: "
                                    "hackrf_set_lna_gain returned {}",
                                    gains.lna_db, rc_text(rc)),
                        rc);
        }
        if (const int rc = hackrf_set_vga_gain(device, static_cast<std::uint32_t>(gains.vga_db));
            rc != HACKRF_SUCCESS) {
            return fail(std::format("the HackRF refused a VGA gain of {} dB: "
                                    "hackrf_set_vga_gain returned {}",
                                    gains.vga_db, rc_text(rc)),
                        rc);
        }
        if (const int rc = hackrf_set_amp_enable(device, amp ? 1 : 0); rc != HACKRF_SUCCESS) {
            return fail(std::format("the HackRF refused to turn its RF amplifier {}: "
                                    "hackrf_set_amp_enable returned {}",
                                    amp ? "on" : "off", rc_text(rc)),
                        rc);
        }
        return {};
    }

    void record_gains_locked()
    {
        for (GainStage& stage : caps_.gain_stages) {
            if (stage.name == kCombinedStage) {
                stage.in_force_db = gains_.total();
            } else if (stage.name == kLnaStage) {
                stage.in_force_db = gains_.lna_db;
            } else if (stage.name == kVgaStage) {
                stage.in_force_db = gains_.vga_db;
            } else if (stage.name == kAmpStage) {
                stage.in_force_db = amp_ ? kHackrfAmpDb : 0.0;
            }
        }
    }

private:
    static int rx_callback(hackrf_transfer* transfer)
    {
        auto* self = static_cast<HackrfSource*>(transfer->rx_ctx);
        if (self == nullptr || transfer->buffer == nullptr || transfer->valid_length <= 0) {
            return 0;
        }
        self->on_transfer(transfer->buffer, static_cast<std::size_t>(transfer->valid_length));
        return self->cancel_requested_.load(std::memory_order_acquire) ? -1 : 0;
    }

    void on_transfer(const std::uint8_t* data, std::size_t bytes)
    {
        const std::size_t samples = bytes / kBytesPerSample;
        const std::uint64_t head = slot_head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = slot_tail_.load(std::memory_order_acquire);
        if (head - tail >= kSlotCount) {
            // Dropped whole and counted, never overwritten. See the queue
            // note in rtlsdr_source.cpp.
            overrun_events_.fetch_add(1, std::memory_order_relaxed);
            samples_lost_.fetch_add(samples, std::memory_order_relaxed);
            last_loss_index_.store(produced_index_, std::memory_order_relaxed);
            produced_index_ += samples;
            pending_gap_ += samples;
            return;
        }
        Slot& slot = slots_[static_cast<std::size_t>(head & kSlotMask)];
        const std::size_t kept = std::min(samples, slot.bytes.size() / kBytesPerSample);
        std::memcpy(slot.bytes.data(), data, kept * kBytesPerSample);
        slot.samples = kept;
        slot.gap_before = pending_gap_;
        pending_gap_ = 0;
        produced_index_ += kept;
        slot_head_.store(head + 1, std::memory_order_release);
    }

    void run_delivery();
    void join_locked();

    void note_stream_error(Error error)
    {
        std::scoped_lock lock(error_lock_);
        if (!has_stop_error_) {
            stop_error_ = std::move(error);
            has_stop_error_ = true;
        }
    }

    mutable std::mutex control_;
    Device device_;
    SourceCapabilities caps_;
    dsp::SampleRate rate_ = 0;
    std::atomic<dsp::Hertz> center_hz_{0};
    HackrfGains gains_;
    bool amp_ = false;

    std::optional<ClockModel> clock_model_;
    std::int64_t anchor_ns_ = 0;

    BlockSink sink_;
    std::size_t block_samples_ = kPreferredBlockSamples;
    std::thread deliver_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> producer_done_{false};

    std::vector<Slot> slots_;
    std::atomic<std::uint64_t> slot_head_{0};
    std::atomic<std::uint64_t> slot_tail_{0};

    // The callback thread's alone.
    dsp::SampleIndex produced_index_ = 0;
    std::uint64_t pending_gap_ = 0;

    std::mutex error_lock_;
    Error stop_error_;
    bool has_stop_error_ = false;

    std::atomic<std::uint64_t> blocks_delivered_{0};
    std::atomic<std::uint64_t> samples_delivered_{0};
    std::atomic<std::uint64_t> overrun_events_{0};
    std::atomic<std::uint64_t> samples_lost_{0};
    std::atomic<dsp::SampleIndex> last_loss_index_{0};
    std::atomic<dsp::SampleIndex> write_index_{0};
};

Status HackrfSource::start(const StreamOptions& options, BlockSink sink)
{
    std::scoped_lock lock(control_);

    if (running_.load(std::memory_order_acquire) || deliver_thread_.joinable()) {
        return fail("the HackRF is already streaming");
    }
    if (!sink) {
        return fail("a source cannot be started without a sink");
    }
    if (options.start_index != 0) {
        return fail(std::format("a HackRF cannot start at sample {}. A Paced source's sample zero "
                                "is whatever the device hands over first.",
                                options.start_index));
    }

    std::size_t block = options.block_samples == 0 ? kPreferredBlockSamples
                                                   : options.block_samples;
    if (block > kMaxBlockSamples) {
        return fail(std::format("a block of {} samples is past the {} sample ceiling; that is a "
                                "buffer, not a block",
                                block, kMaxBlockSamples));
    }

    const std::size_t transfer_bytes = hackrf_get_transfer_buffer_size(device_.get());
    slots_.assign(static_cast<std::size_t>(kSlotCount), Slot{});
    for (Slot& slot : slots_) {
        slot.bytes.assign(transfer_bytes == 0 ? 262'144 : transfer_bytes, 0);
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

    ClockModelConfig clock_config;
    clock_config.source = ClockSource::Internal;
    clock_config.rate = rate_;
    clock_config.anchor_accuracy_ns = kAnchorAccuracyNs;
    clock_config.oscillator_tolerance_ppm = kTolerancePpm;
    auto model = ClockModel::create(clock_config);
    if (!model) {
        return std::unexpected(with_context(model.error(), "HackRF clock"));
    }
    const std::int64_t anchor = unix_now_ns();
    if (auto anchored = model->set_anchor(anchor); !anchored) {
        return std::unexpected(with_context(anchored.error(), "HackRF clock"));
    }
    clock_model_ = std::move(*model);
    anchor_ns_ = anchor;

    block_samples_ = block;
    sink_ = std::move(sink);
    cancel_requested_.store(false, std::memory_order_relaxed);
    producer_done_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    try {
        deliver_thread_ = std::thread([this] { run_delivery(); });
    } catch (const std::system_error& error) {
        running_.store(false, std::memory_order_release);
        return fail(std::format("could not start the HackRF delivery thread: {}", error.what()),
                    error.code().value());
    }

    if (const int rc = hackrf_start_rx(device_.get(), &HackrfSource::rx_callback, this);
        rc != HACKRF_SUCCESS) {
        producer_done_.store(true, std::memory_order_release);
        deliver_thread_.join();
        running_.store(false, std::memory_order_release);
        return fail(std::format("the HackRF would not start receiving: hackrf_start_rx returned {}",
                                rc_text(rc)),
                    rc);
    }
    return {};
}

void HackrfSource::join_locked()
{
    if (!deliver_thread_.joinable()) {
        return;
    }
    cancel_requested_.store(true, std::memory_order_release);

    // hackrf_stop_rx cancels the transfers and returns once libhackrf's
    // transfer thread will call the callback no more, so producer_done_ set
    // after it means nothing further is published.
    static_cast<void>(hackrf_stop_rx(device_.get()));
    producer_done_.store(true, std::memory_order_release);
    deliver_thread_.join();
    running_.store(false, std::memory_order_release);
}

void HackrfSource::run_delivery()
{
    describe_this_thread(L"revenant hackrf delivery", ThreadClass::Listening);
    std::uint64_t sequence = 0;
    dsp::SampleIndex stream_index = 0;

    while (true) {
        const std::uint64_t tail = slot_tail_.load(std::memory_order_relaxed);

        // producer_done_ before head, for the reason rtlsdr_source.cpp's
        // run_delivery gives: the other order can lose the last slot.
        const bool producer_finished = producer_done_.load(std::memory_order_acquire);
        const std::uint64_t head = slot_head_.load(std::memory_order_acquire);
        if (tail == head) {
            if (producer_finished) {
                break;
            }
            std::this_thread::sleep_for(kIdlePoll);
            continue;
        }

        Slot& slot = slots_[static_cast<std::size_t>(tail & kSlotMask)];
        stream_index += slot.gap_before;
        std::uint64_t gap = slot.gap_before;

        bool finished = false;
        std::size_t offset = 0;
        while (offset < slot.samples) {
            const std::size_t count = std::min(block_samples_, slot.samples - offset);

            SourceBlock block;
            block.stamp = dsp::BlockTimestamp{stream_index, anchor_ns_, rate_};
            block.format = SampleFormat::Cs8;
            block.sample_count = count;
            block.bytes = std::as_bytes(std::span<const std::uint8_t>(
                slot.bytes.data() + offset * kBytesPerSample, count * kBytesPerSample));
            block.dropped_before = gap;
            block.sequence = sequence;

            if (Status delivered = sink_(block); !delivered) {
                // A sink refusing after a stop was asked for is the stop
                // arriving, not a fault.
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

        slot_tail_.store(tail + 1, std::memory_order_release);
        if (finished) {
            // Ask libhackrf to stop calling back. The callback reads this and
            // returns nonzero, which ends the transfers from its side.
            cancel_requested_.store(true, std::memory_order_release);
            break;
        }
    }
    running_.store(false, std::memory_order_release);
}

}  // namespace

HackrfGains hackrf_split_gain(double total_db)
{
    const double total = std::isfinite(total_db) ? std::clamp(total_db, 0.0, kHackrfCombinedMaxDb) : 0.0;
    HackrfGains out;
    out.lna_db = std::min(kHackrfLnaMaxDb,
                          std::floor(total / 2.0 / kHackrfLnaStepDb) * kHackrfLnaStepDb);
    out.vga_db = hackrf_snap_vga(total - out.lna_db);
    return out;
}

double hackrf_snap_lna(double db)
{
    // Rounded down rather than to nearest, here and for the VGA: the firmware
    // masks the low bits of the request, so it sets the step at or below it
    // and nearest would report a figure the device does not set.
    if (!std::isfinite(db)) {
        return 0.0;
    }
    const double clamped = std::clamp(db, 0.0, kHackrfLnaMaxDb);
    return std::floor(clamped / kHackrfLnaStepDb) * kHackrfLnaStepDb;
}

double hackrf_snap_vga(double db)
{
    if (!std::isfinite(db)) {
        return 0.0;
    }
    const double clamped = std::clamp(db, 0.0, kHackrfVgaMaxDb);
    return std::floor(clamped / kHackrfVgaStepDb) * kHackrfVgaStepDb;
}

Status validate_hackrf_config(const HackrfSourceConfig& config)
{
    if (config.serial.empty()) {
        return fail("a HackRF needs its serial: hackrf://<serial>, as the device list shows it");
    }
    if (!hackrf_rate_supported(config.rate)) {
        return fail(std::format("rate={} is outside the HackRF's {} to {} S/s", config.rate,
                                kHackrfMinRate, kHackrfMaxRate));
    }
    if (!config.center_given) {
        return fail(std::format("a HackRF needs a centre frequency: freq= was not given. It reaches "
                                "{} to {} Hz.",
                                kHackrfMinFrequency, kHackrfMaxFrequency));
    }
    if (!hackrf_frequency_supported(config.center_hz)) {
        return fail(std::format("freq={} Hz is outside the HackRF's {} to {} Hz", config.center_hz,
                                kHackrfMinFrequency, kHackrfMaxFrequency));
    }
    if (!std::isfinite(config.gain_db) || config.gain_db < 0.0 ||
        config.gain_db > kHackrfCombinedMaxDb) {
        return fail(std::format("gain={} dB is outside the HackRF's 0 to {} dB across its LNA and "
                                "VGA",
                                config.gain_db, kHackrfCombinedMaxDb));
    }
    return {};
}

Expected<std::vector<HackrfDevice>> enumerate_hackrf_devices()
{
    if (auto ready = ensure_library(); !ready) {
        return std::unexpected(ready.error());
    }
    hackrf_device_list_t* list = hackrf_device_list();
    if (list == nullptr) {
        return fail("libhackrf could not list USB devices: hackrf_device_list returned nothing");
    }
    std::vector<HackrfDevice> out;
    for (int i = 0; i < list->devicecount; ++i) {
        // A device whose serial could not be read cannot be named in a URI,
        // and a row nobody can open is worse than no row.
        const char* serial = list->serial_numbers[i];
        if (serial == nullptr || *serial == '\0') {
            continue;
        }
        HackrfDevice device;
        device.serial = serial;
        const char* name = hackrf_usb_board_id_name(list->usb_board_ids[i]);
        device.name = (name == nullptr || *name == '\0') ? "HackRF" : name;
        out.push_back(std::move(device));
    }
    hackrf_device_list_free(list);
    return out;
}

Expected<SourceCapabilities> describe_hackrf_source(const HackrfSourceConfig& config)
{
    if (config.serial.empty()) {
        return fail("a HackRF needs its serial: hackrf://<serial>, as the device list shows it");
    }
    auto name = find_attached(config.serial);
    if (!name) {
        return std::unexpected(name.error());
    }
    return capabilities_of(config, *name);
}

Expected<std::unique_ptr<Source>> open_hackrf_source(const HackrfSourceConfig& config)
{
    if (auto ok = validate_hackrf_config(config); !ok) {
        return std::unexpected(ok.error());
    }
    auto name = find_attached(config.serial);
    if (!name) {
        return std::unexpected(name.error());
    }

    hackrf_device* raw = nullptr;
    if (const int rc = hackrf_open_by_serial(config.serial.c_str(), &raw);
        rc != HACKRF_SUCCESS) {
        return fail(std::format("could not open HackRF {}: hackrf_open_by_serial returned {}. "
                                "Another program streaming from it is the usual cause.",
                                config.serial, rc_text(rc)),
                    rc);
    }
    Device device(raw);
    SourceCapabilities caps = capabilities_of(config, *name);

    // Written off every time, never left as found. See the header.
    if (const int rc = hackrf_set_antenna_enable(device.get(), 0); rc != HACKRF_SUCCESS) {
        return fail(std::format("{}: could not turn antenna port power off: "
                                "hackrf_set_antenna_enable returned {}",
                                caps.display_name, rc_text(rc)),
                    rc);
    }
    if (const int rc = hackrf_set_freq(device.get(), static_cast<std::uint64_t>(config.center_hz));
        rc != HACKRF_SUCCESS) {
        return fail(std::format("{}: the HackRF refused {} Hz: hackrf_set_freq returned {}",
                                caps.display_name, config.center_hz, rc_text(rc)),
                    rc);
    }
    if (auto applied = HackrfSource::apply_rate(device.get(), config.rate); !applied) {
        return std::unexpected(with_context(applied.error(), caps.display_name));
    }
    const HackrfGains gains = hackrf_split_gain(config.gain_db);
    if (auto applied = HackrfSource::apply_gains(device.get(), gains, config.amp); !applied) {
        return std::unexpected(with_context(applied.error(), caps.display_name));
    }

    auto source = std::make_unique<HackrfSource>(std::move(device), std::move(caps), config.rate,
                                                 config.center_hz, gains, config.amp);
    source->record_gains_locked();
    return std::unique_ptr<Source>(std::move(source));
}

}  // namespace revenant::source
