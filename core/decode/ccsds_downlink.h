// A CCSDS AOS downlink receiver, complex baseband to Space Packets: the part
// Meteor-M LRPT, Metop AHRPT, GK-2A LRIT and GOES HRIT share.
//
// SPECIFICATION
//
// The chain is the one CGMS LRIT/HRIT Global Specification issue 2.8 (CGMS/
// DOC/12/0017, 30 October 2013) lays out in sections 7 and 8 and every one of
// those downlinks follows: a Channel Access Data Unit of the marker 0x1ACFFC1D
// and a randomised Coded VCDU, the VCDU an AOS Transfer Frame (CCSDS 732.0)
// carrying one M_PDU whose 884-octet packet zone carries Space Packets
// (CCSDS 133.0). What each layer does is cited in core/decode/ccsds.h, which
// this file drives; what is new here:
//
//   - the convolutional code is decoded here and not in CcsdsReceiver,
//     because two of these downlinks need what CcsdsReceiver does not do:
//     the punctured codes of CCSDS 131.0-B-5 section 3.4 and table 3-1 (Metop
//     AHRPT is r = 3/4), and the basic code without the G2 inversion of
//     131.0 3.3 (GK-2A LRIT Mission Specification issue 1.0 section 9 and
//     Aerospace report ATR-2010(5482)-1 table 8 say "symbol inversion: none"
//     for GOES). 131.0 3.4.1 (5) itself says the punctured code has no
//     symbol inversion.
//   - frames are AOS with an M_PDU per 732.0 4.1.4.2; the insert zone length
//     is a parameter (732.0 4.1.3), zero unless the mission says otherwise.
//
// HOW QPSK IS MAPPED, AND WHAT THAT RESTS ON
//
// None of the documents in hand says how the coded symbol stream is split
// across the I and Q rails of a QPSK or OQPSK carrier. This receiver does not
// assume one. It treats the demodulated rails as a serial stream I0 Q0 I1 Q1
// (the arrangement ATR-2010(5482)-1 section 4.6 describes for GOES EMWIN-N
// OQPSK) and tries every hypothesis that a QPSK phase ambiguity or an unknown
// mapping can produce: the rails swapped within a symbol, the second rail
// inverted, and for OQPSK the pairing shifted by one rail. A punctured code
// adds every alignment of its pattern against the stream. Global inversion is
// left to the marker, because both K = 7 generators have odd weight, so the
// complement of a codeword is the codeword of the complemented input. Every
// hypothesis runs until one yields a frame that Reed-Solomon accepts; that
// one then runs alone until it has gone `lock_loss_codeblocks` codeblocks
// without another, when all are restarted.
//
// CLEAN ROOM
//
// Written from the documents named here and in core/decode/ccsds.h. No
// satellite decoder was read: not SatDump, goestools, xrit-rx, LeanHRPT,
// meteor_demod or any other.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/ccsds.h"
#include "core/decode/psk_demod.h"
#include "core/decode/viterbi_stream.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// CGMS 8.3.1 figure 8-4 and GK-2A 8.3: 892-octet VCDU, RS(255,223) depth 4,
// 1020-octet CVCDU, 1024-octet CADU.
inline constexpr std::size_t kAosVcduOctets = 892;
inline constexpr std::size_t kAosMpduHeaderOctets = 2;
inline constexpr std::uint8_t kAosIdleVirtualChannel = 63;

// 131.0 table 3-1, rate 3/4, in the StreamingViterbi convention of mother-code
// output order C1(1) C2(1) C1(2) C2(2) C1(3) C2(3): transmitted C1(1) C2(1)
// C2(2) C1(3).
[[nodiscard]] std::vector<std::uint8_t> ccsds_puncture_r34();

struct CcsdsDownlinkConfig {
    double sample_rate = 0.0;
    double symbol_rate = 0.0;
    PskModulation modulation = PskModulation::Qpsk;
    double roll_off = 0.5;
    // Empty for the rate 1/2 mother code.
    std::vector<std::uint8_t> puncture;
    bool g2_inverted = false;
    CcsdsRandomizer randomizer = CcsdsRandomizer::Legacy255;
    unsigned rs_interleave = 4;
    std::size_t insert_zone_octets = 0;
    std::size_t lock_loss_codeblocks = 4;
    // Passed to the demodulator.
    double carrier_bandwidth = 0.005;
    double timing_bandwidth = 0.005;
    std::size_t acquisition_symbols = 2048;
};

struct CcsdsDownlinkFrame {
    AosFrameHeader header;
    // The VCDU, Reed-Solomon corrected and check symbols removed.
    std::vector<std::uint8_t> vcdu;
    std::size_t rs_corrected = 0;
    // False when the VC frame count skipped since the last frame on that VC.
    bool continuous = false;
};

struct CcsdsDownlinkEvents {
    std::function<void(const CcsdsDownlinkFrame&)> on_frame;
    // Idle packets (APID 0x7FF) are not delivered.
    std::function<void(std::uint8_t virtual_channel, const SpacePacket&)> on_packet;
};

struct CcsdsDownlinkStats {
    std::size_t frames = 0;       // Reed-Solomon accepted
    std::size_t frames_failed = 0;  // marker found, Reed-Solomon refused
    std::size_t fill_frames = 0;    // VC 63
    std::size_t packets = 0;
    std::size_t packets_dropped = 0;
    std::size_t rs_corrected = 0;
    std::size_t relocks = 0;
};

class CcsdsDownlink {
public:
    [[nodiscard]] static Expected<CcsdsDownlink> create(const CcsdsDownlinkConfig& config,
                                                       CcsdsDownlinkEvents events);

    void push(std::span<const dsp::Complex32> samples);
    // Feeds demodulated rails directly, two per symbol for QPSK, one for
    // BPSK, in the SoftSpan convention. push() ends up here.
    void push_rails(std::span<const float> rails);
    // Releases what the decoders still hold.
    void finish();
    void reset();

    [[nodiscard]] const CcsdsDownlinkStats& stats() const { return stats_; }
    [[nodiscard]] bool demod_locked() const { return demod_.locked(); }
    [[nodiscard]] double frequency_offset_hz() const { return demod_.frequency_offset_hz(); }
    // True once a hypothesis has produced a frame and until it loses lock.
    [[nodiscard]] bool frame_locked() const { return active_ >= 0; }
    [[nodiscard]] std::size_t hypotheses() const { return hyps_.size(); }

private:
    struct Hypothesis {
        std::size_t drop = 0;
        bool swap = false;
        bool flip = false;
        std::optional<StreamingViterbi> viterbi;
        std::optional<CcsdsReceiver> receiver;
        std::uint64_t rails_in = 0;
        float held = 0.0F;
        bool holding = false;
        std::uint64_t out_index = 0;
        std::uint64_t last_good_rail = 0;
    };

    CcsdsDownlink() = default;
    Expected<void> build(Hypothesis& h) const;
    void feed(Hypothesis& h, std::span<const float> rails);
    void deliver(std::span<const CcsdsFrame> frames, std::size_t hyp);

    CcsdsDownlinkConfig config_;
    CcsdsDownlinkEvents events_;
    PskDemod demod_;
    std::vector<Hypothesis> hyps_;
    int active_ = -1;
    std::uint64_t rails_total_ = 0;
    std::uint64_t rails_per_codeblock_ = 0;
    CcsdsDownlinkStats stats_;
    std::array<std::optional<std::uint32_t>, 64> vc_count_{};
    std::array<SpacePacketExtractor, 64> extractors_{};
    std::vector<float> rails_;
    std::vector<float> transformed_;
    std::vector<std::uint8_t> bits_;
    std::vector<float> soft_bits_;
    std::vector<CcsdsFrame> frames_;
    std::vector<SpacePacket> packets_;
};

}  // namespace revenant::decode
