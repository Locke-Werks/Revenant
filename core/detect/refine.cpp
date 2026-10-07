// core/detect/refine.h has the model; this is the tree and the arithmetic.

#include "core/detect/refine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace revenant::detect::refine {
namespace {

using characterise::ModulationFamily;

// Below this an analogue FM reading is NFM and at or above it WFM: the same
// split as detect::kWfmMinimumBandwidthHz in label.h, which cannot be
// included here because label.h includes the detector, which includes this.
constexpr double kWfmMinimumHz = 50'000.0;

// A talker's side, as label.h's kVoiceSidebandConfidence weighs it.
constexpr double kSidebandWeight = 0.5;

// Indexed by Node. Parents before children is not required; depth_of walks.
constexpr std::array<NodeInfo, kNodeCount> kTree = {{
    {Node::Root, "", "", Level::Root},

    {Node::Root, "CW", "CW", Level::Modulation},
    {Node::Cw, "CW", "Morse", Level::Protocol},
    {Node::Root, "AM", "AM", Level::Modulation},
    {Node::Root, "USB", "USB", Level::Modulation},
    {Node::Root, "LSB", "LSB", Level::Modulation},
    {Node::Root, "WFM", "WFM", Level::Modulation},
    {Node::Wfm, "RDS", "RDS", Level::Protocol},

    {Node::Root, "NFM", "NFM", Level::Modulation},
    {Node::Nfm, "2FSK", "2FSK", Level::Family},
    {Node::Nfm, "4FSK", "4FSK", Level::Family},
    {Node::NfmFsk4, "P25", "P25", Level::Protocol},
    {Node::P25, "P25 control", "control", Level::Protocol},
    {Node::P25, "P25 voice", "voice", Level::Protocol},
    {Node::NfmFsk4, "DMR", "DMR", Level::Protocol},
    {Node::NfmFsk4, "M17", "M17", Level::Protocol},
    {Node::NfmFsk4, "NXDN", "NXDN", Level::Protocol},
    {Node::NfmFsk2, "D-STAR", "D-STAR", Level::Protocol},
    {Node::NfmFsk2, "POCSAG", "POCSAG", Level::Protocol},
    {Node::Nfm, "AX.25", "AX.25", Level::Protocol},

    {Node::Root, "FSK", "FSK", Level::Family},
    {Node::Fsk, "2FSK", "2FSK", Level::Family},
    {Node::Fsk, "4FSK", "4FSK", Level::Family},
    {Node::Fsk, "8FSK", "8FSK", Level::Family},
    {Node::Fsk2, "RTTY", "RTTY", Level::Protocol},
    {Node::Fsk2, "SITOR-B", "SITOR-B", Level::Protocol},

    {Node::Root, "PSK", "PSK", Level::Family},
    {Node::Psk, "BPSK", "BPSK", Level::Family},
    {Node::Psk, "QPSK", "QPSK", Level::Family},
    {Node::Psk, "8PSK", "8PSK", Level::Family},
    {Node::Bpsk, "PSK31", "PSK31", Level::Protocol},
    {Node::Qpsk, "TETRA", "TETRA", Level::Protocol},

    {Node::Root, "OFDM", "OFDM", Level::Family},
}};

[[nodiscard]] constexpr std::size_t at(Node node) { return static_cast<std::size_t>(node); }

[[nodiscard]] Node family_node(const Reading& reading) {
    switch (reading.family) {
        case ModulationFamily::Unknown: return Node::Root;
        case ModulationFamily::Unmodulated: return reading.double_sideband ? Node::Am : Node::Cw;
        case ModulationFamily::AnalogueFm:
            return reading.bandwidth_hz >= kWfmMinimumHz ? Node::Wfm : Node::Nfm;
        case ModulationFamily::Fsk:
            if (reading.bandwidth_hz >= kNfmFskMinimumHz) {
                switch (reading.tones) {
                    case 2: return Node::NfmFsk2;
                    case 4: return Node::NfmFsk4;
                    default: return Node::Nfm;
                }
            }
            switch (reading.tones) {
                case 2: return Node::Fsk2;
                case 4: return Node::Fsk4;
                case 8: return Node::Fsk8;
                default: return Node::Fsk;
            }
        case ModulationFamily::Psk:
            switch (reading.order) {
                case 2: return Node::Bpsk;
                case 4: return Node::Qpsk;
                case 8: return Node::Psk8;
                default: return Node::Psk;
            }
        case ModulationFamily::Ofdm: return Node::Ofdm;
    }
    return Node::Root;
}

[[nodiscard]] bool unverified(const Reading& reading, identify::Protocol protocol) {
    return (reading.protocols_unverified & (1U << static_cast<unsigned>(protocol))) != 0;
}

// NXDN has no decoder here and, by scope, gets none: it is identified only.
// The catalogue lists every row a 4FSK reading is consistent with; NXDN is
// supported when every row left after the decoders' misses is an NXDN row.
// Yaesu Fusion and dPMR share NXDN's numbers and have no decoder either, so
// a reading consistent with them supports nothing, which is the honest answer.
[[nodiscard]] bool only_nxdn_left(const Reading& reading) {
    if (reading.family != ModulationFamily::Fsk || reading.tones != 4 ||
        reading.symbol_rate_hz <= 0.0) {
        return false;
    }
    characterise::ProtocolQuery query;
    query.family = ModulationFamily::Fsk;
    query.tone_count = 4;
    query.symbol_rate_hz = reading.symbol_rate_hz;
    query.bandwidth_hz = reading.bandwidth_hz;
    bool nxdn = false;
    for (const characterise::ProtocolCandidate& candidate : characterise::match_protocols(query)) {
        const std::string_view name = candidate.row.name;
        if (name.starts_with("NXDN")) {
            nxdn = true;
        } else if (name.starts_with("P25") && unverified(reading, identify::Protocol::P25Phase1)) {
        } else if (name.starts_with("DMR") && unverified(reading, identify::Protocol::Dmr)) {
        } else if (name.starts_with("M17") && unverified(reading, identify::Protocol::M17)) {
        } else {
            return false;
        }
    }
    return nxdn;
}

// Whether a node is shown: confirmed, with every ancestor confirmed.
[[nodiscard]] bool shown(const Refinement& refinement, Node node) {
    for (Node at_node = node; at_node != Node::Root; at_node = parent_of(at_node)) {
        if (!refinement.nodes[at(at_node)].confirmed) {
            return false;
        }
    }
    return true;
}

}  // namespace

const NodeInfo& node_info(Node node) { return kTree[at(node)]; }

std::size_t depth_of(Node node) {
    std::size_t depth = 0;
    for (; node != Node::Root; node = parent_of(node)) {
        ++depth;
    }
    return depth;
}

bool has_children(Node node) {
    return std::any_of(kTree.begin() + 1, kTree.end(),
                       [node](const NodeInfo& info) { return info.parent == node; });
}

Node node_of(identify::Protocol protocol) {
    switch (protocol) {
        case identify::Protocol::None: return Node::Root;
        case identify::Protocol::P25Phase1: return Node::P25;
        case identify::Protocol::DStar: return Node::DStar;
        case identify::Protocol::Tetra: return Node::Tetra;
        case identify::Protocol::M17: return Node::M17;
        case identify::Protocol::Dmr: return Node::Dmr;
        case identify::Protocol::Pocsag: return Node::Pocsag;
        case identify::Protocol::Ax25: return Node::Ax25;
        case identify::Protocol::Rtty: return Node::Rtty;
        case identify::Protocol::SitorB: return Node::SitorB;
        case identify::Protocol::Psk31: return Node::Psk31;
        case identify::Protocol::Cw: return Node::Morse;
        case identify::Protocol::Rds: return Node::Rds;
    }
    return Node::Root;
}

double Refinement::confidence(Node node) const {
    const NodeState& state = nodes[at(node)];
    const double support = state.support;
    return support / (support + static_cast<double>(state.against) + kPrior);
}

Node Refinement::deepest() const {
    Node current = Node::Root;
    for (;;) {
        Node best = Node::Root;
        double best_confidence = -1.0;
        for (std::size_t k = 1; k < kNodeCount; ++k) {
            const Node child = static_cast<Node>(k);
            if (kTree[k].parent != current || !nodes[k].confirmed) {
                continue;
            }
            if (const double c = confidence(child); c > best_confidence) {
                best = child;
                best_confidence = c;
            }
        }
        if (best == Node::Root) {
            return current;
        }
        current = best;
    }
}

Node Refinement::candidate() const {
    const Node under = deepest();
    Node best = Node::Root;
    float best_support = 0.0F;
    for (std::size_t k = 1; k < kNodeCount; ++k) {
        if (kTree[k].parent != under || nodes[k].confirmed) {
            continue;
        }
        if (nodes[k].support > best_support) {
            best = static_cast<Node>(k);
            best_support = nodes[k].support;
        }
    }
    return best;
}

void observe(Refinement& refinement, const Reading& reading) {
    std::array<double, kNodeCount> support{};
    std::array<double, kNodeCount> against{};

    if (const Node family = family_node(reading);
        family != Node::Root && reading.family_confidence > 0.0) {
        support[at(family)] += reading.family_confidence;
    }

    // A talker on a suppressed carrier, which the characteriser names no
    // family for by construction: label.h's rule 3.
    if (reading.family == ModulationFamily::Unknown) {
        switch (reading.voice_sideband) {
            case characterise::VoiceSideband::Unknown: break;
            case characterise::VoiceSideband::Upper: support[at(Node::Usb)] += kSidebandWeight; break;
            case characterise::VoiceSideband::Lower: support[at(Node::Lsb)] += kSidebandWeight; break;
        }
    }

    if (const Node verified = node_of(reading.protocol); verified != Node::Root) {
        support[at(verified)] += kVerifiedWeight * reading.protocol_confidence;
    }
    for (std::size_t p = 1; p < identify::kProtocolCount; ++p) {
        const auto protocol = static_cast<identify::Protocol>(p);
        if (unverified(reading, protocol)) {
            against[at(node_of(protocol))] += kUnverifiedWeight;
        }
    }

    if (only_nxdn_left(reading)) {
        support[at(Node::Nxdn)] += kCatalogueWeight;
    }

    // The P25 roles. A census is only read when every data unit carried one
    // NAC: two systems in one extract is not one channel's behaviour.
    const identify::P25Census& census = reading.p25;
    if (census.tsbk + census.voice > 0 && census.nac_steady) {
        if (refinement.p25_nac_known && refinement.p25_nac != census.nac) {
            refinement.nodes[at(Node::P25Control)] = NodeState{};
            refinement.nodes[at(Node::P25Voice)] = NodeState{};
        }
        refinement.p25_nac = census.nac;
        refinement.p25_nac_known = true;
        if (census.tsbk >= kCensusMinimum && census.voice == 0) {
            support[at(Node::P25Control)] += kCensusWeight;
        } else if (census.voice >= kCensusMinimum && census.tsbk == 0) {
            support[at(Node::P25Voice)] += kCensusWeight;
        }
    }

    // Continuity is a cue about P25 only once P25 is believed: a continuous
    // NFM carrier is just as likely a broadcast tone or a beacon.
    if (shown(refinement, Node::P25)) {
        switch (reading.continuity) {
            case Continuity::Unknown: break;
            case Continuity::Continuous:
                support[at(Node::P25Control)] += kContinuityWeight;
                break;
            case Continuity::Intermittent:
                support[at(Node::P25Voice)] += kContinuityWeight;
                break;
        }
    }

    for (NodeState& state : refinement.nodes) {
        state.support *= static_cast<float>(kRetain);
        state.against *= static_cast<float>(kRetain);
    }

    for (std::size_t n = 1; n < kNodeCount; ++n) {
        const double weight = support[n];
        if (weight <= 0.0) {
            continue;
        }
        for (Node on = static_cast<Node>(n); on != Node::Root; on = parent_of(on)) {
            refinement.nodes[at(on)].support += static_cast<float>(weight);
            const Node parent = parent_of(on);
            for (std::size_t k = 1; k < kNodeCount; ++k) {
                if (kTree[k].parent == parent && static_cast<Node>(k) != on) {
                    refinement.nodes[k].against += static_cast<float>(weight);
                }
            }
        }
    }
    for (std::size_t n = 1; n < kNodeCount; ++n) {
        refinement.nodes[n].against += static_cast<float>(against[n]);
    }

    for (std::size_t n = 1; n < kNodeCount; ++n) {
        NodeState& state = refinement.nodes[n];
        const double c = refinement.confidence(static_cast<Node>(n));
        if (!state.confirmed && c >= kPromote) {
            state.confirmed = true;
        } else if (state.confirmed && c < kDemote) {
            state.confirmed = false;
        }
    }
    ++refinement.observations;
}

bool wants_refinement(const Refinement& refinement) {
    const Node deepest = refinement.deepest();
    return deepest != Node::Root && has_children(deepest) &&
           refinement.observations < kMaxRefineObservations;
}

EmitterMemory* EmitterBook::match(dsp::Hertz center, dsp::Hertz bandwidth) {
    EmitterMemory* best = nullptr;
    dsp::Hertz best_distance = std::numeric_limits<dsp::Hertz>::max();
    for (auto& [id, entry] : entries_) {
        const dsp::Hertz tolerance =
            std::max<dsp::Hertz>(1'000, std::min(bandwidth, entry.bandwidth) / 2);
        const dsp::Hertz distance = std::abs(center - entry.center);
        if (distance <= tolerance && distance < best_distance) {
            best = &entry;
            best_distance = distance;
        }
    }
    return best;
}

EmitterMemory* EmitterBook::find(std::uint64_t id) {
    const auto found = entries_.find(id);
    return found == entries_.end() ? nullptr : &found->second;
}

EmitterMemory& EmitterBook::remember(dsp::Hertz center, dsp::Hertz bandwidth,
                                     dsp::SampleIndex now) {
    if (entries_.size() >= kCapacity) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.last_live < oldest->second.last_live) {
                oldest = it;
            }
        }
        entries_.erase(oldest);
    }
    const std::uint64_t id = next_id_++;
    EmitterMemory& entry = entries_[id];
    entry.id = id;
    entry.center = center;
    entry.bandwidth = bandwidth;
    entry.last_live = now;
    return entry;
}

void EmitterBook::forget_older_than(dsp::SampleIndex now, dsp::SampleIndex forget_samples) {
    std::erase_if(entries_, [&](const auto& item) {
        return now > item.second.last_live && now - item.second.last_live > forget_samples;
    });
}

}  // namespace revenant::detect::refine
