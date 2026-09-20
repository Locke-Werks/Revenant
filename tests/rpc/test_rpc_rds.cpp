// RDS, from a synthetic transmitter through the GPU chain and out over a
// socket.
//
// WHAT THIS FILE IS FOR, AND WHAT NOTHING BEFORE IT COULD SAY
//
// tests/decode scores the decoder against core/dsp/synth, which is a clean
// room check of one document against another: two implementations of EN
// 50067 written from the same clauses, agreeing. tests/decode/test_rds_bits
// closes that loop through an FM carrier and dsp::reference_vrx_demod, so
// the discriminator is in the path. No radio is, and its own note says so:
// "Nothing here runs the polyphase channelizer or the fine stage ... An
// engine-level case belongs in tests/engine and does not exist yet."
//
// This is that case, and it landed in tests/rpc rather than tests/engine
// because there is no engine-level RDS surface to test: the decoder lives in
// core/rpc/server.cpp, per the long note at the top of that file, and the
// only way to ask it anything is over the wire.
//
// So the claim here is the one nobody had made. A station is rendered to a
// file, read back through the channelizer, the fine stage and the
// demodulation kernel on a real device, fed to a decoder on the engine's
// completion thread, and its PI, PS and RadioText are read out of a Cap'n
// Proto message on another thread. Every stage that was ever going to eat
// the subcarrier is in that path.
//
// WHAT IT REPLACED
//
// tests/rpc/test_rpc_unwired.cpp, which asserted that rdsStation and
// setRdsRegion came back refused in a sentence saying the surface existed
// and was not wired. That file's whole purpose was to make the branch
// serving the surface delete a case rather than leave a gap nobody noticed,
// and this is the branch. Its last two cases went with it, so the file is
// gone rather than emptied.
//
// It also pinned something this file has to keep pinning, which is why the
// refusals below are as detailed as they are: the shape of a refusal is what
// a caller learns from. A struct of zeros would look like a broken engine
// and send whoever saw it to the radio.
//
// THE REACHABLE SHAPES, WHICH ARE NOT THE ONE THE AUDIO RATE SUGGESTS
//
// A bar that asked only "is the audio rate 171000" would certify one shape
// out of four and read as settled. The ones a receiver can actually be in:
//
//   1. It does not exist, or was removed.            refused, engine's words
//   2. It is not an FM discriminator.                refused, condition 1
//   3. It took the engine's default audio rate.      refused, unnameable
//   4. It is at 48 kHz, decimating.                  refused, 148438 bound
//   5. It is under 125000 with decimation one.       refused, 125000 bound
//   6. Its granted passband cannot hold 59375 Hz.    refused, condition 4
//   7. It carries no RDS at all.                     runs, never locks
//   8. It carries RDS under noise.                   runs, claims nothing
//   9. Two of them, on two regions, at once.         both decode
//  10. It is removed, engine stopped.                decoder goes with it
//  11. It is retuned, engine stopped.                state clears
//  12. It is removed WHILE DECODING.                 decoder goes, run lives
//  13. It is retuned WHILE DECODING.                 state clears and refills
//  14. Its traffic flag moves mid-recording.         the change is timed
//  15. It is retuned FOUR TIMES back to back.        the fence still clears
//  16. It is retuned with no chunk yet delivered.    the fence says it is up
//
// Shapes 7 and 8 are the ones that matter most and the ones a bar reaches
// last. A decoder that silently never locks looks exactly like a station
// with no RDS, and a decoder that half locks under noise can produce text
// that was never transmitted. The cases for both assert that NOTHING is
// claimed, because claiming something wrong is the failure and claiming
// nothing is the correct answer.
//
// 10 AND 11 USED TO BE NAMED "WHILE DECODING" AND WERE NOT
//
// Both ran against a STOPPED engine. 10 removed a receiver that had never
// been fed a sample, and 11 retuned one after run_to_completion had already
// joined the run. Nothing was decoding in either, so neither exercised the
// interleaving its name claimed, and the two names between them read as
// though the concurrency was covered. They are renamed to what they check,
// which is worth keeping: the control-plane bookkeeping is where a decoder
// outliving its receiver would show up, and it is cheap.
//
// 12 and 13 are the cases the old names promised. Each one proves the
// overlap rather than hoping for it: the engine is running, and the case
// polls until the decoder's own samplesConsumed has MOVED BETWEEN TWO POLLS
// before it touches anything, so the completion thread is demonstrably
// inside the sink when the removal or the retune is issued.
//
// WHAT THEY CANNOT PIN, so the names do not overclaim a second time. A test
// cannot choose the interleaving, so neither case proves the absence of a
// race; what each one pins is that the operation is reachable under load and
// that the state afterwards is right, including that the run survives, which
// is where a detach racing a dispatch in progress would show. 13's fence
// assertion is the one with real teeth, and it is a POSITIVE one:
// samplesConsumed has to climb off zero again after the retune. An
// implementation whose fence never disarms leaves it at zero for the rest
// of the run and fails there.
//
// 15 AND 16 ARE 13 WITH THE HOLE IN IT CLOSED, and it was a hole a single
// retune could not reach. This paragraph used to say 13 caught "an
// off-by-one in the pending count", which named the mechanism that was
// there and not the one that broke. The fence counted resets on the loop
// thread and took one off per observed change of AudioChunk::tuning_epoch,
// and Graph::drain_control applies the whole queued stack in one pass, so
// two retunes inside one block period moved the epoch twice and produced
// one change. The count stuck above zero, the decoder discarded for the
// rest of the run, and rdsStation answered zeros with an empty fault:
// indistinguishable, on the wire, from a receiver pointed at a quiet
// channel. 13 could not see it because one retune is one change.
//
// 15 fires four in a row so at least two share a drain, and asserts the
// decoder relocks afterwards. 16 asserts the state is VISIBLE: RdsStation::
// discarding is true while the fence is up, which is the field that makes
// this failure mode reportable instead of silent, and it reads it in the
// one arrangement where it is not a race.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <format>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/decode/rds_bits.h"
#include "core/decode/rds_groups.h"
#include "core/dsp/synth/rds_mod.h"
#include "core/dsp/synth/wfm_mod.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

// ---------------------------------------------------------------------------
// The grid these cases run on, and why it is not the one the rest of the
// suite runs on
// ---------------------------------------------------------------------------
//
// Four numbers, each fixed by the one above it.
//
// The composite has to arrive at 171000 S/s. That is the whole tap: three
// times the 57 kHz subcarrier and 144 times the 1187.5 bit/s bit rate, both
// exact, and it is what decode::RdsBitsConfig::rate defaults to.
//
// dsp::minimum_demod_rate for a 200 kHz WFM passband is 1.5 times the width,
// 300000, and dsp::demod_rate_for rounds that up to a whole multiple of the
// audio rate, so the fine stage runs at 342000 and decimates by two. The
// audio filter it designs then has its passband edge at 0.4 of 171000, which
// is 68400 Hz, and the composite reaches 59375. That is the margin the whole
// arrangement rests on and it is 9 kHz wide.
//
// A coarse channel has to carry 342000, so with the project's 2x-oversampled
// grid the channel rate 2R/M has to reach it: R/M at or above 171000.
//
// And the CHANNEL PROTOTYPE has to be flat across the station, which is the
// constraint that actually binds and the one that is easy to miss.
// core/dsp/pfb_design.cpp puts the prototype's passband edge at 0.25*R/M and
// its stopband edge at 0.75*R/M, so a station wider than half a channel
// spacing is filtered before the discriminator ever sees it. Filtering an FM
// signal is not attenuation, it is distortion, and it lands in the data.
// The station below occupies 151250 Hz by Carson, so its half width is 75625
// and R/M has to reach 302500. 342000 is the first multiple of 171000 past
// that, and M = 4 makes R = 1368000, which is a file a test can afford.
//
// WHAT THIS DOES NOT REACH, so the grid is not read as more than it is. The
// station sits at baseband DC and the receiver sits 5000 Hz away from it, so
// the fine stage mixes a real residual and the discriminator sees a carrier
// offset: that much is exercised. What is not is a station placed off the
// grid, because there are four channels and the receiver is on channel zero
// either way.
constexpr dsp::SampleRate kRdsSourceRate = 1'368'000;
constexpr std::uint32_t kRdsChannels = 4;
constexpr dsp::SampleRate kCompositeRate = 171'000;

// Where the receiver sits relative to the station, in hertz. Nonzero on
// purpose: at zero the fine stage's mixer is the identity and this suite
// would be testing a chain with one stage switched off.
constexpr dsp::Hertz kReceiverOffsetHz = 5'000;

// The station's identity, which is what every assertion below is against.
constexpr std::uint16_t kStationPi = 0x2345;

// 26 is chosen for one reason: EN 50067 Annex F names it National Music and
// NRSC-4-B Table F.2 names it Hip hop. It is the code the two-region case
// reads the difference off, and a code the tables agreed on would make that
// case pass with the region ignored.
constexpr std::uint8_t kStationPty = 26;

constexpr std::string_view kStationPs = "REVENANT";
constexpr std::string_view kStationRt = "GPU RESIDENT SDR";

// Four cycles of eight groups. One cycle carries the whole of PS and the
// whole of RadioText, and four of them leaves three spare after the physical
// layer's acquisition, which is about 350 bits, and the block layer's hunt,
// which is one group.
constexpr int kStationCycles = 4;

// Ten for the cases that have to act on a RUNNING engine, which those play
// at realtime rather than unthrottled. One cycle is 8 groups and a group is
// 87.6 ms, so ten cycles is 7.0 seconds: about a second and a half to
// acquire and fill PS, a window in the middle to issue a removal or a
// retune, and enough left afterwards for the decoder to be visibly fed
// again. Four cycles is 2.8 seconds and most of that is acquisition.
constexpr int kRunningCycles = 10;

// One group in composite samples, which is the unit the timing assertions
// below are stated in. 171000 is 144 times 1187.5 exactly, which is the
// whole reason the rate was chosen, so a 104-bit group is 14976 samples with
// nothing rounded.
constexpr std::uint64_t kSamplesPerBit = 144;
static_assert(kSamplesPerBit * 2375 == static_cast<std::uint64_t>(kCompositeRate) * 2,
              "171000 has to be 144 times 1187.5 for the assertions below to be exact");
constexpr std::uint64_t kGroupSamples = 104 * kSamplesPerBit;

// The seed for the noise in the weak-station case. Fixed and PRINTED, per
// the house rule: a case that fails on an unrepeatable draw cannot be
// debugged.
constexpr std::uint64_t kNoiseSeed = 20'260'920;

// ---------------------------------------------------------------------------
// The payload
// ---------------------------------------------------------------------------

struct GroupWords {
    std::uint16_t b1 = 0;
    std::uint16_t b2 = 0;
    std::uint16_t b3 = 0;
    std::uint16_t b4 = 0;
    bool version_b = false;
};

[[nodiscard]] std::uint16_t chars_to_word(char high, char low) {
    return static_cast<std::uint16_t>((static_cast<unsigned char>(high) << 8) |
                                      static_cast<unsigned char>(low));
}

// Block 2 of a type 0A group, assembled field by field so it reads as the
// standard does rather than as a hexadecimal constant. Same shape as the one
// in tests/decode/test_rds_groups.cpp, written out again rather than shared:
// a test that imports the encoder another test wrote proves the two agree
// and not that either matches the document.
[[nodiscard]] std::uint16_t type0_block2(std::uint8_t pty, bool tp, bool ta, bool music,
                                         bool di_bit, std::uint8_t address) {
    std::uint16_t word = 0;
    word = static_cast<std::uint16_t>(word | (tp ? 0x0400u : 0x0000u));
    word = static_cast<std::uint16_t>(word | ((pty & 0x1Fu) << 5));
    word = static_cast<std::uint16_t>(word | (ta ? 0x0010u : 0x0000u));
    word = static_cast<std::uint16_t>(word | (music ? 0x0008u : 0x0000u));
    word = static_cast<std::uint16_t>(word | (di_bit ? 0x0004u : 0x0000u));
    word = static_cast<std::uint16_t>(word | (address & 0x03u));
    return word;
}

// Block 2 of a type 2A group: the group code, the PTY, the A/B flag and a
// four-bit segment address.
[[nodiscard]] std::uint16_t type2a_block2(std::uint8_t pty, bool tp, bool ab,
                                          std::uint8_t address) {
    std::uint16_t word = static_cast<std::uint16_t>(2u << 12);
    word = static_cast<std::uint16_t>(word | (tp ? 0x0400u : 0x0000u));
    word = static_cast<std::uint16_t>(word | ((pty & 0x1Fu) << 5));
    word = static_cast<std::uint16_t>(word | (ab ? 0x0010u : 0x0000u));
    word = static_cast<std::uint16_t>(word | (address & 0x0Fu));
    return word;
}

void push_block(std::vector<std::uint8_t>& bits, std::uint16_t info,
                decode::BlockOffset offset) {
    // EN 50067 clause 2.2: every information word and checkword is
    // transmitted most significant bit first.
    const std::uint32_t block = decode::make_block(info, offset);
    for (int i = 25; i >= 0; --i) {
        bits.push_back(static_cast<std::uint8_t>((block >> i) & 1u));
    }
}

void push_group(std::vector<std::uint8_t>& bits, const GroupWords& words) {
    push_block(bits, words.b1, decode::BlockOffset::kA);
    push_block(bits, words.b2, decode::BlockOffset::kB);
    push_block(bits, words.b3,
               words.version_b ? decode::BlockOffset::kCPrime : decode::BlockOffset::kC);
    push_block(bits, words.b4, decode::BlockOffset::kD);
}

// The whole transmission: four 0A groups carrying PS, four 2A groups
// carrying RadioText, repeated.
//
// PRE-DIFFERENTIAL, which matters. RdsModSpec::differential_encode is left
// at its default, so the modulator applies clause 1.6 and the decoder
// removes it again. Handing it bits that were already differentially encoded
// would produce a transmission whose blocks fail the checkword, which is a
// mistake that looks exactly like a decoder that cannot decode.
//
// ta_from_cycle is the cycle at which the traffic announcement flag turns
// on, and kNoTrafficAnnouncement leaves it off for the whole transmission.
// A station that announces from the first group is not a change, which is
// the distinction the timing case rests on: the first valid TA a decoder
// sees is the state the station was already in.
constexpr int kNoTrafficAnnouncement = -1;

[[nodiscard]] std::vector<std::uint8_t> station_bits(
    int cycles = kStationCycles, int ta_from_cycle = kNoTrafficAnnouncement) {
    std::vector<std::uint8_t> bits;
    bits.reserve(static_cast<std::size_t>(cycles) * 8U * 104U);

    for (int cycle = 0; cycle < cycles; ++cycle) {
        const bool ta = ta_from_cycle != kNoTrafficAnnouncement && cycle >= ta_from_cycle;
        for (std::uint8_t segment = 0; segment < 4; ++segment) {
            // The DI bits are addressed by the same C1 C0 that addresses the
            // PS segment, transmitted d3 first. 0b1000 makes the station
            // stereo with a static PTY, which is what the four-bit pattern
            // below says one bit at a time.
            const bool di_bit = segment == 3;
            push_group(bits, GroupWords{
                                 kStationPi,
                                 type0_block2(kStationPty, true, ta, true, di_bit,
                                              segment),
                                 0xE0E0,  // two "no alternative exists" AF codes
                                 chars_to_word(kStationPs[segment * 2U],
                                               kStationPs[segment * 2U + 1U]),
                                 false,
                             });
        }
        for (std::uint8_t segment = 0; segment < 4; ++segment) {
            const std::size_t at = static_cast<std::size_t>(segment) * 4U;
            push_group(bits, GroupWords{
                                 kStationPi,
                                 type2a_block2(kStationPty, true, false, segment),
                                 chars_to_word(kStationRt[at], kStationRt[at + 1U]),
                                 chars_to_word(kStationRt[at + 2U], kStationRt[at + 3U]),
                                 false,
                             });
        }
    }
    return bits;
}

// ---------------------------------------------------------------------------
// The transmitter
// ---------------------------------------------------------------------------

// A quiet station, and quiet is a requirement rather than a convenience.
//
// FmProgramme's default is 52500 Hz of audio deviation, which with the pilot
// and the RDS injection puts Carson's occupied bandwidth at 268750 Hz and a
// half width of 134375. That does not fit inside one channel of the grid
// above: the prototype's passband edge is 85500. Cutting the programme to
// 7500 Hz brings the half width to 75625, which does.
//
// It is still a legal station. EN 50067 clause 1.1 permits a mono
// transmission carrying RDS, the pilot is left on at the 9 percent broadcast
// practice uses, and the RDS injection is clause 1.3's recommended 2 kHz.
// What it is not is a loud one, and nothing here depends on it being loud:
// the decoder is scale invariant and the audio is in the path only so that
// the composite is not subcarrier and pilot alone.
// A STATION WITH NO RDS IS ONE BIT AT ONE HERTZ, WHICH IS NOT A CHEAT
//
// core/dsp/synth/rds_mod.cpp refuses an empty payload and refuses a
// deviation of zero, both deliberately: there is no such thing as an RDS
// modulator transmitting nothing, and a spec file saying otherwise would be
// a station that reads as configured and radiates no subcarrier. So the
// no-RDS arm asks for the least it will accept.
//
// What that produces is a transmission with no subcarrier in it after the
// first bit period: the data signal goes to zero once the payload runs out
// and clause 1.4's modulation is suppressed-carrier, so there is nothing
// left to suppress. The one bit that does go out carries 1 Hz of deviation
// against the 75000 the composite is scaled to, which is 97 dB under the
// programme, over 842 microseconds of a three second recording.
[[nodiscard]] siggen::WfmSpec station_spec(bool with_rds, int cycles = kStationCycles,
                                           int ta_from_cycle = kNoTrafficAnnouncement) {
    siggen::WfmSpec spec;
    spec.rate = kRdsSourceRate;
    spec.carrier_offset = 0;

    spec.programme.stereo = false;
    spec.programme.left_tone_hz = 1000;
    spec.programme.right_tone_hz = 1000;
    spec.programme.audio_deviation_hz = 7500;
    spec.programme.preemphasis = siggen::Preemphasis::None;

    spec.rds.pilot_enabled = true;
    spec.rds.rds_deviation_hz = with_rds ? 2000 : 1;
    spec.rds.bits = with_rds ? station_bits(cycles, ta_from_cycle)
                             : std::vector<std::uint8_t>{0};
    return spec;
}

// A file the test owns and removes, named for the case that wrote it so two
// cases running at once on one machine cannot collide. Catch2's ctest
// integration runs each TEST_CASE as its own process, so that is the
// ordinary arrangement rather than the exception.
class StationFile {
public:
    explicit StationFile(std::string_view tag)
        : path_(std::filesystem::temp_directory_path() /
                std::format("revenant-rds-{}.cf32", tag)) {}

    StationFile(const StationFile&) = delete;
    StationFile& operator=(const StationFile&) = delete;
    StationFile(StationFile&&) = delete;
    StationFile& operator=(StationFile&&) = delete;

    ~StationFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    [[nodiscard]] std::string uri() const {
        // file:///C:/... is what core/source/registry.cpp parses back into a
        // Windows path, and the format comes off the .cf32 extension.
        std::string text = path_.generic_string();
        return std::format("file:///{}?rate={}&center=0", text, kRdsSourceRate);
    }

    [[nodiscard]] dsp::SampleIndex samples() const { return samples_; }

    // Renders the station and writes it as interleaved float32.
    //
    // In blocks rather than whole, because the whole of a three second
    // station at this rate is thirty megabytes and there is no reason for
    // all of it to be resident at once. WfmModulator::render is pure over
    // the absolute index, so the partition is free.
    // samples of zero takes the length of the RDS payload, which is what
    // every case carrying one wants. The no-RDS arm states its own, because
    // its payload is the one bit the modulator insists on and 842
    // microseconds is not a recording.
    [[nodiscard]] Status write(const siggen::WfmSpec& spec, double noise_sigma,
                               dsp::SampleIndex samples = 0) {
        auto modulator = siggen::WfmModulator::create(spec);
        if (!modulator) {
            return std::unexpected(modulator.error());
        }

        samples_ = samples != 0 ? samples
                                : modulator->composite().rds().nominal_sample_count();

        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(std::format("could not write {}", path_.string()));
        }

        std::mt19937_64 generator(kNoiseSeed);
        std::normal_distribution<double> gaussian(0.0, noise_sigma);

        constexpr std::size_t kBlock = 65'536;
        std::vector<dsp::Complex32> block(kBlock);
        for (dsp::SampleIndex at = 0; at < samples_; at += kBlock) {
            const auto count =
                static_cast<std::size_t>(std::min<dsp::SampleIndex>(kBlock, samples_ - at));
            const std::span<dsp::Complex32> piece(block.data(), count);
            modulator->render(at, piece);

            if (noise_sigma > 0.0) {
                for (dsp::Complex32& sample : piece) {
                    sample += dsp::Complex32(static_cast<float>(gaussian(generator)),
                                             static_cast<float>(gaussian(generator)));
                }
            }

            // std::complex<float> is two packed floats, which core/dsp/
            // types.h asserts, so the buffer is already the cf32 layout the
            // file backend reads.
            out.write(reinterpret_cast<const char*>(piece.data()),
                      static_cast<std::streamsize>(piece.size_bytes()));
            if (!out) {
                return fail(std::format("writing {} failed part way through", path_.string()));
            }
        }
        return {};
    }

private:
    std::filesystem::path path_;
    dsp::SampleIndex samples_ = 0;
};

// ---------------------------------------------------------------------------
// Driving the engine
// ---------------------------------------------------------------------------

// Unthrottled by default. Every other case in this suite runs at pace = 1 so
// there is a window to subscribe inside; there is nothing to subscribe to
// here, the decoder is installed before the engine starts, and a three
// second station at realtime would be three seconds of test.
//
// The two cases that act WHILE THE DECODER IS RUNNING pass pace = 1, and
// that is not a convenience either. Unthrottled, a file source delivers the
// whole recording faster than a client can poll twice, so a case trying to
// remove or retune "during" the decode would in practice be doing it after,
// which is exactly the defect those two cases were written to stop having.
[[nodiscard]] HarnessOptions rds_options(const std::string& uri, double pace = 0.0) {
    HarnessOptions options;
    options.source_uri = uri;
    options.channels = kRdsChannels;
    options.pace = pace;
    options.block_samples = 16'384;
    return options;
}

void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

[[nodiscard]] rpc::VrxParams rds_receiver() {
    return rpc::VrxParams{.center = kReceiverOffsetHz,
                          .bandwidth = 200'000,
                          .demod = rpc::Demod::Wfm,
                          .audio_rate = kCompositeRate};
}

// Runs the source to its end and joins. The engine stops itself when a file
// source runs out, so this waits for that rather than cutting it short:
// every case here is about what the decoder accumulated over the whole
// recording.
void run_to_completion(Harness& harness, dsp::SampleIndex samples, int timeout_ms) {
    const auto ready = harness.start_engine();
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    const std::uint64_t blocks = (samples + 16'383U) / 16'384U;
    const std::uint64_t seen = harness.wait_for_blocks(blocks, timeout_ms);
    INFO(std::format("{} blocks delivered of {}", seen, blocks));
    CHECK(seen >= blocks);

    // The last blocks are still in flight on the device when the source
    // thread finishes delivering them, and the sink that feeds the decoder
    // runs when they retire. stop() flushes rather than abandoning, so the
    // join below is what waits for the tail.
    const auto finished = harness.stop_engine();
    INFO(test::message_of(finished));
    CHECK(finished.has_value());
}

// ---------------------------------------------------------------------------
// Acting while the decoder is running
// ---------------------------------------------------------------------------
//
// THE OVERLAP IS PROVED AND NOT ASSUMED, which is the whole difference
// between these two cases and the pair they replaced. A case that starts the
// engine and then acts immediately is a case that usually acts before the
// first chunk has retired, which is a stopped engine with extra steps.
//
// poll_until below is the instrument: it asks the decoder itself, over the
// wire, until the decoder's own answer satisfies a predicate or a deadline
// passes. Every wait here is bounded and every one of them reports what it
// last saw, so a timeout says which condition was not met rather than
// hanging.

// How long any of these waits will sit before giving up. Generous against a
// loaded CI machine: the longest condition here is a locked decoder with a
// full PS, which is about 1.5 seconds of a realtime recording.
constexpr int kDecodeWaitMs = 30'000;

// The interval between polls. Short against a group's 87.6 ms, so a wait
// that is satisfied mid-recording does not overshoot by a whole group and
// spend the window it was opening.
constexpr int kPollIntervalMs = 5;

// Polls rds_station until want() is happy, and hands back the last answer
// either way. A refusal ends the wait immediately: every predicate here is
// about a decoder that exists, so a receiver that went is a failure to
// report rather than a condition to keep waiting for.
template <typename Predicate>
[[nodiscard]] Expected<rpc::RdsStation> poll_until(Harness& harness, std::uint64_t vrx,
                                                   Predicate want) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kDecodeWaitMs);
    for (;;) {
        auto station = harness.client().rds_station(vrx);
        if (!station) {
            return station;
        }
        if (want(*station)) {
            return station;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return fail(std::format(
                "waited {} ms and the decoder never reached the condition: consumed {}, "
                "groups {}, lock {}, piValid {}",
                kDecodeWaitMs, station->health.samples_consumed,
                station->health.groups_decoded, static_cast<int>(station->health.lock),
                station->pi_valid));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
}

// Waits until the decoder has consumed composite BETWEEN TWO POLLS, which is
// the evidence that the engine's completion thread is inside this sink right
// now rather than that it was at some point.
//
// Two polls and not one. A single samplesConsumed above zero proves only
// that a chunk arrived once, which is also true of a run that has already
// ended; a count that MOVED proves the stream is live.
[[nodiscard]] Status wait_until_decoding(Harness& harness, std::uint64_t vrx) {
    auto first = poll_until(harness, vrx, [](const rpc::RdsStation& station) {
        return station.health.samples_consumed > 0;
    });
    if (!first) {
        return std::unexpected(first.error());
    }
    const std::uint64_t mark = first->health.samples_consumed;
    auto moved = poll_until(harness, vrx, [mark](const rpc::RdsStation& station) {
        return station.health.samples_consumed > mark;
    });
    if (!moved) {
        return std::unexpected(moved.error());
    }
    return {};
}

[[nodiscard]] std::string text_of(const std::vector<std::uint8_t>& bytes, std::size_t count) {
    const std::size_t take = std::min(count, bytes.size());
    return std::string(reinterpret_cast<const char*>(bytes.data()), take);
}

}  // namespace

// ---------------------------------------------------------------------------
// The claim
// ---------------------------------------------------------------------------

TEST_CASE("a synthetic station's PI, PS and RadioText come back over the wire",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    StationFile file("endtoend");
    const auto written = file.write(station_spec(true), 0.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri()));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    // THE FIRST POLL IS BEFORE THE ENGINE RUNS, and that is the point of
    // making it here rather than at the end. It is what builds the decoder
    // and joins it to the receiver's audio fan-out, so the composite reaches
    // it from the first dispatch. A client that polled only after the
    // recording had played would get an empty station and no way to tell
    // that from a station with nothing on it.
    auto first = harness.client().rds_station(*vrx);
    INFO(test::message_of(first));
    REQUIRE(first.has_value());

    // And it answers, rather than refusing or blocking. An unlocked decoder
    // with zero groups is the only honest answer to a poll made before a
    // sample has arrived.
    CHECK(first->vrx == *vrx);
    CHECK(first->region == rpc::RdsRegion::Rds);
    CHECK(first->composite_rate == kCompositeRate);
    CHECK(first->health.lock == rpc::RdsLock::Unlocked);
    CHECK(first->health.sync == rpc::RdsSync::Hunting);
    CHECK(first->health.groups_decoded == 0);
    CHECK_FALSE(first->pi_valid);
    CHECK(first->last_group_sample == 0);

    run_to_completion(harness, file.samples(), 120'000);

    auto station = harness.client().rds_station(*vrx);
    INFO(test::message_of(station));
    REQUIRE(station.has_value());

    INFO(std::format(
        "lock {} sync {} quality {:.3f} coherence {:.3f} bits {} groups {} good {} "
        "corrected {} dropped {} consumed {}",
        static_cast<int>(station->health.lock), static_cast<int>(station->health.sync),
        station->health.quality, station->health.carrier_coherence,
        station->health.bits_emitted, station->health.groups_decoded,
        station->health.blocks_good, station->health.blocks_corrected,
        station->health.blocks_dropped, station->health.samples_consumed));

    // The physical layer.
    CHECK(station->health.lock == rpc::RdsLock::Locked);
    CHECK(station->health.sync == rpc::RdsSync::Synced);
    CHECK(station->health.pilot_locked);
    CHECK(station->health.quality > 0.9);

    // The bit rate the timing loop recovered, against the 1187.5 the
    // standard states. The modulator was given no clock error, so anything
    // more than a fraction of a hertz out means the composite arrived at a
    // rate the decoder was not told about, which is the failure the whole
    // 171000 argument exists to avoid and the one that would otherwise show
    // up only as a slightly worse error rate.
    CHECK(std::abs(station->health.bit_rate_hz - decode::kBitRateHz) < 0.5);

    // The group layer, and the payload.
    CHECK(station->health.groups_decoded >= 8);
    CHECK(station->health.blocks_dropped == 0);

    CHECK(station->pi_valid);
    CHECK(station->pi == kStationPi);
    CHECK(station->pty_valid);
    CHECK(station->pty == kStationPty);
    CHECK(station->tp_valid);
    CHECK(station->tp);
    CHECK(station->ta_valid);
    CHECK_FALSE(station->ta);

    // TA arrived false and never moved, so nothing changed and the index
    // stays at zero. The first valid value is deliberately not recorded as a
    // change: see the note on RdsRoute::ta_changed_at.
    //
    // ON ITS OWN THIS ASSERTION HAS NO TEETH, and it is kept for the half it
    // does cover rather than mistaken for the whole. An implementation that
    // deleted ta_seen, last_ta and the whole change path would leave the
    // field at its default and pass here. What rejects that one is the
    // separate case below, where the flag actually moves mid-recording and
    // the index has to land on the group it moved in.
    CHECK(station->ta_changed_at == 0);

    CHECK(station->ps_received == 0x0F);
    CHECK(text_of(station->ps, 8) == kStationPs);

    CHECK(station->rt_received == 0x000F);
    CHECK(station->rt_length == kStationRt.size());
    CHECK(text_of(station->rt, kStationRt.size()) == kStationRt);

    // The region-dependent names, at the default region.
    CHECK(station->pty_long_name == "National Music");

    // lastGroupSample is in composite samples and a group is 87.6 ms, so the
    // most recent one has to land inside the recording and after the first
    // group could possibly have completed. A zero here would mean the server
    // never updated it, and a value past the end would mean it is counting
    // something other than what it says.
    const std::uint64_t consumed = station->health.samples_consumed;
    CHECK(station->last_group_sample > 0);
    CHECK(station->last_group_sample <= consumed);
    CHECK(consumed > static_cast<std::uint64_t>(kCompositeRate));

    // A second poll returns the same accumulated state, which is what makes
    // this a poll over a state rather than a drain. The engine has stopped,
    // so nothing can have moved between the two.
    auto again = harness.client().rds_station(*vrx);
    INFO(test::message_of(again));
    REQUIRE(again.has_value());
    CHECK(again->pi == station->pi);
    CHECK(again->last_group_sample == station->last_group_sample);
    CHECK(again->health.groups_decoded == station->health.groups_decoded);
}

TEST_CASE("two receivers decode one station under two regions at once",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    StationFile file("regions");
    const auto written = file.write(station_spec(true), 0.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri()));

    // Two receivers on the same station. The region is per receiver, unlike
    // the detection threshold, because two receivers can be on two
    // continents; this pair is on one station, which is the arrangement that
    // makes the difference between the two answers attributable to the
    // setting and nothing else.
    auto europe = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(europe));
    REQUIRE(europe.has_value());

    auto america = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(america));
    REQUIRE(america.has_value());
    CHECK(*europe != *america);

    // setRdsRegion before the first poll, which is the arrangement the
    // schema promises: a client does not have to poll once at the wrong
    // region and correct it afterwards. This is also what builds the second
    // decoder, so both are running before a sample arrives.
    const auto set_europe =
        harness.client().set_rds_region(*europe, rpc::RdsRegion::Rds);
    INFO(test::message_of(set_europe));
    REQUIRE(set_europe.has_value());

    const auto set_america =
        harness.client().set_rds_region(*america, rpc::RdsRegion::Rbds);
    INFO(test::message_of(set_america));
    REQUIRE(set_america.has_value());

    run_to_completion(harness, file.samples(), 120'000);

    auto rds = harness.client().rds_station(*europe);
    INFO(test::message_of(rds));
    REQUIRE(rds.has_value());

    auto rbds = harness.client().rds_station(*america);
    INFO(test::message_of(rbds));
    REQUIRE(rbds.has_value());

    // Two decoders, two fan-out consumers on two receivers, one recording.
    // Both reached lock, which is the part that would fail if the second
    // attach had displaced the first or if the two had shared state.
    INFO(std::format("rds groups {} rbds groups {}", rds->health.groups_decoded,
                     rbds->health.groups_decoded));
    CHECK(rds->health.groups_decoded >= 8);
    CHECK(rbds->health.groups_decoded >= 8);

    // The bytes agree, because the bytes are the same bytes.
    CHECK(rds->pi == kStationPi);
    CHECK(rbds->pi == kStationPi);
    CHECK(rds->pty == kStationPty);
    CHECK(rbds->pty == kStationPty);
    CHECK(text_of(rds->ps, 8) == kStationPs);
    CHECK(text_of(rbds->ps, 8) == kStationPs);

    // And the readings do not. This is the whole case: the same five bits
    // name two different programme types, both display, and nothing
    // downstream could tell them apart.
    CHECK(rds->region == rpc::RdsRegion::Rds);
    CHECK(rbds->region == rpc::RdsRegion::Rbds);
    CHECK(rds->pty_long_name == "National Music");
    CHECK(rbds->pty_long_name == "Hip hop");
    CHECK(rds->pty_long_name != rbds->pty_long_name);

    // The call sign derivation is region dependent too, and in the other
    // direction: EN 50067 has no call signs at all, so the RDS reading
    // derives nothing from a PI the RBDS reading turns into letters.
    CHECK(rds->call_sign.empty());
    CHECK_FALSE(rbds->call_sign.empty());
}

TEST_CASE("a station with no RDS never locks and says so rather than staying silent",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    // THE WORST OUTCOME THIS PROJECT CAN HAVE HERE is a decoder that never
    // locks and cannot be told from one that was never fed. The station
    // below is a real FM transmission with a pilot and a programme tone and
    // no subcarrier, so everything upstream of the decoder works and the
    // decoder has nothing to find.
    StationFile file("nords");
    const auto written =
        file.write(station_spec(false), 0.0, static_cast<dsp::SampleIndex>(kRdsSourceRate) * 3);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri()));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto first = harness.client().rds_station(*vrx);
    INFO(test::message_of(first));
    REQUIRE(first.has_value());

    run_to_completion(harness, file.samples(), 120'000);

    auto station = harness.client().rds_station(*vrx);
    INFO(test::message_of(station));
    REQUIRE(station.has_value());

    INFO(std::format("consumed {} bits {} groups {} quality {:.3f} pilot {}",
                     station->health.samples_consumed, station->health.bits_emitted,
                     station->health.groups_decoded, station->health.quality,
                     station->health.pilot_locked));

    // The decoder ran. samplesConsumed is the proof, and it is why this case
    // is not the same as one that forgot to attach the sink: a decoder that
    // was never fed reports zero here and would otherwise pass every
    // assertion below.
    CHECK(station->health.samples_consumed > static_cast<std::uint64_t>(kCompositeRate));

    // The pilot is there, because the transmission has one. That separates
    // "the receiver delivered nothing" from "the receiver delivered a
    // composite with no data on it", which are two faults with two fixes.
    CHECK(station->health.pilot_locked);

    // And nothing was claimed.
    CHECK(station->health.lock != rpc::RdsLock::Locked);
    CHECK(station->health.sync != rpc::RdsSync::Synced);
    CHECK(station->health.groups_decoded == 0);
    CHECK_FALSE(station->pi_valid);
    CHECK_FALSE(station->pty_valid);
    CHECK(station->ps_received == 0);
    CHECK(station->rt_received == 0);
    CHECK(station->last_group_sample == 0);
}

TEST_CASE("a station buried in noise claims nothing rather than claiming the wrong thing",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    INFO(std::format("noise seed {}", kNoiseSeed));

    // The station is the same one the end-to-end case decodes cleanly, under
    // complex AWGN at four times its own envelope amplitude per axis. The
    // envelope is 1.0, so this is roughly -15 dB of carrier to noise in the
    // full 1368000 Hz band, which is far below the point where an FM
    // discriminator produces anything but its own threshold noise.
    //
    // A BAR ON "DOES NOT DECODE" WOULD CERTIFY THE WRONG THING. What matters
    // is not that the text is absent, it is that no text is CLAIMED: the
    // block layer's checkword can pass on noise, roughly one 26-bit window
    // in a thousand, and a decoder that framed on one of those would publish
    // a PI that was never transmitted with piValid set. So the assertions
    // below are about the validity flags and about the PI specifically
    // differing from the real one if anything arrived at all.
    StationFile file("weak");
    const auto written = file.write(station_spec(true), 4.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri()));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto first = harness.client().rds_station(*vrx);
    INFO(test::message_of(first));
    REQUIRE(first.has_value());

    run_to_completion(harness, file.samples(), 120'000);

    auto station = harness.client().rds_station(*vrx);
    INFO(test::message_of(station));
    REQUIRE(station.has_value());

    INFO(std::format("lock {} quality {:.3f} coherence {:.3f} bits {} groups {} "
                     "dropped {} reacquisitions {}",
                     static_cast<int>(station->health.lock), station->health.quality,
                     station->health.carrier_coherence, station->health.bits_emitted,
                     station->health.groups_decoded, station->health.blocks_dropped,
                     station->health.reacquisitions));

    // It ran, and it consumed the whole recording.
    CHECK(station->health.samples_consumed > static_cast<std::uint64_t>(kCompositeRate));

    // Nothing usable came out. The decoder is permitted to have emitted bits
    // and to have run its acquisition repeatedly, which is what a marginal
    // signal looks like and is exactly what the counters are for; what it is
    // not permitted to do is publish a station.
    CHECK(station->health.groups_decoded == 0);
    CHECK_FALSE(station->pi_valid);
    CHECK(station->ps_received == 0);
    CHECK(station->rt_received == 0);

    // And the quality figure says why. It is the number a display draws, so
    // it has to read low here and high in the clean case, or an operator
    // cannot tell a weak station from a decoder that is broken.
    CHECK(station->health.quality < 0.5);
}

TEST_CASE("an unsuitable receiver is refused in terms of the condition it failed",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    // No station and no file. Every refusal below is decided from the
    // receiver's placement and parameters before a sample is read, so the
    // ordinary synthetic scene at this grid's rate is enough, and it runs in
    // milliseconds where a rendered station takes seconds.
    HarnessOptions options = rds_options(
        std::format("synthetic:wideband?rate={}&emitters=3&seed=515151&noise_dbfs=-100"
                    "&snr_min=30&snr_max=40&samples={}",
                    kRdsSourceRate, kRdsSourceRate));
    Harness harness;
    bring_up(harness, options);

    // 1. A receiver that does not exist. The engine's own sentence, not the
    //    server's: a caller that got an id wrong has to be told that and not
    //    told something about RDS.
    {
        auto missing = harness.client().rds_station(9'999);
        REQUIRE_FALSE(missing.has_value());
        INFO(missing.error().message);
        CHECK(missing.error().message.find("9999") != std::string::npos);
        CHECK(missing.error().message.find("registered") != std::string::npos);

        // And the same for the setter, which has to refuse on the receiver
        // rather than quietly recording a region for an id that names
        // nothing.
        const auto set = harness.client().set_rds_region(9'999, rpc::RdsRegion::Rbds);
        REQUIRE_FALSE(set.has_value());
        INFO(set.error().message);
        CHECK(set.error().message.find("9999") != std::string::npos);
    }

    // 2. Not an FM discriminator. AM at exactly the right rate, which is the
    //    case a bar on the audio rate alone admits.
    {
        auto am = harness.client().add_vrx(rpc::VrxParams{.center = 0,
                                                          .bandwidth = 200'000,
                                                          .demod = rpc::Demod::Am,
                                                          .audio_rate = kCompositeRate});
        INFO(test::message_of(am));
        REQUIRE(am.has_value());

        auto refused = harness.client().rds_station(*am);
        REQUIRE_FALSE(refused.has_value());
        INFO(refused.error().message);
        CHECK(refused.error().message.find("discriminator") != std::string::npos);
        CHECK(refused.error().message.find("am") != std::string::npos);
    }

    // 3. The engine's default audio rate, which is a rate this server cannot
    //    name rather than one it can refuse. VrxParams::audioRate is a
    //    verbatim echo, so it comes back zero, and EngineInfo does not carry
    //    the default.
    {
        auto defaulted = harness.client().add_vrx(
            rpc::VrxParams{.center = 0, .bandwidth = 200'000, .demod = rpc::Demod::Wfm});
        INFO(test::message_of(defaulted));
        REQUIRE(defaulted.has_value());

        auto refused = harness.client().rds_station(*defaulted);
        REQUIRE_FALSE(refused.has_value());
        INFO(refused.error().message);
        CHECK(refused.error().message.find("default audio rate") != std::string::npos);
        CHECK(refused.error().message.find("171000") != std::string::npos);
    }

    // 4. The listening receiver. WFM at 48 kHz is what an operator has open
    //    to hear the station, and it is the one shape somebody will try
    //    first. Its demodulation rate is a multiple of 48000, so it
    //    decimates, so the bound is the receiver's 148438 rather than the
    //    decoder's 125000 and the refusal has to say which.
    {
        auto listening = harness.client().add_vrx(rpc::VrxParams{.center = 0,
                                                                 .bandwidth = 200'000,
                                                                 .demod = rpc::Demod::Wfm,
                                                                 .audio_rate = 48'000});
        INFO(test::message_of(listening));
        REQUIRE(listening.has_value());

        auto refused = harness.client().rds_station(*listening);
        REQUIRE_FALSE(refused.has_value());
        INFO(refused.error().message);
        CHECK(refused.error().message.find("148438") != std::string::npos);
        CHECK(refused.error().message.find("stopband") != std::string::npos);

        // The decoder's own lower bound is named beside it, because a reader
        // who has just come from core/decode/rds_bits.h and its 125000 needs
        // to be told why this one is higher rather than left to find the
        // disagreement.
        CHECK(refused.error().message.find("125000") != std::string::npos);
    }

    // 5. Under the decoder's own bound with the decimation resolved to one,
    //    where the planner designs no audio filter at all and 125000 is the
    //    true bound. A narrow passband is what forces the demodulation rate
    //    down onto the audio rate.
    {
        auto slow = harness.client().add_vrx(rpc::VrxParams{.center = 0,
                                                            .bandwidth = 60'000,
                                                            .demod = rpc::Demod::Wfm,
                                                            .audio_rate = 120'000});
        INFO(test::message_of(slow));
        REQUIRE(slow.has_value());

        auto status = harness.client().vrx_status(*slow);
        INFO(test::message_of(status));
        REQUIRE(status.has_value());
        INFO(std::format("demod rate {}", status->demod_rate));
        REQUIRE(status->demod_rate == 120'000);  // decimation one, no audio filter

        auto refused = harness.client().rds_station(*slow);
        REQUIRE_FALSE(refused.has_value());
        INFO(refused.error().message);
        CHECK(refused.error().message.find("125000") != std::string::npos);
        CHECK(refused.error().message.find("decimation resolved to one") !=
              std::string::npos);
    }

    // 6. The right demodulator at the right rate with a passband too narrow
    //    to hold the composite. This is the fourth condition and the one a
    //    reader is most likely to assume is covered by the other three: the
    //    receiver is correct in every respect except that its filter cuts
    //    the data band off, and the decoder would sit unlocked forever with
    //    nothing saying why.
    {
        auto narrow = harness.client().add_vrx(rpc::VrxParams{.center = 0,
                                                              .bandwidth = 60'000,
                                                              .demod = rpc::Demod::Wfm,
                                                              .audio_rate = kCompositeRate});
        INFO(test::message_of(narrow));
        REQUIRE(narrow.has_value());

        auto refused = harness.client().rds_station(*narrow);
        REQUIRE_FALSE(refused.has_value());
        INFO(refused.error().message);
        CHECK(refused.error().message.find("59375") != std::string::npos);
        CHECK(refused.error().message.find("outside the filter") != std::string::npos);
    }

    // And nothing above left a decoder or a sink behind. Six refusals on
    // five receivers, and the receivers are all still there: a refusal that
    // had torn something down on the way out would be worse than no surface
    // at all, which is the assertion test_rpc_unwired.cpp used to make and
    // this one inherits.
    auto ids = harness.client().vrx_ids();
    INFO(test::message_of(ids));
    REQUIRE(ids.has_value());
    CHECK(ids->size() == 5);
}

TEST_CASE("removing a receiver takes its decoder with it, engine stopped",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    // NAMED FOR WHAT IT CHECKS. This was "a decoder goes when its receiver
    // does" and was listed as the "removed while decoding" case, which it is
    // not: the engine has never been started when the removal happens, so
    // nothing is decoding and no interleaving is exercised. What it does
    // check is the control-plane bookkeeping, which is worth a case of its
    // own and is cheap, and the case that does run against a live decoder is
    // below.

    // THE BUG THIS IS AGAINST IS ONE THIS TREE HAS ALREADY HAD. An audio
    // subscription outlived the receiver it was on and reported healthy
    // counters on a stream that could never move again; the fix was
    // end_audio_for_vrx. A decoder is the same hazard in a new place, and
    // worse, because a station struct that stops updating looks exactly like
    // a station that has stopped transmitting.
    HarnessOptions options = rds_options(
        std::format("synthetic:wideband?rate={}&emitters=3&seed=626262&noise_dbfs=-100"
                    "&snr_min=30&snr_max=40&samples={}",
                    kRdsSourceRate, kRdsSourceRate));
    Harness harness;
    bring_up(harness, options);

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto built = harness.client().rds_station(*vrx);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());

    const auto removed = harness.client().remove_vrx(*vrx);
    INFO(test::message_of(removed));
    REQUIRE(removed.has_value());

    // The engine's own sentence, and not a station struct of the last values
    // it held. A caller reading a frozen struct would be reading a station
    // that no receiver is listening to.
    auto after = harness.client().rds_station(*vrx);
    REQUIRE_FALSE(after.has_value());
    INFO(after.error().message);
    CHECK(after.error().message.find("registered") != std::string::npos);

    // The setter answers the same way, rather than building a fresh decoder
    // on an id that names nothing.
    const auto set = harness.client().set_rds_region(*vrx, rpc::RdsRegion::Rbds);
    REQUIRE_FALSE(set.has_value());
    INFO(set.error().message);
    CHECK(set.error().message.find("registered") != std::string::npos);

    // And the engine runs afterwards. A sink left attached to a removed
    // receiver, or a fan-out entry left behind, would show up here rather
    // than as a leak nobody notices: the run would fail on the dispatch that
    // reached it.
    auto second = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(second));
    REQUIRE(second.has_value());

    auto rebuilt = harness.client().rds_station(*second);
    INFO(test::message_of(rebuilt));
    REQUIRE(rebuilt.has_value());
    CHECK(rebuilt->vrx == *second);

    run_to_completion(harness, kRdsSourceRate, 60'000);
}

TEST_CASE("a retune clears the station the decoder had accumulated, engine stopped",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    // NAMED FOR WHAT IT CHECKS, on the same terms as the removal case above.
    // run_to_completion has already joined the run before the retune below,
    // so this is the clearing and nothing else: no chunk can arrive after
    // it, which is precisely the interleaving the "retuned while decoding"
    // name promised and this arrangement cannot produce. The live case is
    // below and the fence it exercises did not exist when this was written.

    StationFile file("retune");
    const auto written = file.write(station_spec(true), 0.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri()));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto built = harness.client().rds_station(*vrx);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());

    run_to_completion(harness, file.samples(), 120'000);

    auto before = harness.client().rds_station(*vrx);
    INFO(test::message_of(before));
    REQUIRE(before.has_value());
    REQUIRE(before->pi_valid);
    REQUIRE(before->ps_received == 0x0F);

    // A retune that the engine can apply in place: the same mode, the same
    // audio rate, the same bandwidth, a different centre. That is exactly
    // the retune that changes which transmitter the receiver is on, and it
    // is the one the engine does NOT refuse, so it is the one that can
    // splice two stations' text together.
    rpc::VrxParams moved = rds_receiver();
    moved.center = kReceiverOffsetHz + 200'000;
    const auto retuned = harness.client().set_vrx_params(*vrx, moved);
    INFO(test::message_of(retuned));
    REQUIRE(retuned.has_value());

    auto after = harness.client().rds_station(*vrx);
    INFO(test::message_of(after));
    REQUIRE(after.has_value());

    // Everything the old station put there is gone, including the counters,
    // because a rate derived across a retune is dropped blocks from one
    // station over bits from another.
    CHECK_FALSE(after->pi_valid);
    CHECK(after->ps_received == 0);
    CHECK(after->rt_received == 0);
    CHECK(after->health.groups_decoded == 0);
    CHECK(after->health.samples_consumed == 0);
    CHECK(after->last_group_sample == 0);
    CHECK(after->health.lock == rpc::RdsLock::Unlocked);

    // And the region survives it, because a region is the operator's setting
    // and not the station's property. A retune that reset it would put a
    // client back on the wrong continent every time it moved a receiver.
    CHECK(after->region == before->region);
}

TEST_CASE("setting a region on a decoder that has one clears what it accumulated",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    StationFile file("regionreset");
    const auto written = file.write(station_spec(true), 0.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri()));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto built = harness.client().rds_station(*vrx);
    REQUIRE(built.has_value());

    run_to_completion(harness, file.samples(), 120'000);

    auto before = harness.client().rds_station(*vrx);
    INFO(test::message_of(before));
    REQUIRE(before.has_value());
    REQUIRE(before->pi_valid);
    REQUIRE(before->health.samples_consumed > 0);
    CHECK(before->pty_long_name == "National Music");

    const auto changed = harness.client().set_rds_region(*vrx, rpc::RdsRegion::Rbds);
    INFO(test::message_of(changed));
    REQUIRE(changed.has_value());

    auto after = harness.client().rds_station(*vrx);
    INFO(test::message_of(after));
    REQUIRE(after.has_value());

    CHECK(after->region == rpc::RdsRegion::Rbds);
    CHECK_FALSE(after->pi_valid);
    CHECK(after->ps_received == 0);
    CHECK(after->health.groups_decoded == 0);

    // BOTH LAYERS, which is the assertion that would fail if the reset had
    // cleared the group layer alone. samplesConsumed belongs to the physical
    // layer and the region does not reach it, and it is cleared anyway so
    // that every counter in health shares one epoch: a client differencing
    // two polls across a region change would otherwise divide the block
    // layer's delta by the bit layer's elapsed samples.
    CHECK(after->health.samples_consumed == 0);
    CHECK(after->health.bits_emitted == 0);
    CHECK(after->health.lock == rpc::RdsLock::Unlocked);
}

TEST_CASE("a traffic announcement that starts mid-recording is timed to its group",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    // WHAT THIS EXISTS TO REJECT, stated as the implementation it rejects.
    // taChangedAt had one assertion anywhere in the suite and it was
    // `== 0`, against a station whose TA never moved. Deleting RdsRoute's
    // ta_seen, last_ta and the whole change path passes that: the field is
    // zero because nothing ever writes it. So does the other obvious wrong
    // implementation, which records the FIRST valid TA as a change, since
    // this station's first valid TA is false and false is the default.
    //
    // Here the flag is false for six cycles and true for the rest, so both
    // of those produce a wrong answer that is easy to name. The deleted
    // path leaves zero. The first-valid-is-a-change one lands at the first
    // group the decoder completed, which is a couple of groups after the
    // physical layer locks and is nowhere near the sixth cycle.
    constexpr int kQuietCycles = 6;
    constexpr int kCycles = 12;

    StationFile file("ta");
    const auto written =
        file.write(station_spec(true, kCycles, kQuietCycles), 0.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri()));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto built = harness.client().rds_station(*vrx);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    REQUIRE(built->ta_changed_at == 0);

    run_to_completion(harness, file.samples(), 240'000);

    auto station = harness.client().rds_station(*vrx);
    INFO(test::message_of(station));
    REQUIRE(station.has_value());

    // The announcement is on at the end, which is the state the flag itself
    // reports and the thing the index below is the timing of.
    REQUIRE(station->ta_valid);
    CHECK(station->ta);

    // WHERE IT HAS TO LAND. Groups go out in order, eight to a cycle, and
    // the first group carrying TA true is group 8 * kQuietCycles = 48. Its
    // last bit leaves the modulator at the end of group 48, so the index is
    // 49 group-lengths into the composite, which is 49 * 14976 = 733824.
    //
    // The window is two groups either side. It absorbs the modulator's
    // shaping span, the channelizer and fine filter group delays and the
    // audio FIR's, all of which are tens to hundreds of samples against a
    // group's 14976, and it absorbs the decoder missing the first announcing
    // group and taking the second. It does not absorb either wrong
    // implementation: the deleted path gives zero, and first-valid-is-a-
    // change gives about group 6.
    const std::uint64_t announced_at = (8ULL * kQuietCycles + 1ULL) * kGroupSamples;
    const std::uint64_t slack = 2ULL * kGroupSamples;
    INFO(std::format("taChangedAt {} against {} plus or minus {}, consumed {}",
                     station->ta_changed_at, announced_at, slack,
                     station->health.samples_consumed));
    CHECK(station->ta_changed_at > announced_at - slack);
    CHECK(station->ta_changed_at < announced_at + slack);

    // And it is inside the recording rather than past its end, which is what
    // catches an index counted in the wrong unit. Divide by compositeRate
    // for seconds, as the schema says.
    CHECK(station->ta_changed_at < station->health.samples_consumed);
    CHECK(station->composite_rate == kCompositeRate);
}

// ---------------------------------------------------------------------------
// The two that run against a live decoder
// ---------------------------------------------------------------------------

TEST_CASE("a receiver removed while its decoder is running takes it with it",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    StationFile file("removelive");
    const auto written = file.write(station_spec(true, kRunningCycles), 0.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    // Realtime, so there is a middle of the recording to act in. See
    // rds_options: unthrottled, the whole file is delivered faster than a
    // client can poll twice and "while decoding" becomes "after decoding".
    Harness harness;
    bring_up(harness, rds_options(file.uri(), 1.0));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto built = harness.client().rds_station(*vrx);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    // THE OVERLAP, PROVED. Nothing below means anything until the decoder is
    // demonstrably being fed: this waits for its own sample count to move
    // between two polls, so the completion thread is inside this sink.
    const auto live = wait_until_decoding(harness, *vrx);
    INFO(test::message_of(live));
    REQUIRE(live.has_value());

    const auto removed = harness.client().remove_vrx(*vrx);
    INFO(test::message_of(removed));
    REQUIRE(removed.has_value());

    // The engine's own sentence, as in the stopped case, and now against a
    // decoder that was mid-chunk rather than one that had never run.
    auto after = harness.client().rds_station(*vrx);
    REQUIRE_FALSE(after.has_value());
    INFO(after.error().message);
    CHECK(after.error().message.find("registered") != std::string::npos);

    // A SECOND RECEIVER DECODES ON THE SAME RUNNING ENGINE. This is what
    // catches a removal that left the fan-out or the sink list in a state
    // the next attach cannot join, which a stopped engine cannot show
    // because nothing dispatches through it afterwards.
    auto second = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(second));
    REQUIRE(second.has_value());

    auto rebuilt = harness.client().rds_station(*second);
    INFO(test::message_of(rebuilt));
    REQUIRE(rebuilt.has_value());
    CHECK(rebuilt->vrx == *second);

    const auto second_live = wait_until_decoding(harness, *second);
    INFO(test::message_of(second_live));
    REQUIRE(second_live.has_value());

    // AND THE RUN SURVIVES, which is the assertion the detach is really
    // being held to. A sink still attached to a receiver the graph has
    // dropped, or a route freed while a dispatch was inside it, fails the
    // dispatch and ends the run, and that arrives here rather than as a
    // leak nobody notices.
    const auto finished = harness.stop_engine();
    INFO(test::message_of(finished));
    CHECK(finished.has_value());
}

TEST_CASE("a retune while the decoder is running clears it and refills it",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    StationFile file("retunelive");
    const auto written = file.write(station_spec(true, kRunningCycles), 0.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri(), 1.0));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto built = harness.client().rds_station(*vrx);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    // Not merely decoding: LOCKED, with the whole of PS assembled. There has
    // to be accumulated state for the retune to destroy, or the case passes
    // on a decoder that had nothing to lose.
    auto before = poll_until(harness, *vrx, [](const rpc::RdsStation& station) {
        return station.pi_valid && station.ps_received == 0x0F;
    });
    INFO(test::message_of(before));
    REQUIRE(before.has_value());
    REQUIRE(before->health.lock == rpc::RdsLock::Locked);
    REQUIRE(text_of(before->ps, 8) == kStationPs);

    // A retune the engine applies in place: same mode, same audio rate, same
    // bandwidth, a different centre. The only station in the file is at
    // baseband DC and the receiver lands 200 kHz off it, so what it hears
    // from here on carries no subcarrier at all.
    rpc::VrxParams moved = rds_receiver();
    moved.center = kReceiverOffsetHz + 200'000;
    const auto retuned = harness.client().set_vrx_params(*vrx, moved);
    INFO(test::message_of(retuned));
    REQUIRE(retuned.has_value());

    auto cleared = harness.client().rds_station(*vrx);
    INFO(test::message_of(cleared));
    REQUIRE(cleared.has_value());
    CHECK_FALSE(cleared->pi_valid);
    CHECK(cleared->ps_received == 0);
    CHECK(cleared->health.groups_decoded == 0);
    CHECK(cleared->last_group_sample == 0);
    CHECK(cleared->region == before->region);

    // THE FENCE, AND THIS IS THE ASSERTION WITH TEETH IN IT.
    //
    // Engine::set_vrx_params queues the retune, so the server clears the
    // decoder and then tells the sample path to discard until it crosses
    // the tuning boundary AudioChunk::tuning_epoch marks. A fence that never
    // disarms is the failure that arrangement can have, and it is what an
    // off-by-one in the pending count or an epoch the graph forgot to stamp
    // produces: the decoder is fed nothing for the rest of the run and the
    // struct stays at the zeros checked above, which every assertion before
    // this one would happily pass.
    //
    // So the decoder has to be seen consuming AGAIN, from zero, after the
    // retune. It is a positive assertion for that reason.
    auto refilling = poll_until(harness, *vrx, [](const rpc::RdsStation& station) {
        return station.health.samples_consumed > 0;
    });
    INFO(test::message_of(refilling));
    REQUIRE(refilling.has_value());

    const std::uint64_t blocks = (file.samples() + 16'383U) / 16'384U;
    const std::uint64_t seen = harness.wait_for_blocks(blocks, 120'000);
    INFO(std::format("{} blocks delivered of {}", seen, blocks));
    CHECK(seen >= blocks);
    const auto finished = harness.stop_engine();
    INFO(test::message_of(finished));
    CHECK(finished.has_value());

    // And the station it left never comes back. WHAT THIS DOES AND DOES NOT
    // SAY: the receiver is 200 kHz off the only transmitter in the file, so
    // this is a statement about the new tuning carrying nothing rather than
    // proof that the fence discarded the old tuning's last few chunks. Those
    // are at most the engine's pipeline depth of composite, which is under a
    // group, and they reach a physical layer that has just been reset and
    // needs about half a second to lock, so they cannot complete a group
    // either way. The fence is what stops them being counted; the assertion
    // that it is working is the one above.
    auto ended = harness.client().rds_station(*vrx);
    INFO(test::message_of(ended));
    REQUIRE(ended.has_value());
    CHECK(ended->fault.empty());
    CHECK_FALSE(ended->pi_valid);
    CHECK(ended->ps_received == 0);
    CHECK(ended->health.groups_decoded == 0);
    CHECK(ended->health.samples_consumed > 0);
}

TEST_CASE("a burst of retunes does not leave the fence permanently armed",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    // THE REGRESSION, AND IT WAS SILENT, PERMANENT AND INDISTINGUISHABLE
    // FROM DEAD AIR.
    //
    // The first fence counted the retunes the loop thread asked for and
    // took one off per change of AudioChunk::tuning_epoch the sample path
    // observed. Graph::drain_control applies the whole queued control stack
    // in one pass before a frame is recorded, so two retunes for one
    // receiver inside one block period took the epoch from 0 to 2 and
    // produced ONE stamped change. The count went to two, came down to one
    // and stayed there: every later chunk was discarded, RdsBitSync::
    // process was never reached again, and rdsStation answered zeros with
    // an empty fault for the rest of the receiver's life. Only removing the
    // receiver recovered it.
    //
    // FOUR RETUNES AND NOT TWO, which is about certainty rather than
    // severity. One block is 16384 samples of a 1368000 S/s source, which
    // at pace 1 is 12 ms, and four round trips over the test's local
    // connection are well inside that; two of them would almost certainly
    // share a drain and four cannot avoid it. The case asserts what a
    // client can see, so it does not depend on knowing which pair shared
    // one.
    //
    // They also stay ON the station. The existing retune case moves 200 kHz
    // away and asserts the decoder is fed again; this one asserts it
    // RELOCKS, which needs a decodable signal on the far side of the fence
    // and is the stronger statement about the samples getting through.
    constexpr int kRetunes = 4;

    StationFile file("retuneburst");
    const auto written = file.write(station_spec(true, kRunningCycles), 0.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri(), 1.0));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto built = harness.client().rds_station(*vrx);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    CHECK_FALSE(built->discarding);
    CHECK(built->discarded_chunks == 0);

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    // Accumulated state for the retunes to destroy, so the case is not
    // passing on a decoder that had nothing to lose.
    auto before = poll_until(harness, *vrx, [](const rpc::RdsStation& station) {
        return station.pi_valid && station.ps_received == 0x0F;
    });
    INFO(test::message_of(before));
    REQUIRE(before.has_value());
    CHECK_FALSE(before->discarding);

    // Back to back with nothing between them: no poll, no sleep, no
    // assertion. Anything in here would give the recording thread a block
    // boundary to drain on and the case would stop reproducing what it is
    // for. The moves are a few kilohertz, which keeps the shape identical
    // so the graph applies them in place, and keeps the 200 kHz passband
    // over the station so there is still a composite to decode.
    for (int nth = 0; nth < kRetunes; ++nth) {
        rpc::VrxParams moved = rds_receiver();
        moved.center = kReceiverOffsetHz + 500 * (nth + 1);
        const auto retuned = harness.client().set_vrx_params(*vrx, moved);
        INFO(test::message_of(retuned));
        REQUIRE(retuned.has_value());
    }

    // Cleared, which the old fence also managed. Everything below is what
    // it did not.
    auto cleared = harness.client().rds_station(*vrx);
    INFO(test::message_of(cleared));
    REQUIRE(cleared.has_value());
    CHECK_FALSE(cleared->pi_valid);
    CHECK(cleared->health.groups_decoded == 0);
    CHECK(cleared->fault.empty());

    // THE FENCE CLEARS AND SAYS SO. discarding is the field a client reads
    // to tell this state from a receiver on an empty channel, and the
    // assertion is that it goes false rather than that it was ever true:
    // the first chunk of the new tuning can arrive before this poll does,
    // and a case that demanded to catch the fence up would be asserting on
    // the scheduler. The case below it pins the true reading, where it is
    // not a race.
    auto open_again = poll_until(harness, *vrx, [](const rpc::RdsStation& station) {
        return !station.discarding;
    });
    INFO(test::message_of(open_again));
    REQUIRE(open_again.has_value());

    // AND THE SAMPLES GET THROUGH, which is the assertion the old code
    // failed. samplesConsumed moving off zero means RdsBitSync::process was
    // reached, and relocking means what reached it was the composite.
    auto refilled = poll_until(harness, *vrx, [](const rpc::RdsStation& station) {
        return station.pi_valid;
    });
    INFO(test::message_of(refilled));
    REQUIRE(refilled.has_value());
    CHECK(refilled->pi == kStationPi);
    CHECK(refilled->health.samples_consumed > 0);
    CHECK_FALSE(refilled->discarding);
    CHECK(refilled->fault.empty());

    const std::uint64_t blocks = (file.samples() + 16'383U) / 16'384U;
    const std::uint64_t seen = harness.wait_for_blocks(blocks, 120'000);
    INFO(std::format("{} blocks delivered of {}", seen, blocks));
    CHECK(seen >= blocks);
    const auto finished = harness.stop_engine();
    INFO(test::message_of(finished));
    CHECK(finished.has_value());
}

TEST_CASE("a decoder discarding for a retune says so rather than reading as a quiet band",
          "[gpu][rpc][rds]") {
    REVENANT_NEEDS_GPU();

    // THE SEVERITY OF THE BUG ABOVE WAS THAT IT LOOKED LIKE NOTHING. A
    // decoder being fed nothing on purpose and a receiver pointed at an
    // empty channel produce the identical struct: no PI, no PS, zero
    // groups, zero samples consumed, empty fault. RdsStation::discarding is
    // what tells them apart, and this case is the one that reads it true.
    //
    // The retune is issued BEFORE the engine runs, which makes the reading
    // deterministic rather than a race against the first block: no chunk
    // has been delivered, so the fence is up and stays up until one is.
    // That is also the case the counting fence could not arm at all, by its
    // own admission: with no epoch seen there was nothing to count from, so
    // it left the decoder open to whatever the old tuning had already
    // recorded. Comparing against a target has no such hole.
    StationFile file("retunefence");
    const auto written = file.write(station_spec(true, kStationCycles), 0.0);
    INFO(test::message_of(written));
    REQUIRE(written.has_value());

    Harness harness;
    bring_up(harness, rds_options(file.uri()));

    auto vrx = harness.client().add_vrx(rds_receiver());
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    // Builds the decoder and joins it to the receiver's fan-out. A decoder
    // on a receiver nobody has retuned is not discarding: the fence starts
    // where the receiver is, and that is epoch zero here.
    auto built = harness.client().rds_station(*vrx);
    INFO(test::message_of(built));
    REQUIRE(built.has_value());
    CHECK_FALSE(built->discarding);

    rpc::VrxParams moved = rds_receiver();
    moved.center = kReceiverOffsetHz + 1'000;
    const auto retuned = harness.client().set_vrx_params(*vrx, moved);
    INFO(test::message_of(retuned));
    REQUIRE(retuned.has_value());

    auto fenced = harness.client().rds_station(*vrx);
    INFO(test::message_of(fenced));
    REQUIRE(fenced.has_value());

    // THE STATE AND ITS EXPLANATION, SIDE BY SIDE. Everything else here
    // reads as a dead band, and this one field is why it does not.
    CHECK(fenced->discarding);
    CHECK(fenced->fault.empty());
    CHECK_FALSE(fenced->pi_valid);
    CHECK(fenced->health.samples_consumed == 0);
    CHECK(fenced->health.groups_decoded == 0);

    // Nothing was thrown away: the engine has not run, so no chunk reached
    // the fence. discarding and discardedChunks answer different questions
    // and a client watching only the counter would see none of this.
    CHECK(fenced->discarded_chunks == 0);

    run_to_completion(harness, file.samples(), 120'000);

    // AND IT CLEARS BY ITSELF once the sample path delivers the tuning that
    // was asked for. A fence that needed a second retune to unstick would
    // pass every assertion above.
    auto after = harness.client().rds_station(*vrx);
    INFO(test::message_of(after));
    REQUIRE(after.has_value());
    CHECK_FALSE(after->discarding);
    CHECK(after->health.samples_consumed > 0);
    CHECK(after->pi_valid);
    CHECK(after->pi == kStationPi);
}

// ---------------------------------------------------------------------------
// What it costs
// ---------------------------------------------------------------------------

TEST_CASE("one RDS decoder's cost per second of composite is measured", "[rds][cost]") {
    // NO GPU AND NO ENGINE, deliberately. What is being measured is the work
    // the sink does on the engine's completion thread, which is
    // RdsBitSync::process with the group layer on its bit sink, and putting
    // a GPU chain in front of it would measure the chain. The objects here
    // are the same ones core/rpc/server.cpp builds.
    //
    // THIS IS A MEASUREMENT AND THE ASSERTION IS A CEILING, not a
    // benchmark's bar. The number is printed and the check is loose enough
    // that a slow CI machine does not fail it, because what it is guarding
    // against is an order of magnitude rather than a percentage: a decoder
    // costing a whole core per receiver would change where this code can
    // run, and that is the finding worth a red test.
    //
    // NO SEED IS PRINTED HERE, and one used to be. Nothing in this case
    // draws a random number: generate_rds renders the payload below
    // deterministically and no noise is added, so a seed in the output named
    // a value that could not have affected the result and invited whoever
    // read a failure to go looking for a draw that had gone badly. The
    // cases that DO add noise print kNoiseSeed, which is where that rule
    // belongs.

    siggen::RdsModSpec spec;
    spec.rate = kCompositeRate;
    spec.bits = station_bits();

    auto composite = siggen::generate_rds(spec);
    INFO(test::message_of(composite));
    REQUIRE(composite.has_value());
    REQUIRE(composite->samples.size() > static_cast<std::size_t>(kCompositeRate));

    decode::RdsBitsConfig config;
    config.rate = kCompositeRate;
    auto bits = decode::RdsBitSync::create(config);
    INFO(test::message_of(bits));
    REQUIRE(bits.has_value());

    decode::RdsDecoder groups;

    // One second of composite, from the middle of the recording so the
    // decoder is locked and doing the expensive thing rather than scanning.
    const std::size_t offset = composite->samples.size() / 2;
    const std::size_t count = std::min<std::size_t>(
        static_cast<std::size_t>(kCompositeRate), composite->samples.size() - offset);
    const std::span<const float> warm(composite->samples.data(), offset);
    const std::span<const float> timed(composite->samples.data() + offset, count);

    bits->process(warm, [&groups](bool bit) { groups.feed(bit); });

    double best_ms = 0.0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto started = std::chrono::steady_clock::now();
        bits->process(timed, [&groups](bool bit) { groups.feed(bit); });
        const auto finished = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(finished - started).count();
        best_ms = attempt == 0 ? ms : std::min(best_ms, ms);
    }

    const double per_second = best_ms * static_cast<double>(kCompositeRate) /
                              static_cast<double>(count);
    WARN(std::format(
        "RDS decode: {:.3f} ms per second of composite at {} S/s, which is {:.4f} "
        "percent of one core. Groups decoded {}, lock {}.",
        per_second, kCompositeRate, per_second / 10.0, groups.groups_decoded(),
        decode::lock_name(bits->status().lock)));

    // Measured at 12.07 ms on 2026-09-20 on this project's CI machine, which
    // is 1.2 percent of one core per decoding receiver. Forty is three times
    // that: far enough above to survive a busy machine or a slower one, and
    // far enough below a whole core that crossing it means the arithmetic
    // changed rather than that the run was unlucky.
    CHECK(per_second < 40.0);
}
