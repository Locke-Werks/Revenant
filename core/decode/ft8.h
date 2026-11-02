// FT8, and the slotted decoder engine FT4 shares with it.
//
// SPECIFICATION
//
// S. Franke K9AN, B. Somerville G4WJS and J. Taylor K1JT, "The FT4 and FT8
// Communication Protocols", QEX July/August 2020 pp. 7-17. Section 3 and
// core/decode/ft_ldpc.h for the (174, 91) code, section 4 and Table 3 for the
// Gray map and the 3 1 4 0 6 5 2 Costas array at symbols 0, 36 and 72,
// section 5 and Table 4 for the waveform (8-GFSK, h = 1, T = 0.160 s,
// BT = 2, Equation 3's pulse and the T/8 raised-cosine ramps), section 6's
// statement that transmissions start 0.5 s into a 15 s interval, and section
// 7's subtraction of each decoded signal before a further pass. The message
// layer is core/decode/wsjt77.h. The paper's section 9 places the protocol
// description and its reference [14] in the public domain.
//
// WHAT THE RECEIVER DOES, AND WHAT IS OURS RATHER THAN THE PAPER'S
//
// Section 6 describes WSJT-X's receiver only in outline and says the
// receiver is implementation specific. This one is written from that
// outline and from textbook non-coherent MFSK detection:
//
//  1. A spectrogram of each slot, one symbol long, half a tone spacing in
//     frequency and a quarter symbol in time, scored at every start time and
//     frequency by the share of each sync symbol's energy found in the tone
//     the sync pattern names. Local maxima become candidates.
//  2. Each candidate is cut out of one FFT of the whole slot and brought to
//     complex baseband at 32 samples per symbol, where time and frequency
//     are refined to a sample and a sixteenth of a tone spacing.
//  3. Single-symbol detection (the paper's N = 1 case): a DFT per symbol per
//     tone, and a bit metric that is the largest tone amplitude with the bit
//     clear less the largest with it set, normalised over the code word.
//  4. core/decode/ldpc.h's min-sum decoder, then the CRC. There is no ordered
//     statistics stage and no block detection, so the threshold sits near
//     the paper's Table 5 baseline row rather than its best.
//  5. Section 7's subtraction with a moving average one symbol long as the
//     low-pass filter, whose cutoff the paper leaves unstated, and a further
//     pass over what is left.
//
// Every constant in steps 1 to 5 that the paper does not give is marked NOT
// SPECIFIED where it is defined.
//
// TIMING
//
// A slot opens on a whole multiple of its length in UTC. The time of input
// sample 0 comes from FtConfig::utc_of_sample, the wall clock by default, and
// every later sample is counted from it. A signal is found if it starts from
// 0 to about 2.4 s into the slot (DT -0.5 to +1.9 s against the nominal
// 0.5 s) and ends inside it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/decode/ldpc.h"
#include "core/decode/wsjt77.h"
#include "core/decode/wsjt_dsp.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::Complex32;

// Symbol layout markers in FtProtocol::layout.
inline constexpr int kFtData = -1;
inline constexpr int kFtRamp = -2;

// Everything that tells FT8 and FT4 apart.
struct FtProtocol {
    const char* name = "FT8";
    int tones = 8;
    int bits_per_symbol = 3;
    // gray[k]: the bits tone k carries, first bit most significant.
    std::vector<int> gray;
    // One entry per channel symbol: a sync tone (0 or more), kFtData, or
    // kFtRamp (tone 0, carrying nothing).
    std::vector<int> layout;
    // Symbol length in samples at 12 kHz; the tone spacing is 12000 / this.
    int samples_per_symbol = 1920;
    double gaussian_bt = 2.0;
    // Raised-cosine amplitude ramp at each end, in samples at 12 kHz.
    int ramp_samples = 240;
    double slot_seconds = 15.0;
    double nominal_start_seconds = 0.5;
    // Baseband decimation from 12 kHz. Must divide both the slot length and
    // the symbol length.
    int decimation = 60;
    // XORed into the 77-bit payload before the CRC, when set.
    bool scramble = false;
    Wsjt77 scrambler{};
};

// Section 4, Table 3 and section 5.
[[nodiscard]] const FtProtocol& ft8_protocol();

[[nodiscard]] inline double ft_tone_spacing(const FtProtocol& p) {
    return static_cast<double>(kWsjtRate) / p.samples_per_symbol;
}

// The full chain for a transmitter: scramble if the protocol does, CRC-14,
// LDPC, Gray map, sync and ramp symbols. One tone index per channel symbol.
[[nodiscard]] std::vector<int> ft_channel_tones(const FtProtocol& p, const Wsjt77& payload);

// Section 5's waveform at `rate` (a multiple of 12 kHz), as an analytic
// signal with tone 0 at 0 Hz: amplitude ramps included, unit amplitude
// otherwise. Starts at the first symbol and lasts the whole transmission.
[[nodiscard]] std::vector<Complex32> ft_reference(const FtProtocol& p, std::span<const int> tones,
                                                  SampleRate rate);

struct FtConfig {
    FtProtocol protocol = ft8_protocol();
    // Input audio rate: 12 kHz or any whole multiple.
    SampleRate rate = 12000;
    // Lowest and highest tone-0 frequency searched.
    double low_hz = 200.0;
    double high_hz = 3000.0;
    WsjtUtcOfSample utc_of_sample;
    // NOT SPECIFIED. Candidates examined per pass, strongest sync first.
    int max_candidates = 200;
    // NOT SPECIFIED. Sync score, the mean share of a sync symbol's tone
    // energy in the expected tone, times the tone count; 1 for noise.
    double sync_threshold = 1.25;
    // Decoding passes, each after subtracting what the previous found.
    int passes = 3;
    std::size_t ldpc_iterations = 60;
};

struct FtDecode {
    // UTC seconds at the start of the slot the signal was found in.
    double slot_utc = 0.0;
    // Index of the slot's first sample, in input samples.
    SampleIndex slot_start_sample = 0;
    // Frequency of tone 0 in the audio, Hz.
    double frequency_hz = 0.0;
    // Start time against the nominal 0.5 s, seconds.
    double dt_seconds = 0.0;
    // Signal to noise ratio in 2500 Hz, the paper's convention.
    double snr_db = 0.0;
    int pass = 0;
    std::size_t ldpc_iterations = 0;
    // Unscrambled payload.
    Wsjt77 payload{};
    WsjtMessage message;
};

class FtDecoder {
public:
    [[nodiscard]] static Expected<FtDecoder> create(FtConfig config);

    // Consumes audio and appends what every slot completed by it decoded to.
    // The result does not depend on how the audio was split into calls.
    [[nodiscard]] Status process(ConstRealSpan audio, std::vector<FtDecode>& out);

    // Decodes one slot of 12 kHz audio that starts on its boundary, outside
    // the stream: for files and tests. The hash table is shared with process.
    void decode_slot(std::span<const float> slot_12k, double slot_utc, SampleIndex slot_start,
                     std::vector<FtDecode>& out);

    [[nodiscard]] const FtConfig& config() const { return config_; }
    [[nodiscard]] WsjtHashTable& hashes() { return hashes_; }

private:
    FtDecoder(FtConfig config, WsjtInputStage input, LdpcDecoder ldpc);

    FtConfig config_;
    WsjtInputStage input_;
    LdpcDecoder ldpc_;
    WsjtSlotClock clock_;
    WsjtHashTable hashes_;
    std::vector<float> scratch_;
    std::vector<float> buffer_;
    // 12 kHz index of buffer_[0].
    std::int64_t buffer_start_ = 0;
    std::int64_t received_ = 0;
    std::int64_t next_slot_ = 0;
    bool started_ = false;
};

}  // namespace revenant::decode
