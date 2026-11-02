// JT9: channel coding, waveform and a receiver.
//
// SPECIFICATION
//
// A. Talbot G4JNT, "The JT9 Coding Process (Plain Text messages only)",
// March 2016, http://www.g4jnt.com/JT9_Coding_Process.pdf, which quotes the
// WSJT-X user manual's description and then works the chain through: the
// 72-bit message (core/decode/wsjt72.h), 31 zero tail bits, the K = 32
// r = 1/2 code and bit-reversal interleaver shared with WSPR
// (core/decode/wsjt_slow.h), "Convert to Symbols" (three bits per symbol
// with a zero pad bit at the end, 69 symbols), "Gray Coding" (n XOR n >> 1),
// "Merge With Sync" (the 85-symbol vector; sync symbols are tone 0, data
// symbols the Gray value plus 1) and "Modulation" (tone spacing 12000/6912 Hz
// times 1, 2, 4 ... 128 for submodes A to H, 6912 samples per symbol at
// 12 kHz for the slow submodes). The document's last page lists the 85
// symbols for "G4JNT IO90IV" (without the leading space its text example
// has), which the tests reproduce.
//
// DEVIATIONS AND GAPS
//
// "Convert to Symbols" prints "Bit1 * 4 + Bit2 * 3 + Bit3"; read as
// Bit2 * 2, which is the only reading that gives values 0 to 7 and the one
// the document's own symbol listing confirms.
//
// The fast submodes (E to H at 25 to 200 baud) are named but not described,
// so only the slow ones are implemented. The document gives no timing: the
// one-minute slot, the nominal start one second into it and the search
// window of 0 to 3 s are NOT SPECIFIED by it.
//
// The receiver is core/decode/wsjt_slow.h's front end, a non-coherent bit
// likelihood from the eight data tones of each symbol, and core/decode/fano.h.

#pragma once

#include <span>
#include <vector>

#include "core/decode/wsjt72.h"
#include "core/decode/wsjt_slow.h"
#include "core/error.h"

namespace revenant::decode {

// "Merge With Sync": 1 marks a sync symbol.
[[nodiscard]] std::span<const std::uint8_t> jt9_sync_vector();

// Submode 0 to 7 for A to H.
[[nodiscard]] const SlowProtocol& jt9_protocol(int submode = 0);

// The 85 channel symbols, 0 to 8.
[[nodiscard]] std::vector<int> jt9_channel_tones(const Wsjt72& payload);

struct Jt9Config {
    int submode = 0;
    SampleRate rate = 12000;
    double low_hz = 200.0;
    double high_hz = 3000.0;
    WsjtUtcOfSample utc_of_sample;
    // NOT SPECIFIED.
    double max_drift_hz = 2.0;
    int max_candidates = 40;
    double sync_threshold = 0.3;
    std::size_t fano_cycles_per_bit = 10000;
};

class Jt9Decoder {
public:
    [[nodiscard]] static Expected<Jt9Decoder> create(Jt9Config config);
    [[nodiscard]] Status process(ConstRealSpan audio, std::vector<Wsjt72Decode>& out);
    void decode_slot(std::span<const float> slot_12k, double slot_utc, SampleIndex slot_start,
                     std::vector<Wsjt72Decode>& out) const;

private:
    Jt9Decoder(Jt9Config config, SlowStream stream);

    Jt9Config config_;
    SlowStream stream_;
};

}  // namespace revenant::decode
