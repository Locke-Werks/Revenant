// Winlink B2F: command lines against the examples printed in the FBB
// forwarding protocol page and the Open B2F page, message files against the
// B2F address header example, and a whole exchange through the monitor.
// The decompressor in the exchange test is a stand-in that returns the
// payload unchanged; no LZHUF is implemented (see core/decode/b2f.h).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/decode/b2f.h"
#include "core/dsp/synth/b2f_encode.h"

using namespace revenant::decode::b2f;
namespace enc = revenant::dsp::synth::b2f;

namespace {

std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

enc::OutMessage sample() {
    // The Open B2F address header example, with a body and two files of
    // our own sized to suit.
    enc::OutMessage m;
    m.mid = "12345_K4CJX";
    m.date = "1999/09/22 14:33";
    m.from = "SMTP:vpoor@csi.com";
    m.to = {"W1AW", "W4ABC"};
    m.cc = {"N8PGR", "SMTP:hpyle@seanet.com"};
    m.subject = "This is a sample address header";
    m.mbo = "SMTP";
    m.body = "Line one\r\nLine two";
    m.files = {{"NOLA.XLS", {0x00, 0x0D, 0x0A, 0xFF}}, {"NEWBOAT.HOMEPORT.JPG", {1, 2, 3}}};
    return m;
}

}  // namespace

TEST_CASE("B2F proposals from the FBB page examples", "[b2f]") {
    const auto l = parse_line("FB P F6FBB FC1GHV.FFPC.FRA.EU FC1MVP 24657_F6FBB 1345");
    REQUIRE(l.kind == LineKind::Proposal);
    CHECK(l.proposal->code == 'B');
    CHECK(l.proposal->from == "F6FBB");
    CHECK(l.proposal->at == "FC1GHV.FFPC.FRA.EU");
    CHECK(l.proposal->to == "FC1MVP");
    CHECK(l.proposal->mid == "24657_F6FBB");
    CHECK(l.proposal->size == 1345);

    // Six fields is one short of the seven the page requires.
    CHECK(parse_line("FB P F6FBB FC1GHV FC1MVP 24657_F6FBB").kind == LineKind::Text);

    const auto fc = parse_line("FC EM 12345_K4CJX 3021 1544");
    REQUIRE(fc.kind == LineKind::Proposal);
    CHECK(fc.proposal->code == 'C');
    CHECK(fc.proposal->type == "EM");
    CHECK(fc.proposal->size == 3021);
    CHECK(fc.proposal->compressed_size == 1544);

    CHECK(parse_line("F>").kind == LineKind::ProposalEnd);
    CHECK(parse_line("FF").kind == LineKind::NoMore);
    CHECK(parse_line("FQ").kind == LineKind::Quit);
    CHECK(parse_line(";PQ: 12345678").kind == LineKind::Comment);
}

TEST_CASE("B2F FS answers, Version 0 signs and the Version 1 example", "[b2f]") {
    const auto a = parse_line("FS -+=");
    REQUIRE(a.kind == LineKind::Answer);
    REQUIRE(a.answers.size() == 3);
    CHECK(a.answers[0].kind == AnswerKind::Reject);
    CHECK(a.answers[1].kind == AnswerKind::Accept);
    CHECK(a.answers[2].kind == AnswerKind::Later);

    // FBB Version 1: YLA3350RH and +L!3350RH mean the same thing.
    for (const auto* s : {"YLA3350RH", "+L!3350RH"}) {
        const auto v = parse_answers(s);
        REQUIRE(v);
        REQUIRE(v->size() == 5);
        CHECK((*v)[0].kind == AnswerKind::Accept);
        CHECK((*v)[1].kind == AnswerKind::Later);
        CHECK((*v)[2].kind == AnswerKind::AcceptFrom);
        CHECK((*v)[2].offset == 3350);
        CHECK((*v)[3].kind == AnswerKind::Refuse);
        CHECK((*v)[4].kind == AnswerKind::Hold);
    }
    CHECK_FALSE(parse_answers("+X"));
}

TEST_CASE("B2F SID flags", "[b2f]") {
    const auto s = parse_line("[WL2K-5.0-B2FWIHJM$]");
    REQUIRE(s.kind == LineKind::Sid);
    CHECK(s.sid->fbb);
    CHECK(s.sid->b2);
    const auto f = parse_line("[FBB-5.15-B1FHLM$]");
    CHECK(f.sid->b1);
    CHECK_FALSE(f.sid->b2);
}

TEST_CASE("B2F message file round trip", "[b2f]") {
    const auto file = enc::message_file(sample());
    const auto r = parse_message(file);
    REQUIRE(r.message);
    CHECK(r.consumed == file.size());
    const auto& m = *r.message;
    CHECK(m.mid == "12345_K4CJX");
    CHECK(m.date == "1999/09/22 14:33");
    CHECK(m.to == std::vector<std::string>{"W1AW", "W4ABC"});
    CHECK(m.cc.size() == 2);
    CHECK(m.subject == "This is a sample address header");
    CHECK(m.body == "Line one\r\nLine two");
    REQUIRE(m.attachments.size() == 2);
    CHECK(m.attachments[0].name == "NOLA.XLS");
    CHECK(m.attachments[0].data == std::vector<std::uint8_t>{0x00, 0x0D, 0x0A, 0xFF});
    CHECK(m.attachments[1].name == "NEWBOAT.HOMEPORT.JPG");
}

TEST_CASE("B2F message header rules", "[b2f]") {
    // Case insensitive keys, unknown fields ignored.
    const auto ok = bytes("MID: A1\r\nx-foo: bar\r\nto: W1AW\r\nbody: 2\r\n\r\nhi\r\n");
    REQUIRE(parse_message(ok).message);

    CHECK(parse_message(bytes("To: W1AW\r\nMid: A1\r\nBody: 2\r\n\r\nhi\r\n")).error ==
          MessageError::MidNotFirst);
    CHECK(parse_message(bytes("Mid: A1\r\nBody: 2\r\n\r\nhi\r\n")).error ==
          MessageError::NoRecipient);
    CHECK(parse_message(bytes("Mid: A1\r\nTo: W1AW\r\nBody: 5\r\n\r\nhi\r\n")).error ==
          MessageError::Truncated);
    CHECK(parse_message(bytes("Mid: A1\r\nTo: W1AW\r\nBody: 1\r\n\r\nhi\r\n")).error ==
          MessageError::MissingTerminator);
}

TEST_CASE("B2F binary item framing and checksum", "[b2f]") {
    std::vector<std::uint8_t> data(600);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 7);
    }
    auto wire = enc::item("Test subject", 0, data);
    Monitor mon;
    mon.feed(bytes("FC EM X1 600 600\rF>\rFS +\r"));
    auto ev = mon.feed(wire);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == Event::Kind::ItemNotExpanded);
    CHECK(ev[0].item->title == "Test subject");
    CHECK(ev[0].item->data == data);

    // One corrupted data byte fails the checksum.
    wire[40] ^= 0x10;
    mon.feed(bytes("FC EM X2 600 600\rF>\rFS +\r"));
    ev = mon.feed(wire);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == Event::Kind::ItemBad);
}

TEST_CASE("B2F whole exchange through the monitor", "[b2f]") {
    auto m1 = sample();
    auto m2 = sample();
    m2.mid = "67890_K4CJX";
    m2.files.clear();
    auto file = enc::message_file(m1);
    const auto f2 = enc::message_file(m2);
    file.insert(file.end(), f2.begin(), f2.end());
    const auto data = enc::with_prefix(0xBEEF, static_cast<std::uint32_t>(file.size()), file);

    std::vector<std::uint8_t> wire;
    auto add = [&](std::string_view s) { wire.insert(wire.end(), s.begin(), s.end()); };
    add("[WL2K-5.0-B2FWIHJM$]\r");
    add(";PQ: 23753528\r");
    add(enc::fc_line("EM", m1.mid, static_cast<std::uint32_t>(file.size()), 0));
    add("F>\r");
    add("FS Y\r");
    const auto it = enc::item("This is a sample address header", 0, data);
    wire.insert(wire.end(), it.begin(), it.end());
    add("FF\rFQ\r");

    // Stand-in decompressor: payload is the plain file.
    Monitor mon([](std::span<const std::uint8_t> c, std::uint32_t) {
        return std::optional<std::vector<std::uint8_t>>(std::vector<std::uint8_t>(c.begin(), c.end()));
    });
    // Feed in uneven chunks.
    std::vector<Event> ev;
    for (std::size_t i = 0; i < wire.size(); i += 37) {
        const auto n = std::min<std::size_t>(37, wire.size() - i);
        auto e = mon.feed(std::span(wire).subspan(i, n));
        ev.insert(ev.end(), e.begin(), e.end());
    }
    std::vector<Message> msgs;
    int lines = 0;
    for (const auto& e : ev) {
        if (e.kind == Event::Kind::Message) {
            REQUIRE(e.message);
            msgs.push_back(*e.message);
        } else if (e.kind == Event::Kind::Line) {
            ++lines;
        }
    }
    CHECK(lines == 7);
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].mid == "12345_K4CJX");
    CHECK(msgs[0].attachments.size() == 2);
    CHECK(msgs[1].mid == "67890_K4CJX");
    CHECK(mon.proposals().size() == 1);
    CHECK(mon.proposals()[0].mid == "12345_K4CJX");
}
