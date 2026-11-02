// JT65 A, B and C: channel coding, waveform and a receiver with the
// Franke-Taylor soft-decision Reed-Solomon decoder.
//
// SPECIFICATION
//
// J. Taylor K1JT, "The JT65 Communications Protocol", QEX
// September/October 2005 pp. 3-12 (version of March 9, 2005,
// https://wsjt.sourceforge.io/JT65.pdf): section 4 and Figure 2 for the
// 72-bit message (core/decode/wsjt72.h); section 5 for RS(63, 12)
// (core/decode/rs64.h); section 6 for the 7 x 9 interleaver and Gray code;
// section 8 and Figure 3 for 126 intervals of 4096 samples at 11025 Hz, the
// 126-bit sync vector, the sync tone at the frequency of tone 0, data tone
// N at tone 0 plus 2.6917 (N + 2) m Hz with m = 1, 2, 4 for A, B, C, the
// transmission starting 1 s into a UTC minute, and the "OOO" report sent by
// swapping sync and data positions.
//
// S. Franke K9AN and J. Taylor K1JT, "Open Source Soft-Decision Decoder for
// the JT65 (63,12) Reed-Solomon Code", QEX May/June 2016 pp. 8-17
// (https://wsjt.sourceforge.io/FrankeTaylor_QEX_2016.pdf): sections 3 and 4
// and Algorithm 1 for the decoder: stochastic erasure vectors drawn with
// each symbol's erasure probability set to 1.3 times its estimated error
// probability, at most 51 erasures, errors-and-erasures Berlekamp-Massey,
// and the metric u, the mean spectral power along a candidate code word
// (equation 7), with the ratio u2/u1 between the best two as the test.
//
// DEVIATIONS AND GAPS
//
// Section 6's interleaver ("row-by-row into a 7 x 9 matrix, ... out
// column-by-column") leaves the orientation open; the one that reproduces
// Figure 2 writes the code word into 9 rows of 7 and sends column by
// column, so channel position 9c + r carries code word symbol 7r + c.
//
// The 2016 paper's symbol-error probabilities come from an 8 x 8 table it
// does not print, and its acceptance constants X0, D0, D1 and R1 are not
// printed either. Here the error probability of a symbol is one less its
// largest non-coherent posterior over the 64 tones; the 1.3 factor is
// lowered when it would erase more than 38 symbols on average, a measured
// choice explained where it is made; and acceptance is on u1,
// on (u2 - 1)/(u1 - 1) in place of u2/u1, and on the Hamming distance, with
// constants marked NOT SPECIFIED.
//
// Shorthand messages (RO, RRR, 73) are described only in outline and are
// not implemented, nor is hinted (deep search) decoding.

#pragma once

#include <span>
#include <vector>

#include "core/decode/rs64.h"
#include "core/decode/wsjt72.h"
#include "core/decode/wsjt_slow.h"
#include "core/error.h"

namespace revenant::decode {

[[nodiscard]] std::span<const std::uint8_t> jt65_sync_vector();

// Submode 0, 1, 2 for A, B, C. Tone 0 is the sync tone, tone N + 2 data
// symbol N; tone 1 is never sent. `ooo` swaps sync and data positions.
[[nodiscard]] const SlowProtocol& jt65_protocol(int submode = 0, bool ooo = false);

// The 72 bits as twelve 6-bit symbols, most significant first: Figure 2's
// "packed message".
[[nodiscard]] Rs64Message jt65_symbols(const Wsjt72& payload);
[[nodiscard]] Wsjt72 jt65_payload(const Rs64Message& symbols);

// The 63 channel symbol values in transmission order, after interleaving
// and Gray coding: Figure 2's "channel symbols".
[[nodiscard]] std::vector<int> jt65_channel_symbols(const Wsjt72& payload);

// The 126 tones, 0 to 65.
[[nodiscard]] std::vector<int> jt65_channel_tones(const Wsjt72& payload, bool ooo = false);

struct Jt65Config {
    int submode = 0;
    SampleRate rate = 12000;
    double low_hz = 200.0;
    double high_hz = 2700.0;
    WsjtUtcOfSample utc_of_sample;
    // NOT SPECIFIED.
    double max_drift_hz = 0.0;
    int max_candidates = 30;
    double sync_threshold = 0.15;
    // Algorithm 1's timeout T. The paper's simulations use up to 10^5.
    int trials = 10000;
    // Also search for the "OOO" form.
    bool ooo = true;
};

struct Jt65Decode : Wsjt72Decode {
    bool ooo = false;
    int trials = 0;        // erasure trials used; 0 for errors-only
    int hard_errors = 0;   // Hamming distance from the hard decisions
};

class Jt65Decoder {
public:
    [[nodiscard]] static Expected<Jt65Decoder> create(Jt65Config config);
    [[nodiscard]] Status process(ConstRealSpan audio, std::vector<Jt65Decode>& out);
    void decode_slot(std::span<const float> slot_12k, double slot_utc, SampleIndex slot_start,
                     std::vector<Jt65Decode>& out) const;

private:
    Jt65Decoder(Jt65Config config, SlowStream stream);

    Jt65Config config_;
    SlowStream stream_;
};

}  // namespace revenant::decode
