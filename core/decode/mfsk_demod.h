// M-ary frequency shift keying from receiver audio: a non-coherent tone
// bank, symbol timing, frequency tracking, the incremental-frequency-keying
// map, and a raster view of the same bank.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE MODES
//
// core/decode/fsk.h is the two-tone case with a sign at the bottom of it.
// Every multi-tone audio mode (the MFSK family, the IFK modes, FSQ, and the
// raster modes such as Hell and Throb) does the same thing with more tones:
// for each symbol period, measure the energy at each of M equally spaced
// frequencies and say which one carried it. Only M, the spacing, the symbol
// rate and the framing above differ, and those are parameters.
//
// WHAT IS HERE
//
//   - MfskDemodulator: per-symbol energies at M tones (soft, one value per
//     tone), a hard decision, symbol timing found by maximising the share of
//     window energy in the strongest tone over sub-symbol offsets, and an AFC
//     loop that follows a frequency offset and slow drift.
//   - ifk_*: the incremental frequency keying map, where the data is the
//     tone step from one symbol to the next rather than the tone itself.
//   - MfskRaster: the same tone bank on a fixed clock with no decision, as a
//     time-frequency matrix for the raster modes.
//
// WHAT IS NOT HERE
//
// No constant in this file comes from any standard. Non-coherent orthogonal
// MFSK detection is textbook (Proakis, Digital Communications, the
// non-coherent detection of orthogonal signals chapter), as are the
// early/late energy timing criterion and a phase-difference frequency
// discriminator. The numbers with citations belong to a mode file: M, the
// spacing, the symbol rate and the IFK offset.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it, like the rest of core/decode. The
// round trip through core/dsp/synth/mfsk_mod.h stands in for the twin.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ConstRealSpan;
using dsp::SampleRate;

inline constexpr int kMfskMinTones = 2;
inline constexpr int kMfskMaxTones = 256;

struct MfskDemodConfig {
    SampleRate rate = 8000;

    int tones = 16;

    // Fractional on purpose: several MFSK modes space their tones at a
    // symbol rate that is not a whole number of hertz.
    double tone_spacing_hz = 15.625;
    double symbol_rate = 15.625;

    // Frequency of tone 0, the lowest. The starting estimate when search is
    // off, ignored when it is on.
    double base_hz = 1000.0;

    // Look for the signal anywhere whose M tones fit between these two edges,
    // during acquisition. An offset of a whole tone spacing cannot be told
    // apart from a different symbol without a preamble, so with search off
    // the starting estimate must be within half a spacing.
    bool search = false;
    double search_low_hz = 300.0;
    double search_high_hz = 2700.0;

    // Symbols buffered before the first decision, over which the frequency
    // search, the initial timing and the initial frequency correction are
    // averaged. Every symbol in that window is still emitted.
    int acquisition_symbols = 32;

    // Window offsets per symbol period examined for timing, spread evenly
    // over one period.
    int timing_offsets = 8;
    bool track_timing = true;
    // Exponential average of the per-offset energy share, per symbol.
    double timing_average = 0.1;
    // Fraction of the averaged peak's offset applied per symbol.
    double timing_gain = 0.05;

    bool afc = true;
    // Fraction of each symbol's measured frequency error applied.
    double afc_gain = 0.05;
    // A symbol whose strongest tone holds less than this share of the window
    // energy does not steer the AFC: its decision is too likely wrong.
    double afc_min_confidence = 0.5;
};

struct MfskSymbol {
    // Absolute sample index, from the first sample handed to process, of the
    // first sample of the window this symbol was measured over.
    std::int64_t start = 0;

    // Index of the strongest tone, 0 being the lowest frequency.
    int tone = 0;

    // Share of the window's tone energy in the strongest tone, 1/M for noise
    // alone, near 1 for a clean symbol.
    float confidence = 0.0F;

    // The frequency of tone 0 this symbol was measured against, AFC included.
    double tone0_hz = 0.0;

    // One value per tone. Scaled so that a sinusoid of amplitude A exactly on
    // a tone and filling the window reads A squared.
    std::vector<float> energies;
};

class MfskDemodulator {
public:
    [[nodiscard]] static Expected<MfskDemodulator> create(const MfskDemodConfig& config);

    // Appends every symbol whose window is now complete. The output depends
    // only on the concatenated input, never on how it was split into calls.
    void process(ConstRealSpan audio, std::vector<MfskSymbol>& out);

    void reset();

    [[nodiscard]] bool acquired() const { return acquired_; }
    [[nodiscard]] double tone0_hz() const { return tone0_hz_; }
    [[nodiscard]] double samples_per_symbol() const { return samples_per_symbol_; }
    [[nodiscard]] const MfskDemodConfig& config() const { return config_; }

private:
    explicit MfskDemodulator(const MfskDemodConfig& config);

    // Tone energies for the window starting at absolute sample index start.
    // Samples outside what has been received read as zero.
    void energies_at(std::int64_t start, double tone0_hz, std::vector<double>& out) const;
    [[nodiscard]] double share_at(std::int64_t start) const;
    [[nodiscard]] double frequency_error(std::int64_t start, double tone_hz) const;
    [[nodiscard]] float sample(std::int64_t index) const;

    void acquire();
    void emit(std::vector<MfskSymbol>& out);
    void trim();

    MfskDemodConfig config_;
    double samples_per_symbol_ = 0.0;
    std::size_t window_ = 0;

    std::vector<float> buffer_;
    std::int64_t buffer_start_ = 0;
    std::int64_t received_ = 0;

    bool acquired_ = false;
    double next_start_ = 0.0;
    double tone0_hz_ = 0.0;
    std::vector<double> timing_curve_;
    mutable std::vector<float> scratch_;
};

// ---------------------------------------------------------------------------
// Incremental frequency keying
// ---------------------------------------------------------------------------
//
// In IFK the transmitted tone is the previous tone plus the data symbol plus a
// fixed offset, modulo M. The offset keeps a repeated symbol from repeating
// the tone, so every symbol carries a transition, and a frequency error moves
// every tone alike and drops out of the difference. The offset is the mode's
// to cite.

[[nodiscard]] int ifk_next_tone(int previous_tone, int symbol, int tones, int offset);
[[nodiscard]] int ifk_symbol(int previous_tone, int tone, int tones, int offset);

// Symbols to tones and back, each element relative to the one before it and
// the first relative to initial_tone. The two are exact inverses.
[[nodiscard]] std::vector<int> ifk_encode(std::span<const int> symbols, int tones, int offset,
                                          int initial_tone = 0);
[[nodiscard]] std::vector<int> ifk_decode(std::span<const int> tone_sequence, int tones,
                                          int offset, int initial_tone = 0);

// Soft IFK: re-indexes one symbol's tone energies by data symbol given the
// previous tone, so out[s] is the energy at the tone that symbol s would have
// produced.
[[nodiscard]] std::vector<float> ifk_soft(std::span<const float> energies, int previous_tone,
                                          int offset);

// ---------------------------------------------------------------------------
// Raster
// ---------------------------------------------------------------------------

struct MfskRasterConfig {
    SampleRate rate = 8000;
    // Rows per second. Each row is measured over one row period.
    double rows_per_second = 15.625;
    double low_hz = 300.0;
    double bin_spacing_hz = 15.625;
    int bins = 160;
};

class MfskRaster {
public:
    [[nodiscard]] static Expected<MfskRaster> create(const MfskRasterConfig& config);

    // Appends one row of bins energies per completed row period, on a fixed
    // clock from the first sample, scaled as MfskSymbol::energies. Chunking
    // does not change the output.
    void process(ConstRealSpan audio, std::vector<std::vector<float>>& rows);
    void reset();

    [[nodiscard]] const MfskRasterConfig& config() const { return config_; }

private:
    explicit MfskRaster(const MfskRasterConfig& config);

    MfskRasterConfig config_;
    double samples_per_row_ = 0.0;
    std::vector<float> buffer_;
    std::int64_t buffer_start_ = 0;
    std::int64_t next_row_ = 0;
};

}  // namespace revenant::decode
