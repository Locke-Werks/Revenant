// The receiver front end shared by the slow slotted MFSK modes: WSPR, JT9,
// JT4 and JT65. Each of them sends one tone per symbol, phase continuous,
// for a minute or two, and marks its timing with a known pattern of tones.
// What differs between them is described by a SlowProtocol, filled in by the
// mode's own file from its own document; nothing here comes from a mode
// document.
//
// WHAT IS DONE, AND WHY
//
// None of the four documents describes a receiver, so every step below is
// ours, from textbook non-coherent MFSK detection, and every constant that
// is not a property of the waveform is marked NOT SPECIFIED where it is set.
//
//  1. A spectrogram of the slot with one-symbol rectangular windows, a
//     quarter symbol apart, on a frequency grid of about half a tone
//     spacing. Each bin is divided by a local noise floor, the median over
//     neighbouring bins of the per-bin mean.
//  2. Every tone-0 frequency in the search band and every start time in the
//     protocol's window is scored by the sync pattern: the mean normalised
//     power in the tones the pattern says carry energy, less the mean in the
//     tones it says do not. Noise scores zero on average. The best start
//     time and coarse drift per frequency, then local maxima in frequency,
//     become candidates.
//  3. Each candidate is cut out of one FFT of the whole slot as complex
//     baseband a few tens of hertz wide, and its start time, frequency and
//     a linear frequency drift across the transmission are refined on a
//     grid by the same score, now from exact per-symbol DFTs.
//  4. The refined candidate yields the power in every tone of every symbol,
//     divided by the noise per DFT bin. The mode turns that into whatever its
//     decoder wants.

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "core/decode/wsjt_dsp.h"
#include "core/error.h"

namespace revenant::decode {

struct SlowProtocol {
    const char* name = "";
    int symbols = 0;
    // Every tone the waveform can use, sync tones included.
    int tones = 0;
    double symbol_seconds = 0.0;
    double tone_spacing_hz = 0.0;
    double slot_seconds = 60.0;
    // Where a transmission nominally starts in its slot.
    double nominal_start_seconds = 1.0;
    // Start times searched, relative to the slot boundary.
    double start_min_seconds = 0.0;
    double start_max_seconds = 3.0;
    // Per symbol: tones that carry energy when the signal is there, and tones
    // that do not. Either may be empty.
    std::vector<std::vector<int>> sync_on;
    std::vector<std::vector<int>> sync_off;
};

// Phase-continuous MFSK at `rate`, tone 0 at 0 Hz, as an analytic signal of
// unit amplitude. `drift_hz` is a linear frequency change from the start to
// the end of the transmission, centred on the nominal frequency.
[[nodiscard]] std::vector<std::complex<float>> slow_reference(const SlowProtocol& p,
                                                              std::span<const int> tones,
                                                              SampleRate rate, double drift_hz = 0.0);

struct SlowCandidate {
    double frequency_hz = 0.0;  // tone 0, at the middle of the transmission
    double start_seconds = 0.0; // first symbol, from the slot boundary
    double drift_hz = 0.0;
    double sync = 0.0;
};

struct SlowSpectra {
    SlowCandidate where;
    // power[s * tones + k]: tone k in symbol s, divided by the noise in one
    // DFT bin of one symbol.
    std::vector<float> power;
    int tones = 0;

    [[nodiscard]] float at(int symbol, int tone) const {
        return power[static_cast<std::size_t>(symbol) * static_cast<std::size_t>(tones) +
                     static_cast<std::size_t>(tone)];
    }
};

struct SlowSearch {
    double low_hz = 200.0;   // lowest tone-0 frequency
    double high_hz = 3000.0; // highest tone-0 frequency
    // NOT SPECIFIED. Candidates kept, strongest first.
    int max_candidates = 60;
    // NOT SPECIFIED. Coarse sync score, in units of the noise per bin.
    double sync_threshold = 0.12;
    // NOT SPECIFIED. Largest drift searched, Hz over the transmission.
    double max_drift_hz = 0.0;
};

// One slot of 12 kHz audio, analysed lazily.
class SlowSlot {
public:
    SlowSlot(const SlowProtocol& protocol, std::span<const float> slot_12k);

    [[nodiscard]] std::vector<SlowCandidate> candidates(const SlowSearch& search);

    // Refines a candidate and measures every tone of every symbol.
    [[nodiscard]] SlowSpectra demodulate(const SlowCandidate& c, const SlowSearch& search);

private:
    struct Baseband {
        std::vector<std::complex<double>> x;
        double rate = 0.0;
        double centre_hz = 0.0;
    };
    Baseband baseband(double low_hz, double high_hz);
    [[nodiscard]] double score(const Baseband& b, double start, double freq, double drift) const;
    [[nodiscard]] double noise_per_bin(const Baseband& b) const;
    std::complex<double> tone_dft(const Baseband& b, int symbol, double start, double freq) const;

    const SlowProtocol& p_;
    std::span<const float> audio_;
    std::vector<std::complex<double>> spectrum_;  // FFT of the whole slot, filled on demand
};

// Signal to noise ratio in 2500 Hz from the mean normalised power in the
// tones that were sent: the WSJT convention. Power per DFT bin is the noise
// in 1/T Hz, so the bin SNR is scaled by T times 2500.
[[nodiscard]] double slow_snr_2500(const SlowProtocol& p, const SlowSpectra& s,
                                   std::span<const int> sent_tones);

// ln I0(x), the zeroth modified Bessel function, for the non-coherent
// likelihood of a tone's power.
[[nodiscard]] double slow_log_i0(double x);

// Signal amplitude per symbol, in the units of SlowSpectra, from the mean
// power of the strongest tone per symbol. Never below a small floor.
[[nodiscard]] double slow_amplitude(const SlowSpectra& s, int first_tone, int last_tone);

// Cuts a stream into slots of `slot_seconds` aligned to UTC, as FtDecoder
// does, and hands each over once it is complete.
class SlowStream {
public:
    using SlotFn = std::function<void(std::span<const float> slot_12k, double utc,
                                      SampleIndex start_input_sample)>;

    [[nodiscard]] static Expected<SlowStream> create(double slot_seconds, SampleRate rate,
                                                     WsjtUtcOfSample utc_of_sample);

    void process(ConstRealSpan audio, const SlotFn& fn);

private:
    SlowStream(double slot_seconds, WsjtInputStage input, WsjtUtcOfSample utc, SampleRate rate);

    double slot_seconds_;
    WsjtInputStage input_;
    WsjtSlotClock clock_;
    std::vector<float> scratch_;
    std::vector<float> buffer_;
    std::int64_t buffer_start_ = 0;
    std::int64_t received_ = 0;
    std::int64_t next_slot_ = 0;
    bool started_ = false;
};

// The K = 32, r = 1/2 convolutional code and 8-bit bit-reversal interleaver
// that G4JNT describes for WSPR, and identically for JT4 and JT9: "The WSPR
// Coding Process" (2009), sections "Convolutional Encoding" and
// "Interleaving". Bits go in most significant first; the input already
// carries its 31 zero tail bits.
inline constexpr std::uint32_t kWsjtConvG0 = 0xF2D05351U;
inline constexpr std::uint32_t kWsjtConvG1 = 0xE4613C47U;
[[nodiscard]] std::vector<std::uint8_t> wsjt_conv_encode(std::span<const std::uint8_t> bits);
// D[J] = S[P] for the P-th 8-bit address I whose bit reversal J is below n.
[[nodiscard]] std::vector<std::uint8_t> wsjt_interleave(std::span<const std::uint8_t> s);
// The inverse, for soft values: S[P] = D[J].
[[nodiscard]] std::vector<float> wsjt_deinterleave(std::span<const float> d);

}  // namespace revenant::decode
