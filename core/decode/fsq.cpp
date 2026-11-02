#include "core/decode/fsq.h"

#include <algorithm>

namespace revenant::decode {

// Varicode, column by column as printed.
const std::array<FsqVaricodeEntry, 104> kFsqVaricode = {{
    // SPACE to '?'
    {32, 0, -1},   {33, 11, 30},  {34, 12, 30},  {35, 13, 30},  {36, 14, 30},  {37, 15, 30},
    {38, 16, 30},  {39, 17, 30},  {40, 18, 30},  {41, 19, 30},  {42, 20, 30},  {43, 21, 30},
    {44, 27, 29},  {45, 22, 30},  {46, 27, -1},  {47, 23, 30},  {48, 10, 30},  {49, 1, 30},
    {50, 2, 30},   {51, 3, 30},   {52, 4, 30},   {53, 5, 30},   {54, 6, 30},   {55, 7, 30},
    {56, 8, 30},   {57, 9, 30},   {58, 24, 30},  {59, 25, 30},  {60, 26, 30},  {61, 0, 31},
    {62, 27, 30},  {63, 28, 29},
    // '@' to '_'
    {64, 0, 29},   {65, 1, 29},   {66, 2, 29},   {67, 3, 29},   {68, 4, 29},   {69, 5, 29},
    {70, 6, 29},   {71, 7, 29},   {72, 8, 29},   {73, 9, 29},   {74, 10, 29},  {75, 11, 29},
    {76, 12, 29},  {77, 13, 29},  {78, 14, 29},  {79, 15, 29},  {80, 16, 29},  {81, 17, 29},
    {82, 18, 29},  {83, 19, 29},  {84, 20, 29},  {85, 21, 29},  {86, 22, 29},  {87, 23, 29},
    {88, 24, 29},  {89, 25, 29},  {90, 26, 29},  {91, 1, 31},   {92, 2, 31},   {93, 3, 31},
    {94, 4, 31},   {95, 5, 31},
    // '`' to DEL
    {96, 9, 31},   {97, 1, -1},   {98, 2, -1},   {99, 3, -1},   {100, 4, -1},  {101, 5, -1},
    {102, 6, -1},  {103, 7, -1},  {104, 8, -1},  {105, 9, -1},  {106, 10, -1}, {107, 11, -1},
    {108, 12, -1}, {109, 13, -1}, {110, 14, -1}, {111, 15, -1}, {112, 16, -1}, {113, 17, -1},
    {114, 18, -1}, {115, 19, -1}, {116, 20, -1}, {117, 21, -1}, {118, 22, -1}, {119, 23, -1},
    {120, 24, -1}, {121, 25, -1}, {122, 26, -1}, {123, 6, 31},  {124, 7, 31},  {125, 8, 31},
    {126, 0, 30},  {127, 28, 31},
    // The fourth column: CRLF, IDLE, then the extras.
    {13, 28, -1},  {0, 28, 30},   {241, 10, 31}, {246, 11, 31}, {248, 12, 31}, {158, 13, 31},
    {156, 14, 31}, {8, 27, 31},
}};

std::vector<int> fsq_symbols(std::uint8_t code) {
    if (code == 10) {
        code = 13;
    }
    for (const auto& e : kFsqVaricode) {
        if (e.code == code) {
            if (e.second < 0) {
                return {e.first};
            }
            return {e.first, e.second};
        }
    }
    return {};
}

int fsq_lookup(int first, int second) {
    for (const auto& e : kFsqVaricode) {
        if (e.first == first && e.second == second) {
            return e.code;
        }
    }
    return -1;
}

Expected<FsqDecoder> FsqDecoder::create(const FsqConfig& config) {
    MfskDemodConfig dc;
    dc.rate = config.rate;
    dc.tones = kFsqBank;
    dc.tone_spacing_hz = kFsqToneSpacingHz;
    dc.symbol_rate = config.symbol_rate;
    dc.search = true;
    dc.search_low_hz = config.search_low_hz;
    dc.search_high_hz = config.search_high_hz;
    // Engineering choices, as in dominoex.cpp: a few seconds of symbols to
    // acquire over, and an AFC that keeps steering in noise.
    dc.acquisition_symbols = std::max(16, static_cast<int>(4.0 * config.symbol_rate));
    dc.afc_min_confidence = 0.15;
    auto demod = MfskDemodulator::create(dc);
    if (!demod) {
        return std::unexpected(demod.error());
    }
    FsqDecoder d(std::move(*demod));
    d.config_ = config;
    return d;
}

void FsqDecoder::reset() {
    demod_.reset();
    have_previous_ = false;
    peak_ = 0.0;
    pending_ = -1;
    invalid_ = 0;
}

void FsqDecoder::process(ConstRealSpan audio, std::vector<FsqCharacter>& out) {
    symbols_.clear();
    demod_.process(audio, symbols_);
    for (const MfskSymbol& s : symbols_) {
        Bank e{};
        double total = 0.0;
        for (int k = 0; k < kFsqBank; ++k) {
            const int at = config_.lower_sideband ? kFsqBank - 1 - k : k;
            e[static_cast<std::size_t>(k)] = s.energies[static_cast<std::size_t>(at)];
            total += e[static_cast<std::size_t>(k)];
        }
        // Digital silence decides nothing, as in dominoex.cpp.
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

void FsqDecoder::decide(const Bank& cur, SampleIndex position, std::vector<FsqCharacter>& out) {
    // dominoex.cpp's differential detection, with FSQ's 33 tones, offset of
    // one and 32 candidate differences.
    int value = 0;
    double top = -1.0;
    for (int n = 0; n < kFsqTones - 1; ++n) {
        double score = 0.0;
        for (int p = 0; p < kFsqBank; ++p) {
            const int up = p + kFsqIfkOffset + n;
            const int wrapped = up - kFsqTones;
            double after = 0.0;
            if (up < kFsqBank) {
                after = cur[static_cast<std::size_t>(up)];
            }
            if (wrapped >= 0) {
                after = std::max(after, cur[static_cast<std::size_t>(wrapped)]);
            }
            score += previous_[static_cast<std::size_t>(p)] * after;
        }
        if (score > top) {
            top = score;
            value = n;
        }
    }
    on_symbol(value, position, out);
}

void FsqDecoder::on_symbol(int value, SampleIndex position, std::vector<FsqCharacter>& out) {
    if (value >= kFsqFirstShift) {
        if (pending_ < 0) {
            ++invalid_;
            return;
        }
        const int code = fsq_lookup(pending_, value);
        if (code >= 0) {
            emit(code, pending_position_, out);
        } else {
            ++invalid_;
        }
        pending_ = -1;
        return;
    }
    if (pending_ >= 0) {
        emit(fsq_lookup(pending_, -1), pending_position_, out);
    }
    pending_ = value;
    pending_position_ = position;
}

void FsqDecoder::emit(int code, SampleIndex position, std::vector<FsqCharacter>& out) {
    if (code < 0) {
        ++invalid_;
        return;
    }
    if (code == 0) {
        return;  // IDLE
    }
    out.push_back(FsqCharacter{static_cast<std::uint8_t>(code), position});
}

void FsqDecoder::flush(std::vector<FsqCharacter>& out) {
    if (pending_ >= 0) {
        emit(fsq_lookup(pending_, -1), pending_position_, out);
        pending_ = -1;
    }
}

}  // namespace revenant::decode
