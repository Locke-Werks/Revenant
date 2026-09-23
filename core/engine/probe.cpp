// The probe pool. core/engine/probe.h has the design and the numbers; this
// file is the mechanism, and the one part of it worth reading first is the
// capture handshake below, because it is what lets a sink on the completion
// thread and a worker share a buffer with no lock between them.

#include "core/engine/probe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "core/engine/cpu_budget.h"
#include "core/engine/graph.h"
#include "core/engine/load_clock.h"
#include "core/engine/spsc_ring.h"
#include "core/thread_role.h"

namespace revenant::engine {
namespace {

// How often the worker looks for finished captures and new requests when
// nothing wakes it. A probe's dwell is two seconds or more, so ten
// milliseconds of latency on noticing one has filled is half a percent of it,
// and the sink that fills it does not have to signal anything, which is what
// keeps a condition variable off the completion thread.
constexpr std::chrono::milliseconds kWorkerPoll{10};

// Requests and outcomes the rings hold. detect::TierTwo never has more than
// the pool's size outstanding, so both are far past anything it produces.
constexpr std::size_t kRingCapacity = 256;

// CPU the worker's budget may hold at once. See Impl::budget.
constexpr std::uint64_t kProbeBudgetBurstNs = 2'000'000'000;

// THE CAPTURE HANDSHAKE
//
// One capture buffer per receiver, shared between the receiver's audio sink,
// which runs on the engine's completion thread, and the pool's worker. The
// state word decides who may touch the buffer, and every transition is one
// atomic operation:
//
//   Idle     the worker's. It sizes nothing and writes the job's fields.
//   Armed    nobody's. The worker publishes a job by storing this with
//            release; the sink claims the buffer from it with a CAS to
//            Writing, and the worker claims it back with a CAS to Idle when
//            a job is cancelled.
//   Writing  the sink's, for the length of one chunk.
//   Full     the worker's again. The sink stores it with release when the
//            dwell is collected, or when the stream is not the one the job
//            was built for.
//
// The CAS is what makes a cancel safe. A plain store of Idle from the worker
// could land while the sink is part way through a chunk, the worker would
// then re-arm the buffer for the next job, and the sink's last copy would
// land in it. A worker that finds Writing leaves the cancel for its next poll.
enum CaptureState : std::uint32_t {
    kIdle = 0,
    kArmed = 1,
    kWriting = 2,
    kFull = 3,
};

struct Capture {
    std::atomic<std::uint32_t> state{kIdle};

    // Written by the worker before it stores Armed, read by the sink after it
    // has claimed the buffer, so the acquire on that CAS is what makes them
    // visible. Nothing else synchronises them and nothing else needs to.
    std::uint64_t want_epoch = 0;
    dsp::SampleRate rate = 0;
    std::uint32_t wanted = 0;
    std::uint32_t settle = 0;

    // The sink's while Writing, the worker's while Idle or Full.
    std::uint32_t filled = 0;
    dsp::SampleIndex first_sample = 0;

    // Non-zero when a chunk arrived at a rate other than the one the job was
    // built for. The sink sets it and stores Full, so a probe that can never
    // fill fails rather than waiting for ever.
    dsp::SampleRate wrong_rate = 0;

    // Sized once, when the receiver is built, to the dwell its bucket
    // collects. A receiver never changes bucket, because a new bucket is a new
    // receiver, so this is never reallocated while a sink can see it.
    std::vector<dsp::Complex32> samples;
};

// The completion thread. Copies and returns; never waits, never allocates.
[[nodiscard]] Status take_chunk(Capture& capture, const AudioChunk& chunk) {
    std::uint32_t expected = kArmed;
    if (!capture.state.compare_exchange_strong(expected, kWriting, std::memory_order_acquire,
                                               std::memory_order_relaxed)) {
        return {};
    }

    // Frames from before the retune this job waits for. AudioChunk::
    // tuning_epoch is the boundary and VrxStatus::tuning_epoch is where it is
    // heading; core/engine/engine.h says why counting changes instead is
    // wrong.
    if (chunk.tuning_epoch < capture.want_epoch) {
        capture.state.store(kArmed, std::memory_order_release);
        return {};
    }
    if (chunk.channels != 2 || chunk.rate != capture.rate) {
        capture.wrong_rate = chunk.rate != capture.rate ? chunk.rate : -1;
        capture.state.store(kFull, std::memory_order_release);
        return {};
    }

    const std::size_t frames = chunk.samples.size() / 2;
    std::size_t index = std::min<std::size_t>(capture.settle, frames);
    capture.settle -= static_cast<std::uint32_t>(index);

    if (capture.filled == 0 && index < frames) {
        capture.first_sample = chunk.start + index;
    }
    while (index < frames && capture.filled < capture.wanted) {
        capture.samples[capture.filled] =
            dsp::Complex32{chunk.samples[2 * index], chunk.samples[2 * index + 1]};
        ++capture.filled;
        ++index;
    }

    capture.state.store(capture.filled >= capture.wanted ? kFull : kArmed,
                        std::memory_order_release);
    return {};
}

// The most a receiver at this bucket can be asked to collect, which is what its
// capture buffer is sized to when it is built. A receiver never changes
// bucket, but within one bucket a narrow detection collects
// kProbeIdentifyDwellSeconds and a wider one kProbeDwellSeconds, and the
// buffer cannot grow while the sink can see it. Narrow detections only ever
// land at the floor bucket or below, since four times kProbeIdentifyNarrowHz is
// under kProbeFloorRate, so only those buckets pay for the longer buffer.
[[nodiscard]] std::uint32_t capacity_for(dsp::SampleRate rate) {
    const double seconds =
        rate <= kProbeFloorRate ? std::max(kProbeDwellSeconds, kProbeIdentifyDwellSeconds)
                                : kProbeDwellSeconds;
    return std::max(static_cast<std::uint32_t>(seconds * static_cast<double>(rate)),
                    static_cast<std::uint32_t>(characterise::kMinCharacteriseSamples));
}

static_assert(kProbeRateOverOccupied * kProbeIdentifyNarrowHz <= kProbeFloorRate,
              "a narrow detection must land at the floor bucket or below");

}  // namespace

const char* probe_status_name(ProbeStatus status) {
    switch (status) {
        case ProbeStatus::Characterised: return "characterised";
        case ProbeStatus::TooWide: return "too wide";
        case ProbeStatus::Unplaced: return "unplaced";
        case ProbeStatus::Cancelled: return "cancelled";
        case ProbeStatus::Failed: return "failed";
    }
    return "unknown";
}

Expected<ProbeShape> probe_shape(dsp::Hertz occupied_hz, dsp::SampleRate channel_rate,
                                 double dwell_seconds) {
    if (channel_rate <= 0) {
        return fail("probe_shape: the grid has no channel rate, so no probe can be placed on it");
    }

    // A detection a bin wide reports a bandwidth of a few hertz, or of none;
    // a probe for it is the floor bucket either way.
    const dsp::Hertz occupied = std::max<dsp::Hertz>(1, occupied_hz);

    // The largest bucket the channel carries. A bucket above the channel rate
    // would be the fine stage interpolating, which adds samples and no
    // information, and would break the never-clamped argument in probe.h.
    dsp::SampleRate ceiling = 0;
    for (const dsp::SampleRate rate : kProbeRates) {
        if (rate <= channel_rate) {
            ceiling = rate;
        }
    }
    if (ceiling == 0) {
        return fail(std::format(
            "probe_shape: the grid's channels run at {} S/s, under the {} S/s smallest probe "
            "bucket",
            channel_rate, kProbeRates.front()));
    }

    dsp::SampleRate chosen = 0;
    for (const dsp::SampleRate rate : kProbeRates) {
        if (rate > ceiling) {
            break;
        }
        if (rate >= kProbeFloorRate && rate >= kProbeRateOverOccupied * occupied) {
            chosen = rate;
            break;
        }
    }

    // The channel rate is under the floor bucket. The ceiling is the most the
    // grid allows, and the dwell below stretches to reach the sample floor.
    if (chosen == 0 && ceiling < kProbeFloorRate) {
        chosen = ceiling;
    }
    if (chosen == 0 || kProbeRateOverOccupied * occupied > chosen) {
        return fail(std::format(
            "a detection {} Hz wide needs a probe of at least {} S/s and the largest this grid's "
            "{} S/s channels carry is {} S/s",
            occupied, kProbeRateOverOccupied * occupied, channel_rate, ceiling));
    }

    ProbeShape shape;
    shape.rate = chosen;
    shape.bandwidth = chosen / 2;
    const auto floor = static_cast<std::uint32_t>(characterise::kMinCharacteriseSamples);
    const auto dwell = static_cast<std::uint32_t>(kProbeDwellSeconds * static_cast<double>(chosen));
    shape.characterise_samples = std::max<std::uint32_t>(dwell, floor);
    shape.samples = shape.characterise_samples;
    if (dwell_seconds > kProbeDwellSeconds && occupied <= kProbeIdentifyNarrowHz) {
        const double granted = std::min(dwell_seconds, kProbeIdentifyDwellSeconds);
        const auto longer =
            static_cast<std::uint32_t>(granted * static_cast<double>(chosen));
        shape.samples = std::max(shape.samples, longer);
    }
    shape.seconds = static_cast<double>(shape.samples) / static_cast<double>(chosen);
    return shape;
}

struct ProbePool::Impl {
    Graph* graph = nullptr;
    ProbePoolConfig config{};

    // A request as it crosses the ring: what the caller asked, and the cancel
    // generation current when it was submitted. See cancel_generation. No
    // member initialisers, because SpscRing zero-fills its storage rather
    // than constructing and refuses a type that is not trivially default
    // constructible.
    struct Queued {
        ProbeRequest request;
        std::uint64_t generation;
    };

    std::unique_ptr<SpscRing<Queued>> requests;
    std::unique_ptr<SpscRing<ProbeOutcome>> outcomes;

    struct Slot {
        VrxId id{};
        bool built = false;
        dsp::SampleRate rate = 0;
        std::shared_ptr<Capture> capture;

        // The job, while busy. Worker only.
        bool busy = false;
        bool cancelling = false;
        bool built_for_job = false;
        ProbeRequest request{};
        std::uint64_t generation = 0;
        ProbeShape shape{};
    };
    std::vector<Slot> slots;

    // Requests taken off the ring and waiting for a free receiver. Worker
    // only, so a deque is fine here.
    std::deque<Queued> pending;

    std::uint32_t next_id = 0;

    // cancel_all moves this; the worker compares it against the last value
    // it acted on.
    //
    // AND EVERY REQUEST CARRIES THE VALUE IT WAS SUBMITTED UNDER, so the
    // worker cancels only what is older than the move. The worker acts on a
    // move at its next poll, up to kWorkerPoll later, and it used to cancel
    // everything queued by then. A caller that retuned and at once asked
    // about a detection on the new centre had that request cancelled too,
    // for a retune that happened before it was made: 20 of 20 in
    // tests/engine/test_engine_retune.cpp before this, 0 of 20 after.
    std::atomic<std::uint64_t> cancel_generation{0};
    std::uint64_t seen_generation = 0;

    std::thread worker;
    std::mutex wake_lock;
    std::condition_variable wake;
    bool stopping = false;

    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> characterised{0};
    std::atomic<std::uint64_t> too_wide{0};
    std::atomic<std::uint64_t> unplaced{0};
    std::atomic<std::uint64_t> cancelled{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> outcomes_dropped{0};
    std::atomic<std::uint64_t> builds{0};
    std::atomic<std::uint64_t> retunes{0};
    std::atomic<std::uint32_t> receivers{0};
    std::atomic<std::uint32_t> busy{0};
    std::atomic<std::uint64_t> characterise_us{0};

    // The budget in step(), which is the worker's alone; what the worker has
    // spent finishing probes, in wall time and in CPU; and how often a waiting
    // probe was held back.
    //
    // Two seconds of burst, which is a dwell: probes placed together finish
    // together, sixteen of them about 0.4 s of CPU at once on a 300-emitter
    // scene, and credit built up while their captures filled has to be able
    // to pay for that. With half a second of burst the same scene labelled
    // 66 tracks in 30 s against 98 with no budget, the credit an idle dwell
    // earned being thrown away at the cap.
    CpuBudget budget{0.5, kProbeBudgetBurstNs};
    std::atomic<std::uint64_t> worker_us{0};
    std::atomic<std::uint64_t> worker_cpu_us{0};
    std::atomic<std::uint64_t> budget_waits{0};

    // --- the worker ---------------------------------------------------------

    void run() {
        describe_this_thread(L"revenant probe", ThreadClass::SigId);
        for (;;) {
            {
                std::unique_lock held(wake_lock);
                wake.wait_for(held, kWorkerPoll, [this] { return stopping; });
                if (stopping) {
                    return;
                }
            }
            step();
        }
    }

    void step() {
        const std::uint64_t generation = cancel_generation.load(std::memory_order_acquire);
        if (generation != seen_generation) {
            seen_generation = generation;
            drain_requests();

            // Order kept for what survives, which is the order they were
            // submitted in and so the order they are served in.
            std::deque<Queued> kept;
            for (const Queued& queued : pending) {
                if (queued.generation < generation) {
                    emit(cancelled_outcome(queued.request));
                } else {
                    kept.push_back(queued);
                }
            }
            pending.swap(kept);

            for (Slot& slot : slots) {
                if (slot.busy && slot.generation < generation) {
                    slot.cancelling = true;
                }
            }
        }

        for (std::size_t i = 0; i < slots.size(); ++i) {
            Slot& slot = slots[i];
            if (!slot.busy) {
                continue;
            }
            if (slot.cancelling) {
                try_cancel(slot);
                continue;
            }
            if (slot.capture->state.load(std::memory_order_acquire) == kFull) {
                finish(slot, static_cast<std::uint32_t>(i));
            }
        }

        drain_requests();

        // THE BUDGET, core/engine/cpu_budget.h. Every finish is charged the
        // CPU it took, characterise() and identify() together, and a waiting
        // probe starts only while the bucket has credit, so however busy the
        // band is the worker averages no more than cpu_budget of one core.
        // What that costs is probes, not correctness: tier two keeps at most
        // the pool's size outstanding, so a slower worker means fewer probes
        // a second and longer between answers.
        if (!pending.empty() && !budget.may_start(load_clock_ns())) {
            budget_waits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        while (!pending.empty()) {
            Slot* free = choose_slot(pending.front().request);
            if (free == nullptr) {
                break;
            }
            const Queued queued = pending.front();
            pending.pop_front();
            start(*free, queued.request, static_cast<std::uint32_t>(free - slots.data()));
            free->generation = queued.generation;
        }
    }

    void drain_requests() {
        Queued queued{};
        while (requests->read(std::span<Queued>(&queued, 1)) == 1) {
            pending.push_back(queued);
        }
    }

    // A free receiver already running the bucket this request wants, then one
    // never built, then any free one, which is rebuilt. Null when all are busy.
    [[nodiscard]] Slot* choose_slot(const ProbeRequest& request) {
        auto shape = probe_shape(request.occupied_hz, config.channel_rate, request.dwell_seconds);
        Slot* unbuilt = nullptr;
        Slot* any = nullptr;
        for (Slot& slot : slots) {
            if (slot.busy) {
                continue;
            }
            if (shape && slot.built && slot.rate == shape->rate) {
                return &slot;
            }
            if (!slot.built && unbuilt == nullptr) {
                unbuilt = &slot;
            }
            if (any == nullptr) {
                any = &slot;
            }
        }
        return unbuilt != nullptr ? unbuilt : any;
    }

    void start(Slot& slot, const ProbeRequest& request, std::uint32_t index) {
        ProbeOutcome outcome = blank_outcome(request);
        outcome.slot = index;

        auto shape = probe_shape(request.occupied_hz, config.channel_rate, request.dwell_seconds);
        if (!shape) {
            outcome.status = ProbeStatus::TooWide;
            emit(outcome);
            return;
        }
        outcome.rate = shape->rate;

        VrxParams params;
        params.center = request.center;
        params.demod = Demod::Raw;
        params.bandwidth = shape->bandwidth;
        params.audio_rate = shape->rate;
        params.stereo = false;

        auto placement = place(config.grid, config.source_rate, params);
        if (!placement) {
            outcome.status = ProbeStatus::Unplaced;
            emit(outcome);
            return;
        }

        bool built_now = false;
        bool retuned = false;
        if (slot.built && slot.rate == shape->rate) {
            retuned = static_cast<bool>(graph->set_vrx_params(slot.id, params, *placement));
        }
        if (!retuned) {
            if (slot.built) {
                // Discarded: the only refusal is an id the graph no longer
                // holds, and then there is nothing to remove.
                static_cast<void>(graph->remove_vrx(slot.id));
                slot.built = false;
                receivers.fetch_sub(1, std::memory_order_relaxed);
            }
            if (!build(slot, params, *placement, shape->rate, capacity_for(shape->rate))) {
                outcome.status = ProbeStatus::Failed;
                emit(outcome);
                return;
            }
            built_now = true;
        }

        auto status = graph->vrx_status(slot.id);
        if (!status) {
            outcome.status = ProbeStatus::Failed;
            emit(outcome);
            return;
        }

        Capture& capture = *slot.capture;
        capture.want_epoch = status->tuning_epoch;
        capture.rate = shape->rate;
        capture.wanted = shape->samples;
        capture.settle = kProbeSettleSamples;
        capture.filled = 0;
        capture.first_sample = 0;
        capture.wrong_rate = 0;

        slot.busy = true;
        slot.cancelling = false;
        slot.built_for_job = built_now;
        slot.request = request;
        slot.shape = *shape;
        busy.fetch_add(1, std::memory_order_relaxed);
        (built_now ? builds : retunes).fetch_add(1, std::memory_order_relaxed);

        capture.state.store(kArmed, std::memory_order_release);
    }

    [[nodiscard]] bool build(Slot& slot, const VrxParams& params, const VrxPlacement& placement,
                             dsp::SampleRate rate, std::uint32_t samples) {
        auto capture = std::make_shared<Capture>();
        capture->samples.resize(samples);

        const VrxId id{next_id++};
        if (auto added = graph->add_vrx(id, params, placement, VrxRole::Probe); !added) {
            return false;
        }

        // The sink holds its own reference, so a frame still in flight after
        // the receiver is removed writes into a buffer that still exists and
        // that nobody will arm again.
        std::shared_ptr<Capture> held = capture;
        if (auto wired = graph->set_audio_sink(
                id, [held](const AudioChunk& chunk) { return take_chunk(*held, chunk); });
            !wired) {
            static_cast<void>(graph->remove_vrx(id));
            return false;
        }

        slot.id = id;
        slot.built = true;
        slot.rate = rate;
        slot.capture = std::move(capture);
        receivers.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void try_cancel(Slot& slot) {
        std::uint32_t expected = kArmed;
        const bool claimed =
            slot.capture->state.compare_exchange_strong(expected, kIdle, std::memory_order_acquire,
                                                        std::memory_order_relaxed) ||
            expected == kFull;
        if (!claimed) {
            // Writing: the sink holds it for the length of one chunk. The
            // next poll takes it back.
            return;
        }
        slot.capture->state.store(kIdle, std::memory_order_relaxed);
        slot.busy = false;
        slot.cancelling = false;
        busy.fetch_sub(1, std::memory_order_relaxed);
        emit(cancelled_outcome(slot.request));
    }

    void finish(Slot& slot, std::uint32_t index) {
        const std::uint64_t began = load_clock_ns();
        const std::uint64_t cpu_began = this_thread_cpu_ns();
        finish_timed(slot, index);
        const std::uint64_t ended = load_clock_ns();
        const std::uint64_t cpu = this_thread_cpu_ns() - cpu_began;
        worker_us.fetch_add((ended - began) / 1000U, std::memory_order_relaxed);
        worker_cpu_us.fetch_add(cpu / 1000U, std::memory_order_relaxed);

        // See THE BUDGET in step().
        budget.charge(cpu, ended);
    }

    void finish_timed(Slot& slot, std::uint32_t index) {
        Capture& capture = *slot.capture;
        ProbeOutcome outcome = blank_outcome(slot.request);
        outcome.slot = index;
        outcome.built = slot.built_for_job;
        outcome.rate = capture.rate;
        outcome.samples = capture.filled;
        outcome.first_sample = capture.first_sample;

        if (capture.wrong_rate != 0 || capture.filled < capture.wanted) {
            outcome.status = ProbeStatus::Failed;
        } else {
            characterise::CharacteriseConfig asked;
            asked.rate = capture.rate;

            // The detection's own width, so a family keyed faster than the
            // track is wide is refused rather than reported. See
            // CharacteriseConfig::detection_bandwidth_hz.
            asked.detection_bandwidth_hz = static_cast<double>(slot.request.occupied_hz);

            // The first kProbeDwellSeconds, whatever was collected: a narrow
            // detection's longer dwell is for identification, and every
            // measurement of the characteriser was taken at two seconds.
            const std::uint32_t characterised_length =
                std::min(capture.filled, slot.shape.characterise_samples);

            const auto began = std::chrono::steady_clock::now();
            auto result = characterise::characterise(
                dsp::ConstComplexSpan(capture.samples.data(), characterised_length), asked);
            const auto spent = std::chrono::steady_clock::now() - began;
            const auto micros =
                std::chrono::duration_cast<std::chrono::microseconds>(spent).count();
            outcome.characterise_ms = static_cast<double>(micros) / 1000.0;
            characterise_us.fetch_add(static_cast<std::uint64_t>(micros),
                                      std::memory_order_relaxed);

            if (!result) {
                outcome.status = ProbeStatus::Failed;
            } else {
                const characterise::Characterisation& found = *result;
                outcome.status = ProbeStatus::Characterised;
                outcome.family = found.family;
                outcome.confidence = found.family_confidence;
                outcome.symbol_rate_hz =
                    found.symbol_rate.found ? found.symbol_rate.symbol_rate_hz : 0.0;
                outcome.order =
                    found.order.found ? static_cast<std::uint32_t>(found.order.order) : 0U;
                outcome.tone_count =
                    found.tones.found ? static_cast<std::uint32_t>(found.tones.tone_count) : 0U;
                outcome.concentration = found.spectral_concentration;
                outcome.may_drive_detection = characterise::may_drive_detection(found);
                outcome.psk_without_symbol_rate = found.psk_without_symbol_rate;
                outcome.psk_tone_pair = found.psk_tone_pair;
                outcome.symbol_rate_exceeds_detection = found.symbol_rate_exceeds_detection;
                outcome.double_sideband = found.double_sideband;

                // Then the protocols the family and the width make plausible,
                // over everything collected. A failure here is not the probe's:
                // the family stands and the protocol stays None.
                identify::IdentifyHints hints;
                hints.family = found.family;
                hints.symbol_rate_hz = outcome.symbol_rate_hz;
                hints.occupied_hz = static_cast<double>(slot.request.occupied_hz);
                identify::IdentifyConfig identify_config;
                identify_config.rate = capture.rate;
                identify_config.dmr = config.identify_dmr;

                const auto identify_began = std::chrono::steady_clock::now();
                auto identified = identify::identify(
                    dsp::ConstComplexSpan(capture.samples.data(), capture.filled), hints,
                    identify_config);
                const auto identify_spent = std::chrono::steady_clock::now() - identify_began;
                outcome.identify_ms =
                    static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                            identify_spent)
                                            .count()) /
                    1000.0;
                if (identified) {
                    outcome.protocol = identified->protocol;
                    outcome.protocol_confidence = identified->confidence;
                    outcome.protocol_verified = identified->verified;
                }
            }
        }

        capture.state.store(kIdle, std::memory_order_relaxed);
        slot.busy = false;
        busy.fetch_sub(1, std::memory_order_relaxed);
        emit(outcome);
    }

    [[nodiscard]] ProbeOutcome blank_outcome(const ProbeRequest& request) const {
        ProbeOutcome outcome{};
        outcome.tag = request.tag;
        outcome.status = ProbeStatus::Failed;
        outcome.family = characterise::ModulationFamily::Unknown;
        outcome.center = request.center;
        outcome.occupied_hz = request.occupied_hz;
        return outcome;
    }

    [[nodiscard]] ProbeOutcome cancelled_outcome(const ProbeRequest& request) const {
        ProbeOutcome outcome = blank_outcome(request);
        outcome.status = ProbeStatus::Cancelled;
        return outcome;
    }

    void emit(const ProbeOutcome& outcome) {
        switch (outcome.status) {
            case ProbeStatus::Characterised:
                characterised.fetch_add(1, std::memory_order_relaxed);
                break;
            case ProbeStatus::TooWide: too_wide.fetch_add(1, std::memory_order_relaxed); break;
            case ProbeStatus::Unplaced: unplaced.fetch_add(1, std::memory_order_relaxed); break;
            case ProbeStatus::Cancelled: cancelled.fetch_add(1, std::memory_order_relaxed); break;
            case ProbeStatus::Failed: failed.fetch_add(1, std::memory_order_relaxed); break;
        }
        if (outcomes->write(std::span<const ProbeOutcome>(&outcome, 1)) != 1) {
            outcomes_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

Expected<std::unique_ptr<ProbePool>> ProbePool::create(Graph& graph,
                                                       const ProbePoolConfig& config) {
    if (config.size == 0 || config.size > kMaxProbeReceivers) {
        return fail(std::format("a probe pool holds 1 to {} receivers, asked for {}",
                                kMaxProbeReceivers, config.size));
    }
    if (config.source_rate <= 0 || config.channel_rate <= 0) {
        return fail("a probe pool needs the grid's source and channel rates");
    }
    if (config.first_id == 0) {
        return fail("a probe pool's first id cannot be zero, which is no receiver");
    }
    if (!(config.cpu_budget > 0.0) || config.cpu_budget > 1.0) {
        return fail(std::format("a probe pool's CPU budget is a fraction of one core above zero "
                                "and at most one, asked for {}",
                                config.cpu_budget));
    }

    std::unique_ptr<ProbePool> pool(new (std::nothrow) ProbePool());
    if (pool == nullptr) {
        return fail("could not allocate the probe pool");
    }
    pool->impl_ = std::make_unique<Impl>();
    Impl& impl = *pool->impl_;
    impl.graph = &graph;
    impl.config = config;
    impl.budget = CpuBudget(config.cpu_budget, kProbeBudgetBurstNs);
    impl.next_id = config.first_id;
    impl.slots.resize(config.size);

    auto requests = SpscRing<Impl::Queued>::create(kRingCapacity);
    if (!requests) {
        return std::unexpected(with_context(requests.error(), "the probe request ring"));
    }
    auto outcomes = SpscRing<ProbeOutcome>::create(kRingCapacity);
    if (!outcomes) {
        return std::unexpected(with_context(outcomes.error(), "the probe outcome ring"));
    }
    impl.requests = std::move(*requests);
    impl.outcomes = std::move(*outcomes);

    impl.worker = std::thread([&impl] { impl.run(); });
    return pool;
}

ProbePool::~ProbePool() {
    if (impl_ == nullptr) {
        return;
    }
    {
        const std::scoped_lock held(impl_->wake_lock);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    for (const Impl::Slot& slot : impl_->slots) {
        if (slot.built) {
            static_cast<void>(impl_->graph->remove_vrx(slot.id));
        }
    }
}

Status ProbePool::submit(const ProbeRequest& request) {
    // Stamped here, on the caller's thread, so a request made after
    // cancel_all returned carries the new generation however long the worker
    // takes to notice the move.
    const Impl::Queued queued{
        .request = request,
        .generation = impl_->cancel_generation.load(std::memory_order_acquire)};
    if (impl_->requests->write(std::span<const Impl::Queued>(&queued, 1)) != 1) {
        impl_->refused.fetch_add(1, std::memory_order_relaxed);
        return fail(std::format("the probe request ring holds {} and is full; take outcomes "
                                "before submitting more",
                                impl_->requests->capacity()));
    }
    impl_->submitted.fetch_add(1, std::memory_order_relaxed);
    return {};
}

std::size_t ProbePool::take(std::span<ProbeOutcome> out) {
    return impl_->outcomes->read(out);
}

void ProbePool::cancel_all() {
    impl_->cancel_generation.fetch_add(1, std::memory_order_acq_rel);
    impl_->wake.notify_all();
}

ProbeStats ProbePool::stats() const {
    const Impl& impl = *impl_;
    ProbeStats out;
    out.submitted = impl.submitted.load(std::memory_order_relaxed);
    out.characterised = impl.characterised.load(std::memory_order_relaxed);
    out.too_wide = impl.too_wide.load(std::memory_order_relaxed);
    out.unplaced = impl.unplaced.load(std::memory_order_relaxed);
    out.cancelled = impl.cancelled.load(std::memory_order_relaxed);
    out.failed = impl.failed.load(std::memory_order_relaxed);
    out.refused = impl.refused.load(std::memory_order_relaxed);
    out.outcomes_dropped = impl.outcomes_dropped.load(std::memory_order_relaxed);
    out.builds = impl.builds.load(std::memory_order_relaxed);
    out.retunes = impl.retunes.load(std::memory_order_relaxed);
    out.receivers = impl.receivers.load(std::memory_order_relaxed);
    out.busy = impl.busy.load(std::memory_order_relaxed);
    out.size = impl.config.size;
    out.characterise_ms_total =
        static_cast<double>(impl.characterise_us.load(std::memory_order_relaxed)) / 1000.0;
    out.worker_ms_total =
        static_cast<double>(impl.worker_us.load(std::memory_order_relaxed)) / 1000.0;
    out.worker_cpu_ms_total =
        static_cast<double>(impl.worker_cpu_us.load(std::memory_order_relaxed)) / 1000.0;
    out.budget_waits = impl.budget_waits.load(std::memory_order_relaxed);
    return out;
}

bool ProbePool::owns(VrxId id) const { return id.value >= impl_->config.first_id; }

}  // namespace revenant::engine
