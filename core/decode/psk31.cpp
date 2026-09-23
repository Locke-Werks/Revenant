#include "core/decode/psk31.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/dv_codes.h"

namespace revenant::decode {
namespace {

constexpr double kPi = std::numbers::pi;

// Samples per symbol the front end is asked for. The square-law timing
// estimator in core/decode/dv_phy.h needs two, the cubic interpolator it
// reads through wants several more, and sixteen puts the decimated rate at
// 500 Hz for PSK31, which divides every common audio rate.
constexpr double kSamplesPerSymbol = 16.0;

// Symbols per timing estimate, passed to SymbolSync. Its own default and for
// its own reason; see SymbolSyncConfig::window_symbols.
constexpr std::size_t kTimingWindowSymbols = 32;

// Leak of the fine AFC's average, per symbol. One thirty-second follows a
// drift of a fraction of a hertz per second at PSK31's symbol rate, and
// averages the residual over about a second of symbols, which is what keeps
// its own noise well under the 45 degree margin a QPSK decision has.
constexpr double kResidualLeak = 1.0 / 32.0;

// The coarse estimate has to beat the noise by this much before it is
// trusted. Measured on 2026-09-22: over twenty seconds of white audio the
// tallest line any window produced stood 3.86 times the mean of its scan for
// PSK31 and 3.56 for PSK63, which is about what the maximum of a couple of
// thousand Rayleigh samples does. A PSK31 idle preamble at -8 dB in 2500 Hz
// stands at 10.1, and at -16 dB, where the text is no longer readable
// anyway, at 6.2. Six sits between the two populations.
constexpr double kAcquisitionLineRatio = 6.0;

// Bits the QPSK Viterbi decoder commits per run, beyond its decision delay.
constexpr std::size_t kViterbiCommitBits = 32;

// Soft value used to pin the trellis to an already committed state. Large
// against the normalised soft values, which sit near one.
constexpr float kPinnedSoft = 1000.0F;

// Bits of history kept to place a character at its first bit.
constexpr std::size_t kBitSampleRing = 64;

unsigned modulation_order(Psk31Mode mode) {
    return mode == Psk31Mode::Qpsk31 ? 4U : 2U;
}

}  // namespace

double psk31_symbol_rate(Psk31Mode mode) {
    switch (mode) {
        case Psk31Mode::Bpsk31: return kPsk31SymbolRate;
        case Psk31Mode::Bpsk63: return kPsk63SymbolRate;
        case Psk31Mode::Qpsk31: return kPsk31SymbolRate;
    }
    return kPsk31SymbolRate;
}

std::uint8_t qpsk31_shift_from_generators(std::uint32_t run_of_five) {
    const auto in_phase = static_cast<unsigned>(std::popcount(run_of_five & kQpsk31Generators[0]) & 1);
    const auto quadrature =
        static_cast<unsigned>(std::popcount(run_of_five & kQpsk31Generators[1]) & 1);
    // The shift advanced by 45 degrees lands in the quadrant whose signs are
    // the two outputs: (+,+) is 45 so no shift, (-,+) is 135 so an advance,
    // (-,-) is 225 so a reversal, (+,-) is 315 so a retard.
    if (in_phase != 0U) {
        return quadrature != 0U ? kPskShiftNone : kPskShiftRetard;
    }
    return quadrature != 0U ? kPskShiftAdvance : kPskShiftReverse;
}

Expected<Psk31> Psk31::create(const Psk31Config& config) {
    if (config.rate <= 0) {
        return fail(std::format("Psk31 needs a positive audio rate; got {}", config.rate));
    }
    if (!(config.capture_hz > 0.0)) {
        return fail(std::format("Psk31 needs a positive capture range; got {}",
                                config.capture_hz));
    }
    if (config.acquisition_symbols < 8) {
        return fail(std::format(
            "Psk31 needs at least 8 symbols to estimate the carrier from; got {}",
            config.acquisition_symbols));
    }

    Psk31 decoder;
    decoder.config_ = config;
    decoder.symbol_rate_ = psk31_symbol_rate(config.mode);

    // The pulse is a Hann window two symbols long, whose main lobe reaches
    // one symbol rate either side of the carrier. Keep that at the worst
    // tuning error the capture range allows.
    ToneFrontEndConfig front;
    front.rate = config.rate;
    front.centre_hz = config.centre_hz;
    front.passband_hz = config.capture_hz + decoder.symbol_rate_;
    front.minimum_output_rate =
        static_cast<SampleRate>(std::ceil(kSamplesPerSymbol * decoder.symbol_rate_));
    auto made = ToneFrontEnd::create(front);
    if (!made) {
        return std::unexpected(with_context(made.error(), "creating the PSK31 front end"));
    }
    decoder.front_ = std::move(*made);

    const double out_rate = static_cast<double>(decoder.front_.output_rate());
    const double reach = config.capture_hz * modulation_order(config.mode);
    if (reach >= 0.5 * out_rate) {
        return fail(std::format(
            "Psk31 cannot capture +/- {} Hz in this mode: the carrier search raises the "
            "signal to the power {} and would look for a line at {} Hz, past the {} Hz "
            "Nyquist frequency of the decimated stream",
            config.capture_hz, modulation_order(config.mode), reach, 0.5 * out_rate));
    }

    // The matched filter is the transmitted pulse: a raised cosine from one
    // symbol centre to the next, (1 + cos(pi*t/T))/2 for |t| < T.
    const double samples_per_symbol = out_rate / decoder.symbol_rate_;
    const auto half = static_cast<std::size_t>(std::floor(samples_per_symbol));
    decoder.matched_.assign(2 * half + 1, 0.0F);
    double sum = 0.0;
    for (std::size_t i = 0; i < decoder.matched_.size(); ++i) {
        const double t = (static_cast<double>(i) - static_cast<double>(half)) / out_rate;
        const double value = 0.5 * (1.0 + std::cos(kPi * t * decoder.symbol_rate_));
        decoder.matched_[i] = static_cast<float>(value);
        sum += value;
    }
    for (float& tap : decoder.matched_) {
        tap = static_cast<float>(static_cast<double>(tap) / sum);
    }

    SymbolSyncConfig sync;
    sync.rate = decoder.front_.output_rate();
    sync.symbol_rate = decoder.symbol_rate_;
    sync.window_symbols = kTimingWindowSymbols;
    auto timing = SymbolSync::create(sync);
    if (!timing) {
        return std::unexpected(with_context(timing.error(), "creating the PSK31 timing"));
    }
    decoder.sync_ = std::move(*timing);
    decoder.bit_samples_ring_.assign(kBitSampleRing, 0);
    return decoder;
}

void Psk31::reset() {
    front_.reset();
    sync_.reset();
    acquisition_.clear();
    acquired_ = false;
    coarse_offset_hz_ = 0.0;
    acquisition_strength_ = 0.0;
    strongest_rejected_ = 0.0;
    corrected_index_ = 0;
    sync_origin_ = 0;
    matched_history_.clear();
    have_previous_ = false;
    residual_ = {0.0, 0.0};
    level_ = 0.0;
    pending_soft_.clear();
    pending_samples_.clear();
    committed_tail_ = {};
    varicode_.reset();
    bit_count_ = 0;
    std::fill(bit_samples_ring_.begin(), bit_samples_ring_.end(), SampleIndex{0});
    last_bits_.clear();
    last_bit_samples_.clear();
}

double Psk31::frequency_offset_hz() const {
    const double order = modulation_order(config_.mode);
    double fine = 0.0;
    if (std::abs(residual_) > 0.0) {
        fine = std::arg(residual_) / order * symbol_rate_ / (2.0 * kPi);
    }
    return coarse_offset_hz_ + (config_.lower_sideband ? -fine : fine);
}

Status Psk31::process(ConstRealSpan audio, std::vector<Psk31Character>& out) {
    last_bits_.clear();
    last_bit_samples_.clear();

    decimated_.clear();
    front_.process(audio, decimated_);

    if (acquired_) {
        run_symbols(decimated_, out);
        return {};
    }

    acquisition_.insert(acquisition_.end(), decimated_.begin(), decimated_.end());
    const double samples_per_symbol =
        static_cast<double>(front_.output_rate()) / symbol_rate_;
    const auto needed = static_cast<std::size_t>(
        std::ceil(static_cast<double>(config_.acquisition_symbols) * samples_per_symbol));

    while (!acquired_ && acquisition_.size() >= needed) {
        const std::span<const Complex32> window(acquisition_.data(), needed);

        // Squaring strips binary modulation, so it finds the carrier of
        // BPSK, and of QPSK's idle too: QEX page 9's closing note is that a
        // run of zeros gives "continuous reversals, the same as BPSK". The
        // fourth power strips QPSK data as well, at a heavier noise penalty,
        // so for QPSK both are tried and the clearer line wins. The idle
        // preamble is where acquisition normally happens, and there the square
        // is several decibels the better estimator.
        auto estimate = estimate_tone_offset(window, front_.output_rate(), 2, config_.capture_hz);
        if (!estimate) {
            return std::unexpected(
                with_context(estimate.error(), "estimating the PSK31 carrier"));
        }
        if (config_.mode == Psk31Mode::Qpsk31) {
            auto fourth = estimate_tone_offset(window, front_.output_rate(), 4,
                                               config_.capture_hz);
            if (!fourth) {
                return std::unexpected(
                    with_context(fourth.error(), "estimating the QPSK31 carrier"));
            }
            if (fourth->line_to_mean > estimate->line_to_mean) {
                estimate = fourth;
            }
        }
        if (estimate->line_to_mean < kAcquisitionLineRatio) {
            strongest_rejected_ = std::max(strongest_rejected_, estimate->line_to_mean);
            // No signal yet, or not enough of one. Slide by a quarter of a
            // window and look again, rather than decoding noise at a
            // frequency picked out of it.
            const std::size_t slide = needed / 4;
            acquisition_.erase(acquisition_.begin(),
                               acquisition_.begin() + static_cast<std::ptrdiff_t>(slide));
            corrected_index_ += slide;
            continue;
        }
        coarse_offset_hz_ = estimate->offset_hz;
        acquisition_strength_ = estimate->line_to_mean;
        acquired_ = true;
        sync_origin_ = corrected_index_;
    }

    if (acquired_) {
        std::vector<Complex32> held;
        held.swap(acquisition_);
        run_symbols(held, out);
    }
    return {};
}

void Psk31::run_symbols(std::span<const Complex32> baseband, std::vector<Psk31Character>& out) {
    const double out_rate = static_cast<double>(front_.output_rate());
    const std::size_t taps = matched_.size();
    const std::size_t matched_delay = (taps - 1) / 2;

    corrected_.clear();
    corrected_.reserve(baseband.size());
    for (const Complex32 sample : baseband) {
        // Coarse correction by the acquired offset, with the phase taken from
        // the absolute decimated index so the blocking does not matter.
        const double cycles =
            std::fmod(coarse_offset_hz_ * static_cast<double>(corrected_index_) / out_rate, 1.0);
        const std::complex<double> rotation = std::polar(1.0, -2.0 * kPi * cycles);
        const std::complex<double> value =
            std::complex<double>{sample.real(), sample.imag()} * rotation;

        matched_history_.push_back(
            Complex32{static_cast<float>(value.real()), static_cast<float>(value.imag())});
        if (matched_history_.size() > taps) {
            matched_history_.erase(matched_history_.begin());
        }
        double real = 0.0;
        double imag = 0.0;
        const std::size_t have = matched_history_.size();
        for (std::size_t i = 0; i < have; ++i) {
            const Complex32 x = matched_history_[have - 1 - i];
            real += static_cast<double>(matched_[i]) * x.real();
            imag += static_cast<double>(matched_[i]) * x.imag();
        }
        corrected_.push_back(Complex32{static_cast<float>(real), static_cast<float>(imag)});
        ++corrected_index_;
    }

    recovered_.clear();
    sync_.process(corrected_, recovered_);

    // SymbolSync counts positions from the first sample it was given, which
    // is the first sample after any acquisition windows that were discarded.
    const double origin = static_cast<double>(sync_origin_);
    for (const RecoveredSymbol& symbol : recovered_) {
        const double decimated = origin + symbol.position - static_cast<double>(matched_delay);
        const double input = decimated * static_cast<double>(front_.decimation()) -
                             static_cast<double>(front_.group_delay());
        const SampleIndex sample =
            input > 0.0 ? static_cast<SampleIndex>(std::llround(input)) : SampleIndex{0};

        if (have_previous_) {
            Complex32 difference = symbol.value * std::conj(previous_symbol_);
            if (config_.lower_sideband) {
                difference = std::conj(difference);
            }
            decide(difference, sample, out);
        }
        previous_symbol_ = symbol.value;
        have_previous_ = true;
    }
}

void Psk31::decide(Complex32 difference, SampleIndex sample, std::vector<Psk31Character>& out) {
    const unsigned order = modulation_order(config_.mode);
    const std::complex<double> d{difference.real(), difference.imag()};
    const double magnitude = std::abs(d);

    // Fine AFC. A residual frequency error rotates every differential phasor
    // by the same angle, and raising it to the modulation order removes the
    // data, so the angle of the averaged power is the order times that
    // rotation. Weighted by |d| rather than |d|^order so a noise spike does
    // not outvote a run of good symbols.
    if (magnitude > 0.0) {
        std::complex<double> raised{1.0, 0.0};
        for (unsigned i = 0; i < order; ++i) {
            raised *= d / magnitude;
        }
        residual_ = (1.0 - kResidualLeak) * residual_ + kResidualLeak * magnitude * raised;
        level_ = (level_ == 0.0) ? magnitude : (1.0 - kResidualLeak) * level_ +
                                                   kResidualLeak * magnitude;
    }
    double rotation = 0.0;
    if (std::abs(residual_) > 0.0) {
        rotation = std::arg(residual_) / static_cast<double>(order);
    }
    const std::complex<double> corrected = d * std::polar(1.0, -rotation);

    if (config_.mode != Psk31Mode::Qpsk31) {
        // QEX page 6: a steady carrier is a 1 and a reversal a 0.
        emit_bit(corrected.real() > 0.0 ? 1U : 0U, sample, out);
        return;
    }

    // The derivation in core/decode/psk31.h: advanced by 45 degrees, the
    // in-phase and quadrature signs are the two encoder outputs, positive
    // meaning 1. The Viterbi decoder's soft convention is positive for 0.
    const std::complex<double> rotated = corrected * std::polar(1.0, kPi / 4.0);
    const double scale = level_ > 0.0 ? 1.0 / level_ : 1.0;
    pending_soft_.push_back(static_cast<float>(-rotated.real() * scale));
    pending_soft_.push_back(static_cast<float>(-rotated.imag() * scale));
    pending_samples_.push_back(sample);

    if (pending_samples_.size() >= config_.decision_delay_bits + kViterbiCommitBits) {
        run_viterbi(false, out);
    }
}

void Psk31::run_viterbi(bool flush, std::vector<Psk31Character>& out) {
    const std::size_t pending = pending_samples_.size();
    if (pending == 0) {
        return;
    }

    // dv_codes' Viterbi decoder starts every path from state zero. The
    // encoder's real state at the start of this window is the last four bits
    // already committed, so four steps are prefixed whose soft values are
    // what encoding those four bits from state zero emits, held at a strength
    // no noise can argue with. The trellis is then forced through exactly the
    // committed state at the window start, and the bits it decodes for the
    // prefix are the committed ones and are discarded.
    ConvolutionalCode code;
    code.memory = kQpsk31Memory;
    code.generators = kQpsk31Generators;
    auto prefix = convolutional_encode(code, committed_tail_);
    if (!prefix) {
        return;
    }
    std::vector<float> soft;
    soft.reserve(prefix->size() + pending_soft_.size());
    for (const std::uint8_t bit : *prefix) {
        soft.push_back(bit != 0U ? -kPinnedSoft : kPinnedSoft);
    }
    soft.insert(soft.end(), pending_soft_.begin(), pending_soft_.end());

    auto decoded = viterbi_decode(code, soft, false);
    if (!decoded) {
        return;
    }

    const std::size_t commit =
        flush ? pending
              : (pending > config_.decision_delay_bits ? pending - config_.decision_delay_bits
                                                       : 0);
    for (std::size_t i = 0; i < commit; ++i) {
        const std::uint8_t bit = (*decoded)[committed_tail_.size() + i];
        emit_bit(bit, pending_samples_[i], out);
        committed_tail_[0] = committed_tail_[1];
        committed_tail_[1] = committed_tail_[2];
        committed_tail_[2] = committed_tail_[3];
        committed_tail_[3] = bit;
    }
    pending_soft_.erase(pending_soft_.begin(),
                        pending_soft_.begin() + static_cast<std::ptrdiff_t>(2 * commit));
    pending_samples_.erase(pending_samples_.begin(),
                           pending_samples_.begin() + static_cast<std::ptrdiff_t>(commit));
}

void Psk31::emit_bit(std::uint8_t bit, SampleIndex sample, std::vector<Psk31Character>& out) {
    last_bits_.push_back(bit);
    last_bit_samples_.push_back(sample);
    bit_samples_ring_[bit_count_ % kBitSampleRing] = sample;

    characters_.clear();
    varicode_.push(bit, bit_count_, characters_);
    for (const VaricodeCharacter& character : characters_) {
        Psk31Character decoded;
        decoded.ascii = character.ascii;
        decoded.recognised = character.recognised;
        const std::uint64_t oldest =
            bit_count_ + 1 > kBitSampleRing ? bit_count_ + 1 - kBitSampleRing : 0;
        const std::uint64_t first = std::max(character.first_bit, oldest);
        decoded.first_sample = bit_samples_ring_[first % kBitSampleRing];
        out.push_back(decoded);
    }
    ++bit_count_;
}

}  // namespace revenant::decode
