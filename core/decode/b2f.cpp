// Winlink B2F. Provenance and limits are in b2f.h.

#include "core/decode/b2f.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace revenant::decode::b2f {

namespace {

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            ++i;
        }
        if (i > start) {
            out.push_back(s.substr(start, i - start));
        }
    }
    return out;
}

std::optional<std::uint32_t> to_u32(std::string_view s) {
    std::uint32_t v = 0;
    const auto* end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, v);
    if (s.empty() || r.ec != std::errc{} || r.ptr != end) {
        return std::nullopt;
    }
    return v;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<Proposal> parse_proposal(std::string_view line) {
    const auto f = split_ws(line);
    if (f.empty() || f[0].size() != 2) {
        return std::nullopt;
    }
    Proposal p;
    p.code = f[0][1];
    if (p.code == 'C') {
        // B2F: FC type id usize csize. Extra fields are allowed by the
        // Version 1 rule that a submit line may carry more than it needs.
        if (f.size() < 5) {
            return std::nullopt;
        }
        p.type = std::string(f[1]);
        p.mid = std::string(f[2]);
        const auto u = to_u32(f[3]);
        const auto c = to_u32(f[4]);
        if (!u || !c || p.type.empty() || p.type.size() > 2) {
            return std::nullopt;
        }
        p.size = *u;
        p.compressed_size = *c;
        return p;
    }
    // FBB Ascii Basic: "ALL the fields are necessary", seven of them.
    if (f.size() < 7) {
        return std::nullopt;
    }
    p.type = std::string(f[1]);
    p.from = std::string(f[2]);
    p.at = std::string(f[3]);
    p.to = std::string(f[4]);
    p.mid = std::string(f[5]);
    const auto sz = to_u32(f[6]);
    if (!sz) {
        return std::nullopt;
    }
    p.size = *sz;
    return p;
}

std::optional<Sid> parse_sid(std::string_view line) {
    if (line.size() < 3 || line.front() != '[' || line.back() != ']') {
        return std::nullopt;
    }
    Sid sid;
    sid.text = std::string(line.substr(1, line.size() - 2));
    std::string_view flags = sid.text;
    if (const auto dash = flags.rfind('-'); dash != std::string_view::npos) {
        flags = flags.substr(dash + 1);
    }
    if (!flags.empty() && flags.back() == '$') {
        flags.remove_suffix(1);
    }
    sid.flags = std::string(flags);
    sid.fbb = flags.find('F') != std::string_view::npos;
    // B followed by a digit names the level; a bare B is Version 0.
    for (std::size_t i = 0; i + 1 < flags.size(); ++i) {
        if (flags[i] == 'B' && flags[i + 1] == '1') {
            sid.b1 = true;
        }
        if (flags[i] == 'B' && flags[i + 1] == '2') {
            sid.b2 = true;
        }
    }
    return sid;
}

}  // namespace

std::optional<std::vector<Answer>> parse_answers(std::string_view body) {
    std::vector<Answer> out;
    std::size_t i = 0;
    while (i < body.size()) {
        const char c = body[i++];
        Answer a;
        switch (c) {
            case '+': case 'Y': a.kind = AnswerKind::Accept; break;
            case '-': case 'N': a.kind = AnswerKind::Reject; break;
            case '=': case 'L': a.kind = AnswerKind::Later; break;
            case 'H': a.kind = AnswerKind::Hold; break;
            case 'R': a.kind = AnswerKind::Refuse; break;
            case 'E': a.kind = AnswerKind::Error; break;
            case '!': case 'A': {
                a.kind = AnswerKind::AcceptFrom;
                const std::size_t start = i;
                while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i]))) {
                    ++i;
                }
                const auto v = to_u32(body.substr(start, i - start));
                if (!v) {
                    return std::nullopt;
                }
                a.offset = *v;
                break;
            }
            default:
                return std::nullopt;
        }
        out.push_back(a);
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

Line parse_line(std::string_view raw) {
    const std::string_view s = trim(raw);
    Line line;
    line.text = std::string(s);
    if (s.empty()) {
        return line;
    }
    if (s.front() == ';') {
        line.kind = LineKind::Comment;
        return line;
    }
    if (auto sid = parse_sid(s)) {
        line.kind = LineKind::Sid;
        line.sid = std::move(sid);
        return line;
    }
    if (s.size() < 2 || s[0] != 'F') {
        return line;
    }
    const std::string_view cmd = s.substr(0, 2);
    if (cmd == "F>") {
        line.kind = LineKind::ProposalEnd;
    } else if (cmd == "FF" && s.size() == 2) {
        line.kind = LineKind::NoMore;
    } else if (cmd == "FQ" && s.size() == 2) {
        line.kind = LineKind::Quit;
    } else if (cmd == "FS") {
        if (auto a = parse_answers(trim(s.substr(2)))) {
            line.kind = LineKind::Answer;
            line.answers = std::move(*a);
        }
    } else if (cmd == "FA" || cmd == "FB" || cmd == "FC") {
        if (auto p = parse_proposal(s)) {
            line.kind = LineKind::Proposal;
            line.proposal = std::move(p);
        }
    }
    return line;
}

std::optional<Prefix> split_prefix(std::span<const std::uint8_t> data) {
    if (data.size() < kPrefixBytes) {
        return std::nullopt;
    }
    Prefix p;
    p.crc16 = static_cast<std::uint16_t>(data[0] | (data[1] << 8));
    p.uncompressed_size = static_cast<std::uint32_t>(data[2]) |
                          (static_cast<std::uint32_t>(data[3]) << 8) |
                          (static_cast<std::uint32_t>(data[4]) << 16) |
                          (static_cast<std::uint32_t>(data[5]) << 24);
    p.compressed = data.subspan(kPrefixBytes);
    return p;
}

MessageResult parse_message(std::span<const std::uint8_t> bytes) {
    MessageResult r;
    auto fail = [&](MessageError e) {
        r.message.reset();
        r.error = e;
        return r;
    };
    Message m;
    std::size_t pos = 0;
    std::uint32_t body_len = 0;
    bool have_body = false;
    std::vector<std::pair<std::uint32_t, std::string>> files;
    bool first = true;
    bool ended = false;
    while (pos < bytes.size()) {
        std::size_t eol = pos;
        while (eol < bytes.size() && bytes[eol] != '\n') {
            ++eol;
        }
        if (eol == bytes.size()) {
            break;
        }
        std::string_view line(reinterpret_cast<const char*>(bytes.data() + pos), eol - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = eol + 1;
        if (line.empty()) {
            ended = true;
            break;
        }
        const auto colon = line.find(':');
        const std::string key = upper(trim(line.substr(0, colon)));
        const std::string_view value =
            colon == std::string_view::npos ? std::string_view{} : trim(line.substr(colon + 1));
        if (first && key != "MID") {
            return fail(MessageError::MidNotFirst);
        }
        first = false;
        if (key == "MID") {
            m.mid = value;
        } else if (key == "DATE") {
            m.date = value;
        } else if (key == "TYPE") {
            m.type = value;
        } else if (key == "FROM") {
            m.from = value;
        } else if (key == "TO") {
            m.to.emplace_back(value);
        } else if (key == "CC") {
            m.cc.emplace_back(value);
        } else if (key == "SUBJECT") {
            m.subject = value;
        } else if (key == "MBO") {
            m.mbo = value;
        } else if (key == "BODY") {
            const auto v = to_u32(value);
            if (!v) {
                return fail(MessageError::NoBodyField);
            }
            body_len = *v;
            have_body = true;
        } else if (key == "FILE") {
            const auto sp = value.find(' ');
            const auto v = sp == std::string_view::npos ? std::nullopt : to_u32(value.substr(0, sp));
            const auto name = sp == std::string_view::npos ? std::string_view{} : trim(value.substr(sp + 1));
            if (!v || name.empty()) {
                return fail(MessageError::BadFileField);
            }
            files.emplace_back(*v, std::string(name));
        }
        // B2F: any other field is ignored.
    }
    if (!ended) {
        return fail(MessageError::NoHeaderEnd);
    }
    if (!have_body) {
        return fail(MessageError::NoBodyField);
    }
    if (m.to.empty()) {
        return fail(MessageError::NoRecipient);
    }
    auto take = [&](std::uint32_t n, std::vector<std::uint8_t>& out) -> std::optional<MessageError> {
        if (bytes.size() - pos < n) {
            return MessageError::Truncated;
        }
        out.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                   bytes.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        if (bytes.size() - pos < 2) {
            return MessageError::Truncated;
        }
        if (bytes[pos] != '\r' || bytes[pos + 1] != '\n') {
            return MessageError::MissingTerminator;
        }
        pos += 2;
        return std::nullopt;
    };
    std::vector<std::uint8_t> body;
    if (auto e = take(body_len, body)) {
        return fail(*e);
    }
    m.body.assign(body.begin(), body.end());
    for (auto& [n, name] : files) {
        Attachment a;
        a.name = std::move(name);
        if (auto e = take(n, a.data)) {
            return fail(*e);
        }
        m.attachments.push_back(std::move(a));
    }
    r.message = std::move(m);
    r.consumed = pos;
    return r;
}

void Monitor::finish_line(std::vector<Event>& out) {
    Line line = parse_line(line_);
    line_.clear();
    switch (line.kind) {
        case LineKind::Proposal:
            pending_.push_back(*line.proposal);
            break;
        case LineKind::ProposalEnd:
            block_ = pending_;
            break;
        case LineKind::Answer: {
            if (pending_.empty() && !block_.empty()) {
                pending_ = block_;
            }
            block_ = pending_;
            pending_.clear();
            items_expected_ = 0;
            for (const auto& a : line.answers) {
                if (a.kind == AnswerKind::Accept || a.kind == AnswerKind::AcceptFrom ||
                    a.kind == AnswerKind::Hold) {
                    ++items_expected_;
                }
            }
            break;
        }
        default:
            break;
    }
    if (line.text.empty()) {
        return;
    }
    Event e;
    e.kind = Event::Kind::Line;
    e.line = std::move(line);
    out.push_back(std::move(e));
}

void Monitor::finish_item(std::uint8_t checksum, std::vector<Event>& out) {
    std::uint8_t sum = checksum;
    for (const auto b : item_.data) {
        sum = static_cast<std::uint8_t>(sum + b);
    }
    item_.checksum_ok = sum == 0;
    Event e;
    e.item = std::move(item_);
    item_ = Item{};
    if (items_expected_ > 0) {
        --items_expected_;
    }
    if (!e.item->checksum_ok) {
        e.kind = Event::Kind::ItemBad;
        out.push_back(std::move(e));
        return;
    }
    e.prefix = split_prefix(e.item->data);
    std::optional<std::vector<std::uint8_t>> plain;
    if (e.prefix && decompress_) {
        plain = decompress_(e.prefix->compressed, e.prefix->uncompressed_size);
        if (plain && plain->size() != e.prefix->uncompressed_size) {
            plain.reset();
        }
    }
    if (!plain) {
        e.kind = Event::Kind::ItemNotExpanded;
        out.push_back(std::move(e));
        return;
    }
    // Up to five messages share one file.
    std::span<const std::uint8_t> rest(*plain);
    bool any = false;
    while (!rest.empty()) {
        auto r = parse_message(rest);
        Event m;
        m.kind = Event::Kind::Message;
        m.message = std::move(r.message);
        m.message_error = r.error;
        if (!any) {
            m.item = e.item;
            m.prefix = e.prefix;
        }
        out.push_back(std::move(m));
        any = true;
        if (r.error || r.consumed == 0) {
            break;
        }
        rest = rest.subspan(r.consumed);
    }
}

std::vector<Event> Monitor::feed(std::span<const std::uint8_t> bytes) {
    std::vector<Event> out;
    for (const auto b : bytes) {
        switch (state_) {
            case State::Lines:
                if (b == kSoh && items_expected_ > 0 && line_.empty()) {
                    state_ = State::Header;
                    item_ = Item{};
                } else if (b == '\r') {
                    finish_line(out);
                } else if (b != '\n') {
                    line_.push_back(static_cast<char>(b));
                }
                break;
            case State::Header:
                header_left_ = b;
                header_.clear();
                state_ = header_left_ == 0 ? State::BlockLen : State::HeaderBody;
                break;
            case State::HeaderBody:
                header_.push_back(b);
                if (--header_left_ == 0) {
                    // title NUL offset NUL
                    const auto nul = std::find(header_.begin(), header_.end(), kNul);
                    item_.title.assign(header_.begin(), nul);
                    if (nul != header_.end()) {
                        const auto nul2 = std::find(nul + 1, header_.end(), kNul);
                        const std::string off(nul + 1, nul2);
                        item_.offset = to_u32(off).value_or(0);
                    }
                    state_ = State::BlockLen;
                }
                break;
            case State::BlockLen:
                if (b == kStx) {
                    state_ = State::BlockData;
                    block_left_ = 0;
                    header_left_ = 1;  // count byte next
                } else if (b == kEot) {
                    state_ = State::Checksum;
                } else {
                    // Lost framing: back to lines and let the text show it.
                    items_expected_ = 0;
                    state_ = State::Lines;
                }
                break;
            case State::BlockData:
                if (header_left_ == 1) {
                    header_left_ = 0;
                    block_left_ = b == 0 ? 256 : b;
                } else {
                    item_.data.push_back(b);
                    if (--block_left_ == 0) {
                        state_ = State::BlockLen;
                    }
                }
                break;
            case State::Checksum:
                finish_item(b, out);
                state_ = State::Lines;
                break;
        }
    }
    return out;
}

}  // namespace revenant::decode::b2f
