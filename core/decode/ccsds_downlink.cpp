#include "core/decode/ccsds_downlink.h"

#include <algorithm>
#include <utility>

namespace revenant::decode {

std::vector<std::uint8_t> ccsds_puncture_r34() {
    // 131.0 table 3-1: C1 1 0 1, C2 1 1 0, interleaved per bit time.
    return {1, 1, 0, 1, 1, 0};
}

namespace {

// Channel symbols the punctured stream emits per period of the pattern, and
// the period in mother-code steps.
std::size_t transmitted_per_period(const std::vector<std::uint8_t>& puncture) {
    if (puncture.empty()) {
        return 2;
    }
    return static_cast<std::size_t>(std::count(puncture.begin(), puncture.end(), std::uint8_t{1}));
}

}  // namespace

Expected<CcsdsDownlink> CcsdsDownlink::create(const CcsdsDownlinkConfig& config, CcsdsDownlinkEvents events) {
    if (config.modulation == PskModulation::Psk8) {
        return fail("CCSDS downlink: BPSK, QPSK or OQPSK only");
    }
    if (!config.puncture.empty() && (config.puncture.size() % 2 != 0 || transmitted_per_period(config.puncture) == 0)) {
        return fail("CCSDS downlink: puncture pattern must cover whole steps of the rate 1/2 code");
    }
    CcsdsDownlink d;
    d.config_ = config;
    d.events_ = std::move(events);

    PskDemodConfig dc;
    dc.rate = static_cast<dsp::SampleRate>(config.sample_rate);
    dc.symbol_rate = config.symbol_rate;
    dc.modulation = config.modulation;
    dc.roll_off = config.roll_off;
    dc.carrier_bandwidth = config.carrier_bandwidth;
    dc.timing_bandwidth = config.timing_bandwidth;
    dc.acquisition_symbols = config.acquisition_symbols;
    auto demod = PskDemod::create(dc);
    if (!demod) {
        return std::unexpected(demod.error());
    }
    d.demod_ = std::move(*demod);

    // The hypotheses of the header comment. The pattern period in channel
    // symbols bounds the alignments worth trying.
    const std::size_t period = transmitted_per_period(config.puncture);
    const bool paired = config.modulation != PskModulation::Bpsk;
    const bool offset = config.modulation == PskModulation::Oqpsk;
    for (std::size_t drop = 0; drop < period; ++drop) {
        if (paired && !offset && drop % 2 != 0) {
            continue;  // QPSK rails come in symbol pairs; an odd drop re-pairs them
        }
        for (const bool swap : {false, true}) {
            for (const bool flip : {false, true}) {
                if (!paired && (swap || flip)) {
                    continue;
                }
                Hypothesis h;
                h.drop = drop;
                h.swap = swap;
                h.flip = flip;
                d.hyps_.push_back(std::move(h));
            }
        }
    }
    for (auto& h : d.hyps_) {
        if (auto ok = d.build(h); !ok) {
            return std::unexpected(ok.error());
        }
    }
    // Channel symbols per codeblock: marker and CVCDU, coded.
    const std::size_t block_bits = (4 + 255 * config.rs_interleave) * 8;
    const std::size_t steps = config.puncture.empty() ? 1 : config.puncture.size() / 2;
    d.rails_per_codeblock_ = block_bits * period / steps;
    return d;
}

Expected<void> CcsdsDownlink::build(Hypothesis& h) const {
    StreamingViterbiConfig vc;
    vc.memory = kCcsdsK7Memory;
    vc.generators = {kCcsdsK7G1, kCcsdsK7G2};
    vc.invert_mask = config_.g2_inverted ? kCcsdsK7InvertMask : 0U;
    vc.puncture = config_.puncture;
    // Punctured codes want a longer window than the 5K rule (Lin and Costello
    // chapter 12); the rate 3/4 code is given 12K.
    vc.traceback = config_.puncture.empty() ? 0 : 84;
    auto v = StreamingViterbi::create(std::move(vc));
    if (!v) {
        return std::unexpected(v.error());
    }
    CcsdsReceiverConfig rc;
    rc.reed_solomon = true;
    rc.rs = {16, config_.rs_interleave, 0};
    rc.randomizer = config_.randomizer;
    rc.convolutional = false;
    auto r = CcsdsReceiver::create(rc);
    if (!r) {
        return std::unexpected(r.error());
    }
    h.viterbi = std::move(*v);
    h.receiver = std::move(*r);
    h.rails_in = 0;
    h.holding = false;
    h.out_index = 0;
    h.last_good_rail = rails_total_;
    return {};
}

void CcsdsDownlink::push(std::span<const dsp::Complex32> samples) {
    rails_.clear();
    for (const auto& s : demod_.process(samples)) {
        rails_.push_back(s.symbol.real());
        if (config_.modulation != PskModulation::Bpsk) {
            rails_.push_back(s.symbol.imag());
        }
    }
    push_rails(rails_);
}

void CcsdsDownlink::feed(Hypothesis& h, std::span<const float> rails) {
    transformed_.clear();
    for (const float v : rails) {
        const std::uint64_t g = h.rails_in++;
        if (g < h.drop) {
            continue;
        }
        if (h.swap) {
            if (!h.holding) {
                h.held = v;
                h.holding = true;
                continue;
            }
            h.holding = false;
            transformed_.push_back(v);
            transformed_.push_back(h.flip ? -h.held : h.held);
            h.out_index += 2;
            continue;
        }
        const bool odd = (h.out_index++ & 1U) != 0U;
        transformed_.push_back(h.flip && odd ? -v : v);
    }
    bits_.clear();
    h.viterbi->push(transformed_, bits_);
    soft_bits_.resize(bits_.size());
    for (std::size_t i = 0; i < bits_.size(); ++i) {
        soft_bits_[i] = bits_[i] != 0U ? -1.0F : 1.0F;
    }
    frames_.clear();
    h.receiver->push(soft_bits_, frames_);
}

void CcsdsDownlink::push_rails(std::span<const float> rails) {
    if (rails.empty()) {
        return;
    }
    const std::uint64_t end = rails_total_ + rails.size();
    if (active_ >= 0) {
        auto& h = hyps_[static_cast<std::size_t>(active_)];
        feed(h, rails);
        rails_total_ = end;
        deliver(frames_, static_cast<std::size_t>(active_));
        if (active_ >= 0 && rails_total_ - h.last_good_rail > config_.lock_loss_codeblocks * rails_per_codeblock_) {
            // Lost: restart the others from here, keep this one running so a
            // stream that comes back on the same phase loses nothing.
            const auto keep = static_cast<std::size_t>(active_);
            active_ = -1;
            for (std::size_t i = 0; i < hyps_.size(); ++i) {
                if (i != keep) {
                    (void)build(hyps_[i]);
                    // A restarted hypothesis counts rails from now; its drop
                    // applies to the new stream.
                }
            }
        }
        return;
    }
    rails_total_ = end;
    for (std::size_t i = 0; i < hyps_.size(); ++i) {
        feed(hyps_[i], rails);
        deliver(frames_, i);
    }
}

void CcsdsDownlink::deliver(std::span<const CcsdsFrame> frames, std::size_t hyp) {
    for (const auto& f : frames) {
        if (!f.rs.ok()) {
            // Before lock every wrong hypothesis finds the odd marker; only
            // the locked one's refusals are worth counting.
            if (active_ == static_cast<int>(hyp)) {
                ++stats_.frames_failed;
            }
            continue;
        }
        if (active_ < 0) {
            active_ = static_cast<int>(hyp);
            ++stats_.relocks;
        } else if (active_ != static_cast<int>(hyp)) {
            continue;
        }
        hyps_[hyp].last_good_rail = rails_total_;
        auto header = parse_aos_frame_header(f.frame);
        if (!header) {
            continue;
        }
        ++stats_.frames;
        stats_.rs_corrected += f.rs.total_corrected();
        CcsdsDownlinkFrame out;
        out.header = *header;
        out.vcdu = f.frame;
        out.rs_corrected = f.rs.total_corrected();
        const std::uint8_t vc = header->virtual_channel;
        auto& last = vc_count_[vc];
        out.continuous = last.has_value() && ((*last + 1U) & 0xFFFFFFU) == header->vc_frame_count;
        last = header->vc_frame_count;
        if (events_.on_frame) {
            events_.on_frame(out);
        }
        if (vc == kAosIdleVirtualChannel) {
            ++stats_.fill_frames;
            continue;
        }
        const std::size_t mpdu = kAosFrameHeaderOctets + config_.insert_zone_octets;
        if (f.frame.size() < mpdu + kAosMpduHeaderOctets) {
            continue;
        }
        const auto fhp = parse_mpdu_first_header_pointer(std::span(f.frame).subspan(mpdu));
        if (!fhp) {
            continue;
        }
        auto& ex = extractors_[vc];
        const std::size_t before = ex.dropped();
        packets_.clear();
        ex.push(std::span(f.frame).subspan(mpdu + kAosMpduHeaderOctets), *fhp, out.continuous, packets_);
        stats_.packets_dropped += ex.dropped() - before;
        for (const auto& p : packets_) {
            if (p.header.apid == kIdleApid) {
                continue;
            }
            ++stats_.packets;
            if (events_.on_packet) {
                events_.on_packet(vc, p);
            }
        }
    }
}

void CcsdsDownlink::finish() {
    for (std::size_t i = 0; i < hyps_.size(); ++i) {
        if (active_ >= 0 && static_cast<int>(i) != active_) {
            continue;
        }
        auto& h = hyps_[i];
        bits_.clear();
        h.viterbi->flush(false, bits_);
        soft_bits_.resize(bits_.size());
        for (std::size_t k = 0; k < bits_.size(); ++k) {
            soft_bits_[k] = bits_[k] != 0U ? -1.0F : 1.0F;
        }
        frames_.clear();
        h.receiver->push(soft_bits_, frames_);
        h.receiver->flush(frames_);
        deliver(frames_, i);
    }
}

void CcsdsDownlink::reset() {
    demod_.reset();
    active_ = -1;
    rails_total_ = 0;
    stats_ = {};
    vc_count_ = {};
    for (auto& e : extractors_) {
        e.reset();
    }
    for (auto& h : hyps_) {
        (void)build(h);
    }
}

}  // namespace revenant::decode
