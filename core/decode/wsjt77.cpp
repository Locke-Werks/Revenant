#include "core/decode/wsjt77.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace revenant::decode {

namespace {

// std_call_to_c28.f90's four alphabets.
constexpr std::string_view kA1 = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::string_view kA2 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::string_view kA3 = "0123456789";
constexpr std::string_view kA4 = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
// hashcodes.f90 and nonstd_to_c58.f90.
constexpr std::string_view kC38 = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
// free_text_to_f71.f90.
constexpr std::string_view kC42 = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";
// hashcodes.f90.
constexpr std::uint64_t kNprime = 47055833459ULL;

// states_provinces.txt, reference [14]: s13 values 8001 onwards.
constexpr std::array<std::string_view, 65> kStates = {
    "AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "HI", "ID", "IL",
    "IN", "IA", "KS", "KY", "LA", "ME", "MD", "MA", "MI", "MN", "MS", "MO", "MT",
    "NE", "NV", "NH", "NJ", "NM", "NY", "NC", "ND", "OH", "OK", "OR", "PA", "RI",
    "SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA", "WV", "WI", "WY", "NB", "NS",
    "QC", "ON", "MB", "SK", "AB", "BC", "NWT", "NF", "LB", "NU", "YT", "PEI", "DC"};

// arrl_rac_sections.txt, reference [14]: S7 values 0 to 83.
constexpr std::array<std::string_view, 84> kSections = {
    "AB",  "AK",  "AL",  "AR",  "AZ",  "BC",  "CO",  "CT",  "DE",  "EB",  "EMA", "ENY",
    "EPA", "EWA", "GA",  "GTA", "IA",  "ID",  "IL",  "IN",  "KS",  "KY",  "LA",  "LAX",
    "MAR", "MB",  "MDC", "ME",  "MI",  "MN",  "MO",  "MS",  "MT",  "NC",  "ND",  "NE",
    "NFL", "NH",  "NL",  "NLI", "NM",  "NNJ", "NNY", "NT",  "NTX", "NV",  "OH",  "OK",
    "ONE", "ONN", "ONS", "OR",  "ORG", "PAC", "PR",  "QC",  "RI",  "SB",  "SC",  "SCV",
    "SD",  "SDG", "SF",  "SFL", "SJV", "SK",  "SNJ", "STX", "SV",  "TN",  "UT",  "VA",
    "VI",  "VT",  "WCF", "WI",  "WMA", "WNY", "WPA", "WTX", "WV",  "WWA", "WY",  "DX"};

// The FT4 scrambler is FT4's, in core/decode/ft4.h, and is applied by the
// mode: a payload here is always the unscrambled one.

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ' ') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_letter(char c) { return c >= 'A' && c <= 'Z'; }

class BitWriter {
public:
    void put(std::uint64_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            out_[pos_++] = static_cast<std::uint8_t>((value >> i) & 1U);
        }
    }
    void put_bits(std::span<const std::uint8_t> b) {
        for (const auto v : b) {
            out_[pos_++] = v;
        }
    }
    [[nodiscard]] bool full() const { return pos_ == 77; }
    [[nodiscard]] const Wsjt77& bits() const { return out_; }

private:
    Wsjt77 out_{};
    std::size_t pos_ = 0;
};

class BitReader {
public:
    explicit BitReader(const Wsjt77& b) : b_(b) {}
    std::uint64_t get(int bits) {
        std::uint64_t v = 0;
        for (int i = 0; i < bits; ++i) {
            v = (v << 1) | b_[pos_++];
        }
        return v;
    }
    void skip(std::size_t n) { pos_ += n; }

private:
    const Wsjt77& b_;
    std::size_t pos_ = 0;
};

// Splits a /R or /P suffix off a call.
std::string strip_suffix(std::string_view call, char& suffix) {
    suffix = 0;
    if (call.size() > 2 && call[call.size() - 2] == '/' &&
        (call.back() == 'R' || call.back() == 'P')) {
        suffix = call.back();
        return std::string(call.substr(0, call.size() - 2));
    }
    return std::string(call);
}

// The six-position form std_call_to_c28 indexes, or nullopt.
std::optional<std::string> six_positions(std::string_view call) {
    if (call.size() < 3 || call.size() > 6) {
        return std::nullopt;
    }
    std::string s;
    if (is_digit(call[2])) {
        s = std::string(call);
    } else if (is_digit(call[1])) {
        s = " " + std::string(call);
    } else {
        return std::nullopt;
    }
    if (s.size() > 6) {
        return std::nullopt;
    }
    s.resize(6, ' ');
    if (kA1.find(s[0]) == std::string_view::npos || kA2.find(s[1]) == std::string_view::npos ||
        kA3.find(s[2]) == std::string_view::npos) {
        return std::nullopt;
    }
    // Section 2 via the User Guide: the prefix holds at least one letter.
    if (!is_letter(s[0]) && !is_letter(s[1])) {
        return std::nullopt;
    }
    bool blank = false;
    for (int i = 3; i < 6; ++i) {
        if (kA4.find(s[static_cast<std::size_t>(i)]) == std::string_view::npos) {
            return std::nullopt;
        }
        if (s[static_cast<std::size_t>(i)] == ' ') {
            blank = true;
        } else if (blank) {
            return std::nullopt;
        }
    }
    return s;
}

std::string fmt_report(int r) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%+03d", r);
    return buf;
}

std::string fmt_num(unsigned v, int width) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*u", width, v);
    return buf;
}

std::string trim(std::string s) {
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') {
        ++i;
    }
    return s.substr(i);
}

std::string hashed(std::uint32_t h, int bits, WsjtHashTable* hashes) {
    if (hashes != nullptr) {
        if (auto c = hashes->lookup(h, bits)) {
            return "<" + *c + ">";
        }
    }
    return "<...>";
}

// Table 7, inverted.
std::string c28_text(std::uint32_t n, WsjtHashTable* hashes) {
    if (n == 0) return "DE";
    if (n == 1) return "QRZ";
    if (n == 2) return "CQ";
    if (n <= 1002) return "CQ " + fmt_num(n - 3, 3);
    if (n >= 1004 && n <= 532443) {
        std::uint32_t v = n - 1003;
        std::string s;
        for (int i = 0; i < 4; ++i) {
            s.insert(s.begin(), kA4[v % 27]);
            v /= 27;
        }
        s = trim(s);
        if (s.empty() || s.find(' ') != std::string::npos) return "CQ <?>";
        return "CQ " + s;
    }
    if (n >= kWsjtNtokens && n < kWsjtNtokens + kWsjtMax22) {
        return hashed(n - kWsjtNtokens, 22, hashes);
    }
    if (n >= kWsjtNtokens + kWsjtMax22) {
        std::uint32_t v = n - kWsjtNtokens - kWsjtMax22;
        const std::uint32_t i6 = v % 27; v /= 27;
        const std::uint32_t i5 = v % 27; v /= 27;
        const std::uint32_t i4 = v % 27; v /= 27;
        const std::uint32_t i3 = v % 10; v /= 10;
        const std::uint32_t i2 = v % 36; v /= 36;
        const std::uint32_t i1 = v;
        if (i1 >= kA1.size()) return "<?>";
        std::string s{kA1[i1], kA2[i2], kA3[i3], kA4[i4], kA4[i5], kA4[i6]};
        s = trim(s);
        if (hashes != nullptr) hashes->remember(s);
        return s;
    }
    return "<?>";
}

std::string g15_text(std::uint32_t g, bool& is_grid) {
    is_grid = false;
    if (g < kWsjtMaxGrid4) {
        is_grid = true;
        std::string s(4, ' ');
        s[0] = static_cast<char>('A' + g / 1800);
        s[1] = static_cast<char>('A' + (g / 100) % 18);
        s[2] = static_cast<char>('0' + (g / 10) % 10);
        s[3] = static_cast<char>('0' + g % 10);
        return s;
    }
    const std::uint32_t irpt = g - kWsjtMaxGrid4;
    if (irpt == 1) return "";
    if (irpt == 2) return "RRR";
    if (irpt == 3) return "RR73";
    if (irpt == 4) return "73";
    if (irpt >= 5 && irpt <= 134) return fmt_report(static_cast<int>(irpt) - 35);
    return "<?>";
}

std::string g25_text(std::uint32_t g) {
    std::string s(6, ' ');
    const std::uint32_t sub = g % (24 * 24);
    const std::uint32_t g4 = g / (24 * 24);
    if (g4 >= kWsjtMaxGrid4) return "<?>";
    s[0] = static_cast<char>('A' + g4 / 1800);
    s[1] = static_cast<char>('A' + (g4 / 100) % 18);
    s[2] = static_cast<char>('0' + (g4 / 10) % 10);
    s[3] = static_cast<char>('0' + g4 % 10);
    s[4] = static_cast<char>('A' + sub / 24);
    s[5] = static_cast<char>('A' + sub % 24);
    return s;
}

std::string join(std::initializer_list<std::string> parts) {
    std::string out;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out.push_back(' ');
        out += p;
    }
    return out;
}

bool is_report_word(std::string_view w) {
    if (w.size() < 2 || w.size() > 3 || (w[0] != '+' && w[0] != '-')) return false;
    for (std::size_t i = 1; i < w.size(); ++i) {
        if (!is_digit(w[i])) return false;
    }
    return true;
}

bool is_grid4(std::string_view w) {
    return w.size() == 4 && w[0] >= 'A' && w[0] <= 'R' && w[1] >= 'A' && w[1] <= 'R' &&
           is_digit(w[2]) && is_digit(w[3]);
}

bool is_grid6(std::string_view w) {
    return w.size() == 6 && is_grid4(w.substr(0, 4)) && w[4] >= 'A' && w[4] <= 'X' &&
           w[5] >= 'A' && w[5] <= 'X';
}

bool is_bracketed(std::string_view w) {
    return w.size() > 2 && w.front() == '<' && w.back() == '>';
}

std::string unbracket(std::string_view w) {
    return is_bracketed(w) ? std::string(w.substr(1, w.size() - 2)) : std::string(w);
}

bool is_c58_call(std::string_view c) {
    if (c.empty() || c.size() > 11) return false;
    return std::all_of(c.begin(), c.end(),
                       [](char ch) { return ch != ' ' && kC38.find(ch) != std::string_view::npos; });
}

}  // namespace

std::uint32_t wsjt_hash(std::string_view call, int bits) {
    std::string s = upper(call);
    s.resize(11, ' ');
    std::uint64_t n8 = 0;
    for (const char c : s) {
        auto j = kC38.find(c);
        if (j == std::string_view::npos) j = 0;
        n8 = 38 * n8 + j;
    }
    return static_cast<std::uint32_t>((kNprime * n8) >> (64 - bits));
}

void WsjtHashTable::remember(std::string_view call) {
    const std::string c = trim(upper(call));
    if (c.empty() || c.front() == '<') return;
    h10_[wsjt_hash(c, 10)] = c;
    h12_[wsjt_hash(c, 12)] = c;
    h22_[wsjt_hash(c, 22)] = c;
}

std::optional<std::string> WsjtHashTable::lookup(std::uint32_t hash, int bits) const {
    const auto& m = bits == 10 ? h10_ : bits == 12 ? h12_ : h22_;
    const auto it = m.find(hash);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

bool wsjt_is_standard_call(std::string_view call) {
    char suffix = 0;
    return six_positions(strip_suffix(upper(call), suffix)).has_value();
}

Expected<std::uint32_t> wsjt_c28(std::string_view word_in) {
    const std::string w = trim(upper(word_in));
    if (w == "DE") return 0U;
    if (w == "QRZ") return 1U;
    if (w == "CQ") return 2U;
    if (w.rfind("CQ ", 0) == 0) {
        const std::string mod = w.substr(3);
        if (mod.size() == 3 && std::all_of(mod.begin(), mod.end(), is_digit)) {
            return 3U + static_cast<std::uint32_t>(std::stoi(mod));
        }
        if (!mod.empty() && mod.size() <= 4 && std::all_of(mod.begin(), mod.end(), is_letter)) {
            std::uint32_t v = 0;
            for (const char c : mod) v = 27 * v + static_cast<std::uint32_t>(c - 'A' + 1);
            return 1003U + v;
        }
        return fail("CQ modifier must be three digits or one to four letters: " + w);
    }
    if (is_bracketed(w)) {
        return kWsjtNtokens + wsjt_hash(unbracket(w), 22);
    }
    const auto s = six_positions(w);
    if (!s) return fail("not a standard call sign: " + w);
    const auto i1 = static_cast<std::uint32_t>(kA1.find((*s)[0]));
    const auto i2 = static_cast<std::uint32_t>(kA2.find((*s)[1]));
    const auto i3 = static_cast<std::uint32_t>(kA3.find((*s)[2]));
    const auto i4 = static_cast<std::uint32_t>(kA4.find((*s)[3]));
    const auto i5 = static_cast<std::uint32_t>(kA4.find((*s)[4]));
    const auto i6 = static_cast<std::uint32_t>(kA4.find((*s)[5]));
    return kWsjtNtokens + kWsjtMax22 + 36U * 10U * 27U * 27U * 27U * i1 +
           10U * 27U * 27U * 27U * i2 + 27U * 27U * 27U * i3 + 27U * 27U * i4 + 27U * i5 + i6;
}

Expected<std::uint32_t> wsjt_g15(std::string_view word_in) {
    const std::string w = trim(upper(word_in));
    if (is_grid4(w) && w != "RR73") {
        return static_cast<std::uint32_t>((w[0] - 'A') * 18 * 10 * 10 + (w[1] - 'A') * 10 * 10 +
                                          (w[2] - '0') * 10 + (w[3] - '0'));
    }
    int irpt = 0;
    if (w.empty()) irpt = 1;
    else if (w == "RRR") irpt = 2;
    else if (w == "RR73") irpt = 3;
    else if (w == "73") irpt = 4;
    else if (is_report_word(w)) {
        const int r = std::stoi(w);
        if (r < -30 || r > 99) return fail("report out of range: " + w);
        irpt = r + 35;
    } else {
        return fail("not a locator, report, RRR, RR73 or 73: " + w);
    }
    return kWsjtMaxGrid4 + static_cast<std::uint32_t>(irpt);
}

Expected<std::uint32_t> wsjt_g25(std::string_view grid_in) {
    const std::string w = upper(grid_in);
    if (!is_grid6(w)) return fail("not a six-character locator: " + w);
    return static_cast<std::uint32_t>((w[0] - 'A') * 18 * 10 * 10 * 24 * 24 +
                                      (w[1] - 'A') * 10 * 10 * 24 * 24 +
                                      (w[2] - '0') * 10 * 24 * 24 + (w[3] - '0') * 24 * 24 +
                                      (w[4] - 'A') * 24 + (w[5] - 'A'));
}

Expected<std::uint64_t> wsjt_c58(std::string_view call_in) {
    const std::string c = upper(call_in);
    if (!is_c58_call(c)) return fail("not a call of up to 11 characters: " + c);
    std::string s = c;
    s.resize(11, ' ');
    std::uint64_t n = 0;
    for (const char ch : s) n = n * 38 + kC38.find(ch);
    return n;
}

Expected<std::array<std::uint8_t, 71>> wsjt_f71(std::string_view text_in) {
    const std::string t = upper(text_in);
    if (t.size() > 13) return fail("free text is at most 13 characters");
    std::string w(13 - t.size(), ' ');
    w += t;
    // Three 32-bit limbs, least significant first; 42^13 < 2^71.
    std::uint32_t limb[3] = {0, 0, 0};
    for (const char c : w) {
        const auto j = kC42.find(c);
        if (j == std::string_view::npos) return fail("character not in the free text alphabet");
        std::uint64_t carry = j;
        for (auto& l : limb) {
            const std::uint64_t v = static_cast<std::uint64_t>(l) * 42U + carry;
            l = static_cast<std::uint32_t>(v);
            carry = v >> 32;
        }
    }
    std::array<std::uint8_t, 71> out{};
    for (int i = 0; i < 71; ++i) {
        const int bit = 70 - i;
        out[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((limb[bit / 32] >> (bit % 32)) & 1U);
    }
    return out;
}

Expected<Wsjt77> wsjt_pack_telemetry(std::string_view hex_in) {
    std::string h = upper(trim(std::string(hex_in)));
    if (h.empty() || h.size() > 18 ||
        !std::all_of(h.begin(), h.end(), [](char c) { return is_digit(c) || (c >= 'A' && c <= 'F'); })) {
        return fail("telemetry is one to 18 hexadecimal digits");
    }
    h.insert(h.begin(), 18 - h.size(), '0');
    if (h[0] > '7') return fail("with 18 digits the first must be 0 to 7");
    BitWriter w;
    for (std::size_t i = 0; i < 18; ++i) {
        const char c = h[i];
        const unsigned v = is_digit(c) ? static_cast<unsigned>(c - '0') : static_cast<unsigned>(c - 'A' + 10);
        w.put(v, i == 0 ? 3 : 4);
    }
    w.put(5, 3);  // n3
    w.put(0, 3);  // i3
    return w.bits();
}

Expected<Wsjt77> wsjt_pack(std::string_view text_in) {
    const std::string text = trim(upper(text_in));
    auto tok = split(text);

    const auto free_text = [&]() -> Expected<Wsjt77> {
        auto f = wsjt_f71(text);
        if (!f) return std::unexpected(f.error());
        BitWriter w;
        w.put_bits(*f);
        w.put(0, 3);
        w.put(0, 3);
        return w.bits();
    };

    // Type 0.1: "K1ABC RR73; W9XYZ <KH1/KH7Z> -08".
    if (tok.size() == 5 && tok[1] == "RR73;" && is_bracketed(tok[3]) && is_report_word(tok[4])) {
        auto a = wsjt_c28(tok[0]);
        auto b = wsjt_c28(tok[2]);
        const int r = std::stoi(tok[4]);
        if (a && b && *a >= kWsjtNtokens + kWsjtMax22 && *b >= kWsjtNtokens + kWsjtMax22 &&
            r >= -30 && r <= 32 && r % 2 == 0) {
            BitWriter w;
            w.put(*a, 28);
            w.put(*b, 28);
            w.put(wsjt_hash(unbracket(tok[3]), 10), 10);
            w.put(static_cast<std::uint64_t>((r + 30) / 2), 5);
            w.put(1, 3);
            w.put(0, 3);
            return w.bits();
        }
    }

    // Type 3: "[TU;] K1ABC W9XYZ [R] 579 WI".
    {
        std::vector<std::string> t = tok;
        bool tu = false;
        if (!t.empty() && t[0] == "TU;") {
            tu = true;
            t.erase(t.begin());
        }
        bool roger = false;
        if (t.size() == 5 && t[2] == "R") {
            roger = true;
            t.erase(t.begin() + 2);
        }
        if (t.size() == 4 && t[2].size() == 3 && t[2][0] == '5' && t[2][2] == '9' &&
            t[2][1] >= '2' && t[2][1] <= '9' && wsjt_is_standard_call(t[0]) &&
            wsjt_is_standard_call(t[1])) {
            int s13 = -1;
            if (std::all_of(t[3].begin(), t[3].end(), is_digit) && !t[3].empty() && t[3].size() <= 4) {
                s13 = std::stoi(t[3]);
            } else {
                for (std::size_t i = 0; i < kStates.size(); ++i) {
                    if (kStates[i] == t[3]) s13 = 8001 + static_cast<int>(i);
                }
            }
            auto a = wsjt_c28(t[0]);
            auto b = wsjt_c28(t[1]);
            if (s13 >= 0 && s13 <= 8065 && s13 != 8000 && a && b) {
                BitWriter w;
                w.put(tu ? 1 : 0, 1);
                w.put(*a, 28);
                w.put(*b, 28);
                w.put(roger ? 1 : 0, 1);
                w.put(static_cast<std::uint64_t>(t[2][1] - '2'), 3);
                w.put(static_cast<std::uint64_t>(s13), 13);
                w.put(3, 3);
                return w.bits();
            }
        }
    }

    // Types 0.3 and 0.4: "K1ABC W9XYZ [R] 6A WI".
    {
        std::vector<std::string> t = tok;
        bool roger = false;
        if (t.size() == 5 && t[2] == "R") {
            roger = true;
            t.erase(t.begin() + 2);
        }
        if (t.size() == 4 && t[2].size() >= 2 && t[2].size() <= 3 && t[2].back() >= 'A' &&
            t[2].back() <= 'F' &&
            std::all_of(t[2].begin(), t[2].end() - 1, is_digit) && wsjt_is_standard_call(t[0]) &&
            wsjt_is_standard_call(t[1])) {
            const int n = std::stoi(t[2].substr(0, t[2].size() - 1));
            int sec = -1;
            for (std::size_t i = 0; i < kSections.size(); ++i) {
                if (kSections[i] == t[3]) sec = static_cast<int>(i);
            }
            auto a = wsjt_c28(t[0]);
            auto b = wsjt_c28(t[1]);
            if (n >= 1 && n <= 32 && sec >= 0 && a && b) {
                BitWriter w;
                w.put(*a, 28);
                w.put(*b, 28);
                w.put(roger ? 1 : 0, 1);
                w.put(static_cast<std::uint64_t>(n <= 16 ? n - 1 : n - 17), 4);
                w.put(static_cast<std::uint64_t>(t[2].back() - 'A'), 3);
                w.put(static_cast<std::uint64_t>(sec), 7);
                w.put(n <= 16 ? 3 : 4, 3);
                w.put(0, 3);
                return w.bits();
            }
        }
    }

    // Type 5: "<G4ABC> <PA9XYZ> [R] 570007 JO22DB".
    {
        std::vector<std::string> t = tok;
        bool roger = false;
        if (t.size() == 5 && t[2] == "R") {
            roger = true;
            t.erase(t.begin() + 2);
        }
        if (t.size() == 4 && is_bracketed(t[0]) && is_bracketed(t[1]) && t[2].size() == 6 &&
            std::all_of(t[2].begin(), t[2].end(), is_digit) && t[2][0] == '5' &&
            t[2][1] >= '2' && is_grid6(t[3])) {
            const int serial = std::stoi(t[2].substr(2));
            if (serial <= 2047) {
                BitWriter w;
                w.put(wsjt_hash(unbracket(t[0]), 12), 12);
                w.put(wsjt_hash(unbracket(t[1]), 22), 22);
                w.put(roger ? 1 : 0, 1);
                w.put(static_cast<std::uint64_t>(t[2][1] - '2'), 3);
                w.put(static_cast<std::uint64_t>(serial), 11);
                w.put(*wsjt_g25(t[3]), 25);
                w.put(5, 3);
                return w.bits();
            }
        }
    }

    // Types 1, 2 and 4: "C1 C2 [R] [word]".
    if (tok.size() >= 2 && tok.size() <= 5) {
        std::vector<std::string> t = tok;
        // "CQ nnn" and "CQ ABCD" are one c28 word.
        if (t[0] == "CQ" && t.size() >= 3 && !wsjt_is_standard_call(t[1]) &&
            (std::all_of(t[1].begin(), t[1].end(), is_digit) ||
             std::all_of(t[1].begin(), t[1].end(), is_letter)) &&
            t[1].size() <= 4) {
            t[0] = "CQ " + t[1];
            t.erase(t.begin() + 1);
        }
        bool roger = false;
        std::string word;
        if (t.size() == 4 && t[2] == "R") {
            roger = true;
            word = t[3];
        } else if (t.size() == 3 && t[2].size() > 1 && t[2][0] == 'R' &&
                   (t[2][1] == '+' || t[2][1] == '-')) {
            roger = true;
            word = t[2].substr(1);
        } else if (t.size() == 3) {
            word = t[2];
        } else if (t.size() != 2) {
            return free_text();
        }
        const bool cq_like = t[0] == "CQ" || t[0] == "DE" || t[0] == "QRZ" || t[0].rfind("CQ ", 0) == 0;
        char s1 = 0;
        char s2 = 0;
        const std::string c1 = cq_like ? t[0] : strip_suffix(t[0], s1);
        const std::string c2 = strip_suffix(t[1], s2);
        const bool std1 = cq_like || six_positions(c1).has_value() || is_bracketed(c1);
        const bool std2 = six_positions(c2).has_value() || is_bracketed(c2);
        auto g = wsjt_g15(word);
        if (std1 && std2 && g && !(is_bracketed(c1) && is_bracketed(c2)) &&
            !((s1 == 'R' && s2 == 'P') || (s1 == 'P' && s2 == 'R')) &&
            !(roger && word.empty())) {
            auto a = wsjt_c28(c1);
            auto b = wsjt_c28(c2);
            if (a && b) {
                const bool p = s1 == 'P' || s2 == 'P';
                BitWriter w;
                w.put(*a, 28);
                w.put(s1 != 0 ? 1 : 0, 1);
                w.put(*b, 28);
                w.put(s2 != 0 ? 1 : 0, 1);
                w.put(roger ? 1 : 0, 1);
                w.put(*g, 15);
                w.put(p ? 2 : 1, 3);
                return w.bits();
            }
        }
        // Type 4: one nonstandard call, the other hashed or CQ.
        if (!roger) {
            int r2 = -1;
            if (word.empty()) r2 = 0;
            else if (word == "RRR") r2 = 1;
            else if (word == "RR73") r2 = 2;
            else if (word == "73") r2 = 3;
            if (r2 >= 0) {
                BitWriter w;
                if (t[0] == "CQ" && is_c58_call(t[1]) && r2 == 0) {
                    w.put(0, 12);
                    w.put(*wsjt_c58(t[1]), 58);
                    w.put(0, 1);
                    w.put(0, 2);
                    w.put(1, 1);
                    w.put(4, 3);
                    return w.bits();
                }
                if (is_bracketed(t[0]) && is_c58_call(t[1])) {
                    w.put(wsjt_hash(unbracket(t[0]), 12), 12);
                    w.put(*wsjt_c58(t[1]), 58);
                    w.put(0, 1);
                    w.put(static_cast<std::uint64_t>(r2), 2);
                    w.put(0, 1);
                    w.put(4, 3);
                    return w.bits();
                }
                if (is_bracketed(t[1]) && is_c58_call(t[0])) {
                    w.put(wsjt_hash(unbracket(t[1]), 12), 12);
                    w.put(*wsjt_c58(t[0]), 58);
                    w.put(1, 1);
                    w.put(static_cast<std::uint64_t>(r2), 2);
                    w.put(0, 1);
                    w.put(4, 3);
                    return w.bits();
                }
            }
        }
    }
    return free_text();
}

WsjtMessage wsjt_unpack(const Wsjt77& bits, WsjtHashTable* hashes) {
    WsjtMessage m;
    BitReader rd(bits);
    m.i3 = (bits[74] << 2) | (bits[75] << 1) | bits[76];
    m.n3 = (bits[71] << 2) | (bits[72] << 1) | bits[73];

    if (m.i3 == 0 && m.n3 == 0) {
        // f71: 71 bits to 13 characters of base 42.
        std::uint32_t limb[3] = {0, 0, 0};
        for (int i = 0; i < 71; ++i) {
            const int bit = 70 - i;
            limb[bit / 32] |= static_cast<std::uint32_t>(bits[static_cast<std::size_t>(i)]) << (bit % 32);
        }
        std::string s(13, ' ');
        for (int k = 12; k >= 0; --k) {
            std::uint64_t rem = 0;
            for (int l = 2; l >= 0; --l) {
                const std::uint64_t v = (rem << 32) | limb[l];
                limb[l] = static_cast<std::uint32_t>(v / 42);
                rem = v % 42;
            }
            s[static_cast<std::size_t>(k)] = kC42[rem];
        }
        m.free_text = trim(s);
        m.text = m.free_text;
        return m;
    }
    if (m.i3 == 0 && m.n3 == 1) {
        m.call_to = c28_text(static_cast<std::uint32_t>(rd.get(28)), hashes);
        m.call_third = c28_text(static_cast<std::uint32_t>(rd.get(28)), hashes);
        m.call_de = hashed(static_cast<std::uint32_t>(rd.get(10)), 10, hashes);
        m.report = fmt_report(static_cast<int>(rd.get(5)) * 2 - 30);
        m.text = m.call_to + " RR73; " + m.call_third + " " + m.call_de + " " + m.report;
        return m;
    }
    if (m.i3 == 0 && (m.n3 == 3 || m.n3 == 4)) {
        m.call_to = c28_text(static_cast<std::uint32_t>(rd.get(28)), hashes);
        m.call_de = c28_text(static_cast<std::uint32_t>(rd.get(28)), hashes);
        m.roger = rd.get(1) != 0;
        const int n = static_cast<int>(rd.get(4)) + (m.n3 == 3 ? 1 : 17);
        const char cls = static_cast<char>('A' + rd.get(3));
        const auto sec = rd.get(7);
        m.exchange = std::to_string(n) + cls + " " +
                     (sec < kSections.size() ? std::string(kSections[sec]) : "<?>");
        m.text = join({m.call_to, m.call_de, m.roger ? "R" : "", m.exchange});
        return m;
    }
    if (m.i3 == 0 && m.n3 == 5) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string h;
        h.push_back(kHex[rd.get(3)]);
        for (int i = 1; i < 18; ++i) h.push_back(kHex[rd.get(4)]);
        const auto nz = h.find_first_not_of('0');
        m.telemetry = nz == std::string::npos ? "0" : h.substr(nz);
        m.text = m.telemetry;
        return m;
    }
    if (m.i3 == 1 || m.i3 == 2) {
        const char suffix = m.i3 == 1 ? 'R' : 'P';
        m.call_to = c28_text(static_cast<std::uint32_t>(rd.get(28)), hashes);
        if (rd.get(1) != 0) m.call_to += std::string("/") + suffix;
        m.call_de = c28_text(static_cast<std::uint32_t>(rd.get(28)), hashes);
        if (rd.get(1) != 0) m.call_de += std::string("/") + suffix;
        m.roger = rd.get(1) != 0;
        bool is_grid = false;
        const std::string w = g15_text(static_cast<std::uint32_t>(rd.get(15)), is_grid);
        if (is_grid) {
            m.grid = w;
            m.text = join({m.call_to, m.call_de, m.roger ? "R" : "", w});
        } else {
            m.report = w;
            m.text = join({m.call_to, m.call_de, (m.roger ? "R" : "") + w});
        }
        return m;
    }
    if (m.i3 == 3) {
        const bool tu = rd.get(1) != 0;
        m.call_to = c28_text(static_cast<std::uint32_t>(rd.get(28)), hashes);
        m.call_de = c28_text(static_cast<std::uint32_t>(rd.get(28)), hashes);
        m.roger = rd.get(1) != 0;
        m.report = "5" + std::to_string(rd.get(3) + 2) + "9";
        const auto s13 = static_cast<unsigned>(rd.get(13));
        std::string ex;
        if (s13 <= 7999) ex = fmt_num(s13, 4);
        else if (s13 >= 8001 && s13 <= 8065) ex = std::string(kStates[s13 - 8001]);
        else ex = "<?>";
        m.exchange = m.report + " " + ex;
        m.text = join({tu ? "TU;" : "", m.call_to, m.call_de, m.roger ? "R" : "", m.exchange});
        return m;
    }
    if (m.i3 == 4) {
        const auto h12 = static_cast<std::uint32_t>(rd.get(12));
        const auto n58 = rd.get(58);
        const bool second = rd.get(1) != 0;
        const auto r2 = rd.get(2);
        const bool cq = rd.get(1) != 0;
        std::string s(11, ' ');
        std::uint64_t v = n58;
        for (int i = 10; i >= 0; --i) {
            s[static_cast<std::size_t>(i)] = kC38[v % 38];
            v /= 38;
        }
        const std::string full = trim(s);
        if (hashes != nullptr) hashes->remember(full);
        static constexpr const char* kR2[] = {"", "RRR", "RR73", "73"};
        m.report = kR2[r2];
        if (cq) {
            m.call_to = "CQ";
            m.call_de = full;
        } else {
            const std::string h = hashed(h12, 12, hashes);
            m.call_to = second ? full : h;
            m.call_de = second ? h : full;
        }
        m.text = join({m.call_to, m.call_de, m.report});
        return m;
    }
    if (m.i3 == 5) {
        m.call_to = hashed(static_cast<std::uint32_t>(rd.get(12)), 12, hashes);
        m.call_de = hashed(static_cast<std::uint32_t>(rd.get(22)), 22, hashes);
        m.roger = rd.get(1) != 0;
        m.report = "5" + std::to_string(rd.get(3) + 2);
        const auto serial = static_cast<unsigned>(rd.get(11));
        m.grid = g25_text(static_cast<std::uint32_t>(rd.get(25)));
        m.exchange = m.report + fmt_num(serial, 4);
        m.text = join({m.call_to, m.call_de, m.roger ? "R" : "", m.exchange, m.grid});
        return m;
    }
    m.text = "<unknown message type " + std::to_string(m.i3) + "." + std::to_string(m.n3) + ">";
    return m;
}

std::array<std::uint8_t, 14> wsjt_crc14(std::span<const std::uint8_t> bits77) {
    // gen_crc14.f90, transcribed with zero-based indices.
    static constexpr std::uint8_t kPoly[15] = {1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1};
    std::uint8_t mc[96] = {};
    for (std::size_t i = 0; i < 77 && i < bits77.size(); ++i) mc[i] = bits77[i] & 1U;
    std::uint8_t r[15];
    for (int i = 0; i < 15; ++i) r[i] = mc[i];
    for (int i = 0; i <= 81; ++i) {
        r[14] = mc[i + 14];
        if (r[0] != 0) {
            for (int j = 0; j < 15; ++j) r[j] ^= kPoly[j];
        }
        const std::uint8_t first = r[0];
        for (int j = 0; j < 14; ++j) r[j] = r[j + 1];
        r[14] = first;
    }
    std::array<std::uint8_t, 14> out{};
    for (int i = 0; i < 14; ++i) out[static_cast<std::size_t>(i)] = r[i];
    return out;
}

}  // namespace revenant::decode
