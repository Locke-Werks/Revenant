// DRM30 receiver: synchronisation, channel estimation and the channel
// decoders. See drm.h for the specification and the engineering choices.

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/decode/drm.h"

namespace revenant::decode {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr DrmMode kModes[] = {DrmMode::A, DrmMode::B, DrmMode::C, DrmMode::D};
constexpr std::size_t kAcqSymbols = 10;
// ES 7.6: D = 5 for long interleaving.
constexpr std::size_t kLongDepth = 5;
// Consecutive FAC failures before the receiver gives up its lock. An
// engineering choice: a little over three super frames.
constexpr int kLossFrames = 10;

std::size_t acquisition_window() {
    std::size_t w = 0;
    for (DrmMode m : kModes) {
        const auto p = drm_mode_params(m);
        w = std::max(w, (kAcqSymbols + 1) * (p.tu + p.tg) + p.tu);
    }
    return w;
}

std::vector<std::uint8_t> to_bytes(std::span<const std::uint8_t> bits) {
    std::vector<std::uint8_t> out(bits.size() / 8, 0);
    for (std::size_t i = 0; i < out.size() * 8; ++i) {
        out[i / 8] = static_cast<std::uint8_t>(out[i / 8] | ((bits[i] & 1U) << (7 - i % 8)));
    }
    return out;
}

void descramble(std::vector<std::uint8_t>& bits) {
    const auto prbs = drm_prbs(bits.size());
    for (std::size_t i = 0; i < bits.size(); ++i) {
        bits[i] ^= prbs[i];
    }
}

}  // namespace

Expected<DrmReceiver> DrmReceiver::create(const DrmConfig& config) {
    if (config.rate != kDrmSampleRate) {
        return fail("the DRM receiver takes 48000 S/s complex baseband");
    }
    DrmReceiver r;
    r.config_ = config;
    return r;
}

void DrmReceiver::reset() {
    const std::int64_t total = total_;
    const DrmConfig config = config_;
    *this = DrmReceiver{};
    config_ = config;
    total_ = total;
    buffer_start_ = total;
    next_attempt_ = total;
}

void DrmReceiver::lose_lock(std::int64_t resume) {
    sync_ = {};
    frame_locked_ = false;
    symbols_.clear();
    structure_.reset();
    probe_.reset();
    channel_.reset();
    frame_in_super_.reset();
    mux_history_.clear();
    sf_frames_ = 0;
    loss_ = 0;
    integer_offset_ = 0;
    next_attempt_ = std::max(resume, buffer_start_);
}

void DrmReceiver::process(ConstComplexSpan samples, DrmOutput& out) {
    buffer_.insert(buffer_.end(), samples.begin(), samples.end());
    total_ += static_cast<std::int64_t>(samples.size());
    for (;;) {
        if (!sync_.locked) {
            if (!acquire()) {
                break;
            }
        }
        if (!frame_locked_) {
            if (!frame_sync()) {
                if (sync_.locked) {
                    break;  // waiting for samples
                }
                continue;  // frame sync failed and dropped the lock
            }
        }
        demodulate_available();
        process_frames(out);
        break;
    }
    trim();
}

bool DrmReceiver::acquire() {
    const std::size_t window = acquisition_window();
    for (;;) {
        if (next_attempt_ < buffer_start_) {
            next_attempt_ = buffer_start_;
        }
        if (next_attempt_ + static_cast<std::int64_t>(window) > total_) {
            return false;
        }
        const auto base = static_cast<std::size_t>(next_attempt_ - buffer_start_);
        double best_metric = 0.0;
        DrmMode best_mode = DrmMode::A;
        std::size_t best_theta = 0;
        Complex32 best_gamma{};
        for (DrmMode mode : kModes) {
            const auto p = drm_mode_params(mode);
            const std::size_t ts = p.tu + p.tg;
            const std::size_t span = (kAcqSymbols + 1) * ts;
            // Prefix sums of r[n] conj(r[n+Tu]) and of the two energies.
            std::vector<std::complex<double>> cp(span + 1);
            std::vector<double> ep(span + 1);
            for (std::size_t n = 0; n < span; ++n) {
                const auto a = std::complex<double>(buffer_[base + n]);
                const auto b = std::complex<double>(buffer_[base + n + p.tu]);
                cp[n + 1] = cp[n] + a * std::conj(b);
                ep[n + 1] = ep[n] + std::norm(a) + std::norm(b);
            }
            for (std::size_t theta = 0; theta < ts; ++theta) {
                std::complex<double> g{};
                double e = 0.0;
                for (std::size_t j = 0; j < kAcqSymbols; ++j) {
                    const std::size_t n = theta + j * ts;
                    g += cp[n + p.tg] - cp[n];
                    e += ep[n + p.tg] - ep[n];
                }
                const double metric = e > 0.0 ? std::abs(g) / (0.5 * e) : 0.0;
                if (metric > best_metric) {
                    best_metric = metric;
                    best_mode = mode;
                    best_theta = theta;
                    best_gamma = Complex32(g);
                }
            }
        }
        if (best_metric >= config_.acquisition_threshold) {
            params_ = drm_mode_params(best_mode);
            fft_ = DrmFft(params_.tu);
            sync_ = {};
            sync_.locked = true;
            sync_.mode = best_mode;
            sync_.guard_metric = best_metric;
            // Guard correlation phase is -2 pi times the offset in carriers.
            cfo_carriers_ = -std::arg(best_gamma) / kTwoPi;
            backoff_ = params_.tg / 4;
            symbol0_ = next_attempt_ + static_cast<std::int64_t>(best_theta);
            frame_locked_ = false;
            probe_ = drm_structure(best_mode, -1).value();
            kmin_all_ = probe_->kmin;
            kmax_all_ = probe_->kmax;
            return true;
        }
        next_attempt_ += static_cast<std::int64_t>(window / 2);
    }
}

std::vector<Complex32> DrmReceiver::fft_at(std::int64_t start) const {
    // `start` is the first sample of the FFT window, backoff_ samples before
    // the end of the guard interval.
    const std::size_t tu = params_.tu;
    std::vector<Complex32> x(tu);
    const auto local = static_cast<std::size_t>(start - buffer_start_);
    for (std::size_t n = 0; n < tu; ++n) {
        const std::int64_t t = start + static_cast<std::int64_t>(n);
        // Phase of exp(-j 2 pi eps t / Tu), from the absolute index so it does
        // not depend on how the stream was split.
        // The whole offset, integer part included, is removed here: an
        // integer offset left as a bin shift would still turn each symbol's
        // phase by 2 pi (Tg/Tu) per carrier of offset, which the time
        // interpolation of the gain references cannot follow.
        const double cfo = cfo_carriers_ + integer_offset_;
        const std::int64_t q = t / static_cast<std::int64_t>(tu);
        const std::int64_t r = t % static_cast<std::int64_t>(tu);
        double cycles = std::fmod(cfo * static_cast<double>(q), 1.0) +
                        cfo * static_cast<double>(r) / static_cast<double>(tu);
        const double ph = -kTwoPi * cycles;
        x[n] = buffer_[local + n] * Complex32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    fft_.transform(x, false);
    const float scale = 1.0F / std::sqrt(static_cast<float>(tu));
    std::vector<Complex32> bins(static_cast<std::size_t>(kmax_all_ - kmin_all_ + 1));
    for (int k = kmin_all_; k <= kmax_all_; ++k) {
        const int bin = (k % static_cast<int>(tu) + static_cast<int>(tu)) % static_cast<int>(tu);
        // Undo the early window: a delay of backoff_ samples.
        const double ph = kTwoPi * static_cast<double>(k) * static_cast<double>(backoff_) / static_cast<double>(tu);
        bins[static_cast<std::size_t>(k - kmin_all_)] =
            x[static_cast<std::size_t>(bin)] * scale * Complex32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    return bins;
}

bool DrmReceiver::frame_sync() {
    const std::size_t ts = params_.tu + params_.tg;
    const std::size_t count = params_.ns + static_cast<std::size_t>(params_.y);
    const std::int64_t need = symbol0_ + static_cast<std::int64_t>(count * ts + params_.tu + params_.tg);
    if (need > total_) {
        return false;
    }
    // Integer offset from the frequency references' power, ES 8.4.2. Searched
    // over raw FFT bins before any offset is applied.
    const std::size_t tu = params_.tu;
    std::vector<std::vector<Complex32>> raw(count);
    const int saved = integer_offset_;
    integer_offset_ = 0;
    const int span = config_.max_integer_offset;
    const int lo = kmin_all_;
    const int hi = kmax_all_;
    kmin_all_ = -static_cast<int>(tu) / 2;
    kmax_all_ = static_cast<int>(tu) / 2 - 1;
    for (std::size_t j = 0; j < count; ++j) {
        raw[j] = fft_at(symbol0_ + static_cast<std::int64_t>(j * ts + params_.tg - backoff_));
    }
    kmin_all_ = lo;
    kmax_all_ = hi;
    integer_offset_ = saved;
    auto at = [&](std::size_t j, int k) {
        const int idx = k - (-static_cast<int>(tu) / 2);
        if (idx < 0 || idx >= static_cast<int>(tu)) {
            return Complex32{};
        }
        return raw[j][static_cast<std::size_t>(idx)];
    };
    const DrmStructure& st = *probe_;
    std::vector<int> freq_k;
    std::vector<std::pair<int, Complex32>> time_k;
    for (int k = st.kmin; k <= st.kmax; ++k) {
        const auto idx = st.index(0, 0, k);
        if (st.type[idx] == DrmCell::Frequency) {
            freq_k.push_back(k);
        }
        if (st.type[idx] == DrmCell::Time || st.type[idx] == DrmCell::Frequency) {
            time_k.emplace_back(k, st.pilot[idx]);
        }
    }
    double mean = 0.0;
    for (std::size_t j = 0; j < count; ++j) {
        for (const Complex32& v : raw[j]) {
            mean += std::norm(v);
        }
    }
    mean /= static_cast<double>(count * tu);
    int best_o = 0;
    double best_score = -1.0;
    for (int o = -span; o <= span; ++o) {
        double score = 0.0;
        for (std::size_t j = 0; j < count; ++j) {
            for (int k : freq_k) {
                score += std::norm(at(j, k + o));
            }
        }
        if (score > best_score) {
            best_score = score;
            best_o = o;
        }
    }
    // Frame start from the time references, ES 8.4.3: differential
    // correlation between neighbouring reference carriers, so the channel's
    // phase cancels.
    double best_t = -1.0;
    std::size_t best_j = 0;
    for (std::size_t j = 0; j < params_.ns; ++j) {
        std::complex<double> acc{};
        double energy = 0.0;
        for (std::size_t i = 0; i + 1 < time_k.size(); ++i) {
            const Complex32 a = at(j, time_k[i].first + best_o) * std::conj(time_k[i].second);
            const Complex32 b = at(j, time_k[i + 1].first + best_o) * std::conj(time_k[i + 1].second);
            acc += std::complex<double>(a * std::conj(b));
            energy += std::abs(std::complex<double>(a)) * std::abs(std::complex<double>(b));
        }
        const double metric = energy > 0.0 ? std::abs(acc) / energy : 0.0;
        if (metric > best_t) {
            best_t = metric;
            best_j = j;
        }
    }
    const double power_ratio = best_score / (static_cast<double>(count * freq_k.size()) * mean + 1e-30);
    if (best_t < 0.5 || power_ratio < 2.0) {
        sync_.locked = false;
        next_attempt_ = symbol0_ + static_cast<std::int64_t>(acquisition_window() / 2);
        return false;
    }
    integer_offset_ = best_o;
    sync_.cfo_hz = (cfo_carriers_ + best_o) * static_cast<double>(kDrmSampleRate) / static_cast<double>(tu);
    symbol0_ += static_cast<std::int64_t>(best_j * ts);
    sync_.frame_start_sample = symbol0_;
    frame_locked_ = true;
    next_symbol_ = 0;
    symbols_.clear();
    first_frame_ = 0;
    next_frame_ = 0;
    return true;
}

void DrmReceiver::demodulate_available() {
    const std::size_t ts = params_.tu + params_.tg;
    for (;;) {
        const std::int64_t start =
            symbol0_ + static_cast<std::int64_t>(next_symbol_ * ts + params_.tg - backoff_);
        if (start + static_cast<std::int64_t>(params_.tu) > total_) {
            return;
        }
        symbols_.push_back({fft_at(start)});
        ++next_symbol_;
    }
}

void DrmReceiver::process_frames(DrmOutput& out) {
    const std::size_t ns = params_.ns;
    const auto y = static_cast<std::size_t>(params_.y);
    while (frame_locked_) {
        const std::size_t have = symbols_.size();
        const std::size_t offset = static_cast<std::size_t>(next_frame_ - first_frame_) * ns;
        if (have < offset + ns + y) {
            return;
        }
        decode_frame(static_cast<std::size_t>(next_frame_ - first_frame_), out);
        ++next_frame_;
        // Keep one frame behind the next for time interpolation.
        while (next_frame_ - first_frame_ > 1) {
            for (std::size_t i = 0; i < ns && !symbols_.empty(); ++i) {
                symbols_.pop_front();
            }
            ++first_frame_;
        }
    }
}

void DrmReceiver::decode_frame(std::size_t local_frame, DrmOutput& out) {
    const DrmStructure& st = structure_ ? *structure_ : *probe_;
    const std::size_t ns = params_.ns;
    const std::size_t ncar = st.carriers();
    const std::size_t base = local_frame * ns;
    ++stats_.frames;

    auto bin = [&](std::size_t g, int k) {
        return symbols_[g].bins[static_cast<std::size_t>(k - kmin_all_)];
    };
    // Gain references from the previous frame (if held), this one and the
    // first y symbols of the next, ES 8.4.4. Pilot pattern is identical in
    // every frame, so frame 1 of the structure serves for all.
    const std::size_t g_first = local_frame > 0 ? base - ns : base;
    const std::size_t g_last = std::min(symbols_.size(), base + ns + static_cast<std::size_t>(params_.y));
    std::vector<std::vector<std::pair<double, Complex32>>> column(ncar);
    for (std::size_t g = g_first; g < g_last; ++g) {
        const std::size_t s = (g - base + ns) % ns;
        for (int k = st.kmin; k <= st.kmax; ++k) {
            const auto idx = st.index(1, s, k);
            if (st.type[idx] == DrmCell::Gain) {
                column[static_cast<std::size_t>(k - st.kmin)].emplace_back(
                    static_cast<double>(g), bin(g, k) / st.pilot[idx]);
            }
        }
    }
    std::vector<int> columns;
    for (std::size_t c = 0; c < ncar; ++c) {
        if (!column[c].empty()) {
            columns.push_back(st.kmin + static_cast<int>(c));
        }
    }
    std::vector<Complex32> eq(ns * ncar);
    std::vector<float> wt(ns * ncar);
    std::vector<Complex32> h_col(columns.size());
    for (std::size_t s = 0; s < ns; ++s) {
        const double g = static_cast<double>(base + s);
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const auto& pts = column[static_cast<std::size_t>(columns[c] - st.kmin)];
            // Linear in time between the bracketing references.
            std::size_t after = 0;
            while (after < pts.size() && pts[after].first < g) {
                ++after;
            }
            if (after < pts.size() && pts[after].first == g) {
                h_col[c] = pts[after].second;
            } else if (after == 0) {
                h_col[c] = pts.front().second;
            } else if (after == pts.size()) {
                h_col[c] = pts.back().second;
            } else {
                const auto& a = pts[after - 1];
                const auto& b = pts[after];
                const auto t = static_cast<float>((g - a.first) / (b.first - a.first));
                h_col[c] = a.second * (1.0F - t) + b.second * t;
            }
        }
        // Linear in frequency between columns.
        std::size_t c = 0;
        for (int k = st.kmin; k <= st.kmax; ++k) {
            while (c + 1 < columns.size() && columns[c + 1] <= k) {
                ++c;
            }
            Complex32 h;
            if (k <= columns.front()) {
                h = h_col.front();
            } else if (k >= columns.back()) {
                h = h_col.back();
            } else {
                const int k0 = columns[c];
                const int k1 = columns[c + 1];
                const float t = static_cast<float>(k - k0) / static_cast<float>(k1 - k0);
                h = h_col[c] * (1.0F - t) + h_col[c + 1] * t;
            }
            const float p = std::norm(h);
            const auto cell = s * ncar + static_cast<std::size_t>(k - st.kmin);
            eq[cell] = p > 1e-12F ? bin(base + s, k) / h : Complex32{};
            wt[cell] = p;
        }
    }

    // FAC, ES 6.3 and 7.5.3. Its cells are in the same place in every frame.
    std::vector<Complex32> fac_cells;
    std::vector<float> fac_w;
    for (std::uint32_t idx : st.fac[0]) {
        const std::size_t within = idx % (ns * ncar);
        fac_cells.push_back(eq[within]);
        fac_w.push_back(wt[within]);
    }
    double err = 0.0;
    double sig = 0.0;
    const float a = static_cast<float>(1.0 / std::sqrt(2.0));
    for (const Complex32& z : fac_cells) {
        const Complex32 ref{z.real() >= 0 ? a : -a, z.imag() >= 0 ? a : -a};
        err += std::norm(z - ref);
        sig += std::norm(ref);
    }
    stats_.last_mer_db = err > 0 ? 10.0 * std::log10(sig / err) : 99.0;

    auto bits = drm_mlc_decode(drm_fac_plan(), fac_cells, fac_w);
    std::optional<int> frame_in_super;
    if (frame_in_super_) {
        frame_in_super = (*frame_in_super_ + 1) % 3;
    }
    if (bits) {
        descramble(*bits);
        DrmFac fac = drm_parse_fac(*bits);
        if (fac.crc_ok && !fac.channel.rm && drm_occupancy_valid(sync_.mode, fac.channel.occupancy)) {
            ++stats_.fac_ok;
            sync_.locked = true;
            if (!structure_ || structure_->occupancy != fac.channel.occupancy) {
                const bool had = structure_.has_value();
                structure_ = drm_structure(sync_.mode, fac.channel.occupancy).value();
                sf_frames_ = 0;
                if (had) {
                    // `st` referred to the old map; this frame's cells are
                    // laid out for it, so start afresh with the next frame.
                    channel_ = fac.channel;
                    frame_in_super_ = (fac.channel.identity == 0 || fac.channel.identity == 3) ? 0
                                      : (fac.channel.identity == 1 ? 1 : 2);
                    out.fac.push_back(fac);
                    loss_ = 0;
                    return;
                }
            }
            channel_ = fac.channel;
            const int id = fac.channel.identity;
            frame_in_super = (id == 0 || id == 3) ? 0 : (id == 1 ? 1 : 2);
            out.fac.push_back(fac);
            loss_ = 0;
        } else {
            ++stats_.fac_bad;
            ++loss_;
        }
    }
    frame_in_super_ = frame_in_super;
    if (loss_ >= kLossFrames) {
        lose_lock(symbol0_ + static_cast<std::int64_t>(next_symbol_ * (params_.tu + params_.tg)));
        return;
    }

    if (!structure_ || !frame_in_super || !channel_) {
        return;
    }
    const DrmStructure& sf = *structure_;
    const std::size_t f = static_cast<std::size_t>(*frame_in_super);
    if (f == 0) {
        sf_cells_.assign(sf.type.size(), Complex32{});
        sf_weights_.assign(sf.type.size(), 0.0F);
        sf_frames_ = 0;
    }
    if (sf_cells_.size() != sf.type.size()) {
        return;
    }
    if (&sf == &st) {
        std::copy(eq.begin(), eq.end(), sf_cells_.begin() + static_cast<std::ptrdiff_t>(f * ns * ncar));
        std::copy(wt.begin(), wt.end(), sf_weights_.begin() + static_cast<std::ptrdiff_t>(f * ns * ncar));
    } else {
        // Estimated on the probe map: copy the carriers the real map has.
        for (std::size_t s = 0; s < ns; ++s) {
            for (int k = sf.kmin; k <= sf.kmax; ++k) {
                const auto from = s * ncar + static_cast<std::size_t>(k - st.kmin);
                sf_cells_[sf.index(f, s, k)] = eq[from];
                sf_weights_[sf.index(f, s, k)] = wt[from];
            }
        }
    }
    sf_frames_ |= std::size_t{1} << f;
    if (f == 2 && sf_frames_ == 7U) {
        decode_super_frame(out);
        sf_frames_ = 0;
    }
}

void DrmReceiver::decode_super_frame(DrmOutput& out) {
    const DrmStructure& st = *structure_;
    // SDC, ES 6.4 and 7.5.2.
    auto plan = drm_sdc_plan(st.sdc.size(), channel_->sdc_mode);
    if (plan) {
        std::vector<Complex32> cells;
        std::vector<float> w;
        for (std::uint32_t idx : st.sdc) {
            cells.push_back(sf_cells_[idx]);
            w.push_back(sf_weights_[idx]);
        }
        auto bits = drm_mlc_decode(*plan, cells, w);
        if (bits) {
            descramble(*bits);
            DrmSdc sdc = drm_parse_sdc(*bits);
            if (sdc.crc_ok) {
                ++stats_.sdc_ok;
                if (!multiplex_) {
                    multiplex_ = DrmSdc{};
                }
                if (!sdc.streams.empty()) {
                    multiplex_->streams = sdc.streams;
                    multiplex_->protection_a = sdc.protection_a;
                    multiplex_->protection_b = sdc.protection_b;
                }
                if (!sdc.audio.empty()) {
                    multiplex_->audio = sdc.audio;
                }
                if (!sdc.applications.empty()) {
                    multiplex_->applications = sdc.applications;
                }
            } else {
                ++stats_.sdc_bad;
            }
            out.sdc.push_back(std::move(sdc));
        }
    }
    // MSC, ES 7.7: three multiplex frames in order, dummy cells after.
    const std::size_t n_mux = st.n_mux();
    for (std::size_t m = 0; m < 3; ++m) {
        std::vector<Complex32> cells(n_mux);
        std::vector<float> w(n_mux);
        for (std::size_t i = 0; i < n_mux; ++i) {
            const std::uint32_t idx = st.msc[m * n_mux + i];
            cells[i] = sf_cells_[idx];
            w[i] = sf_weights_[idx];
        }
        mux_history_.emplace_back(std::move(cells), std::move(w));
        decode_msc(out);
    }
}

void DrmReceiver::decode_msc(DrmOutput& out) {
    const std::size_t depth = channel_->short_interleave ? 1 : kLongDepth;
    while (mux_history_.size() > depth) {
        mux_history_.pop_front();
    }
    if (mux_history_.size() < depth) {
        return;
    }
    const std::size_t n_mux = mux_history_.front().first.size();
    // ES 7.6: received z^_n,i = z_{n - (i mod D)}, Pi(i). The oldest held
    // frame is complete once D frames are held.
    const auto perm = drm_interleaver(n_mux, 5);
    std::vector<Complex32> z(n_mux);
    std::vector<float> w(n_mux);
    for (std::size_t i = 0; i < n_mux; ++i) {
        const auto& src = mux_history_[depth == 1 ? 0 : i % depth];
        z[perm[i]] = src.first[i];
        w[perm[i]] = src.second[i];
    }
    mux_history_.pop_front();
    if (depth > 1) {
        // The frame popped is complete; the remaining ones still wait.
    }
    ++mux_count_;
    if (!multiplex_ || multiplex_->streams.empty() || !multiplex_->protection_b) {
        return;
    }
    const DrmQam qam = channel_->msc_mode == 3 ? DrmQam::Qam16 : DrmQam::Qam64;
    std::size_t part_a = 0;
    for (const auto& s : multiplex_->streams) {
        part_a += s.part_a;
    }
    auto plan = drm_msc_plan(qam, multiplex_->protection_a.value_or(0), *multiplex_->protection_b,
                             part_a, n_mux);
    if (!plan) {
        return;
    }
    auto bits = drm_mlc_decode(*plan, z, w);
    if (!bits) {
        return;
    }
    descramble(*bits);
    ++stats_.multiplex_frames;
    deliver(*bits, out);
}

void DrmReceiver::deliver(std::span<const std::uint8_t> mux_bits, DrmOutput& out) {
    const auto bytes = to_bytes(mux_bits);
    const auto& streams = multiplex_->streams;
    std::size_t part_a_total = 0;
    for (const auto& s : streams) {
        part_a_total += s.part_a;
    }
    std::size_t off_a = 0;
    std::size_t off_b = part_a_total;
    if (text_.size() < streams.size()) {
        text_.resize(streams.size());
    }
    if (units_.size() < streams.size() * 4) {
        units_.resize(streams.size() * 4);
    }
    for (std::size_t s = 0; s < streams.size(); ++s) {
        DrmStreamFrame frame;
        frame.multiplex_frame = mux_count_ - 1;
        frame.stream = static_cast<std::uint8_t>(s);
        if (off_a + streams[s].part_a > bytes.size() || off_b + streams[s].part_b > bytes.size()) {
            return;
        }
        frame.bytes.insert(frame.bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(off_a),
                           bytes.begin() + static_cast<std::ptrdiff_t>(off_a + streams[s].part_a));
        frame.bytes.insert(frame.bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(off_b),
                           bytes.begin() + static_cast<std::ptrdiff_t>(off_b + streams[s].part_b));
        off_a += streams[s].part_a;
        off_b += streams[s].part_b;

        // ES 6.5: an audio stream's text message rides in the last four bytes
        // of its lower protected part.
        for (const auto& ai : multiplex_->audio) {
            if (ai.stream_id == s && ai.text && streams[s].part_b >= 4) {
                std::array<std::uint8_t, 4> piece{};
                std::copy_n(frame.bytes.end() - 4, 4, piece.begin());
                if (auto msg = text_[s].push(static_cast<std::uint8_t>(s), piece)) {
                    out.texts.push_back(std::move(*msg));
                }
            }
        }
        // ES 6.6: packet mode.
        for (const auto& app : multiplex_->applications) {
            if (app.stream_id != s || !app.packet_mode || app.packet_length == 0) {
                continue;
            }
            const std::size_t plen = static_cast<std::size_t>(app.packet_length) + 3;
            for (std::size_t at = 0; at + plen <= frame.bytes.size(); at += plen) {
                std::span<const std::uint8_t> raw(frame.bytes.data() + at, plen);
                DrmPacket pk;
                pk.stream = static_cast<std::uint8_t>(s);
                pk.first = (raw[0] & 0x80U) != 0;
                pk.last = (raw[0] & 0x40U) != 0;
                pk.packet_id = static_cast<std::uint8_t>((raw[0] >> 4U) & 3U);
                pk.continuity = static_cast<std::uint8_t>(raw[0] & 7U);
                std::vector<std::uint8_t> crc_bits;
                for (std::size_t i = 0; i + 2 < plen; ++i) {
                    for (int b = 7; b >= 0; --b) {
                        crc_bits.push_back(static_cast<std::uint8_t>((raw[i] >> b) & 1U));
                    }
                }
                const std::uint32_t crc = (static_cast<std::uint32_t>(raw[plen - 2]) << 8U) | raw[plen - 1];
                pk.crc_ok = crc == drm_crc(crc_bits, 16, kDrmCrc16Poly);
                if ((raw[0] & 0x08U) != 0) {
                    const std::size_t useful = std::min<std::size_t>(raw[1], plen - 4);
                    pk.data.assign(raw.begin() + 2, raw.begin() + 2 + static_cast<std::ptrdiff_t>(useful));
                } else {
                    pk.data.assign(raw.begin() + 1, raw.end() - 2);
                }
                if (pk.packet_id == app.packet_id && app.data_units && pk.crc_ok && !pk.data.empty()) {
                    auto& unit = units_[s * 4 + pk.packet_id];
                    if (pk.first) {
                        unit.clear();
                    }
                    unit.insert(unit.end(), pk.data.begin(), pk.data.end());
                    if (pk.last) {
                        out.data_units.push_back({static_cast<std::uint8_t>(s), pk.packet_id, unit});
                        unit.clear();
                    }
                }
                out.packets.push_back(std::move(pk));
            }
        }
        out.streams.push_back(std::move(frame));
    }
}

void DrmReceiver::trim() {
    std::int64_t keep = next_attempt_;
    if (sync_.locked) {
        keep = symbol0_;
        if (frame_locked_) {
            keep = symbol0_ + static_cast<std::int64_t>(next_symbol_ * (params_.tu + params_.tg));
        }
    }
    keep = std::min(keep, total_);
    if (keep <= buffer_start_) {
        return;
    }
    const auto drop = static_cast<std::size_t>(keep - buffer_start_);
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
    buffer_start_ = keep;
}

}  // namespace revenant::decode
