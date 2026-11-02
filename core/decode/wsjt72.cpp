#include "core/decode/wsjt72.h"

#include <format>
#include <sstream>
#include <vector>

namespace revenant::decode {

namespace {

// "The WSPR Coding Process", "Source Coding": '0'-'9' are 0-9, 'A'-'Z'
// 10-35, space 36.
int call_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c == ' ') return 36;
    return -1;
}

char call_char(int v) {
    if (v < 10) return static_cast<char>('0' + v);
    if (v < 36) return static_cast<char>('A' + v - 10);
    return ' ';
}

// "The JT9 Coding Process", "Source Coding": digits, letters, then
// space + - . / ? as 36 to 41.
constexpr std::string_view kText = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?";

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

Wsjt72 assemble(std::uint32_t n1, std::uint32_t n2, std::uint32_t n3) {
    Wsjt72 b{};
    for (int i = 0; i < 28; ++i) b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((n1 >> (27 - i)) & 1U);
    for (int i = 0; i < 28; ++i) b[static_cast<std::size_t>(28 + i)] = static_cast<std::uint8_t>((n2 >> (27 - i)) & 1U);
    for (int i = 0; i < 16; ++i) b[static_cast<std::size_t>(56 + i)] = static_cast<std::uint8_t>((n3 >> (15 - i)) & 1U);
    return b;
}

std::uint32_t field(const Wsjt72& b, int first, int n) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1U) | b[static_cast<std::size_t>(first + i)];
    return v;
}

}  // namespace

std::optional<std::uint32_t> wsjt_call28(std::string_view call_in) {
    std::string call = upper(call_in);
    if (call.size() >= 2 && call[1] >= '0' && call[1] <= '9') call.insert(call.begin(), ' ');
    if (call.size() < 3 || call.size() > 6) return std::nullopt;
    call.resize(6, ' ');
    const int c1 = call_value(call[0]);
    const int c2 = call_value(call[1]);
    const int c3 = call_value(call[2]);
    if (c1 < 0 || c2 < 0 || c2 == 36 || c3 < 0 || c3 > 9) return std::nullopt;
    std::uint32_t n = static_cast<std::uint32_t>(c1);
    n = n * 36 + static_cast<std::uint32_t>(c2);
    n = n * 10 + static_cast<std::uint32_t>(c3);
    bool ended = false;
    for (int i = 3; i < 6; ++i) {
        const int c = call_value(call[static_cast<std::size_t>(i)]);
        if (c < 10) return std::nullopt;
        if (c == 36) ended = true;
        else if (ended) return std::nullopt;
        n = 27 * n + static_cast<std::uint32_t>(c - 10);
    }
    return n;
}

std::optional<std::string> wsjt_call28_text(std::uint32_t n) {
    if (n >= 262177560U) return std::nullopt;
    std::string call(6, ' ');
    for (int i = 5; i >= 3; --i) {
        call[static_cast<std::size_t>(i)] = call_char(static_cast<int>(n % 27) + 10);
        n /= 27;
    }
    call[2] = call_char(static_cast<int>(n % 10));
    n /= 10;
    call[1] = call_char(static_cast<int>(n % 36));
    n /= 36;
    call[0] = call_char(static_cast<int>(n));
    const auto first = call.find_first_not_of(' ');
    const auto last = call.find_last_not_of(' ');
    if (first == std::string::npos || first > 1) return std::nullopt;
    std::string t = call.substr(first, last - first + 1);
    if (t.find(' ') != std::string::npos) return std::nullopt;
    return t;
}

std::optional<std::uint32_t> wsjt_grid15(std::string_view grid_in) {
    const std::string g = upper(grid_in);
    if (g.size() != 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || g[2] < '0' || g[2] > '9' ||
        g[3] < '0' || g[3] > '9') {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>((179 - 10 * (g[0] - 'A') - (g[2] - '0')) * 180 + 10 * (g[1] - 'A') + (g[3] - '0'));
}

std::optional<std::string> wsjt_grid15_text(std::uint32_t m1) {
    if (m1 < 179 || m1 > 32220) return std::nullopt;
    const int q = 179 - static_cast<int>(m1 / 180);
    const int r = static_cast<int>(m1 % 180);
    if (q / 10 > 17 || r / 10 > 17) return std::nullopt;
    return std::string{static_cast<char>('A' + q / 10), static_cast<char>('A' + r / 10), static_cast<char>('0' + q % 10),
                       static_cast<char>('0' + r % 10)};
}

Expected<Wsjt72> wsjt72_pack_text(std::string_view text_in) {
    std::string t = upper(text_in);
    if (t.size() > 13) return fail(std::format("plain text \"{}\" is longer than 13 characters", text_in));
    t.resize(13, ' ');
    int v[13];
    for (int i = 0; i < 13; ++i) {
        const auto pos = kText.find(t[static_cast<std::size_t>(i)]);
        if (pos == std::string_view::npos) return fail(std::format("plain text \"{}\" has a character outside 0-9 A-Z space + - . / ?", text_in));
        v[i] = static_cast<int>(pos);
    }
    std::uint32_t n1 = 0;
    std::uint32_t n2 = 0;
    std::uint32_t n3 = 0;
    for (int i = 0; i < 5; ++i) n1 = n1 * 42 + static_cast<std::uint32_t>(v[i]);
    for (int i = 5; i < 10; ++i) n2 = n2 * 42 + static_cast<std::uint32_t>(v[i]);
    for (int i = 10; i < 13; ++i) n3 = n3 * 42 + static_cast<std::uint32_t>(v[i]);
    n1 = n1 * 2 + ((n3 >> 15) & 1U);
    n2 = n2 * 2 + ((n3 >> 16) & 1U);
    n3 = (n3 & 32767U) + 32768U;
    return assemble(n1, n2, n3);
}

Expected<Wsjt72> wsjt72_pack_standard(std::string_view call1, std::string_view call2, std::string_view grid) {
    const auto a = wsjt_call28(call1);
    const auto b = wsjt_call28(call2);
    const auto g = wsjt_grid15(grid);
    if (!a || !b || !g) return fail(std::format("\"{} {} {}\" is not two standard callsigns and a locator", call1, call2, grid));
    return assemble(*a, *b, *g);
}

Expected<Wsjt72> wsjt72_pack(std::string_view message) {
    std::istringstream in{std::string(message)};
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    if (words.size() == 3) {
        auto s = wsjt72_pack_standard(words[0], words[1], words[2]);
        if (s) return s;
    }
    return wsjt72_pack_text(message);
}

Wsjt72Message wsjt72_unpack(const Wsjt72& bits) {
    std::uint32_t n1 = field(bits, 0, 28);
    std::uint32_t n2 = field(bits, 28, 28);
    std::uint32_t n3 = field(bits, 56, 16);
    Wsjt72Message m;
    if (n3 & 32768U) {
        m.plain_text = true;
        m.standard = true;
        n3 = (n3 & 32767U) | ((n1 & 1U) << 15) | ((n2 & 1U) << 16);
        n1 >>= 1;
        n2 >>= 1;
        char out[13];
        for (int i = 4; i >= 0; --i) {
            out[i] = kText[n1 % 42];
            n1 /= 42;
        }
        for (int i = 9; i >= 5; --i) {
            out[i] = kText[n2 % 42];
            n2 /= 42;
        }
        for (int i = 12; i >= 10; --i) {
            out[i] = kText[n3 % 42];
            n3 /= 42;
        }
        // Values past 42^5 cannot come from a transmitter that follows the
        // document; they still unpack, but are not standard.
        if (n1 != 0 || n2 != 0 || n3 != 0) m.standard = false;
        m.text.assign(out, 13);
        const auto last = m.text.find_last_not_of(' ');
        m.text.erase(last == std::string::npos ? 0 : last + 1);
        return m;
    }
    const auto a = wsjt_call28_text(n1);
    const auto b = wsjt_call28_text(n2);
    const auto g = wsjt_grid15_text(n3);
    m.standard = a && b && g;
    m.text = std::format("{} {} {}", a ? *a : std::format("<{}>", n1), b ? *b : std::format("<{}>", n2),
                         g ? *g : std::format("<{}>", n3));
    return m;
}

}  // namespace revenant::decode
