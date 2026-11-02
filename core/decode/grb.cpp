#include "core/decode/grb.h"

#include <algorithm>
#include <cmath>

#include "core/decode/ccsds_downlink.h"

namespace revenant::decode {

namespace {

using dsp::Complex32;

// Normalised header correlations (dvbs2.h): SOF alone to look closer, the
// whole PLHEADER to accept a frame while searching, and the whole PLHEADER
// at the position the last frame predicted. A noise-only 26-symbol SOF
// reaches 0.5 about once in 10^3 positions; the 90-symbol header at 0.6
// with the best of 128 code words essentially never. Both figures are
// estimated from the statistics of a random correlation, not measured.
constexpr float kSofThreshold = 0.5F;
constexpr float kAcquireThreshold = 0.6F;
constexpr float kTrackThreshold = 0.35F;

constexpr std::size_t kMarkerBits = 32;
constexpr std::size_t kCaduBits = kGrbCaduOctets * 8;

}  // namespace

Expected<GrbDecoder> GrbDecoder::create(const GrbConfig& config, GrbEvents events) {
    if (config.sample_rate <= 0.0) {
        return fail("GRB needs a sample rate");
    }
    GrbDecoder d;
    d.config_ = config;
    d.events_ = std::move(events);
    const bool psk8 = config.modulation == GrbModulation::Psk8Rate23;
    if (d.config_.symbol_rate <= 0.0) {
        d.config_.symbol_rate = psk8 ? kGrb8pskSymbolRate : kGrbQpskSymbolRate;
    }
    PskDemodConfig pc;
    pc.rate = static_cast<SampleRate>(std::llround(config.sample_rate));
    pc.symbol_rate = d.config_.symbol_rate;
    pc.modulation = psk8 ? PskModulation::Psk8 : PskModulation::Qpsk;
    pc.roll_off = config.roll_off;
    pc.carrier_bandwidth = config.carrier_bandwidth;
    pc.timing_bandwidth = config.timing_bandwidth;
    pc.acquisition_symbols = config.acquisition_symbols;
    auto demod = PskDemod::create(pc);
    if (!demod) {
        return std::unexpected(demod.error());
    }
    d.demod_ = std::move(*demod);
    // The longest PLFRAME this decodes: QPSK, 360 slots, pilots.
    d.scrambling_ = dvbs2_pl_scrambling(config.gold_code, dvbs2_plframe_symbols(kDvbs2ModcodQpsk910, 1));
    d.llr_.resize(kDvbs2NormalFrameBits);
    d.llr_deint_.resize(kDvbs2NormalFrameBits);
    d.bbframe_.resize(kDvbs2NormalFrameBits);
    return d;
}

Dvbs2Fec* GrbDecoder::fec_for(std::uint8_t modcod) {
    auto& slot = modcod == kDvbs2ModcodQpsk910 ? fec_qpsk_ : fec_8psk_;
    if (!slot) {
        auto f = Dvbs2Fec::create(modcod, config_.ldpc_iterations);
        if (!f) {
            return nullptr;
        }
        slot = std::move(*f);
    }
    return &*slot;
}

void GrbDecoder::push(std::span<const Complex32> samples) {
    const auto syms = demod_.process(samples);
    data_.clear();
    for (const auto& s : syms) {
        data_.push_back(s.symbol);
    }
    push_symbols(data_);
}

void GrbDecoder::push_symbols(std::span<const Complex32> symbols) {
    sym_.insert(sym_.end(), symbols.begin(), symbols.end());
    scan();
}

void GrbDecoder::finish() { scan(); }

void GrbDecoder::reset() {
    demod_.reset();
    sym_.clear();
    next_ = 0;
    locked_ = false;
    stream_.clear();
    stream_pos_ = 0;
    cadu_sync_ = false;
    vc_count_ = {};
    for (auto& e : extractors_) {
        e.reset();
    }
    stats_ = {};
}

void GrbDecoder::scan() {
    for (;;) {
        if (!locked_) {
            bool found = false;
            while (next_ + kDvbs2SlotSymbols <= sym_.size()) {
                const auto at = std::span<const Complex32>(sym_).subspan(next_);
                if (dvbs2_sof(at).metric >= kSofThreshold) {
                    const auto h = dvbs2_decode_plheader(at);
                    if (h.metric >= kAcquireThreshold && dvbs2_plframe_symbols(h.modcod, h.type) != 0) {
                        found = true;
                        break;
                    }
                }
                ++next_;
            }
            if (!found) {
                break;
            }
            locked_ = true;
            ++stats_.relocks;
        }
        if (next_ + kDvbs2SlotSymbols > sym_.size()) {
            break;
        }
        const auto h = dvbs2_decode_plheader(std::span<const Complex32>(sym_).subspan(next_));
        const std::size_t len = dvbs2_plframe_symbols(h.modcod, h.type);
        if (h.metric < kTrackThreshold || len == 0) {
            locked_ = false;
            ++next_;
            lose_stream();
            continue;
        }
        if (next_ + len > sym_.size()) {
            break;
        }
        decode_plframe(next_, h);
        next_ += len;
        // Keep the buffer from growing; only the unconsumed tail matters.
        if (next_ > (std::size_t{1} << 17)) {
            sym_.erase(sym_.begin(), sym_.begin() + static_cast<std::ptrdiff_t>(next_));
            next_ = 0;
        }
    }
    if (!locked_ && next_ > (std::size_t{1} << 17)) {
        sym_.erase(sym_.begin(), sym_.begin() + static_cast<std::ptrdiff_t>(next_));
        next_ = 0;
    }
}

void GrbDecoder::decode_plframe(std::size_t at, const Dvbs2Header& h) {
    ++stats_.plframes;
    if (h.modcod == kDvbs2ModcodDummy) {
        ++stats_.dummy_plframes;
        return;
    }
    const auto code = dvbs2_code(h.modcod);
    if (!code || (h.type & 2U) != 0U) {
        ++stats_.skipped_plframes;
        lose_stream();
        return;
    }
    Dvbs2Fec* fec = fec_for(h.modcod);
    if (fec == nullptr) {
        ++stats_.skipped_plframes;
        lose_stream();
        return;
    }
    const unsigned bits = psk_bits_per_symbol(code->modulation);
    const std::size_t slots = kDvbs2NormalFrameBits / bits / kDvbs2SlotSymbols;
    const bool pilots = (h.type & 1U) != 0U;

    // 5.5.4: descramble everything after the header, pilots included, and
    // take the header's rotation off; 5.5.3: drop the pilot blocks.
    const Complex32 derotate = std::conj(h.phase);
    data_.clear();
    std::size_t k = 0;  // symbol index after the header
    const std::size_t body = at + kDvbs2SlotSymbols;
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (pilots && slot > 0 && slot % 16 == 0) {
            k += kDvbs2PilotSymbols;
        }
        for (std::size_t i = 0; i < kDvbs2SlotSymbols; ++i, ++k) {
            static constexpr std::array<Complex32, 4> kUnscramble = {
                Complex32{1.0F, 0.0F}, Complex32{0.0F, -1.0F}, Complex32{-1.0F, 0.0F}, Complex32{0.0F, 1.0F}};
            data_.push_back(sym_[body + k] * derotate * kUnscramble[scrambling_[k] & 3U]);
        }
    }

    // Noise from the distance to the nearest point of the constellation.
    double noise = 0.0;
    const unsigned order = psk_order(code->modulation);
    for (const auto& s : data_) {
        float best = 1e9F;
        for (unsigned p = 0; p < order; ++p) {
            best = std::min(best, std::norm(s - psk_point(code->modulation, p)));
        }
        noise += best;
    }
    const auto n0 = static_cast<float>(std::max(noise / static_cast<double>(data_.size()), 1e-3));

    dvbs2_demap(code->modulation, data_, n0, llr_);
    std::span<const float> cw = llr_;
    if (code->modulation == PskModulation::Psk8) {
        dvbs2_deinterleave_8psk(llr_, llr_deint_);
        cw = llr_deint_;
    }
    const auto r = fec->decode(cw, bbframe_);
    stats_.ldpc_iterations += r.ldpc_iterations;
    if (!r.ok) {
        ++stats_.fec_failures;
        lose_stream();
        return;
    }
    stats_.bch_corrected += r.bch_corrected;
    auto bb = std::span(bbframe_).first(code->kbch);
    dvbs2_bb_scramble(bb);
    const auto bh = dvbs2_parse_bbheader(bb);
    if (!bh || bh->dfl > code->kbch - kDvbs2BbHeaderBits) {
        ++stats_.bbheader_failures;
        lose_stream();
        return;
    }
    push_bits(bb.subspan(kDvbs2BbHeaderBits, bh->dfl));
}

void GrbDecoder::lose_stream() {
    stream_.clear();
    stream_pos_ = 0;
    cadu_sync_ = false;
    vc_count_ = {};
}

void GrbDecoder::push_bits(std::span<const std::uint8_t> bits) {
    stream_.insert(stream_.end(), bits.begin(), bits.end());
    std::vector<std::uint8_t> frame(kGrbFrameOctets);
    for (;;) {
        if (!cadu_sync_) {
            if (stream_.size() < kMarkerBits) {
                break;
            }
            std::uint32_t reg = 0;
            bool found = false;
            std::size_t i = stream_pos_;
            for (std::size_t j = 0; j + 1 < kMarkerBits && i + j < stream_.size(); ++j) {
                reg = (reg << 1U) | stream_[i + j];
            }
            for (; i + kMarkerBits <= stream_.size(); ++i) {
                reg = (reg << 1U) | stream_[i + kMarkerBits - 1];
                if (reg == kGrbMarker) {
                    found = true;
                    break;
                }
            }
            stream_pos_ = i;
            if (!found) {
                break;
            }
            cadu_sync_ = true;
        }
        if (stream_pos_ + kCaduBits > stream_.size()) {
            break;
        }
        std::uint32_t m = 0;
        for (std::size_t j = 0; j < kMarkerBits; ++j) {
            m = (m << 1U) | stream_[stream_pos_ + j];
        }
        if (m != kGrbMarker) {
            cadu_sync_ = false;
            ++stream_pos_;
            continue;
        }
        for (std::size_t o = 0; o < kGrbFrameOctets; ++o) {
            unsigned v = 0;
            for (std::size_t b = 0; b < 8; ++b) {
                v = (v << 1U) | stream_[stream_pos_ + kMarkerBits + 8 * o + b];
            }
            frame[o] = static_cast<std::uint8_t>(v);
        }
        stream_pos_ += kCaduBits;
        deliver(frame);
    }
    if (stream_pos_ > kCaduBits * 4) {
        stream_.erase(stream_.begin(), stream_.begin() + static_cast<std::ptrdiff_t>(stream_pos_));
        stream_pos_ = 0;
    }
}

void GrbDecoder::deliver(std::span<const std::uint8_t> frame) {
    if (!ccsds_fecf_ok(frame)) {
        ++stats_.fecf_failures;
        vc_count_ = {};
        return;
    }
    // Accept the version number the PUG prints (0b00) as well as 732.0's.
    std::array<std::uint8_t, kAosFrameHeaderOctets> hdr{};
    std::copy_n(frame.begin(), hdr.size(), hdr.begin());
    if ((hdr[0] >> 6U) == 0U) {
        hdr[0] = static_cast<std::uint8_t>(hdr[0] | 0x40U);
    }
    const auto h = parse_aos_frame_header(hdr);
    if (!h) {
        ++stats_.fecf_failures;
        return;
    }
    ++stats_.frames;
    const std::uint8_t vc = h->virtual_channel;
    if (vc == kAosIdleVirtualChannel) {
        ++stats_.idle_frames;
        return;
    }
    auto& last = vc_count_[vc];
    const bool continuous = last.has_value() && ((*last + 1U) & 0xFFFFFFU) == h->vc_frame_count;
    last = h->vc_frame_count;
    if (events_.on_frame) {
        GrbFrame f;
        f.header = *h;
        f.frame.assign(frame.begin(), frame.end());
        f.continuous = continuous;
        events_.on_frame(f);
    }
    const auto mpdu = frame.subspan(kAosFrameHeaderOctets, kGrbFrameOctets - kAosFrameHeaderOctets - 2);
    const auto fhp = parse_mpdu_first_header_pointer(mpdu);
    if (!fhp) {
        return;
    }
    packets_.clear();
    auto& ex = extractors_[vc];
    const std::size_t before = ex.dropped();
    ex.push(mpdu.subspan(kAosMpduHeaderOctets), *fhp, continuous, packets_);
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

}  // namespace revenant::decode
