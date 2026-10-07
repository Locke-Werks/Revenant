// Progressive refinement of what a detection is: a label tree walked from the
// obvious to the specific, one level at a time, each level on its own
// evidence.
//
// THE OWNER'S REQUEST OF 2026-10-07. "P25 control channels should be detected
// as such. Additionally, the detector needs to drill down. Start with the most
// obvious. In the P25 example, it'd detect NFM first, then P25 in general and
// then P25 Control last. This way detections that erroneously get detected as
// QDFM or whatever will eventually get detected once more examples are
// observed."
//
// WHAT IT REPLACES. core/detect/label.h names a track from the latest probe
// that may drive and from a protocol that, once verified, never left. A
// single misread probe therefore decided the bracket until the next one, and a
// wrong protocol stuck for the life of the track. Here every probe is one more
// observation, every level of the tree keeps its own tally, and the label is
// the deepest level whose tally has passed a bar with every level above it
// also past the bar. label.h still holds the one-probe rule, and uses it only
// while nothing here has been confirmed yet.
//
// THE CONFIDENCE MODEL
//
// Per node, two tallies: support and against. An observation adds its weight
// to the support of the node it names and to every ancestor of it, and to the
// against of every sibling along that path, because siblings are exclusive:
// a signal is not both NFM and AM, nor both 2FSK and 4FSK. Supporting a
// parent is neutral about its children, which is what lets "NFM" today and
// "P25" tomorrow agree instead of contradict.
//
//   confidence = support / (support + against + kPrior)
//
// The prior is what makes it a stopwatch: one observation at weight w reads
// w / (w + kPrior), and every further consistent one moves it toward one. Each
// observation first scales both tallies of every node by kRetain, so evidence
// fades with the observations that follow it and an early misreading is
// outvoted rather than held forever.
//
// A node is confirmed when its confidence reaches kPromote and stays confirmed
// until it falls under kDemote. The gap between the two is the hysteresis that
// stops a bracket flickering between two names on alternate probes. A
// confirmed node is shown only if every ancestor is confirmed, so a child
// whose parent has been demoted falls back with it.
//
// WHY THESE WEIGHTS. Every weight below is this file's choice, sized against
// the bars rather than calibrated:
//   - a characteriser family at its own confidence, as label.h already trusted
//     it, so one confident probe confirms one level as it did before;
//   - a verified sync at kVerifiedWeight times its confidence, because
//     identify.h's rule is that no modulation reading overturns a verified
//     sync, and three times the largest family weight is what keeps that true
//     after one contrary family reading;
//   - a row that ran and did not verify at kUnverifiedWeight against that
//     protocol only: a two-second extract can miss a sync on a real signal,
//     so one miss must not demote a verified sync, and four in a row do;
//   - the P25 census at kCensusWeight, since a TSBK passed a CRC;
//   - continuity at kContinuityWeight, a cue and not a proof, which on its own
//     takes several probes to confirm a role;
//   - a catalogue-only leaf (NXDN) at kCatalogueWeight, which needs two
//     consistent observations, and which is identify-only by scope: no
//     decoder is attempted and nothing is decrypted.
//
// PER EMITTER, NOT PER TRACK
//
// A repeater or a trunked voice channel is a new track at every keyup. The
// EmitterBook below remembers a refinement by frequency across tracks, so a
// track born where an emitter was seeds its refinement from it, and every
// probe writes back. The book also counts keyups, which is one of the cues
// that separate a voice channel from a control channel.
//
// PURITY. No I/O and no clocks. The detector owns the book and feeds it the
// decision index as time.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "core/characterise/catalogue.h"
#include "core/characterise/characterise.h"
#include "core/dsp/types.h"
#include "core/identify/identify.h"

namespace revenant::detect::refine {

// The tree. Root is the empty answer. Order is irrelevant to the logic; the
// parent table in refine.cpp is the structure.
enum class Node : std::uint8_t {
    Root = 0,

    // Analogue, and the carriers.
    Cw,
    Morse,
    Am,
    Usb,
    Lsb,
    Wfm,
    Rds,

    // Constant envelope in a land mobile channel, analogue or digital: the
    // obvious first answer for anything an FM receiver would carry.
    Nfm,
    NfmFsk2,
    NfmFsk4,
    P25,
    P25Control,
    P25Voice,
    Dmr,
    M17,
    Nxdn,
    DStar,
    Pocsag,
    Ax25,

    // Narrow FSK, the HF data modes, not inside an FM channel.
    Fsk,
    Fsk2,
    Fsk4,
    Fsk8,
    Rtty,
    SitorB,

    // Linear modulation.
    Psk,
    Bpsk,
    Qpsk,
    Psk8,
    Psk31,
    Tetra,

    Ofdm,
};

inline constexpr std::size_t kNodeCount = static_cast<std::size_t>(Node::Ofdm) + 1;

// What sort of answer a node is, which label.h maps onto LabelKind.
enum class Level : std::uint8_t { Root, Modulation, Family, Protocol };

struct NodeInfo {
    Node parent = Node::Root;

    // On the bracket: "P25 control".
    std::string_view name;

    // In a path: "control", so "NFM > 4FSK > P25 > control" reads.
    std::string_view step;

    Level level = Level::Root;
};

[[nodiscard]] const NodeInfo& node_info(Node node);
[[nodiscard]] inline Node parent_of(Node node) { return node_info(node).parent; }
[[nodiscard]] std::size_t depth_of(Node node);
[[nodiscard]] bool has_children(Node node);

// The node a verified protocol lands on, Root for None.
[[nodiscard]] Node node_of(identify::Protocol protocol);

inline constexpr double kPrior = 0.5;
inline constexpr double kPromote = 0.6;
inline constexpr double kDemote = 0.4;
inline constexpr double kRetain = 0.85;
inline constexpr double kVerifiedWeight = 3.0;
inline constexpr double kUnverifiedWeight = 0.8;
inline constexpr double kCensusWeight = 0.9;
inline constexpr double kContinuityWeight = 0.3;
inline constexpr double kCatalogueWeight = 0.4;

// An FSK reading at least this wide is in an FM channel, and goes under NFM.
// The narrowest land mobile digital channel is 6.25 kHz and the widest HF FSK
// mode docs/modes.md lists, 2G ALE, occupies 3 kHz, so 4 kHz separates them.
inline constexpr double kNfmFskMinimumHz = 4'000.0;

// How long a P25 track must have been up, unbroken, before its continuity
// counts toward a control channel. A voice transmission over five seconds
// happens; the census outweighs the cue when it does.
inline constexpr double kContinuousSeconds = 5.0;

// An emitter quiet this long and then live again has keyed up. Longer than a
// fade the detector bridges in a decision or two, shorter than the pause
// between two overs on a repeater.
inline constexpr double kKeyupGapSeconds = 1.0;

// An emitter nothing has been live on for this long is forgotten: half an
// hour, so a repeater with a quiet spell keeps its history and a frequency
// reused by someone else tomorrow does not inherit it.
inline constexpr double kEmitterMemorySeconds = 1'800.0;

// A role needs a P25 census at least this large to count: two TSDUs with a
// checked TSBK, or two voice data units.
inline constexpr std::uint32_t kCensusMinimum = 2;

struct NodeState {
    float support = 0.0F;
    float against = 0.0F;
    bool confirmed = false;
};

struct Refinement {
    std::array<NodeState, kNodeCount> nodes{};
    std::uint32_t observations = 0;

    // The NAC the emitter's P25 census last read. A different NAC on the same
    // frequency is a different system, and the role evidence is cleared.
    std::uint16_t p25_nac = 0;
    bool p25_nac_known = false;

    [[nodiscard]] double confidence(Node node) const;

    // The deepest node confirmed with every ancestor confirmed. Root when
    // nothing is.
    [[nodiscard]] Node deepest() const;

    // The child of deepest() with the most support that is not confirmed,
    // Root when no child has any. What the hover shows as "next".
    [[nodiscard]] Node candidate() const;

    [[nodiscard]] bool empty() const { return observations == 0; }
};

// Whether the emitter is on, continuously, intermittently, or unknown.
enum class Continuity : std::uint8_t { Unknown, Continuous, Intermittent };

// One probe's answer, in the terms the tree needs. Every field is something a
// probe or the tracker already measured; nothing here is new measurement.
struct Reading {
    // The family only when characterise::may_drive_detection accepted it,
    // Unknown otherwise, which is label.h's gate kept.
    characterise::ModulationFamily family = characterise::ModulationFamily::Unknown;
    double family_confidence = 0.0;
    std::uint32_t tones = 0;
    std::uint32_t order = 0;
    double symbol_rate_hz = 0.0;
    double bandwidth_hz = 0.0;
    bool double_sideband = false;
    characterise::VoiceSideband voice_sideband = characterise::VoiceSideband::Unknown;

    identify::Protocol protocol = identify::Protocol::None;
    double protocol_confidence = 0.0;
    std::uint32_t protocols_unverified = 0;

    identify::P25Census p25;

    Continuity continuity = Continuity::Unknown;
};

// Folds one reading into a refinement. The gates that depend on what is
// already believed (continuity counts only once P25 is confirmed) read
// `refinement` before it changes.
void observe(Refinement& refinement, const Reading& reading);

// Whether a track carrying this refinement is worth another probe: it has
// confirmed something that has children, and has not yet been observed
// kMaxRefineObservations times.
inline constexpr std::uint32_t kMaxRefineObservations = 12;
[[nodiscard]] bool wants_refinement(const Refinement& refinement);

// The emitters this detector has seen, by frequency.
struct EmitterMemory {
    std::uint64_t id = 0;
    dsp::Hertz center = 0;
    dsp::Hertz bandwidth = 0;
    Refinement refinement;

    // The last decision a track on it was live, and how many times a new
    // track has been born on it after it went quiet.
    dsp::SampleIndex last_live = 0;
    std::uint32_t keyups = 0;
};

class EmitterBook {
public:
    // At most this many emitters, the longest quiet one dropped first, and
    // none kept past `forget_samples` of silence.
    static constexpr std::size_t kCapacity = 512;

    // The emitter a track at this frequency and width belongs to, or nullptr.
    // Matches within half the narrower width, and never closer than a
    // kilohertz, so two adjacent 12.5 kHz channels stay two emitters.
    [[nodiscard]] EmitterMemory* match(dsp::Hertz center, dsp::Hertz bandwidth);
    [[nodiscard]] EmitterMemory* find(std::uint64_t id);

    // A new entry, evicting as above.
    EmitterMemory& remember(dsp::Hertz center, dsp::Hertz bandwidth, dsp::SampleIndex now);

    void forget_older_than(dsp::SampleIndex now, dsp::SampleIndex forget_samples);

    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    std::unordered_map<std::uint64_t, EmitterMemory> entries_;
    std::uint64_t next_id_ = 1;
};

}  // namespace revenant::detect::refine
