// 3G ALE burst transmitter (BW0, BW1), for testing the decoder.
//
// MIL-STD-188-141B Appendix C, the clauses core/decode/ale_3g.h cites: the
// symbol sequence of C.5.1.3 and C.5.1.4, mapped by table C-XX and placed on
// an 1800 Hz carrier at 2400 symbols/s (C.5.1.8, figure C-16). The low pass
// filter of figure C-16 is not specified; this one is a root raised cosine,
// evaluated in closed form at each output sample so the symbol clock need not
// divide the sample rate.

#pragma once

#include <cstdint>
#include <vector>

#include "core/decode/ale_3g.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct Ale3gModConfig {
    dsp::SampleRate rate = 8000;
    double amplitude = 0.5;   // peak of the unfiltered carrier
    double roll_off = 0.35;
    double offset_hz = 0.0;
    double lead_in_samples = 0.0;
    double tail_samples = 0.0;
};

struct Ale3gModBurst {
    decode::Ale3gWaveform waveform = decode::Ale3gWaveform::Bw0;
    std::uint64_t payload = 0;
    double gap_samples = 0.0;  // silence after this burst
};

[[nodiscard]] Expected<std::vector<float>> ale3g_render(const Ale3gModConfig& config,
                                                        const std::vector<Ale3gModBurst>& bursts);

}  // namespace revenant::siggen
