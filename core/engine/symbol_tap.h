// The constellation and eye diagram tap: a low-rate view of a digital
// receiver's symbols, for diagnostics and nothing else.
//
// The decoders in core/decode do their own symbol timing, but they run
// wherever the complex baseband is consumed, which is a client's decode lane
// and not this process's completion thread. A passband frame is built here,
// so the tap runs its own timing loop on the same baseband the decoders get:
// core/dsp/gardner.h on the FM discriminator for the FSK family (P25 phase
// one and DMR C4FM, D-STAR GMSK) and on a root raised cosine matched filter's
// output for TETRA's pi/4-DQPSK. A raw receiver has no mode to read a symbol
// rate from, so core/characterise's cyclostationary estimate supplies one and
// says which family it is.
//
// Nothing here decodes. Levels and points are what a scope shows; no bit is
// sliced and no frame is looked for.
//
// Run only for a receiver with VrxParams::display_symbols set, on the frames
// that carry a passband. Pure host arithmetic, tested without a device in
// tests/engine/test_symbol_tap.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/dsp/gardner.h"
#include "core/dsp/types.h"
#include "core/engine/vrx.h"

namespace revenant::engine {

enum class SymbolKind : std::uint8_t {
    None,
    // One real level per symbol, FSK family, in units of the mode's inner
    // deviation so C4FM's ideal levels are -3, -1, 1 and 3.
    Levels,
    // One complex point per symbol, PSK family, unit RMS.
    Constellation,
};

// Most symbols one frame carries.
inline constexpr std::size_t kSymbolTapMaxSymbols = 512;

// The eye window: this many traces, each two symbol periods long and
// resampled to kSymbolEyePerSymbol points a symbol, centred on a strobe.
inline constexpr std::size_t kSymbolEyeTraces = 64;
inline constexpr std::size_t kSymbolEyePerSymbol = 8;
inline constexpr std::size_t kSymbolEyeTraceLength = 2 * kSymbolEyePerSymbol + 1;

struct SymbolScope {
    SymbolKind kind = SymbolKind::None;
    double symbol_rate_hz = 0.0;

    // Levels: one float a symbol. Constellation: interleaved I and Q.
    // Oldest first.
    std::vector<float> points;

    // Where the points should be, in the same layout.
    std::vector<float> ideal;

    // Traces of eye_trace_length floats each, back to back, oldest first.
    std::vector<float> eye;
    std::uint32_t eye_trace_length = 0;

    // RMS distance from the nearest ideal point as a fraction of the
    // outermost ideal's magnitude: EVM for a constellation, the symbol
    // deviation error for levels.
    float error_rms = 0.0F;

    // The timing loop's phase against a free-running clock, in symbols in
    // [-0.5, 0.5), and the clock error it settled on, in parts per million.
    float timing_offset = 0.0F;
    float rate_error_ppm = 0.0F;
};

class SymbolTap {
public:
    SymbolTap() = default;

    void reset();

    // Interleaved I and Q, the complex tap's layout, at `rate`. A change of
    // mode or rate restarts the tap.
    void push(Demod mode, std::span<const float> iq, dsp::SampleRate rate);

    // The latest symbols, or null while there are none.
    [[nodiscard]] const SymbolScope* scope();

private:
    struct Plan {
        SymbolKind kind = SymbolKind::None;
        double symbol_rate_hz = 0.0;
        // Levels: hertz of deviation per level unit.
        double unit_hz = 0.0;
        std::vector<float> ideal;
        // Constellation: the RRC matched filter's roll-off.
        double rolloff = 0.0;
    };

    void configure(const Plan& plan, dsp::SampleRate rate);
    void estimate_raw(dsp::SampleRate rate);
    void run(std::span<const dsp::Complex32> samples);

    Demod mode_ = Demod::Raw;
    dsp::SampleRate rate_ = 0;
    bool configured_ = false;
    Plan plan_{};

    // Raw receivers: samples gathered for the estimate, and how many more to
    // see before trying again after a refusal.
    std::vector<dsp::Complex32> estimate_buffer_;
    std::size_t estimate_wait_ = 0;

    std::optional<dsp::GardnerTiming> timing_;
    std::vector<float> taps_;
    std::vector<dsp::Complex32> filter_history_;
    std::vector<dsp::Complex32> filtered_;
    std::size_t boxcar_ = 1;
    std::vector<float> boxcar_history_;
    dsp::Complex32 last_sample_{};
    bool have_last_sample_ = false;
    double dc_hz_ = 0.0;
    double eye_power_ = 0.0;

    std::vector<dsp::Complex32> symbols_;
    std::size_t symbols_head_ = 0;
    std::size_t symbols_count_ = 0;
    std::vector<float> eye_;
    std::size_t eye_head_ = 0;
    std::size_t eye_count_ = 0;
    std::optional<dsp::GardnerTiming::Strobe> previous_strobe_;
    dsp::Complex32 previous_symbol_{};
    bool have_previous_symbol_ = false;

    SymbolScope scope_{};
};

}  // namespace revenant::engine
