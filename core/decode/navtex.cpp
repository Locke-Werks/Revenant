#include "core/decode/navtex.h"

namespace revenant::decode {

namespace {

// Stands in the tail for a character lost in both copies, so a loss inside
// "ZCZC" or "NNNN" breaks the match rather than being read as a letter.
constexpr char kLostMarker = '\x01';

void append_utf8(std::string& out, char32_t c) {
    const auto v = static_cast<std::uint32_t>(c);
    if (v < 0x80U) {
        out.push_back(static_cast<char>(v));
    } else if (v < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (v >> 6U)));
        out.push_back(static_cast<char>(0x80U | (v & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xE0U | (v >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((v >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (v & 0x3FU)));
    }
}

bool line_end(const SitorCharacter& c) { return !c.mutilated && (c.glyph == U'\r' || c.glyph == U'\n'); }

bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Removes the last `n` code points' worth of bytes from a UTF-8 string of
// which the last `n` characters are known to be ASCII.
void drop_ascii_tail(std::string& s, std::size_t n) { s.resize(s.size() >= n ? s.size() - n : 0); }

}  // namespace

Expected<NavtexDecoder> NavtexDecoder::create(const NavtexConfig& config) {
    SitorConfig sitor = config.sitor;
    // See SitorConfig::wait_for_line_end: M.540-2 Annex II Figure 1 has
    // "ZCZC" follow phasing directly.
    sitor.wait_for_line_end = false;
    auto decoder = SitorBDecoder::create(sitor);
    if (!decoder) {
        return std::unexpected(with_context(decoder.error(), "NAVTEX"));
    }
    NavtexDecoder d;
    d.config_ = config;
    d.sitor_ = std::move(*decoder);
    d.reset();
    return d;
}

void NavtexDecoder::reset() {
    sitor_->reset();
    characters_.clear();
    tail_.clear();
    tail_positions_.clear();
    state_ = State::Hunt;
    current_ = {};
    preamble_.clear();
    body_characters_ = 0;
    body_started_ = false;
    phasing_ = 0;
}

void NavtexDecoder::process(ConstRealSpan audio, std::vector<NavtexMessage>& out) {
    characters_.clear();
    sitor_->process(audio, characters_);
    for (const SitorCharacter& c : characters_) {
        // A character from a later phasing than the message in progress
        // means that message was cut off, by a loss of phase or by the end
        // of its transmission, before its "NNNN".
        if (c.phasing != phasing_) {
            flush(out);
            phasing_ = c.phasing;
        }
        on_character(c, out);
    }
}

void NavtexDecoder::flush(std::vector<NavtexMessage>& out) {
    if (state_ != State::Hunt) {
        current_.complete = false;
        out.push_back(std::move(current_));
    }
    state_ = State::Hunt;
    current_ = {};
    preamble_.clear();
    tail_.clear();
    tail_positions_.clear();
    body_characters_ = 0;
    body_started_ = false;
}

void NavtexDecoder::on_character(const SitorCharacter& c, std::vector<NavtexMessage>& out) {
    // Shifts print nothing and take no part in the framing.
    if (!c.mutilated && c.glyph == 0) {
        return;
    }

    // The last four printing characters.
    const char key = c.mutilated ? kLostMarker
                                 : (c.glyph < 0x80 ? static_cast<char>(c.glyph) : kLostMarker);
    tail_.push_back(key);
    tail_positions_.push_back(c.position);
    if (tail_.size() > 4) {
        tail_.erase(tail_.begin());
        tail_positions_.erase(tail_positions_.begin());
    }

    // Figure 1: "ZCZC" opens a message wherever it arrives. One arriving
    // inside a message means the "NNNN" before it was lost.
    if (ends_with(tail_, "ZCZC")) {
        if (state_ == State::Body || state_ == State::Preamble) {
            if (state_ == State::Body) {
                drop_ascii_tail(current_.text, 3);
            }
            current_.complete = false;
            out.push_back(std::move(current_));
        }
        current_ = {};
        current_.position = tail_positions_.front();
        preamble_.clear();
        body_characters_ = 0;
        body_started_ = false;
        state_ = State::Preamble;
        return;
    }

    if (state_ == State::Hunt) {
        return;
    }

    if (state_ == State::Preamble) {
        if (!line_end(c)) {
            preamble_.push_back(c);
            // A preamble with no line end in any reasonable length is not
            // one: read on as message text rather than waiting forever.
            if (preamble_.size() <= 8) {
                return;
            }
        }
        // Figure 1: one space, then B1 B2 B3 B4, then carriage return and
        // line feed.
        std::vector<SitorCharacter> b;
        bool had_space = false;
        for (const SitorCharacter& p : preamble_) {
            if (!p.mutilated && p.glyph == U' ' && b.empty()) {
                had_space = true;
                continue;
            }
            b.push_back(p);
        }
        if (b.size() >= 4) {
            const auto letter = [](const SitorCharacter& x) {
                return !x.mutilated && x.glyph >= U'A' && x.glyph <= U'Z';
            };
            const auto digit = [](const SitorCharacter& x) {
                return !x.mutilated && x.glyph >= U'0' && x.glyph <= U'9';
            };
            current_.area = letter(b[0]) ? static_cast<char>(b[0].glyph) : '?';
            current_.subject = letter(b[1]) ? static_cast<char>(b[1].glyph) : '?';
            if (digit(b[2]) && digit(b[3])) {
                current_.serial = static_cast<int>((b[2].glyph - U'0') * 10 + (b[3].glyph - U'0'));
            }
            // Clause 3.
            current_.preamble_clean = had_space && b.size() == 4 && letter(b[0]) && letter(b[1]) &&
                                      digit(b[2]) && digit(b[3]);
        }
        state_ = State::Body;
        if (!line_end(c)) {
            // The overlong preamble case: what was gathered is message text.
            for (const SitorCharacter& p : preamble_) {
                append_utf8(current_.text, p.glyph);
                current_.mutilated_characters += p.mutilated ? 1U : 0U;
            }
            body_started_ = true;
        }
        return;
    }

    // Body. The line end that closed the preamble may be followed by more
    // line ends before the text; those belong to the preamble's layout.
    if (!body_started_ && line_end(c)) {
        return;
    }
    body_started_ = true;
    append_utf8(current_.text, c.glyph);
    current_.mutilated_characters += c.mutilated ? 1U : 0U;
    ++body_characters_;

    if (ends_with(tail_, "NNNN")) {
        drop_ascii_tail(current_.text, 4);
        while (!current_.text.empty() &&
               (current_.text.back() == '\r' || current_.text.back() == '\n')) {
            current_.text.pop_back();
        }
        current_.complete = true;
        out.push_back(std::move(current_));
        current_ = {};
        state_ = State::Hunt;
        return;
    }
    if (body_characters_ > config_.max_message_characters) {
        flush(out);
    }
}

}  // namespace revenant::decode
