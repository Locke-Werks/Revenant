#include "core/decode/dominoex.h"

#include <algorithm>
#include <map>

namespace revenant::decode {

// Table pages 1 and 2, PRIVAR then SECVAR, by character code.
const std::array<DominoVaricodeEntry, 256> kDominoVaricode = {{
    {{1, 15, 9}, {0, 0, 0}},  // 0
    {{1, 15, 10}, {0, 0, 0}},  // 1
    {{1, 15, 11}, {0, 0, 0}},  // 2
    {{1, 15, 12}, {0, 0, 0}},  // 3
    {{1, 15, 13}, {0, 0, 0}},  // 4
    {{1, 15, 14}, {0, 0, 0}},  // 5
    {{1, 15, 15}, {0, 0, 0}},  // 6
    {{2, 8, 8}, {0, 0, 0}},  // 7
    {{2, 12, 0}, {0, 0, 0}},  // 8
    {{2, 8, 9}, {0, 0, 0}},  // 9
    {{2, 8, 10}, {0, 0, 0}},  // 10
    {{2, 8, 11}, {0, 0, 0}},  // 11
    {{2, 8, 12}, {0, 0, 0}},  // 12
    {{2, 13, 0}, {0, 0, 0}},  // 13
    {{2, 8, 13}, {0, 0, 0}},  // 14
    {{2, 8, 14}, {0, 0, 0}},  // 15
    {{2, 8, 15}, {0, 0, 0}},  // 16
    {{2, 9, 8}, {0, 0, 0}},  // 17
    {{2, 9, 9}, {0, 0, 0}},  // 18
    {{2, 9, 10}, {0, 0, 0}},  // 19
    {{2, 9, 11}, {0, 0, 0}},  // 20
    {{2, 9, 12}, {0, 0, 0}},  // 21
    {{2, 9, 13}, {0, 0, 0}},  // 22
    {{2, 9, 14}, {0, 0, 0}},  // 23
    {{2, 9, 15}, {0, 0, 0}},  // 24
    {{2, 10, 8}, {0, 0, 0}},  // 25
    {{2, 10, 9}, {0, 0, 0}},  // 26
    {{2, 10, 10}, {0, 0, 0}},  // 27
    {{2, 10, 11}, {0, 0, 0}},  // 28
    {{2, 10, 12}, {0, 0, 0}},  // 29
    {{2, 10, 13}, {0, 0, 0}},  // 30
    {{2, 10, 14}, {0, 0, 0}},  // 31
    {{0, 0, 0}, {3, 8, 8}},  // 32
    {{7, 11, 0}, {4, 15, 11}},  // 33
    {{0, 8, 14}, {5, 8, 14}},  // 34
    {{0, 10, 11}, {5, 10, 11}},  // 35
    {{0, 9, 10}, {5, 9, 10}},  // 36
    {{0, 9, 9}, {5, 9, 9}},  // 37
    {{0, 8, 15}, {5, 8, 15}},  // 38
    {{7, 10, 0}, {4, 15, 10}},  // 39
    {{0, 8, 12}, {5, 8, 12}},  // 40
    {{0, 8, 11}, {5, 8, 11}},  // 41
    {{0, 9, 13}, {5, 9, 13}},  // 42
    {{0, 8, 8}, {5, 8, 8}},  // 43
    {{2, 11, 0}, {4, 10, 11}},  // 44
    {{7, 14, 0}, {4, 15, 14}},  // 45
    {{7, 13, 0}, {4, 15, 13}},  // 46
    {{0, 8, 9}, {5, 8, 9}},  // 47
    {{3, 15, 0}, {4, 11, 15}},  // 48
    {{4, 10, 0}, {4, 12, 10}},  // 49
    {{4, 15, 0}, {4, 12, 15}},  // 50
    {{5, 9, 0}, {4, 13, 9}},  // 51
    {{6, 8, 0}, {4, 15, 8}},  // 52
    {{5, 12, 0}, {4, 13, 12}},  // 53
    {{5, 14, 0}, {4, 13, 14}},  // 54
    {{6, 12, 0}, {4, 14, 12}},  // 55
    {{6, 11, 0}, {4, 14, 11}},  // 56
    {{6, 14, 0}, {4, 14, 14}},  // 57
    {{0, 8, 10}, {5, 8, 10}},  // 58
    {{0, 8, 13}, {5, 8, 13}},  // 59
    {{0, 8, 10}, {5, 8, 10}},  // 60
    {{7, 15, 0}, {4, 15, 15}},  // 61
    {{0, 9, 15}, {5, 9, 15}},  // 62
    {{7, 12, 0}, {4, 15, 12}},  // 63
    {{0, 9, 8}, {5, 9, 8}},  // 64
    {{3, 9, 0}, {4, 11, 9}},  // 65
    {{4, 14, 0}, {4, 12, 14}},  // 66
    {{3, 12, 0}, {4, 11, 12}},  // 67
    {{3, 14, 0}, {4, 11, 14}},  // 68
    {{3, 8, 0}, {4, 11, 8}},  // 69
    {{4, 12, 0}, {4, 12, 12}},  // 70
    {{5, 8, 0}, {4, 13, 8}},  // 71
    {{5, 10, 0}, {4, 13, 10}},  // 72
    {{3, 10, 0}, {4, 11, 10}},  // 73
    {{7, 8, 0}, {4, 15, 8}},  // 74
    {{6, 10, 0}, {4, 14, 10}},  // 75
    {{4, 11, 0}, {4, 12, 11}},  // 76
    {{4, 8, 0}, {4, 12, 8}},  // 77
    {{4, 13, 0}, {4, 12, 13}},  // 78
    {{3, 11, 0}, {4, 11, 11}},  // 79
    {{4, 9, 0}, {4, 12, 9}},  // 80
    {{6, 15, 0}, {4, 14, 15}},  // 81
    {{3, 13, 0}, {4, 11, 13}},  // 82
    {{2, 15, 0}, {4, 10, 15}},  // 83
    {{2, 14, 0}, {4, 10, 14}},  // 84
    {{5, 11, 0}, {4, 13, 11}},  // 85
    {{6, 13, 0}, {4, 14, 13}},  // 86
    {{5, 13, 0}, {4, 13, 13}},  // 87
    {{5, 15, 0}, {4, 13, 15}},  // 88
    {{6, 9, 0}, {4, 14, 9}},  // 89
    {{7, 9, 0}, {4, 15, 9}},  // 90
    {{0, 10, 14}, {5, 10, 14}},  // 91
    {{0, 10, 9}, {5, 10, 9}},  // 92
    {{0, 10, 15}, {5, 10, 15}},  // 93
    {{0, 10, 10}, {5, 10, 10}},  // 94
    {{0, 9, 12}, {5, 9, 12}},  // 95
    {{0, 9, 11}, {5, 9, 11}},  // 96
    {{4, 0, 0}, {3, 8, 12}},  // 97
    {{1, 11, 0}, {4, 9, 11}},  // 98
    {{0, 12, 0}, {4, 8, 12}},  // 99
    {{0, 11, 0}, {4, 8, 11}},  // 100
    {{1, 0, 0}, {3, 8, 9}},  // 101
    {{0, 15, 0}, {4, 8, 15}},  // 102
    {{1, 9, 0}, {4, 9, 9}},  // 103
    {{0, 10, 0}, {4, 8, 10}},  // 104
    {{5, 0, 0}, {3, 8, 13}},  // 105
    {{2, 10, 0}, {4, 10, 10}},  // 106
    {{1, 14, 0}, {4, 9, 14}},  // 107
    {{0, 9, 0}, {4, 8, 9}},  // 108
    {{0, 14, 0}, {4, 8, 14}},  // 109
    {{6, 0, 0}, {3, 8, 14}},  // 110
    {{3, 0, 0}, {3, 8, 11}},  // 111
    {{1, 8, 0}, {4, 9, 8}},  // 112
    {{2, 8, 0}, {4, 10, 8}},  // 113
    {{7, 0, 0}, {3, 8, 15}},  // 114
    {{0, 8, 0}, {4, 8, 8}},  // 115
    {{2, 0, 0}, {3, 8, 10}},  // 116
    {{0, 13, 0}, {4, 8, 13}},  // 117
    {{1, 13, 0}, {4, 9, 13}},  // 118
    {{1, 12, 0}, {4, 9, 12}},  // 119
    {{1, 15, 0}, {4, 9, 15}},  // 120
    {{1, 10, 0}, {4, 9, 10}},  // 121
    {{2, 9, 0}, {4, 10, 9}},  // 122
    {{0, 10, 12}, {5, 10, 12}},  // 123
    {{0, 9, 14}, {5, 9, 14}},  // 124
    {{0, 10, 13}, {5, 10, 13}},  // 125
    {{0, 11, 8}, {5, 11, 8}},  // 126
    {{2, 10, 15}, {7, 10, 15}},  // 127
    {{2, 11, 8}, {7, 11, 8}},  // 128
    {{2, 11, 9}, {7, 11, 9}},  // 129
    {{2, 11, 10}, {7, 11, 10}},  // 130
    {{2, 11, 11}, {7, 11, 11}},  // 131
    {{2, 11, 12}, {7, 11, 12}},  // 132
    {{2, 11, 13}, {7, 11, 13}},  // 133
    {{2, 11, 14}, {7, 11, 14}},  // 134
    {{2, 11, 15}, {7, 11, 15}},  // 135
    {{2, 12, 8}, {7, 12, 8}},  // 136
    {{2, 12, 9}, {7, 12, 9}},  // 137
    {{2, 12, 10}, {7, 12, 10}},  // 138
    {{2, 12, 11}, {7, 12, 11}},  // 139
    {{2, 12, 12}, {7, 12, 12}},  // 140
    {{2, 12, 13}, {7, 12, 13}},  // 141
    {{2, 12, 14}, {7, 12, 14}},  // 142
    {{2, 12, 15}, {7, 12, 15}},  // 143
    {{2, 13, 8}, {7, 13, 8}},  // 144
    {{2, 13, 9}, {7, 13, 9}},  // 145
    {{2, 13, 10}, {7, 13, 10}},  // 146
    {{2, 13, 11}, {7, 13, 11}},  // 147
    {{2, 13, 12}, {7, 13, 12}},  // 148
    {{2, 13, 13}, {7, 13, 13}},  // 149
    {{2, 13, 14}, {7, 13, 14}},  // 150
    {{2, 13, 15}, {7, 13, 15}},  // 151
    {{2, 14, 8}, {7, 14, 8}},  // 152
    {{2, 14, 9}, {7, 14, 9}},  // 153
    {{2, 14, 10}, {7, 14, 10}},  // 154
    {{2, 14, 11}, {7, 14, 11}},  // 155
    {{2, 14, 12}, {7, 14, 12}},  // 156
    {{2, 14, 13}, {7, 14, 13}},  // 157
    {{2, 14, 14}, {7, 14, 14}},  // 158
    {{2, 14, 15}, {7, 14, 15}},  // 159
    {{0, 11, 9}, {5, 11, 9}},  // 160
    {{0, 11, 10}, {5, 11, 10}},  // 161
    {{0, 11, 11}, {5, 11, 11}},  // 162
    {{0, 11, 12}, {5, 11, 12}},  // 163
    {{0, 11, 13}, {5, 11, 13}},  // 164
    {{0, 11, 14}, {5, 11, 14}},  // 165
    {{0, 11, 15}, {5, 11, 15}},  // 166
    {{0, 12, 8}, {5, 12, 8}},  // 167
    {{0, 12, 9}, {5, 12, 9}},  // 168
    {{0, 12, 10}, {5, 12, 10}},  // 169
    {{0, 12, 11}, {5, 12, 11}},  // 170
    {{0, 12, 12}, {5, 12, 12}},  // 171
    {{0, 12, 13}, {5, 12, 13}},  // 172
    {{0, 12, 14}, {5, 12, 14}},  // 173
    {{0, 12, 15}, {5, 12, 15}},  // 174
    {{0, 13, 8}, {5, 13, 8}},  // 175
    {{0, 13, 9}, {5, 13, 9}},  // 176
    {{0, 13, 10}, {5, 13, 10}},  // 177
    {{0, 13, 11}, {5, 13, 11}},  // 178
    {{0, 13, 12}, {5, 13, 12}},  // 179
    {{0, 13, 13}, {5, 13, 13}},  // 180
    {{0, 13, 14}, {5, 13, 14}},  // 181
    {{0, 13, 15}, {5, 13, 15}},  // 182
    {{0, 14, 8}, {5, 14, 8}},  // 183
    {{0, 14, 9}, {5, 14, 9}},  // 184
    {{0, 14, 10}, {5, 14, 10}},  // 185
    {{0, 14, 11}, {5, 14, 11}},  // 186
    {{0, 14, 12}, {5, 14, 12}},  // 187
    {{0, 14, 13}, {5, 14, 13}},  // 188
    {{0, 14, 14}, {5, 14, 14}},  // 189
    {{0, 14, 15}, {5, 14, 15}},  // 190
    {{0, 15, 8}, {5, 15, 8}},  // 191
    {{0, 15, 9}, {5, 15, 9}},  // 192
    {{0, 15, 10}, {5, 15, 10}},  // 193
    {{0, 15, 11}, {5, 15, 11}},  // 194
    {{0, 15, 12}, {5, 15, 12}},  // 195
    {{0, 15, 13}, {5, 15, 13}},  // 196
    {{0, 15, 14}, {5, 15, 14}},  // 197
    {{0, 15, 15}, {5, 15, 15}},  // 198
    {{1, 8, 8}, {6, 8, 8}},  // 199
    {{1, 8, 9}, {6, 8, 9}},  // 200
    {{1, 8, 10}, {6, 8, 10}},  // 201
    {{1, 8, 11}, {6, 8, 11}},  // 202
    {{1, 8, 12}, {6, 8, 12}},  // 203
    {{1, 8, 13}, {6, 8, 13}},  // 204
    {{1, 8, 14}, {6, 8, 14}},  // 205
    {{1, 8, 15}, {6, 8, 15}},  // 206
    {{1, 9, 8}, {6, 9, 8}},  // 207
    {{1, 9, 9}, {6, 9, 9}},  // 208
    {{1, 9, 10}, {6, 9, 10}},  // 209
    {{1, 9, 11}, {6, 9, 11}},  // 210
    {{1, 9, 12}, {6, 9, 12}},  // 211
    {{1, 9, 13}, {6, 9, 13}},  // 212
    {{1, 9, 14}, {6, 9, 14}},  // 213
    {{1, 9, 15}, {6, 9, 15}},  // 214
    {{1, 10, 8}, {6, 10, 8}},  // 215
    {{1, 10, 9}, {6, 10, 9}},  // 216
    {{1, 10, 10}, {6, 10, 10}},  // 217
    {{1, 10, 11}, {6, 10, 11}},  // 218
    {{1, 10, 12}, {6, 10, 12}},  // 219
    {{1, 10, 13}, {6, 10, 13}},  // 220
    {{1, 10, 14}, {6, 10, 14}},  // 221
    {{1, 10, 15}, {6, 10, 15}},  // 222
    {{1, 11, 8}, {6, 11, 8}},  // 223
    {{1, 11, 9}, {6, 11, 9}},  // 224
    {{1, 11, 10}, {6, 11, 10}},  // 225
    {{1, 11, 11}, {6, 11, 11}},  // 226
    {{1, 11, 12}, {6, 11, 12}},  // 227
    {{1, 11, 13}, {6, 11, 13}},  // 228
    {{1, 11, 14}, {6, 11, 14}},  // 229
    {{1, 11, 15}, {6, 11, 15}},  // 230
    {{1, 12, 8}, {6, 12, 8}},  // 231
    {{1, 12, 9}, {6, 12, 9}},  // 232
    {{1, 12, 10}, {6, 12, 10}},  // 233
    {{1, 12, 11}, {6, 12, 11}},  // 234
    {{1, 12, 12}, {6, 12, 12}},  // 235
    {{1, 12, 13}, {6, 12, 13}},  // 236
    {{1, 12, 14}, {6, 12, 14}},  // 237
    {{1, 12, 15}, {6, 12, 15}},  // 238
    {{1, 13, 8}, {6, 13, 8}},  // 239
    {{1, 13, 9}, {6, 13, 9}},  // 240
    {{1, 13, 10}, {6, 13, 10}},  // 241
    {{1, 13, 11}, {6, 13, 11}},  // 242
    {{1, 13, 12}, {6, 13, 12}},  // 243
    {{1, 13, 13}, {6, 13, 13}},  // 244
    {{1, 13, 14}, {6, 13, 14}},  // 245
    {{1, 13, 15}, {6, 13, 15}},  // 246
    {{1, 14, 8}, {6, 14, 8}},  // 247
    {{1, 14, 9}, {6, 14, 9}},  // 248
    {{1, 14, 10}, {6, 14, 10}},  // 249
    {{1, 14, 11}, {6, 14, 11}},  // 250
    {{1, 14, 12}, {6, 14, 12}},  // 251
    {{1, 14, 13}, {6, 14, 13}},  // 252
    {{1, 14, 14}, {6, 14, 14}},  // 253
    {{1, 14, 15}, {6, 14, 15}},  // 254
    {{1, 15, 8}, {6, 15, 8}},  // 255
}};

namespace {

std::size_t nibble_count(const std::array<std::uint8_t, 3>& code) {
    std::size_t n = 1;
    while (n < 3 && code[n] != 0) {
        ++n;
    }
    return n;
}

bool has_secondary(const DominoVaricodeEntry& e) {
    return e.secondary[0] != 0 || e.secondary[1] != 0;
}

std::uint32_t key(std::span<const std::uint8_t> nibbles) {
    std::uint32_t k = static_cast<std::uint32_t>(nibbles.size());
    for (const std::uint8_t n : nibbles) {
        k = (k << 4U) | n;
    }
    return k;
}

const std::map<std::uint32_t, DominoDecodedCharacter>& lookup_table() {
    static const std::map<std::uint32_t, DominoDecodedCharacter> table = [] {
        std::map<std::uint32_t, DominoDecodedCharacter> t;
        for (int c = 0; c < 256; ++c) {
            const auto& e = kDominoVaricode[static_cast<std::size_t>(c)];
            const auto pn = nibble_count(e.primary);
            // emplace keeps the first, so the lower code of a duplicated
            // pair wins (see the header).
            t.emplace(key(std::span(e.primary.data(), pn)),
                      DominoDecodedCharacter{static_cast<std::uint8_t>(c), false, true});
            if (has_secondary(e)) {
                t.emplace(key(std::span(e.secondary.data(), 3)),
                          DominoDecodedCharacter{static_cast<std::uint8_t>(c), true, true});
            }
        }
        return t;
    }();
    return table;
}

}  // namespace

DominoGeometry domino_geometry(DominoSpeed speed) {
    switch (speed) {
        case DominoSpeed::D4: return {8000.0 / 2048.0, 2.0 * 8000.0 / 2048.0};
        case DominoSpeed::D5: return {11025.0 / 2048.0, 2.0 * 11025.0 / 2048.0};
        case DominoSpeed::D8: return {16000.0 / 2048.0, 2.0 * 16000.0 / 2048.0};
        case DominoSpeed::D11: return {11025.0 / 1024.0, 11025.0 / 1024.0};
        case DominoSpeed::D16: return {16000.0 / 1024.0, 16000.0 / 1024.0};
        case DominoSpeed::D22: return {22050.0 / 1024.0, 22050.0 / 1024.0};
    }
    return {};
}

std::vector<std::uint8_t> domino_nibbles(std::uint8_t code, bool secondary) {
    const auto& e = kDominoVaricode[code];
    if (secondary) {
        if (!has_secondary(e)) {
            return {};
        }
        return {e.secondary.begin(), e.secondary.end()};
    }
    return {e.primary.begin(), e.primary.begin() + static_cast<std::ptrdiff_t>(nibble_count(e.primary))};
}

DominoDecodedCharacter domino_lookup(std::span<const std::uint8_t> nibbles) {
    if (nibbles.empty() || nibbles.size() > 3) {
        return {};
    }
    const auto& t = lookup_table();
    const auto it = t.find(key(nibbles));
    return it == t.end() ? DominoDecodedCharacter{} : it->second;
}

Expected<DominoDecoder> DominoDecoder::create(const DominoConfig& config) {
    const DominoGeometry g = domino_geometry(config.speed);
    MfskDemodConfig dc;
    dc.rate = config.rate;
    // An engineering choice: the bank is four tones wider than the signal on
    // each side. Until IFK+'s rotation has visited every tone, the search
    // cannot tell where the 18 sit, and with a bank of exactly 18 it placed
    // them one low, so the top tone fell outside and every step to or from
    // it was misread. IFK reads differences, so where in the wider bank the
    // signal sits does not matter.
    dc.tones = kDominoBank;
    dc.tone_spacing_hz = g.tone_spacing_hz;
    dc.symbol_rate = g.symbol_rate;
    dc.search = true;
    dc.search_low_hz = config.search_low_hz;
    dc.search_high_hz = config.search_high_hz;
    // An engineering choice: four seconds or so of symbols before the first
    // decision, enough for IFK+'s rotation to cross the whole tone set.
    dc.acquisition_symbols = std::max(16, static_cast<int>(4.0 * g.symbol_rate));
    // Engineering choices. Technical "Performance" claims tolerance of
    // drift to 200 Hz a minute, which IFK gives only while the tones stay in
    // the bank, so the AFC has to keep steering in noise: a symbol steers it
    // if its strongest tone holds a fifth of the bank's energy, against one
    // twenty-second for noise alone.
    dc.afc_min_confidence = 0.2;
    auto demod = MfskDemodulator::create(dc);
    if (!demod) {
        return std::unexpected(demod.error());
    }
    DominoDecoder d(std::move(*demod));
    d.config_ = config;
    return d;
}

void DominoDecoder::reset() {
    demod_.reset();
    have_previous_ = false;
    peak_ = 0.0;
    nibbles_.clear();
    invalid_ = 0;
}

void DominoDecoder::process(ConstRealSpan audio, std::vector<DominoCharacter>& out) {
    symbols_.clear();
    demod_.process(audio, symbols_);
    for (const MfskSymbol& s : symbols_) {
        // Bank energies, low tone first on either sideband, as shares of the
        // symbol's total.
        Bank e{};
        double total = 0.0;
        for (int k = 0; k < kDominoBank; ++k) {
            const int at = config_.lower_sideband ? kDominoBank - 1 - k : k;
            e[static_cast<std::size_t>(k)] = s.energies[static_cast<std::size_t>(at)];
            total += e[static_cast<std::size_t>(k)];
        }
        // Digital silence (a gap in a recording, the padding of a test)
        // decides nothing; without this every nibble ties at zero, which is
        // the primary SPACE. Noise is not silence and still decodes.
        peak_ = std::max(peak_, total);
        if (total <= 1e-6 * peak_) {
            continue;
        }
        for (double& v : e) {
            v /= total;
        }
        if (have_previous_) {
            decide(e, static_cast<SampleIndex>(s.start), out);
        }
        previous_ = e;
        have_previous_ = true;
    }
}

void DominoDecoder::decide(const Bank& cur, SampleIndex position,
                           std::vector<DominoCharacter>& out) {
    // Non-coherent differential detection over the whole bank. Each nibble
    // scores the sum over previous bins p of the energy at p in the previous
    // symbol times the energy where IFK+ puts the next tone: p + 2 + nibble,
    // or that less 18 when the step wraps. Which of the two the transmitter
    // used depends on where the 18 tones sit in the wider bank, which is not
    // known, so the stronger is taken; an empty guard bin never wins against
    // the tone. A hard reading of the previous tone is the special case with
    // all of its energy in one bin; this lets a doubtful previous symbol
    // share its weight between candidates. Steps of 16 and 17, which IFK+
    // never makes, are not candidates.
    int nibble = 0;
    double top = -1.0;
    for (int n = 0; n < 16; ++n) {
        double score = 0.0;
        for (int p = 0; p < kDominoBank; ++p) {
            const double before = previous_[static_cast<std::size_t>(p)];
            const int up = p + kDominoIfkOffset + n;
            const int wrapped = up - kDominoTones;
            double after = 0.0;
            if (up < kDominoBank) {
                after = cur[static_cast<std::size_t>(up)];
            }
            if (wrapped >= 0) {
                after = std::max(after, cur[static_cast<std::size_t>(wrapped)]);
            }
            score += before * after;
        }
        if (score > top) {
            top = score;
            nibble = n;
        }
    }
    on_nibble(nibble, position, out);
}

void DominoDecoder::on_nibble(int nibble, SampleIndex position, std::vector<DominoCharacter>& out) {
    if (nibble < 8) {
        finish(out);
        start_ = position;
        nibbles_.push_back(static_cast<std::uint8_t>(nibble));
        return;
    }
    if (nibbles_.empty()) {
        return;  // a continuation with no initial nibble: joined mid-character
    }
    nibbles_.push_back(static_cast<std::uint8_t>(nibble));
    if (nibbles_.size() > 3) {
        ++invalid_;
        nibbles_.clear();
    }
}

void DominoDecoder::finish(std::vector<DominoCharacter>& out) {
    if (nibbles_.empty()) {
        return;
    }
    const DominoDecodedCharacter c = domino_lookup(nibbles_);
    if (c.valid) {
        out.push_back(DominoCharacter{c.code, c.secondary, start_});
    } else {
        ++invalid_;
    }
    nibbles_.clear();
}

void DominoDecoder::flush(std::vector<DominoCharacter>& out) {
    finish(out);
}

}  // namespace revenant::decode
