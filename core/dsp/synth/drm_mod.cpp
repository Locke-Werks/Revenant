#include "core/dsp/synth/drm_mod.h"

#include <cmath>
#include <format>
#include <string_view>

namespace revenant::siggen {

using namespace decode;

namespace {

std::vector<std::uint8_t> bits_of(std::span<const std::uint8_t> bytes, std::size_t total) {
    std::vector<std::uint8_t> bits;
    bits.reserve(total);
    for (std::uint8_t b : bytes) {
        for (int i = 7; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((b >> i) & 1U));
        }
    }
    bits.resize(total, 0);
    return bits;
}

void scramble(std::vector<std::uint8_t>& bits) {
    const auto prbs = drm_prbs(bits.size());
    for (std::size_t i = 0; i < bits.size(); ++i) {
        bits[i] ^= prbs[i];
    }
}

}  // namespace

Expected<DrmModulator> DrmModulator::create(const DrmTxConfig& config) {
    DrmModulator m;
    m.config_ = config;
    auto st = drm_structure(config.mode, config.occupancy);
    if (!st) {
        return std::unexpected(st.error());
    }
    m.structure_ = std::move(*st);
    std::size_t part_a = 0;
    for (const auto& s : config.streams) {
        part_a += s.part_a;
    }
    auto plan = drm_msc_plan(config.msc, config.protection_a, config.protection_b, part_a,
                             m.structure_.n_mux());
    if (!plan) {
        return std::unexpected(plan.error());
    }
    m.msc_plan_ = *plan;
    auto sdc = drm_sdc_plan(m.structure_.sdc.size(), config.sdc_mode);
    if (!sdc) {
        return std::unexpected(sdc.error());
    }
    m.sdc_plan_ = *sdc;
    m.fft_ = DrmFft(drm_mode_params(config.mode).tu);
    return m;
}

std::size_t DrmModulator::sdc_data_bytes() const { return drm_sdc_data_bytes(sdc_plan_.l2); }

std::size_t DrmModulator::multiplex_bytes() const { return (msc_plan_.l1 + msc_plan_.l2) / 8; }

Status DrmModulator::super_frame(const DrmTxSuperFrame& frame, std::vector<Complex32>& out) {
    const DrmStructure& st = structure_;
    std::vector<Complex32> cells(st.type.size());
    for (std::size_t i = 0; i < cells.size(); ++i) {
        cells[i] = st.pilot[i];
    }

    // FAC per frame, ES 6.3: identity 00, 01, 10.
    for (std::size_t f = 0; f < 3; ++f) {
        DrmFacChannel c;
        c.identity = static_cast<std::uint8_t>(f);
        c.occupancy = static_cast<std::uint8_t>(config_.occupancy);
        c.short_interleave = !config_.long_interleave;
        c.msc_mode = config_.msc == DrmQam::Qam16 ? 3 : 0;
        c.sdc_mode = static_cast<std::uint8_t>(config_.sdc_mode);
        c.services = config_.services_code;
        auto bits = drm_fac_bits(c, config_.service);
        scramble(bits);
        auto z = drm_mlc_encode(drm_fac_plan(), bits);
        if (!z) {
            return std::unexpected(z.error());
        }
        for (std::size_t i = 0; i < z->size(); ++i) {
            cells[st.fac[f][i]] = (*z)[i];
        }
    }

    // SDC, ES 6.4.2.
    auto sdc_bits = drm_sdc_bits(config_.afs_index, frame.sdc, sdc_plan_.l2);
    if (!sdc_bits) {
        return std::unexpected(sdc_bits.error());
    }
    scramble(*sdc_bits);
    auto sdc_cells = drm_mlc_encode(sdc_plan_, *sdc_bits);
    if (!sdc_cells) {
        return std::unexpected(sdc_cells.error());
    }
    for (std::size_t i = 0; i < sdc_cells->size(); ++i) {
        cells[st.sdc[i]] = (*sdc_cells)[i];
    }

    // MSC, ES 6.2.3 and clause 7.
    const std::size_t n_mux = st.n_mux();
    const std::size_t depth = config_.long_interleave ? 5 : 1;
    const auto perm = drm_interleaver(n_mux, 5);
    std::size_t part_a_total = 0;
    for (const auto& s : config_.streams) {
        part_a_total += s.part_a;
    }
    for (std::size_t m = 0; m < 3; ++m) {
        const auto& logical = frame.logical[m];
        std::vector<std::uint8_t> mux(part_a_total, 0);
        std::size_t off_a = 0;
        for (std::size_t s = 0; s < config_.streams.size(); ++s) {
            const auto& d = config_.streams[s];
            const std::vector<std::uint8_t> empty;
            const auto& lf = s < logical.size() ? logical[s] : empty;
            for (std::size_t i = 0; i < d.part_a; ++i) {
                mux[off_a + i] = i < lf.size() ? lf[i] : 0;
            }
            off_a += d.part_a;
        }
        for (std::size_t s = 0; s < config_.streams.size(); ++s) {
            const auto& d = config_.streams[s];
            const std::vector<std::uint8_t> empty;
            const auto& lf = s < logical.size() ? logical[s] : empty;
            for (std::size_t i = 0; i < d.part_b; ++i) {
                const std::size_t j = d.part_a + i;
                mux.push_back(j < lf.size() ? lf[j] : 0);
            }
        }
        const std::size_t l = msc_plan_.l1 + msc_plan_.l2;
        if (mux.size() * 8 > l) {
            return fail(std::format("DRM multiplex frame holds {} bits, streams need {}", l, mux.size() * 8));
        }
        auto bits = bits_of(mux, l);
        scramble(bits);
        auto z = drm_mlc_encode(msc_plan_, bits);
        if (!z) {
            return std::unexpected(z.error());
        }
        history_.push_back(std::move(*z));
        while (history_.size() > depth) {
            history_.pop_front();
        }
        // ES 7.6: z^_n,i = z_{n - (i mod D)}, Pi(i); frames before the first
        // are taken as zero.
        for (std::size_t i = 0; i < n_mux; ++i) {
            const std::size_t back = depth == 1 ? 0 : i % depth;
            Complex32 v{};
            if (back < history_.size()) {
                v = history_[history_.size() - 1 - back][perm[i]];
            }
            cells[st.msc[m * n_mux + i]] = v;
        }
    }
    // ES 7.7 table 46: dummy cells.
    const float a = config_.msc == DrmQam::Qam16 ? static_cast<float>(1.0 / std::sqrt(10.0))
                                                 : static_cast<float>(1.0 / std::sqrt(42.0));
    const std::size_t dummies = st.msc.size() - 3 * n_mux;
    if (dummies >= 1) {
        cells[st.msc[3 * n_mux]] = {a, a};
    }
    if (dummies >= 2) {
        cells[st.msc[3 * n_mux + 1]] = {a, -a};
    }

    // OFDM, ES 8.1: guard interval as a cyclic prefix of the useful part.
    const DrmModeParams p = drm_mode_params(config_.mode);
    // Unit mean sample power for unit-power cells.
    const auto norm = static_cast<float>(config_.amplitude / std::sqrt(static_cast<double>(st.carriers())));
    for (std::size_t f = 0; f < 3; ++f) {
        for (std::size_t s = 0; s < st.ns; ++s) {
            std::vector<Complex32> x(p.tu);
            for (int k = st.kmin; k <= st.kmax; ++k) {
                const std::size_t bin = static_cast<std::size_t>((k % static_cast<int>(p.tu) + static_cast<int>(p.tu)) %
                                                                 static_cast<int>(p.tu));
                x[bin] = cells[st.index(f, s, k)];
            }
            fft_.transform(x, true);
            for (std::size_t i = p.tu - p.tg; i < p.tu; ++i) {
                out.push_back(x[i] * norm);
            }
            for (std::size_t i = 0; i < p.tu; ++i) {
                out.push_back(x[i] * norm);
            }
        }
    }
    return {};
}

std::vector<std::uint8_t> drm_sdc_multiplex(int pa, int pb, std::span<const DrmStreamDescription> s) {
    std::vector<std::uint8_t> body;
    for (const auto& d : s) {
        body.push_back(static_cast<std::uint8_t>(d.part_a >> 4U));
        body.push_back(static_cast<std::uint8_t>(((d.part_a & 0xFU) << 4U) | ((d.part_b >> 8U) & 0xFU)));
        body.push_back(static_cast<std::uint8_t>(d.part_b & 0xFFU));
    }
    return drm_sdc_entity(0, false, static_cast<std::uint8_t>(((pa & 3) << 2) | (pb & 3)), body);
}

std::vector<std::uint8_t> drm_sdc_label(std::uint8_t short_id, std::string_view label) {
    std::vector<std::uint8_t> body(label.begin(), label.end());
    return drm_sdc_entity(1, false, static_cast<std::uint8_t>((short_id & 3U) << 2U), body);
}

std::vector<std::uint8_t> drm_sdc_time(std::uint32_t mjd, std::uint8_t hours, std::uint8_t minutes) {
    // ES 6.4.3.9: 17 + 11 bits, the first four in the nibble.
    const std::uint32_t v = ((mjd & 0x1FFFFU) << 11U) | ((hours & 0x1FU) << 6U) | (minutes & 0x3FU);
    const std::vector<std::uint8_t> body = {static_cast<std::uint8_t>(v >> 16U),
                                            static_cast<std::uint8_t>(v >> 8U),
                                            static_cast<std::uint8_t>(v)};
    return drm_sdc_entity(8, false, static_cast<std::uint8_t>((v >> 24U) & 0xFU), body);
}

std::vector<std::uint8_t> drm_sdc_audio(std::uint8_t short_id, std::uint8_t stream, bool text) {
    // ES 6.4.3.10: AAC, no SBR, mono, sampling rate code 0, then the text flag.
    const std::vector<std::uint8_t> body = {0x00, static_cast<std::uint8_t>(text ? 0x80 : 0x00)};
    return drm_sdc_entity(9, false, static_cast<std::uint8_t>(((short_id & 3U) << 2U) | (stream & 3U)), body);
}

std::vector<std::uint8_t> drm_sdc_packet_app(std::uint8_t short_id, std::uint8_t stream,
                                             std::uint8_t packet_id, bool data_units,
                                             std::uint8_t packet_length) {
    // ES 6.4.3.6, packet mode indicator 1, application domain 0.
    const std::vector<std::uint8_t> body = {
        static_cast<std::uint8_t>(0x80U | (data_units ? 0x40U : 0U) | ((packet_id & 3U) << 4U)),
        packet_length};
    return drm_sdc_entity(5, false, static_cast<std::uint8_t>(((short_id & 3U) << 2U) | (stream & 3U)), body);
}

}  // namespace revenant::siggen
