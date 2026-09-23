// The engine: the whole chain from a source to audio, and the only thing that
// knows the order the stages run in.
//
// It is headless and has no Qt anywhere. The GUI is one client of the RPC
// surface above this, a script is another, and a remote instance is a third.
// That falls out for free only if it is true from the start, which is why the
// engine is built and tested with no client at all.
//
// The one promise everything else rests on: samples cross the bus once, into
// the device ring, and nothing returns to host memory except audio PCM,
// decoded symbols and detection metadata. Any method here that would break
// that does not exist.
//
// THE SPECTRUM SURFACE IS A DELIBERATE ADDITION TO A FILE TREATED AS FROZEN
//
// Everything above was written before there was any way to see a signal that
// no receiver was tuned to, and docs/detection.md records the consequence:
// the waterfall, the detector and the fine-tuning display are all downstream
// of a full-span spectrum, and there was no method here that returned one.
//
// It is shaped exactly like the audio sink, and that is the argument for it
// rather than for a polling method. A frame is produced by the device once
// per block whether anyone is looking or not, it is already coming back
// through a readback buffer on the completion thread, and a poll would either
// hand back the frame that happened to be latest, which drops most of them
// and tears the one it returns, or grow a queue this layer would then own. A
// callback delivers each frame once, on the thread that read it, with its
// buffer valid for the call and not after.
//
// A spectrum frame is metadata rather than samples, and it is not cheap. An
// earlier version of this comment called it "a fraction of a percent of what
// the stream itself would cost to bring home", which is wrong by about two
// orders of magnitude and was the only argument offered for letting it cross
// a bus this file declares closed.
//
// Measured at the defaults above. M*N/2 at 64 channels and a 2048-point
// transform is 65536 bins of four bytes, so 256 KiB per frame, one frame per
// block. At 20 MS/s a block is 65536 samples, so that is 305 frames a second
// and 80 MB/s coming back, against the 160 MB/s the cf32 stream costs going
// out. Half, not a fraction of a percent.
//
// It is still the right trade and the reason is different from the one that
// was written down. The frame is a REDUCTION: 80 MB/s buys the display and
// the detector everything they need about the whole span, where bringing the
// span itself home would cost twice that and leave both of them to transform
// it on the host. What makes it affordable is that it is bounded by the bin
// count rather than by the sample rate, so it does not grow when the radio
// gets wider. A caller budgeting PCIe or callback latency should use the
// number above and not the adjective. Nothing here returns complex baseband.
//
// AND THE PASSBAND IS A SECOND REDUCTION, WHICH THE PROMISE WAS NEVER
// AMENDED FOR
//
// The paragraphs above were written when the full-span frame was the only
// addition, and they read as though it still is. PassbandFrame is a second
// thing crossing the bus that is neither audio, nor a decoded symbol, nor
// detection metadata, and it arrived without this block saying so. Recorded
// here rather than folded in, because the promise at the top of this file is
// what a reader checks a new surface against, and a surface that is already
// in the tree and not in the promise makes the promise the wrong instrument.
//
// It passes the same test the full-span frame passes, on the same arithmetic
// and more cheaply. A passband frame is passband_transform / 2 bins of four
// bytes, one per block per SUBSCRIBED receiver, so at 4096 points that is
// 8 KiB a frame and 2.5 MB/s at the 305 blocks a second a 20 MS/s source
// runs. Against the full-span frame's 80 MB/s it is noise, and against the
// 160 MB/s the cf32 stream costs going out it is under two percent. It is
// bounded by the transform size rather than by the display rate, so
// widening a receiver's filter does not widen it.
//
// WHAT THAT PARAGRAPH USED TO SAY: "passband_transform bins", "16 KiB a
// frame and 5.0 MB/s" and "three percent". The frame kept every bin of its
// transform until the display tap, which keeps the central half.
//
// What keeps it bounded in the other direction is that it is opt in per
// receiver: a rack of fifty receivers with one under examination pays for
// one. Fifty at once would be 250 MB/s and past what the full-span frame
// costs, so the per-receiver sink is load-bearing rather than tidy, and a
// caller that attaches sinks in bulk has left this promise's terms.
//
// The promise is therefore: samples cross once, and what comes back is audio
// PCM, decoded symbols, detection metadata, and bounded spectral reductions
// of the span and of a receiver's own passband. A fourth thing crossing
// amends this block again rather than arriving quietly.
//
// AND THE FOURTH THING IS A PROBE'S BASEBAND, WHICH THIS PARAGRAPH IS THE
// AMENDMENT FOR
//
// core/engine/probe.h places receivers of its own on detections and brings
// their complex baseband home for core/characterise, because that library is
// host code and tier two of docs/detection.md is a question only complex
// baseband answers. It is bounded the way the other two reductions are: by a
// count and a rate the engine sets, not by the source. A probe runs at one of
// the rates in kProbeRates, eight bytes a sample, so the floor bucket of
// 12000 S/s is 96 KB/s and the top one of 192000 S/s is 1.5 MB/s, and there
// are at most EngineConfig::probe_receivers of them, never more than
// kMaxProbeReceivers. Four at the floor bucket is 384 KB/s against the
// 160 MB/s the cf32 stream costs going out.
//
// It is paid whether or not a probe is collecting, because an idle probe is a
// receiver the graph still records and reads back every block: the pool keeps
// them so a retune is a push constant rather than a rebuild. Zero
// probe_receivers is none of it, which is the default.
//
// So: samples cross once, and what comes back is audio PCM, decoded symbols,
// detection metadata, the two bounded spectral reductions, and a bounded
// number of probes' complex baseband at their own bounded rates.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/dsp/front_end_correction.h"
#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/engine/device_ring.h"
#include "core/engine/probe.h"
#include "core/engine/vrx.h"
#include "core/error.h"
#include "core/gpu/context.h"
#include "core/source/calibration.h"
#include "core/source/registry.h"
#include "core/source/source.h"

namespace revenant::engine {

struct EngineConfig {
    // -1 chooses, or REVENANT_GPU_INDEX decides. The conformance matrix passes
    // an explicit index.
    int gpu_index = -1;

    // How much capture history the device ring holds. Shrunk to fit the
    // device, and what was achieved is reported rather than assumed.
    double ring_seconds = 8.0;

    // Coarse grid. channels must be a power of two and decimation must divide
    // it; the project uses 2x oversampling, so decimation is channels/2.
    //
    // ZERO CHOOSES IT FROM THE SOURCE, which is what a tool with an operator
    // behind it should pass. See default_channel_count below: 64 is a good
    // grid at 20 MS/s and an unusable one at 2.4 MS/s, and the difference is
    // the whole of whether a broadcast FM receiver works.
    //
    // The default stays 64 rather than zero because this is a library knob
    // and every existing caller, every test and every benchmark was written
    // against a fixed grid. revenant-engine passes zero.
    std::uint32_t channels = 64;
    std::uint32_t taps_per_branch = 17;

    // Audio rate every receiver resamples to unless it asks for another.
    dsp::SampleRate audio_rate = 48'000;

    // Samples the upload stages at a time. Larger is fewer submissions and
    // more latency.
    std::size_t block_samples = 65'536;

    // How fast a Demand source is asked to deliver, as a multiple of
    // realtime. Zero, the default, is unthrottled, and unthrottled is still
    // what happens when nothing is holding a stopwatch.
    //
    // This is not a throttle on the engine and there is still no code path
    // for faster than realtime. It is the source being asked to deliver on a
    // clock, which is the one thing a live monitor needs and a radio supplies
    // for free: a capture replayed as fast as the GPU retires it produces
    // audio a loudspeaker cannot accept, and what the listener hears is the
    // monitor's backlog being trimmed rather than the recording. A Paced
    // source ignores this, because its own hardware already sets the rate.
    //
    // A DEFAULT, NOT THE LAST WORD. A file whose URI states pace= runs at that
    // (Source::own_pace), and set_source_pace changes it while a source is
    // open. SourcePacing::paced_by is what is in force.
    double pace = 0.0;

    // Points in the full-span spectrum's per-channel transform. Zero, the
    // default, builds no spectrum stage at all.
    //
    // Off by default because it is not free: a pipeline, a twiddle circle, a
    // window, and one readback region per frame in flight, for a frame nobody
    // has asked to see. A headless recording pays none of it.
    //
    // The stage is not one transform across the whole span. The span is
    // covered by transforming each coarse channel the channelizer has already
    // produced and keeping the central half of each one, which is what fits
    // the shared-memory budget docs/fft.md scopes the FFT kernel to. The
    // number here is therefore per channel, so a frame is
    // grid.channels * spectrum_transform / 2 bins wide. It is clamped to what
    // this device's shared memory holds, and EngineInfo::spectrum reports
    // what was settled on.
    std::uint32_t spectrum_transform = 0;

    // Holds one or both ends of the spectrum's colour map still, in dBFS.
    //
    // Empty is automatic, which docs/ui-spectrum.md makes the default because
    // it is right almost always. The exception it names is comparing two
    // captures, where a scale that moves is a scale that lies about which
    // signal was stronger, and that is what these are for.
    //
    // They sit on the config rather than behind a setter because that is
    // where the rest of the spectrum stage's shape sits, and because a pin
    // taken for a comparison is decided before the run rather than during it.
    // A display that wants to pin interactively has everything it needs on
    // the frame already: SpectrumFrame carries the raw percentiles beside the
    // smoothed ends.
    std::optional<float> spectrum_floor_db;
    std::optional<float> spectrum_ceiling_db;

    // Points in a receiver's passband transform. Zero, the default, builds no
    // passband stage at all, and then set_passband_sink is refused the same
    // way set_spectrum_sink is at spectrum_transform zero.
    //
    // Two levels of opt-in, and they are answering different costs. This one
    // decides whether the machinery exists: a pipeline, a window table, and
    // in every receiver a display ring a window deep and a display tap
    // table. A headless recording leaves it zero and pays none of that.
    // Attaching a sink to one receiver is the second: until a receiver has
    // one, it allocates no transform buffers and dispatches neither its
    // display tap nor a transform, so a rack of fifty receivers with one
    // under examination pays for one.
    //
    // Unlike spectrum_transform this is not per channel. A passband frame is
    // the central half of a transform this many points long, so it is this
    // many over two bins wide and covers half the receiver's display rate:
    // core/dsp/vrx_reference.h, "The display tap", has the rate rule.
    //
    // WHAT THIS USED TO SAY: that a passband frame "is this many bins wide
    // and covers one receiver's whole demodulation rate, because the fine
    // stream is one stream rather than a bank", and that the machinery was
    // "two window tables, and room in every receiver's fine ring". The pane
    // transformed the fine stream, which is after the receiver's own filter.
    std::uint32_t passband_transform = 0;

    // Probe receivers, for tier two of docs/detection.md: receivers the
    // engine places on detections for itself and characterises. Zero, the
    // default, builds no pool, and then submit_probe is refused.
    //
    // A standing cost rather than a per-probe one. core/engine/probe.h keeps
    // every receiver it has built so the next probe in the same rate bucket
    // is a retune, and a receiver the graph holds is recorded and read back
    // on every block whether or not it is collecting. The top of this file
    // has the bus arithmetic and docs/detection.md the measured GPU cost.
    // Clamped to kMaxProbeReceivers.
    std::uint32_t probe_receivers = 0;

    // Where each device's calibration is kept between sessions, keyed by its
    // serial. EMPTY KEEPS NOTHING: a calibration set on this engine applies
    // for the life of the open source and is not restored next time, which is
    // what every test and every library caller wants. revenant-engine passes
    // %LOCALAPPDATA%\Revenant\calibration.txt. core/source/calibration.h has
    // the format and why the engine rather than the client keeps it.
    std::string calibration_path;
};

// The widest receiver a grid is expected to be able to carry, anywhere on it.
//
// 200 kHz, which is dsp::default_passband(Demod::Wfm) and the channel the
// broadcast FM band plan allocates. It is the widest entry in that table by
// a factor of twelve, so a grid that carries it carries every other mode
// with room to spare.
inline constexpr dsp::Hertz kWidestReceiverHz = 200'000;

// The channel count a source rate implies, which is what EngineConfig gets
// when it is left at zero.
//
// WHY THE DEFAULT CANNOT BE A CONSTANT, WHICH IS WHAT IT WAS
//
// The grid is 2x oversampled, so one channel's stream is 2*rate/M wide and a
// receiver sits up to half a spacing off its channel's centre.
// dsp::max_channel_bandwidth turns that into a width of rate/M guaranteed
// for a receiver landing anywhere, and twice that for one landing on a
// centre. At 2.4 MS/s and 64 channels the guaranteed figure is 37.5 kHz, so
// an operator clicking a broadcast FM station gets a 200 kHz receiver
// clamped to a fraction of what it asked for: the audio is mush and the
// waterfall shows full strength, because the signal is all there and the
// receiver is not looking at it.
//
// So the rule is the constraint rather than a number: the largest power of
// two whose guaranteed channel width still carries kWidestReceiverHz. At
// 20 MS/s it answers 64, which is why nothing about a wideband capture
// changes; at 2.4 MS/s it answers 8, which is what revenant-engine's own
// --help example has been telling the reader to pass by hand.
//
// WHAT IT COSTS, BECAUSE IT IS NOT FREE. A full-span frame is
// M * transform / 2 bins wide and a bin is 2*rate/(M*transform) hertz, so
// eight times fewer channels is eight times fewer bins and eight times
// coarser resolution on the waterfall. That is the trade: a receiver an
// operator clicks on works, and the display is coarser. A caller that wants
// the other side of it names a count and gets exactly that count.
//
// Two is the floor rather than one. A source too narrow to carry
// kWidestReceiverHz in one channel cannot carry it at any M, and one channel
// is a channelizer that channelizes nothing; two keeps the bank real and
// keeps the oversampling that stops a signal on a boundary falling between
// two channels.
//
// WHAT HAPPENS WHEN A CALLER PINS SOMETHING NARROWER ANYWAY, which this
// function cannot stop and which is why it is not the whole answer. A count
// named explicitly is honoured, so a 64-channel grid over 2.4 MS/s is still
// reachable and a broadcast FM receiver still does not fit on it. That case
// is refused at placement rather than clamped: engine::place, which calls
// engine::channel_count_for against the mode's own channel plan to name the
// count that would have worked. The grid is chosen here, once, and
// core/engine/vrx_place.cpp lists what holding it fixed buys.
//
// Pure, and exported so revenant-engine can say what the default would have
// been when an operator pins something narrower. Two copies of this rule
// would be two policies, which is also why the arithmetic itself lives in
// engine::channel_count_for and this is that function at kWidestReceiverHz.
[[nodiscard]] std::uint32_t default_channel_count(dsp::SampleRate rate);

// The frequency axis of a spectrum frame, and how wide one is.
//
// Both frequencies are exact rationals rather than integer hertz, for the
// reason dsp::ChannelCentre gives: a bin is source_rate / (D * N) hertz wide
// and a coarse channel centre is k * source_rate / M, and neither is a whole
// number of hertz in general. Rounding either here would put an unsourceable
// offset into every frequency a person clicks on, which is what the
// integer-hertz convention exists to prevent. A display converts once, at the
// point it draws a label.
//
// Both are relative to the source's baseband DC, the same frame VrxParams is
// in. Add EngineInfo::source_center for absolute radio frequency.
struct SpectrumGeometry {
    // Points in one coarse channel's transform, and the central half of that
    // which is kept.
    std::uint32_t transform = 0;
    std::uint32_t bins_per_channel = 0;

    // Coarse channels the frame tiles, and the total bins across the span.
    std::uint32_t channels = 0;
    std::uint32_t bins = 0;

    std::int64_t bin_width_numerator = 0;
    std::int64_t bin_width_denominator = 1;
    std::int64_t bin_zero_numerator = 0;
    std::int64_t bin_zero_denominator = 1;

    [[nodiscard]] constexpr bool enabled() const { return bins != 0; }

    [[nodiscard]] constexpr double bin_width_hz() const {
        return bin_width_denominator == 0 ? 0.0
                                          : static_cast<double>(bin_width_numerator) /
                                                static_cast<double>(bin_width_denominator);
    }

    // Centre frequency of bin zero.
    //
    // Half a coarse channel below the negative edge of the band, because the
    // lowest channel is centred on the Nyquist frequency and owns the half
    // channel either side of it. The part below -rate/2 is the same physical
    // spectrum as the part below +rate/2, since a sampled band is periodic,
    // and on any real receiver both are the front end's roll-off. A display
    // labels it as written and the axis stays continuous.
    [[nodiscard]] constexpr double bin_zero_hz() const {
        return bin_zero_denominator == 0 ? 0.0
                                         : static_cast<double>(bin_zero_numerator) /
                                               static_cast<double>(bin_zero_denominator);
    }
};

// The frequency axis of one receiver's passband frame.
//
// A separate type from SpectrumGeometry rather than a reuse of it, because
// two of that struct's fields would have to mean something else here:
// `channels` is a coarse channel count and a passband has no bank behind it,
// and `bins_per_channel` is the central half of a transform where a passband
// keeps every bin. Both are load-bearing in the full-span frame's layout, so
// a passband borrowing the struct would be a frame whose fields a consumer
// has to know not to believe.
//
// Rationals for the same reason SpectrumGeometry carries them: Fdisp / N is
// rarely a whole hertz, and neither is the fine stream's DC, which is a
// coarse channel centre plus the residual the fine stage mixed out. Rounding
// either here puts an unsourceable offset into every frequency a person
// clicks on.
//
// Both frequencies are relative to the source's baseband DC, the same frame
// VrxParams::center is in. Add EngineInfo::source_center for absolute radio
// frequency.
struct PassbandGeometry {
    // Points in the transform, and the bins kept of it, which are its
    // central half: the part of the display stream that is flat and free of
    // aliases.
    //
    // WHAT THIS USED TO SAY: "which is also the bin count: the whole band is
    // kept, edge to edge". It was, while the pane was the fine stream.
    std::uint32_t transform = 0;
    std::uint32_t bins = 0;

    // The display stream's rate, which is TWICE the width of the frame: the
    // frame is its central half. core/dsp/vrx_reference.h, "The display
    // tap", has the rule that picks it, which gives a pane at least four
    // times as wide as the passband's further edge is from the pane's
    // centre, and moves only in steps of two or more.
    //
    // WHAT THIS USED TO SAY: "The fine stream's rate, which is the width of
    // the frame", wider than the receiver's bandwidth, "and the margin is
    // where the filter's skirts are drawn". Those skirts were the fault: the
    // fine stream is after the filter, so the pane showed the filter and not
    // the air around it.
    dsp::SampleRate rate = 0;

    std::int64_t bin_width_numerator = 0;
    std::int64_t bin_width_denominator = 1;
    std::int64_t bin_zero_numerator = 0;
    std::int64_t bin_zero_denominator = 1;

    [[nodiscard]] constexpr bool enabled() const { return bins != 0; }

    [[nodiscard]] constexpr double bin_width_hz() const {
        return bin_width_denominator == 0 ? 0.0
                                          : static_cast<double>(bin_width_numerator) /
                                                static_cast<double>(bin_width_denominator);
    }

    // Centre frequency of bin zero, which is a quarter of the display rate
    // below whatever the fine stage mixed to DC. (Half the demodulation rate
    // below it, this used to say, while the frame was the whole fine stream.)
    //
    // That is NOT always the receiver's centre. For CW the fine stage
    // translates the carrier to the operator's pitch rather than to DC, so
    // this sits a pitch below VrxParams::center. Carried rather than derived
    // for exactly that reason: a display that computed the axis from
    // params.center would be one pitch out on one mode and right on the other
    // seven, which is the shape of a bug nobody finds.
    [[nodiscard]] constexpr double bin_zero_hz() const {
        return bin_zero_denominator == 0 ? 0.0
                                         : static_cast<double>(bin_zero_numerator) /
                                               static_cast<double>(bin_zero_denominator);
    }
};

// What the engine settled on, which is frequently not what was asked for.
struct EngineInfo {
    gpu::DeviceInfo device;
    RingGeometry ring;

    // A MEASUREMENT OF THE OPEN SOURCE AND NOT A SETTING, which matters
    // because dsp::GridParams does not default to empty: on its own it
    // constructs to the design's canonical M=64 D=32 and 17 taps per branch.
    // Those are the right defaults for a caller who did not name a grid and
    // the wrong ones to report from an engine that has no source, so
    // Engine::close_source zeroes all three rather than leaving the struct's.
    // Zero channels means no source, and source_rate of zero says the same.
    dsp::GridParams grid;

    dsp::SampleRate source_rate = 0;
    dsp::SampleRate channel_rate = 0;
    dsp::Hertz channel_spacing = 0;

    // Left empty when EngineConfig::spectrum_transform was zero.
    SpectrumGeometry spectrum{};

    // Points in a passband transform, after the same clamp against this
    // device's shared memory the full-span transform takes. Zero when
    // EngineConfig::passband_transform was zero.
    //
    // The transform size is all that is engine-wide. The rest of a passband's
    // axis is the receiver's, because the rate is the receiver's, so the
    // geometry arrives on each frame rather than here.
    std::uint32_t passband_transform = 0;

    // What the source's baseband DC corresponds to in real radio frequency.
    //
    // Every frequency in VrxParams is an offset from baseband DC, because
    // that is the only frame the grid has. A person tunes in absolute hertz,
    // so something has to hold the constant that relates the two, and a
    // caller that had to parse it back out of the source URI would be
    // reimplementing each backend's grammar to do it. Zero for a source whose
    // baseband is not a translation of anything, which is what a synthetic
    // scene with no declared centre is.
    //
    // IT MOVES. Engine::set_source_center writes what the device landed on
    // back here, so a caller that cached this across a retune is holding a
    // constant that no longer relates the two frames. Read it beside the
    // frequency it is being added to rather than once at startup.
    //
    // WHAT THIS COMMENT USED TO SAY: "read once when the source was opened",
    // with core/engine/engine.cpp adding that a snapshot was "the whole
    // truth for the life of the engine" because the engine had no tune call.
    // It has one as of 2026-09-20 and both have been corrected. Anything
    // written against the old reading is a client that draws every label and
    // every detection a retune's worth of hertz out, on a display where
    // nothing else looks wrong.
    dsp::Hertz source_center = 0;

    // WHICH STREAM THE SAMPLE INDICES BELONG TO. Zero before any source has
    // been opened, one for the first, and one higher for every open after it.
    //
    // THIS IS THE ONE THING A RE-OPENABLE SOURCE BREAKS THAT A RETUNE DOES
    // NOT, and it is worth being plain about the difference. A retune leaves
    // the stream alone: the samples keep arriving, the index keeps counting,
    // and what changes is the constant in source_center. Closing a source and
    // opening another starts a NEW stream, and docs/conventions.md is explicit
    // that time in this engine is an exact sample index from the start of the
    // stream. So the second stream's index 0 is a different instant from the
    // first stream's index 0, and every AudioChunk::start, SpectrumFrame
    // index, PassbandFrame window and recording offset is numbered against
    // whichever stream produced it.
    //
    // Without this field those two are indistinguishable. A consumer that
    // correlates by index, which is every consumer this engine has, would
    // silently line up audio from one radio against a spectrum frame from
    // another and find the arithmetic consistent, because it is: both are
    // honest indices into streams nothing said were different.
    //
    // It is a COUNT AND NOT A HANDLE. Compare it for equality against the
    // epoch a frame or a chunk was correlated at, and re-derive rather than
    // adjust when it differs: there is no offset between two streams, because
    // the gap between them is however long an operator spent choosing a
    // radio.
    //
    // Monotonic for the life of the engine and never reused, on the same
    // ground the receiver ids are: a number that came back would make a stale
    // correlation look current.
    std::uint64_t source_epoch = 0;
};

// Whether the front end can be pointed somewhere else, and where.
//
// ONE RANGE AND NOT THE DEVICE'S LIST, WHICH IS AN ENVELOPE AND NOT A
// PROMISE. An E4000 reaches 52 to 2200 MHz with a gap in the middle, and
// this reports the outer pair. A frequency inside a gap is still refused, by
// the source and in the source's own words, so the honest reading of this
// struct is "outside this, do not bother asking" rather than "inside this,
// it will work". It exists so a client can grey out a control it could never
// use, which is the case where a refusal is not good enough because the
// operator has to discover it by trying.
struct SourceTuning {
    // False for every file and every synthetic scene. Their centre is a
    // property of bytes already written rather than a setting, and
    // Engine::set_source_center answers with the source's own sentence
    // saying so.
    bool can_retune = false;

    dsp::Hertz low = 0;
    dsp::Hertz high = 0;
};

// One receiver a front-end retune took away.
//
// WHAT THIS USED TO SAY: "because its centre fell outside the new span". That
// was the only cause the engine reported, and not the only one there was. A
// receiver whose centre stays inside the span can still need a different
// filter from its new place in a channel, which the graph refuses to apply in
// place, and until 2026-09-23 that refusal was discarded and the receiver left
// on its old offset. It is removed now and reported here like the other.
//
// Which of the three causes it was, as a value a caller can branch on. The
// sentence below says the same thing to a person; this says it to code, which
// otherwise has to search the sentence for a phrase, and a reworded sentence
// then changes what the code does with nothing failing to say so. The cause
// decides what a caller can offer: a receiver refused only for its filter
// shape comes back from an add at the same frequency, and one outside the span
// or refused by place() does not.
enum class RetuneCause : std::uint8_t {
    // Its centre fell strictly outside the new span.
    OutsideSpan,
    // Inside the span, and place() refused it on the new grid.
    Unplaceable,
    // Placed, and the graph refused the new place because it needs a
    // different filter shape, which it will not swap into a running receiver.
    ShapeChanged,
};

struct RetuneRemoval {
    VrxId id;

    RetuneCause cause = RetuneCause::OutsideSpan;

    // The absolute frequency its centre was on: the old source centre plus
    // its baseband offset, read before the tune. This is the number an
    // operator would recognise, where the id is one only a client holds.
    dsp::Hertz frequency = 0;

    // Why, in a sentence that names the new source centre and this frequency:
    // outside the span, refused by place() on the new grid, or refused by the
    // graph because the new place needs a different filter. The last quotes
    // the graph's own refusal, which ends "a remove and an add".
    std::string reason;
};

// What Engine::set_source_center did.
//
// THE REMOVALS ARE PART OF THE ANSWER because nothing else reports them. The
// graph drops a receiver without calling anybody, so until 2026-09-23 the only
// trace of one was its id missing from vrx_ids: an audio subscriber on it got
// no end and went quiet, which is indistinguishable from a shut squelch, and
// the server above kept its RDS route, decoder routes and ownership record.
// The caller holding this list is the one place that can take those down with
// a reason.
struct SourceRetune {
    // The centre the device took, which a synthesiser with a tuning step
    // rounds.
    dsp::Hertz center = 0;

    // In the order vrx_ids listed them. Empty when every receiver came
    // along.
    std::vector<RetuneRemoval> removed;
};

// What the front-end correction stage is doing.
//
// The stage is core/shaders/iq_moments.comp and core/shaders/iq_correct.comp,
// run on each block after the convert kernel and before the channelizer, so
// every consumer of the ring sees the corrected stream. The estimate behind it
// is core/dsp/front_end_correction.h's, and it only runs while at least one
// half is switched on: a stream nobody asked to correct is not read.
struct FrontEndCorrectionStatus {
    bool dc_removal = false;
    bool iq_correction = false;

    // Blocks whose moments have been read into the estimate since the stage
    // was last switched on from off.
    std::uint64_t blocks_measured = 0;

    // Fields are published one at a time by the recording thread, so two of
    // them can come from consecutive blocks. That is a display's worth of
    // tearing and nothing acts on these numbers but a person.
    dsp::FrontEndEstimate estimate{};
};

// A device's calibration as the engine holds it, and what the engine has
// measured of its front end. docs/calibration.md is the operator's account.
struct CalibrationState {
    // False with no source open, and nothing below means anything then.
    bool open = false;

    // What it is kept under, "rtlsdr:00000001", or empty for a source that
    // carries no serial: every file and every synthetic scene.
    std::string key;

    // What is in force for the open source.
    source::DeviceCalibration settings;

    // False when the device was told a correction of its own at open, which
    // an rtlsdr URI with ppm= does. settings.correction_ppb is then held and
    // stored but not applied, because applying both corrects one crystal
    // twice. note says so.
    bool correction_applied = true;

    // True when settings are written to EngineConfig::calibration_path and
    // restored the next time this device is opened. False with no key, with
    // no path configured, or when the file could not be read or written; note
    // says which.
    bool persisted = false;
    std::string note;

    // Where the device itself is tuned, on its own crystal's scale, beside
    // EngineInfo::source_center, which is where it really listens. Equal when
    // nothing is corrected.
    dsp::Hertz device_center = 0;

    FrontEndCorrectionStatus front_end{};
};

// How fast capture is arriving, against the wall clock.
//
// THE DIAGNOSIS NOBODY COULD MAKE FROM OUTSIDE THE PROCESS. A synthetic
// source asked for 20 MS/s on this host generates about 0.20 of realtime, so
// every stage downstream starves and a listener hears audio in fragments.
// What a client could see was an audio queue that kept running dry, which is
// true about the queue and points at the wrong component: the ring is not
// starving because the wire is slow, it is starving because the source never
// produced the samples.
//
// The number existed before this struct did, in revenant-engine's own status
// line as "x 0.20", printed to a terminal a GUI operator never sees.
struct SourcePacing {
    // Capture seconds delivered per wall second over the last
    // window_seconds. 1.0 is realtime, above 1.0 is a recording being
    // replayed faster than it was made, and below 1.0 is the source falling
    // behind.
    //
    // OVER A WINDOW, AND A PAUSE THE ENGINE CAUSED IS NOT CHARGED TO IT.
    // core/engine/pacing_window.h has both and the measurement behind them:
    // the samples an RTL-SDR loses while its transfers are stopped for a
    // retune or a gain change count as delivered here, because they were
    // never late, and SourceStats::samples_lost still counts them as lost.
    // A stall nobody asked for pulls the factor down and leaves it within
    // kPacingWindowSeconds of ending.
    //
    // ZERO MEANS NOT MEASURED, which is a third state and not a stalled
    // source. Nothing has been measured until the stream has started and at
    // least one block has been delivered, so zero is what info() answers
    // between open_source and run(). A source that has stopped producing
    // reads lower from the moment its next block is more than a block's time
    // late, down to two blocks over the window, and never as zero.
    //
    // WHAT THIS COMMENT USED TO SAY, when the factor was taken over the whole
    // run: "A source that has genuinely stopped producing reports a factor
    // that decays towards zero without reaching it, because the elapsed time
    // keeps growing while the sample count does not."
    double realtime_factor = 0.0;

    // The pace in force, zero for unthrottled: the source's own when it has
    // one, a file's pace= (Source::own_pace), and EngineConfig::pace when it
    // has none, until set_source_pace changes it.
    //
    // WHAT THIS COMMENT USED TO SAY: "EngineConfig::pace, echoed." True until
    // a file could carry its own pace and a client could change it.
    //
    // CARRIED BESIDE THE MEASUREMENT BECAUSE THE MEASUREMENT ALONE CANNOT
    // TELL A FAULT FROM A SETTING. A factor of 0.5 is a source that cannot
    // keep up when this is zero and is exactly what was asked for when this
    // is 0.5. Reporting one without the other invites a client to raise an
    // alarm about a deliberate half-speed replay.
    double paced_by = 0.0;

    // True for a source whose consumer sets the rate, which is every file
    // and every synthetic scene. A Paced source runs on the device's own
    // clock and ignores paced_by entirely, so a factor below one there is
    // the ring refusing samples rather than the source being slow.
    bool demand = false;

    // Since run() started, both of them. The factor is not computed from
    // these any more; they are the run's totals, which a headless host
    // reads to wait out a start-up before it warns.
    //
    // WHAT THIS COMMENT USED TO SAY: "What the factor was computed from, so a
    // client can say how long it has been averaging over rather than
    // presenting a lifetime mean as an instantaneous reading." That is
    // window_seconds now.
    double elapsed_seconds = 0.0;
    dsp::SampleIndex samples_delivered = 0;

    // The wall seconds realtime_factor covers: kPacingWindowSeconds, plus up
    // to a tenth of a second and one block's time, and the whole run while
    // the run is younger. Zero before run(). See core/engine/pacing_window.h.
    double window_seconds = 0.0;
};

// One receiver's audio, handed to the caller on the host.
//
// WHAT THIS COMMENT USED TO SAY. Until 2026-09-20 it read "this and the
// spectrum frame are the only things that come back across the bus".
// PassbandFrame below is a third and it was added without this sentence
// being touched. The count was right once, and counting is the wrong shape
// for it, so the rule is stated instead: what comes back is audio PCM,
// decoded symbols, detection metadata and the two bounded spectral
// reductions, with the arithmetic for all of it at the top of this file.
// Everything else the engine computes stays on the device.
struct AudioChunk {
    VrxId vrx;
    dsp::SampleIndex start = 0;
    dsp::SampleRate rate = 0;

    // Interleaved if stereo, mono otherwise.
    //
    // WHAT THIS COMMENT USED TO SAY, and the raw tap has never obeyed it:
    // "Real, not complex: a demodulator that hands out complex baseband is the
    // raw tap, and that goes to a decoder rather than here."
    //
    // It comes here. core/engine/graph.cpp builds one of these for a raw tap
    // with channels set to 2 and the pair holding I and Q, and that is the
    // only complex baseband anything on the host ever sees. Measured
    // 2026-09-22 by revenant-cli --characterise, which collects an extract for
    // core/characterise through exactly this path.
    //
    // NOTHING ON THIS STRUCT SAYS WHICH IT IS. StageOutput carries a
    // complex_iq flag and it is dropped at this boundary, so a consumer tells
    // the two apart by asking engine::produces_audio about the receiver's
    // demodulator. core/rpc/server.cpp and tools/cli/main.cpp both do exactly
    // that, and a third consumer that forgets will read an IQ pair as a stereo
    // frame and hear nothing wrong.
    std::span<const float> samples;
    std::uint32_t channels = 1;

    // The squelch gate at the moment these frames were produced.
    //
    // False means the samples above are ZEROS THE GRAPH WROTE, not a quiet
    // band. core/engine/graph.cpp mutes a closed receiver in the readback
    // buffer before any sink is called, so every consumer of this chunk sees
    // the same silence, and without this flag none of them can tell the gate
    // shutting from the transmitter stopping. A recording is identical either
    // way; a subscriber's indicator is not.
    //
    // Defaulted true so a producer that does not gate says nothing about a
    // gate. It is not a claim that the signal was strong.
    bool squelch_open = true;

    // Which tuning produced these samples. Zero when the receiver was added,
    // and one higher for every retune the graph has APPLIED to it, so it is
    // monotonic per receiver and means nothing across two of them.
    //
    // WHAT IT EXISTS TO CLOSE, WHICH IS AN ORDERING AND NOT A VALUE. A
    // consumer that accumulates state about the transmitter it is hearing,
    // and there is one, has to throw that state away when the receiver is
    // pointed somewhere else. It cannot do that when set_vrx_params returns:
    // that call only QUEUES a control op, the recording thread applies it at
    // the next block boundary, and every frame already recorded is still the
    // old tuning and still on its way. A consumer resetting on the call
    // therefore resets and is then handed the old station again, which is
    // the one arrangement worse than not resetting at all, because the
    // result reads as the new station.
    //
    // So the boundary is here, on the chunk, where it is exact: the first
    // chunk carrying a higher number is the first sample of the new tuning,
    // and everything at or below the old number is the old one however late
    // it arrives. core/rpc/server.cpp's RDS decoder is the consumer this was
    // added for; a recording and a loudspeaker both ignore it, which is
    // right, because an operator dragging a dial wants the audio to follow
    // the dial rather than to gap.
    //
    // COMPARE IT AGAINST VrxStatus::tuning_epoch, AND DO NOT COUNT THE
    // TIMES IT CHANGES. Both numbers are the same monotonic per-receiver
    // count of queued retunes, read at the two ends of the control queue,
    // so a consumer reads the status after set_vrx_params returns and
    // discards every chunk below what it read. Counting the changes it
    // observes instead is wrong twice over, and both were reachable and
    // both were silent. Two retunes drained in one pass move this by two
    // and produce ONE change, so a consumer waiting for two changes waits
    // for ever. And a frame that produces no samples is never delivered, so
    // an epoch that lands only on a silent frame is never observed at all.
    // A consumer that got either wrong stopped decoding permanently and
    // read, on the wire, as a receiver pointed at a quiet band.
    //
    // IT COUNTS APPLIED RETUNES AND NOT CHANGES OF TUNING. A retune the
    // stage refuses still moves it, because the control op was applied and
    // GraphStats::vrx_retune_refusals is required to stay at zero anyway. A
    // consumer that reset for one of those pays a reacquisition on a
    // receiver that did not move, which is the cheap direction to be wrong
    // in when the expensive one is text from two stations in one struct.
    std::uint64_t tuning_epoch = 0;
};

using AudioSink = std::function<Status(const AudioChunk&)>;

// One consumer's place in a receiver's audio.
//
// Issued by Engine::attach_audio_sink, drawn from one counter for the whole
// process and never reused by anything. A token that has already been
// detached therefore detaches nothing, rather than detaching whichever
// consumer arrived next, and that holds across receivers and across engines
// as well as within one.
//
// WHAT THIS PARAGRAPH USED TO SAY, AND WHY THE COUNTER MOVED. Until
// 2026-09-20 it said "unique for the life of one Engine and never reused"
// while the counter it described was a member of AudioFanout, and there is
// one fan-out per receiver. Token 1 was therefore issued to the first
// consumer of every receiver, so detach_audio_sink called with the right
// token and the wrong VrxId detached a real consumer instead of refusing,
// and the error text below promised that could not happen. Worse, the
// engine erases a fan-out the moment its last consumer detaches, which took
// the counter with it: the next attach on that receiver started at 1 again
// and reissued a token that had already been handed out and given back. It
// was latent only because core/rpc/server.cpp erases its own bookkeeping
// before every detach, so no live caller held a token long enough to collide
// with one.
using AudioSinkId = std::uint64_t;

// More than one consumer on one receiver's audio.
//
// WHAT THIS EXISTS TO STOP. set_audio_sink below holds one sink per receiver
// and the second call replaces the first, silently and with no way to ask
// what was there. That is survivable while the only caller is the process
// that built the engine; it stops being survivable the moment a second one
// can ask for audio over the wire, because the first subscribeAudio on a
// host that is also recording or playing locally takes the sound card and
// nothing anywhere says so.
//
// This paragraph used to end "tools/cli/main.cpp already hand-rolls a
// two-way version of this in a lambda, which is the shape of the problem
// rather than a solution to it". It did, and it stopped on 2026-09-20: the
// CLI attaches its recording and its monitor as two consumers here, so the
// example of the problem is gone and the sentence would now point a reader
// at code that does the right thing.
//
// ORDERING. Sinks are called in attach order, in one pass, on the producer
// thread, and every one of them is called: a sink that fails does not stop
// the sinks behind it. Attach order is stable and is not a priority; nothing
// here promises a recording sees a chunk before a subscriber does, only that
// each of them sees every chunk exactly once and in stream order.
//
// FAILURE. The first error is remembered and returned after the pass, so the
// caller learns that something refused while every other consumer still got
// its samples. core/engine/graph.cpp turns that error into a failing
// dispatch and ends the run, which is right: core/engine/audio_egress.h
// reserves a sink error for a wiring mistake, and a consumer that merely
// cannot keep up counts a drop of its own and returns success.
//
// WHAT IT DOES NOT DO, WHICH IS THE PART THAT MATTERS. A slow sink is NOT
// isolated. The pass is synchronous on the thread that retired the GPU
// readback, so a sink that blocks for ten milliseconds delays every sink
// behind it and the completion thread with them. No fan-out can fix that,
// because the buffer that absorbs a slow consumer has to be sized in that
// consumer's own units: a recording wants seconds of disk hiccup
// (AudioEgressConfig::ring_seconds), a loudspeaker wants one device period,
// and a wire subscription wants the depth its client asked for. Every sink
// attached here must therefore copy and return. The ones in this tree do:
// AudioEgress::publish writes into a lock-free ring, and the RPC server's
// copies into that subscription's own queue.
//
// THREAD SAFETY. attach, detach and empty are the control thread and take
// lock_. deliver is the producer thread: it allocates nothing and copies
// nothing, loading one shared_ptr to an immutable list that a concurrent
// attach replaces rather than mutates.
//
// THIS PARAGRAPH USED TO SAY deliver "takes no lock". It takes one, and the
// correction matters because the sentence was being read as a promise about
// the thread retiring GPU readbacks.
//
// std::atomic<std::shared_ptr<T>> is not lock free anywhere. MSVC's
// is_always_lock_free is a hardcoded false and load() calls
// _Repptr._Lock_and_load(), which spins on a lock bit stashed in the
// control-block pointer, copies the pointer pair, increments the refcount
// and unlocks. So the producer takes a spin lock, an atomic increment and a
// release on every chunk.
//
// THE SHAPE STANDS, and the reason is the rate rather than the cost. The
// lock bit lives in this object, not in a process-wide table, so the only
// thing that can contend for it is an attach, a detach or another deliver on
// THIS fan-out. attach and detach are control plane, meaning an operator
// action or a client subscribing, and they hold it for the length of a
// pointer swap. deliver runs once per chunk per receiver, which at
// 16384-sample blocks on a 2.4 MS/s source is 146 times a second: an
// uncontended lock bit against an interval of 6.8 milliseconds. Neither
// alternative is better. A std::mutex here puts a real lock on the same
// thread and blocks it behind an attach; giving the producer its own
// unsynchronised copy needs a reclamation scheme for the list the control
// thread just replaced, which is the problem shared_ptr is already solving.
//
// What would change the answer is a per-sample or per-frame fan-out rather
// than a per-chunk one. It is not that, and AudioChunk is the only thing
// this class carries.
// The message for a sink that threw, built where a second failure cannot
// escape.
//
// std::format and the string inside Error both allocate, and this runs in a
// catch block on a thread where an escaping exception is std::terminate. An
// Error with no message is a poor report and an enormous improvement on the
// process ending without one.
[[nodiscard]] inline Status sink_threw(const char* detail) noexcept {
    try {
        return fail(detail != nullptr
                        ? std::format("the sink threw an exception: {}", detail)
                        : "the sink threw an exception that is not a std::exception");
    } catch (...) {
    }
    return std::unexpected(Error{});
}

// Calls one caller-supplied sink and turns a throw into an ordinary error.
//
// EVERY SINK CALL ON THE COMPLETION THREAD GOES THROUGH HERE, for two
// reasons. That thread is created with std::thread in
// core/engine/scheduler.cpp, so an exception leaving a sink is
// std::terminate and the process is gone with no error of its own;
// core/engine/scheduler.h says in as many words that one bad frame does not
// wedge the engine and that the error surfaces through Scheduler::error,
// which was true of a sink that returned a failure and false of one that
// threw. core/engine/audio_wasapi.cpp has had this catch since it was
// written, on a thread that runs nothing a caller supplied.
//
// The second reason is the one a catch further up would not cover. The work
// after a sink call is not optional: in core/engine/graph.cpp the receiver's
// stream index moves, the ring retires and the frame slot goes back to the
// recording thread. Catching at the call keeps all of it, where catching in
// the completion thread's loop would leave the frame half finished and the
// recording thread parked on a slot that never comes back.
template <class Sink, class Payload>
[[nodiscard]] Status call_sink(const Sink& sink, const Payload& payload) noexcept {
    try {
        return sink(payload);
    } catch (const std::exception& thrown) {
        return sink_threw(thrown.what());
    } catch (...) {
        return sink_threw(nullptr);
    }
}

class AudioFanout {
public:
    AudioFanout() = default;

    AudioFanout(const AudioFanout&) = delete;
    AudioFanout& operator=(const AudioFanout&) = delete;
    AudioFanout(AudioFanout&&) = delete;
    AudioFanout& operator=(AudioFanout&&) = delete;

    // Control thread. Returns the token that detaches this one consumer.
    [[nodiscard]] AudioSinkId attach(AudioSink sink) {
        const std::scoped_lock held(lock_);
        const AudioSinkId token = next_.fetch_add(1, std::memory_order_relaxed);

        // Copy, append, publish. The list the producer thread may be walking
        // right now is never written to, so it needs no lock to read one.
        auto next = std::make_shared<Entries>(*live_.load());
        next->push_back(Entry{token, std::move(sink)});
        live_.store(std::move(next));
        return token;
    }

    // Control thread. False when that token is not attached, which is an
    // ordinary answer rather than an error: a client can cancel and then drop
    // the capability.
    bool detach(AudioSinkId token) {
        const std::scoped_lock held(lock_);
        auto current = live_.load();
        auto next = std::make_shared<Entries>();
        next->reserve(current->size());
        bool found = false;
        for (const Entry& entry : *current) {
            if (entry.id == token) {
                found = true;
                continue;
            }
            next->push_back(entry);
        }
        if (!found) {
            return false;
        }
        live_.store(std::move(next));
        return true;
    }

    [[nodiscard]] bool empty() const { return live_.load()->empty(); }
    [[nodiscard]] std::size_t size() const { return live_.load()->size(); }

    // The producer thread, through the one AudioSink this fan-out installs.
    [[nodiscard]] Status deliver(const AudioChunk& chunk) const {
        const std::shared_ptr<const Entries> entries = live_.load();
        Status outcome;
        for (const Entry& entry : *entries) {
            // Unreachable through Engine::attach_audio_sink, which refuses an
            // empty sink before it gets here, and reachable through this
            // class, which has no error channel on attach and so takes what
            // it is given. A caller that lost its callable gets a token that
            // detaches and a fan-out that skips it, rather than a crash on
            // the thread retiring GPU readbacks.
            // tests/engine/test_audio_fanout.cpp drives it through the class.
            if (!entry.sink) {
                continue;
            }

            // A THROW FROM ONE CONSUMER IS THAT CONSUMER'S ERROR, not the end
            // of the process and not the end of the pass. This runs on the
            // completion thread, which core/engine/scheduler.cpp created with
            // std::thread, so an exception leaving here is std::terminate;
            // and the promise above is that every sink is called, which a
            // throw out of the first one breaks for all the rest. Caught per
            // entry, so the consumers behind a broken one still get their
            // samples and the failure arrives as the ordinary error this
            // method already returns.
            if (auto handed = call_sink(entry.sink, chunk); !handed && outcome) {
                // The first failure, kept and returned after the pass. Later
                // ones are lost on purpose: the run is ending either way and
                // the first message names the consumer that started it.
                outcome = std::unexpected(handed.error());
            }
        }
        return outcome;
    }

private:
    struct Entry {
        AudioSinkId id = 0;
        AudioSink sink;
    };
    using Entries = std::vector<Entry>;

    // Serialises attach against detach. The producer never takes it.
    std::mutex lock_;

    // Replaced whole rather than mutated, which is what lets deliver() read
    // it with one atomic load and no lock. Never null.
    std::atomic<std::shared_ptr<const Entries>> live_{std::make_shared<const Entries>()};

    // ONE COUNTER FOR THE PROCESS, not one per fan-out.
    //
    // The token space has to outlive any single fan-out, because Engine
    // erases a fan-out as soon as its last consumer detaches and a member
    // here would be destroyed with it and restart at 1. Making it static is
    // what lets AudioSinkId mean what the comment on it says without every
    // holder of a fan-out having to pass a counter in. A uint64 issued one
    // per attach does not wrap in any run this will see.
    //
    // Relaxed is enough: nothing is published through this value. The
    // ordering that matters is the lock above, which is what serialises the
    // list rebuild.
    inline static std::atomic<AudioSinkId> next_{1};
};

// One full-span spectrum frame, handed to the caller on the host.
//
// One frame per dispatch, which is one per source block, so the frame rate is
// the source rate over EngineConfig::block_samples and is not something a
// consumer chooses. A consumer that wants fewer decimates in time; a consumer
// that wants more asks for smaller blocks, which costs submissions.
struct SpectrumFrame {
    // Power per bin in decibels relative to full scale, ascending in
    // frequency across the whole span with no gaps and nothing counted twice.
    //
    // Valid for the duration of the call and not after. A consumer that keeps
    // it copies it, the same as AudioChunk::samples.
    std::span<const float> power_db;

    SpectrumGeometry geometry;

    // The source samples this frame's window covers, as a half-open range
    // [start, start + count). Absolute from the start of the stream, so it
    // lines up with an AudioChunk's start and with anything else indexed the
    // way docs/conventions.md says time is indexed. The window is N coarse
    // channel samples, so it spans N * grid.decimation source samples, and
    // the filter's group delay has already been taken off.
    dsp::SampleIndex start = 0;
    dsp::SampleIndex count = 0;

    // Frames delivered before this one. A waterfall that skipped a row knows
    // it skipped a row, rather than drawing a gap as though the band went
    // quiet.
    std::uint64_t sequence = 0;

    // The colour map's two ends for this frame, in the same dBFS as power_db.
    //
    // A SECOND DELIBERATE ADDITION TO THIS FILE, AND WHY IT IS HERE RATHER
    // THAN IN EACH CONSUMER
    //
    // docs/ui-spectrum.md, "Auto-scaling, both ends": the floor and the
    // ceiling both track the signal over about thirty seconds, from
    // percentiles rather than extremes, expanding in a frame or two and
    // contracting over the thirty, and measured on the device. Four numbers
    // is what that costs on the wire.
    //
    // They are on the frame because otherwise every consumer computes them,
    // and then the waterfall, the detector's threshold and a screenshot's
    // legend disagree about where the noise floor is while all three are
    // looking at the same frame. That disagreement is invisible: each one is
    // internally consistent and nothing crosses to compare them.
    //
    // The two that actually come out of the device are the percentiles below.
    // These two are the smoothed ends, carried alongside rather than instead,
    // because a consumer with its own time constant, a detector that wants
    // this frame's noise floor and not a thirty-second average among them,
    // needs the measurement rather than the display's version of it.
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;

    // What the device measured of THIS frame, before any smoothing: the low
    // and high percentiles named in core/dsp/spectrum_levels_reference.h.
    //
    // These are the honest instantaneous figure. They move several decibels
    // frame to frame even on a static band, which is why nothing draws
    // against them directly.
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;
};

using SpectrumSink = std::function<Status(const SpectrumFrame&)>;

// One receiver's passband, handed to the caller on the host.
//
// docs/ui-spectrum.md, "The fine-tuning display": a second spectrum over the
// receiver's neighbourhood rather than the wide span, which is what makes
// parking a filter on a signal precise rather than approximate. It is a
// transform of the receiver's own display stream and not a zoom of the wide
// one, and the difference is resolution: at 20 MS/s on 64 channels the
// narrowest display rate is 31.25 kS/s, where a 4096-point transform
// resolves 7.6 Hz and a 2^18-point transform of the 20 MHz span resolves
// 76 Hz.
//
// THE RECEIVER'S FILTER IS NOT IN IT. The display stream is the channel
// mixed to the fine stream's DC behind a fixed anti-alias filter that is
// flat across the whole frame, so the noise floor is flat across the pane,
// a signal just outside the passband shows at its true level, and the
// client draws the filter over the top. core/dsp/vrx_reference.h, "The
// display tap".
//
// WHAT THIS USED TO SAY. That the frame was "a transform of the fine
// stream", that "the response shaping it is the receiver's own fine
// filter", and that "The skirts ARE the feature: a filter parked on a signal
// is judged by where its edges fall against the signal's". The owner asked
// why the detail spectrum had a massive hump that moved with the filter,
// and the hump was those skirts: a pane wider than the filter carried the
// filter's own response in its noise floor, 86 to 91 dB below the
// passband's floor at the edges of a 6 kHz NFM receiver's pane, and hid the
// neighbour an operator needed to see. The 11.7 Hz figure above
// was a 4096-point transform of a 48 kS/s fine ring.
//
// NO CHANNEL-SHAPE CORRECTION IS APPLIED, AND THAT IS THE OPPOSITE OF THE
// FULL-SPAN FRAME
//
// SpectrumFrame's bins are divided by the channelizer prototype's droop
// across each channel's kept band, because that band's edges sit exactly on
// the prototype's cutoff and the span tiles the droop once per channel. None
// of that reasoning reaches here. A passband frame is one piece with no seam
// to hide, and its edges are a quarter of the display rate either side of
// the fine stream's DC rather than on any filter's cutoff.
//
// What that leaves in is the coarse prototype's own shape across the
// receiver's slice of its channel. A receiver near its channel's edge, or
// one whose pane is as wide as a channel spacing, shows the prototype
// rolling off at one side. That is a fact about what the receiver can see
// rather than a display artefact: the fine stage filters the same channel,
// so the audio has it too.
struct PassbandFrame {
    VrxId vrx;

    // Power per bin in decibels relative to full scale, ascending in
    // frequency across half the display rate with no gaps.
    //
    // Valid for the duration of the call and not after, the same as
    // AudioChunk::samples and SpectrumFrame::power_db.
    std::span<const float> power_db;

    PassbandGeometry geometry;

    // The source samples this frame's window covers, as a half-open range
    // [start, start + count). Absolute from the start of the stream, so it
    // lines up with an AudioChunk's start and with a SpectrumFrame's. Both
    // the channelizer prototype's group delay and the display filter's have
    // already been taken off, so this is when the energy was on the air and
    // not when the samples reached this stage.
    dsp::SampleIndex start = 0;
    dsp::SampleIndex count = 0;

    // Frames delivered to this receiver before this one, so a waterfall that
    // skipped a row knows it skipped a row.
    //
    // The receiver's count and not the engine's, and it survives a sink
    // being replaced: the buffers behind a passband are rebuilt on every
    // attach, but frames recorded against the old set can still be in flight
    // when the new one arrives, and a count that restarted with the buffers
    // would run backwards across that handover.
    std::uint64_t sequence = 0;

    // This receiver's own colour map, smoothed by its own SpectrumScale with
    // the same time constants the full-span one uses.
    //
    // Its own and not the span's, because docs/ui-spectrum.md is explicit
    // that the fine display scales on the receiver's passband: a display
    // scaled by something it is not showing is a display that lies, and a
    // passband holding one 30 dB signal has nothing in common with a 20 MHz
    // span whose percentiles are mostly noise.
    float floor_db = 0.0F;
    float ceiling_db = 0.0F;

    // What the device measured of THIS frame, before any smoothing. A
    // consumer with its own time constant wants these rather than the two
    // above; there are no pins on the passband scale for that reason.
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;
};

using PassbandSink = std::function<Status(const PassbandFrame&)>;

// Two things the graph detected that nothing outside this process could see.
//
// Both are counted rather than logged, on the same ground every other counter
// in this engine is: a condition that reaches a log line is a condition nothing
// downstream can act on. What makes these two different from the losses in
// source::SourceStats is that neither is a loss, so neither belongs in the
// struct that carries them.
struct GraphConditions {
    // Retunes a STAGE refused, which are the ones set_vrx_params reported as
    // successes.
    //
    // set_vrx_params answers as soon as the control op is queued;
    // core/engine/graph.cpp applies it at the next block boundary and a stage
    // that will not take it counts one here and leaves the receiver where it
    // was. Until this crossed, what a caller saw was a successful call and a
    // receiver that did not move.
    //
    // REQUIRED TO STAY AT ZERO, which is what makes it worth reporting rather
    // than handling. engine::place refuses a placement the demodulator cannot
    // carry before the op is queued, so a non-zero value is not an operator
    // asking for the impossible: it is a request that passed placement and then
    // failed at the stage, which means the two disagree. It is exactly the kind
    // of requirement that stops being true quietly.
    std::uint64_t vrx_retune_refusals = 0;

    // Times the recording thread waited for a frame slot.
    //
    // EXPECTED ON A DEMAND SOURCE AND A WARNING ON A PACED ONE, so it has to be
    // read beside source::SourceCapabilities::flow. A Demand source advances at
    // exactly the rate its consumer retires work, so waiting IS the
    // backpressure and this climbing is the mechanism running; a Paced source
    // has its own clock and nowhere to put samples that keep arriving, so a
    // wait there is the graph falling behind and overrun_events is the next
    // thing to move.
    std::uint64_t frame_stalls = 0;
};

class Engine {
public:
    [[nodiscard]] static Expected<std::unique_ptr<Engine>> create(const EngineConfig& config);

    virtual ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Opens a source by URI and sizes the grid and the ring against it. The
    // engine takes ownership: a source outliving the graph that reads it is a
    // use-after-free waiting for a scheduling accident.
    //
    // REFUSED WHEN A SOURCE IS ALREADY OPEN, and close_source below is how a
    // caller gets from one to the other. It is not a replace, deliberately: a
    // replace that failed on the new URI would have already destroyed the
    // working source, and the caller would be holding a success-or-failure
    // answer to a question with three outcomes. Two calls means a failed open
    // leaves an engine with no source, which is a state the caller asked for.
    //
    // WHAT THE REFUSAL USED TO SAY: "this engine already has a source open. A
    // source is owned for the life of the engine, because a source outliving
    // the graph that reads it is a use-after-free waiting for a scheduling
    // accident." The second sentence conflated two things. Ownership for the
    // life of the GRAPH is what stops the use-after-free and that still holds;
    // ownership for the life of the ENGINE was a consequence of there being no
    // way to take the graph down, and there is one now.
    [[nodiscard]] virtual Status open_source(std::string_view uri) = 0;

    // Stops the stream, tears the graph and the ring down, and leaves an
    // engine that open_source can be called on again.
    //
    // WHAT IT COSTS, WHICH IS MORE THAN A CALLER WOULD GUESS
    //
    // Every receiver goes. They live in the graph and their placement was
    // computed against this grid, so there is nothing to carry forward: a
    // receiver's centre is an offset from a baseband whose meaning is set by
    // the source that is being closed. Re-placing them on the next source's
    // grid would put each one at a plausible offset from the wrong centre,
    // which is worse than dropping them, because a receiver at the wrong
    // absolute frequency looks like a receiver.
    //
    // Every sink goes with them: audio, passband, and the full-span spectrum.
    // The audio fan-outs this class holds are dropped too, so a subscriber's
    // callable is released here rather than being kept alive by a map keyed on
    // a receiver that no longer exists.
    //
    // The next stream is numbered from zero again and EngineInfo::source_epoch
    // is how a consumer tells the two apart. Read that field's note: it is the
    // whole of what makes a re-openable source safe to correlate against.
    //
    // IT BLOCKS ON A RUNNING ENGINE AND IT HAS TO. run() holds a raw Graph*
    // in the source callback and calls graph_->flush() after the stream ends,
    // so destroying the graph while run() is between those two is a
    // use-after-free. This requests the stop and waits for run() to have
    // finished with both. That means it MUST NOT be called on the thread that
    // called run(), which would wait for itself; every caller in this tree
    // runs the engine on a thread of its own for unrelated reasons.
    //
    // A source that will not stop is reported rather than waited on forever.
    // Idempotent: closing an engine with no source open succeeds and does
    // nothing, because the caller's intent is already satisfied.
    [[nodiscard]] virtual Status close_source() = 0;

    // Whether a source is open. A host that serves an engine whose source can
    // be closed and reopened loops on this, because run() refuses with no
    // source and returns when one is closed.
    [[nodiscard]] virtual bool has_source() const = 0;

    [[nodiscard]] virtual const source::SourceCapabilities& source_capabilities() const = 0;

    [[nodiscard]] virtual const EngineInfo& info() const = 0;

    // Where the front end can be pointed, and whether it can be pointed at
    // all. Everything false and zero before a source is open.
    [[nodiscard]] virtual SourceTuning source_tuning() const = 0;

    // Points the front end somewhere else, and answers with the centre the
    // device actually took, which a synthesiser with a tuning step will
    // round, and with the receivers the move removed.
    //
    // WHAT THIS USED TO SAY, AND IT IS NOW FALSE
    //
    // This block was headed "WHY THIS IS A SMALL CHANGE" and argued that a
    // retune "moves two things and no more: the device's own oscillator, and
    // the constant in EngineInfo::source_center that relates baseband to real
    // radio frequency", because "no build of this engine has ever rebased a
    // receiver's centre".
    //
    // It rebases them now, and the reason the old behaviour was wrong was
    // never visible from inside this file. VrxParams::center is a BASEBAND
    // offset, so leaving every receiver alone meant carrying them along with
    // the span: a receiver at +100 kHz was hearing 98.2 MHz at a centre of
    // 98.1 and heard 435.1 MHz at a centre of 435, without refusing, moving on
    // any display, or saying anything. An operator retuned from broadcast FM
    // on 2026-09-21 and found their receiver still making noise at a frequency
    // they had not chosen. A receiver is a VFO and a VFO stays where it was
    // put.
    //
    // SO A RETUNE MOVES THREE THINGS, AND CAN REMOVE RECEIVERS
    //
    // The device's oscillator, EngineInfo::source_center, and every receiver's
    // baseband offset, recomputed to hold the absolute frequency it was tuned
    // to. A receiver whose CENTRE then falls outside the new span is REMOVED
    // rather than clamped or parked, because a receiver the front end cannot
    // reach is not a receiver any more.
    //
    // The centre and not the passband, which are different by up to half a
    // receiver's width at the span edges. The centre is the frequency somebody
    // typed or clicked, so it is the one they would say the receiver is on. A
    // receiver left half over the edge keeps running and sounds wrong, and both
    // the passband highlight and the client's own fit sentence already say so.
    //
    // A receiver still inside the span is removed too when its new place in a
    // channel needs a different filter. AM, DSB, the sidebands and CW are
    // narrowed to what their channel carries from where they sit in it, a
    // retune by anything but a multiple of the channel spacing moves where
    // that is, and the graph will not swap a running receiver's filter shape.
    // Leaving it on its old offset instead would put it as far from its
    // frequency as the front end moved.
    //
    // The answer carries the centre the device took and every receiver the
    // retune removed, with the frequency each was on and why. The retune succeeded,
    // and a receiver that could not come along is not a failed retune, so a
    // removal is reported beside the success rather than as a failure.
    //
    // WHAT THIS PARAGRAPH USED TO SAY: "A client finds out the way it finds
    // out about any receiver that has gone, through vrx_status refusing and
    // its subscriptions ending." Half of that was never true. The graph
    // removes a receiver without telling anybody, so a subscription on it did
    // not end: an audio subscriber went quiet with no ended() call, and
    // core/rpc/server.cpp kept the receiver's RDS route, decoder routes and
    // ownership record. The server now reads SourceRetune::removed and runs
    // the same cleanup removeVrx does, with a reason naming the retune.
    //
    // WHAT IS DELIBERATELY NOT RESET, AND WHY THE RING IS THE EASY ONE
    //
    // The ring holds samples captured at the old centre and nothing is done
    // about them. It is a streaming window and not a cache: the only thing
    // that reads behind the write cursor is the channelizer's own filter
    // support, so the stale samples are bounded by the prototype length plus
    // one block and are gone within a dispatch. Discarding them instead
    // would mean moving the write cursor, and the absolute sample index is
    // what every chunk, frame, recording and counter in this engine is
    // correlated against, so a retune that renumbered the stream would break
    // a correlation to avoid a transient of tens of milliseconds.
    //
    // The spectrum's colour map is not reset either. It tracks percentiles
    // over about thirty seconds, expands in a frame or two and contracts
    // over the thirty, so a retune from a busy band to a quiet one leaves
    // the ceiling high for a while. That is visible, self-correcting, and
    // the same behaviour docs/ui-spectrum.md already describes for a signal
    // that stops; a reset here would instead make the two ends jump on a
    // band that had not changed, whenever a retune was small.
    //
    // WHAT IS RESET, AND IT IS NOT THE SAMPLES
    //
    // Every receiver's tuning epoch. This sentence used to read "Nothing
    // about a receiver's placement moves", and a receiver's offset does move
    // now, for the reason the block above gives. The epoch argument is
    // unchanged by that: every sample a receiver produces after the boundary
    // came from a different front-end centre, and AudioChunk::tuning_epoch is
    // exactly the marker a consumer that accumulates state about the
    // transmitter it is hearing waits for. So this re-queues each receiver's
    // params, now rebased, which moves that receiver's epoch through the one
    // path the graph already applies at a block boundary. The alternative was a
    // second, source-level epoch beside it, which would leave a consumer
    // having to watch two numbers to answer one question.
    //
    // Re-applying identical params is not a no-op and is not a refusal:
    // set_vrx_params refuses a change of SHAPE, and identical params are not
    // one, so the op is applied in place and GraphStats::vrx_retune_refusals
    // stays at zero. A receiver added between the tune and this pass is
    // already on the new centre and misses the epoch bump, which is correct.
    //
    // What this does NOT reset is anything above the engine. The wideband
    // detector holds tracks measured against the old centre and the RDS
    // decoders hold one station's text each; both live in
    // core/rpc/server.cpp, which drops them when this call returns.
    //
    // Refused, in the SOURCE's own words, on a source that cannot retune,
    // which is every file and every synthetic scene. Their sentence says
    // what to do instead, which is to reopen the URI, and a generic refusal
    // here would replace advice with a category.
    [[nodiscard]] virtual Expected<SourceRetune> set_source_center(dsp::Hertz center) = 0;

    // The front end's gain, by the stage's own name, answering with the value
    // the device took.
    //
    // NOT A PERCENTAGE AND NOT ONE NUMBER, for the reason
    // source::GainStage exists: an R820T has a single stage with 29 discrete
    // steps, an Airspy has three, and a file has none. The stages are already
    // readable through source_capabilities(), including each one's steps and
    // whether the device will drive it itself, so this takes the name from
    // there rather than offering a control the device does not have. A request
    // between two steps lands on the nearest, which is why this answers with a
    // number instead of a status: a slider over a stepped stage that showed the
    // request would be showing a gain the tuner never held.
    //
    // NOTHING NEW CROSSES THE BUS, so the promise at the top of this file is
    // untouched. This is control plane, shaped like set_source_center above,
    // and it was added for the same reason: the gain was fixed at open time, so
    // an operator whose audio was overloading had to close the source and
    // reopen it to try a different one.
    //
    // Refused in the SOURCE's own words on a source with no such stage, which
    // is every file and every synthetic scene, and on a stage name the device
    // does not carry. Their sentence names what the device does have.
    [[nodiscard]] virtual Expected<double> set_source_gain(std::string_view stage,
                                                           double db) = 0;

    // Hands the stage to the device's own AGC, or takes it back.
    //
    // A CHOICE TO OFFER RATHER THAN THE SENSIBLE SETTING. README.md carries
    // the measurement: on an RTL-SDR v3 at 95.1 MHz in a suburban FM
    // environment the tuner's AGC made the wideband detector report three
    // intermodulation products as real tracks at confidence 1.00, and a fixed
    // 20 dB removed all three and improved the measured SNR of the station by
    // 5.7 dB. GainStage::has_auto says whether the device will do it at all;
    // whether it should is the operator's call and depends on their antenna.
    [[nodiscard]] virtual Status set_source_gain_auto(std::string_view stage, bool on) = 0;

    // How fast capture is arriving against the wall clock, and what was
    // asked for. See SourcePacing: the pair is what separates a source that
    // cannot keep up from one that was deliberately throttled.
    [[nodiscard]] virtual SourcePacing source_pacing() const = 0;

    // Changes how fast the open source plays, as a multiple of realtime with
    // zero for unthrottled, and answers with the pace now in force. Applies
    // from the next block, running or not, and the stopwatch restarts there,
    // so a change never makes the stream catch up.
    //
    // Refused before a source is open, on a Paced source, whose device sets
    // the rate, and in the source's own words on one that cannot change it,
    // which is every source but a file today. The pace is the source's until
    // it is closed; the next one starts from its own again.
    //
    // Virtual with a refusal rather than pure, for the reason submit_probe is.
    [[nodiscard]] virtual Expected<double> set_source_pace(double pace);

    // Adding a receiver must not rebuild the coarse stage, and nothing in
    // VrxParams appears in GridParams, so it cannot.
    //
    // params.center is a baseband offset and nothing in here rebases it: it
    // reaches engine::place as given and is stored as given. A caller that
    // knows a frequency in absolute hertz, which is every caller with an
    // operator or a detection behind it, subtracts info().source_center
    // first. The note on VrxParams::center has the argument for that being
    // the caller's job.
    [[nodiscard]] virtual Expected<VrxId> add_vrx(const VrxParams& params) = 0;
    [[nodiscard]] virtual Status remove_vrx(VrxId id) = 0;

    // The same frame in both directions: vrx_status hands back the params it
    // was given, so reading one out and passing it straight back in leaves
    // the receiver where it was.
    [[nodiscard]] virtual Status set_vrx_params(VrxId id, const VrxParams& params) = 0;
    [[nodiscard]] virtual Expected<VrxStatus> vrx_status(VrxId id) const = 0;
    [[nodiscard]] virtual std::vector<VrxId> vrx_ids() const = 0;

    // Routes a receiver's audio to a callback. One sink per receiver; setting
    // a second replaces the first, and an empty one detaches.
    //
    // THIS IS THE SLOT AND NOT THE SEAM. It is the low-level call, kept
    // because the graph has exactly one place to put a sink and something has
    // to fill it. A caller that wants audio alongside whatever else is
    // already listening uses attach_audio_sink below; a caller that reaches
    // this directly displaces every consumer the fan-out was holding and is
    // not told it did.
    [[nodiscard]] virtual Status set_audio_sink(VrxId id, AudioSink sink) = 0;

    // Adds a consumer to this receiver's audio without displacing the ones
    // already there, and hands back the token that removes it again.
    //
    // Non-virtual and written once, here, on top of set_audio_sink: the first
    // attach on a receiver installs an AudioFanout in the slot and every
    // attach after it joins that fan-out. The last detach takes the slot off
    // again, so a receiver nobody is listening to costs one std::function
    // call per dispatch and no more. AudioFanout above has the ordering and
    // the failure semantics, including what a slow consumer does to the
    // others, which is the part a caller has to design around.
    //
    // Refused in the engine's own words for a receiver that does not exist
    // and before a source is open. The first attach on a receiver gets that
    // refusal out of set_audio_sink; a later one gets it out of vrx_status,
    // because a fan-out outlives the receiver it was installed on and
    // joining a stale one would report success and deliver nothing. The body
    // has the whole of that, including the residual: this map is not pruned
    // when a receiver is removed, and a stale entry is dropped when somebody
    // next asks about that id rather than when the receiver goes.
    //
    // WHAT IT CANNOT DO. It cannot stop a caller from reaching
    // set_audio_sink directly and throwing the fan-out away; nothing can,
    // short of removing that method, and it is the only way to fill the
    // slot. No caller in this tree does it any more: tools/cli/main.cpp was
    // the last one and moved to this method on 2026-09-20.
    // tests/rpc/test_rpc_audio.cpp pins the displacement, so the hazard this
    // paragraph describes is a case rather than a warning.
    [[nodiscard]] Expected<AudioSinkId> attach_audio_sink(VrxId id, AudioSink sink);

    // Removes one consumer. The token is the one attach_audio_sink returned.
    // Detaching a token that is not attached is an error and not a silent
    // success, because the two things it means, a double detach and a token
    // from another receiver, are both mistakes a caller wants to hear about.
    [[nodiscard]] Status detach_audio_sink(VrxId id, AudioSinkId sink);

    // Routes the full-span spectrum to a callback. One sink for the engine,
    // because there is one span; setting a second replaces the first and an
    // empty one stops delivery.
    //
    // Refused when EngineConfig::spectrum_transform was zero, because the
    // stage is built with the rest of the coarse chain when the source is
    // opened and adding one later would mean rebuilding it, which is the one
    // thing the graph promises never to do while a stream is running.
    [[nodiscard]] virtual Status set_spectrum_sink(SpectrumSink sink) = 0;

    // Routes one receiver's passband to a callback, and is the per-receiver
    // opt-in: until a sink is attached that receiver records no transform and
    // holds no buffers for one. An empty sink detaches and frees them again.
    //
    // One sink per receiver; setting a second replaces the first. Refused
    // when EngineConfig::passband_transform was zero, and refused for a raw
    // tap, which has no fine stage to transform: the graph's raw tap is a
    // copy out of the coarse channel ring and never mixes or filters, so
    // there is no per-receiver baseband on the device to look at. The
    // full-span spectrum already covers that channel.
    [[nodiscard]] virtual Status set_passband_sink(VrxId id, PassbandSink sink) = 0;

    // Runs the graph until the source ends or stop() is called. The source's
    // own thread drives it; this returns once the stream is finished and
    // hands back whatever ended it.
    [[nodiscard]] virtual Status run() = 0;
    [[nodiscard]] virtual Status stop() = 0;
    [[nodiscard]] virtual bool running() const = 0;

    // Counters, not logs. An overrun is a correctness event: a recording that
    // looks continuous and is not, with nothing downstream able to tell, is
    // the failure mode a log line produces.
    [[nodiscard]] virtual source::SourceStats source_stats() const = 0;

    // What the GRAPH counted that the source cannot know.
    //
    // A SEPARATE CALL AND NOT TWO MORE FIELDS ON SourceStats, because that
    // struct is the SOURCE's and these are not. source_stats already folds the
    // ring's losses into it, and that fold is argued for: a sample the graph
    // could not place is gone in precisely the way a sample the device dropped
    // is gone, and the frozen Engine has one place to report a counted loss.
    // Neither of these is a loss, so neither has that argument behind it, and
    // putting them there would make a client reading "the source's stats"
    // reason about two numbers the source has never heard of.
    [[nodiscard]] virtual GraphConditions graph_conditions() const = 0;

    // Tier two: put a probe receiver on a detection, collect its baseband and
    // characterise it. core/engine/probe.h has the pool, the buckets and the
    // dwell.
    //
    // ONE THREAD SUBMITS AND ONE TAKES, through lock-free rings, which is what
    // lets the caller be the completion thread a spectrum sink runs on.
    // detect::TierTwo is the caller in this tree and does both from there.
    //
    // A probe is not a receiver anybody else can see. It is not in vrx_ids,
    // it is never rebased by set_source_center, which cancels it instead, and
    // every receiver method here refuses its id. What it finds comes back as
    // a ProbeOutcome to whoever submitted it and goes nowhere else: nothing
    // puts a family on the wire, as docs/detection.md settled.
    //
    // Virtual with a refusal rather than pure, so an Engine written before the
    // pool, tests/rpc/retunable_engine.h among them, has none and says so.
    [[nodiscard]] virtual Status submit_probe(const ProbeRequest& request);
    [[nodiscard]] virtual std::size_t take_probe_outcomes(std::span<ProbeOutcome> out);
    [[nodiscard]] virtual ProbeStats probe_stats() const;

    // The open source's calibration: its crystal correction, the DC removal
    // and the I/Q correction, and what the front-end stage has measured.
    //
    // A calibration is restored from EngineConfig::calibration_path when a
    // device with a serial is opened, so an operator measures a dongle once.
    // CalibrationState::open is false with no source, and that is a state
    // rather than a failure, for the reason sourceDescriptor gives on the wire.
    //
    // Virtual with a refusal rather than pure, for the reason submit_probe
    // is: an Engine written before calibration has none and says so.
    [[nodiscard]] virtual Expected<CalibrationState> calibration() const;

    // Replaces the open source's calibration, applies it, and stores it
    // under the device's key when there is one and a path is configured.
    //
    // A CHANGE OF CORRECTION MOVES THE NUMBERS AND NOT THE RADIO. The device
    // is not retuned, so every receiver stays on the signal it was on and
    // EngineInfo::source_center, and every absolute frequency built from it,
    // moves to what it should have read. core/source/corrected_source.h has
    // the reasoning and the part the correction does not reach.
    //
    // Refused before a source is open, and for a correction past a thousand
    // ppm. A failure to write the file is not a refusal: the settings are in
    // force and CalibrationState::persisted and note say they were not kept.
    [[nodiscard]] virtual Expected<CalibrationState> set_calibration(
        const source::DeviceCalibration& settings);

protected:
    Engine() = default;

    // Releases every fan-out this class is holding, for an implementation that
    // has just destroyed the receivers they were installed on.
    //
    // PROTECTED BECAUSE THE MAP IS PRIVATE AND THE TEARDOWN IS NOT. close_source
    // destroys the graph, which takes every receiver and every audio slot with
    // it, and the entries left here would then be fan-outs on receivers that no
    // longer exist. attach_audio_sink prunes one of those when somebody happens
    // to ask about that id again, which is enough for a removed receiver and is
    // not enough here: after a close there is no id anybody will ask about, so
    // every entry would be held for the life of the engine along with the
    // subscriber callables inside it.
    //
    // It does not call set_audio_sink on the way out, unlike detach_audio_sink.
    // There is no graph left to call it on.
    void drop_audio_fanouts();

private:
    // The fan-outs this engine has installed, one per receiver that has at
    // least one attached consumer. Control-plane state on an otherwise pure
    // interface, which is deliberate: it has to sit above set_audio_sink to
    // be shared by every caller, and there is nowhere above Engine that every
    // caller passes through.
    //
    // Keyed by VrxId::value rather than by VrxId, because VrxId has equality
    // and no ordering and giving it one would be a comparison nobody means.
    std::mutex audio_fanout_lock_;
    std::map<std::uint32_t, std::shared_ptr<AudioFanout>> audio_fanouts_;
};

inline Status Engine::submit_probe(const ProbeRequest&) {
    return fail("this engine has no probe pool. EngineConfig::probe_receivers was zero, or the "
                "engine was written before core/engine/probe.h existed");
}

inline std::size_t Engine::take_probe_outcomes(std::span<ProbeOutcome>) { return 0; }

inline ProbeStats Engine::probe_stats() const { return {}; }

inline Expected<CalibrationState> Engine::calibration() const {
    return fail("this engine keeps no calibration: it was written before "
                "core/source/calibration.h existed");
}

inline Expected<double> Engine::set_source_pace(double) {
    return fail("this engine cannot change a source's pace: it was written before "
                "Engine::set_source_pace existed");
}

inline Expected<CalibrationState> Engine::set_calibration(const source::DeviceCalibration&) {
    return fail("this engine keeps no calibration: it was written before "
                "core/source/calibration.h existed");
}

inline void Engine::drop_audio_fanouts() {
    const std::scoped_lock held(audio_fanout_lock_);
    audio_fanouts_.clear();
}

inline Expected<AudioSinkId> Engine::attach_audio_sink(VrxId id, AudioSink sink) {
    if (!sink) {
        return fail("Engine::attach_audio_sink was given an empty sink. Detaching is "
                    "detach_audio_sink with the token attach handed back, so an empty one "
                    "here is a caller that lost its callable rather than one asking to stop");
    }

    const std::scoped_lock held(audio_fanout_lock_);

    auto found = audio_fanouts_.find(id.value);
    if (found != audio_fanouts_.end()) {
        // A FAN-OUT CAN OUTLIVE ITS RECEIVER, SO JOINING ONE HAS TO ASK
        //
        // Nothing prunes this map on remove_vrx. remove_vrx is pure virtual
        // and the implementation does not pass through here, so a receiver
        // removed while a consumer was still attached leaves its entry
        // behind; the entry goes only when that consumer detaches, and a
        // consumer that never does leaves it forever.
        //
        // Without this check the branch below would hand back a token and
        // report success for a receiver the graph has already torn down, and
        // that consumer would then hear nothing for the life of the engine.
        // The comment on this method promises a refusal in the engine's own
        // words for a receiver that does not exist, and until 2026-09-20 it
        // only delivered one on the first attach, where set_audio_sink was
        // the thing being asked.
        //
        // Asked of vrx_status rather than tracked here, because this class
        // has no hook on a removal to track it with. The stale entry is
        // dropped on the way out, which is the only pruning there is: the
        // ids Engine issues are monotonic and never reused, so the only
        // caller who can reach a stale fan-out is one attaching to an id it
        // removed itself.
        if (auto alive = vrx_status(id); !alive) {
            audio_fanouts_.erase(found);
            return std::unexpected(alive.error());
        }
        return found->second->attach(std::move(sink));
    }

    std::shared_ptr<AudioFanout> fanout = std::make_shared<AudioFanout>();
    const AudioSinkId token = fanout->attach(std::move(sink));

    // The slot is filled before the map records it, so a refusal leaves this
    // engine exactly as it was: the local fan-out dies here with the sink
    // that was just attached to it, and the receiver keeps whatever it had.
    if (auto installed = set_audio_sink(id, [fanout](const AudioChunk& chunk) -> Status {
            return fanout->deliver(chunk);
        });
        !installed) {
        return std::unexpected(installed.error());
    }

    audio_fanouts_.emplace(id.value, std::move(fanout));
    return token;
}

inline Status Engine::detach_audio_sink(VrxId id, AudioSinkId sink) {
    const std::scoped_lock held(audio_fanout_lock_);

    auto found = audio_fanouts_.find(id.value);
    if (found == audio_fanouts_.end()) {
        return fail(std::format(
            "receiver {} has no attached audio consumers, so sink {} cannot be detached from "
            "it",
            id.value, sink));
    }
    if (!found->second->detach(sink)) {
        return fail(std::format(
            "sink {} is not attached to receiver {}. A token is unique for the life of an "
            "engine, so this is either a second detach of the same one or a token issued for "
            "another receiver",
            sink, id.value));
    }
    if (!found->second->empty()) {
        return {};
    }

    audio_fanouts_.erase(found);

    // Discarded, and the one place in this pair where a refusal is not the
    // caller's business. A receiver removed while a consumer was still
    // attached is the ordinary teardown order and set_audio_sink refuses an
    // id the graph no longer knows; the fan-out has already been dropped
    // above, so there is nothing left on the receiver either way.
    static_cast<void>(set_audio_sink(id, {}));
    return {};
}

}  // namespace revenant::engine
