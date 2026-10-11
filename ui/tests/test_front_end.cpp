// The front-end switches as URI keys. See models/front_end.h.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "models/front_end.h"

using revenant::ui::front_end_of;
using revenant::ui::FrontEnd;
using revenant::ui::query_value;
using revenant::ui::radio_label_key;
using revenant::ui::serial_problem;
using revenant::ui::with_front_end;

// Rejects a writer that emits defaults: an untouched radio keeps its URI.
TEST_CASE("an all-default front end leaves the URI alone", "[front-end]")
{
    const std::string uri = "rtlsdr://0?freq=98100000&gain=20";
    CHECK(with_front_end(uri, FrontEnd{}, true) == uri);
}

// Rejects a round trip that loses a key or reads one it did not write.
TEST_CASE("front-end keys round-trip through the URI", "[front-end]")
{
    FrontEnd wanted;
    wanted.bias_tee = true;
    wanted.digital_agc = true;
    wanted.offset_tuning = true;
    wanted.bandwidth_hz = 1'500'000;
    const std::string uri = with_front_end("rtlsdr://0?freq=98100000", wanted, true);
    const FrontEnd read = front_end_of(uri);
    CHECK(read.bias_tee);
    CHECK(read.digital_agc);
    CHECK(read.offset_tuning);
    CHECK(read.bandwidth_hz == 1'500'000U);
    CHECK(query_value(uri, "freq") == std::string("98100000"));
}

// Rejects writing offset= to an R820T, whose open would then fail.
TEST_CASE("offset tuning is dropped where the tuner refuses it", "[front-end]")
{
    FrontEnd wanted;
    wanted.offset_tuning = true;
    CHECK(!query_value(with_front_end("rtlsdr://0", wanted, false), "offset").has_value());
}

// Rejects a switch turned off that stays in the URI as offset=0 or bw=0.
TEST_CASE("turning a switch off removes its key", "[front-end]")
{
    const std::string on = "rtlsdr://0?bias=1&agc=1&bw=600000";
    const std::string off = with_front_end(on, FrontEnd{}, true);
    CHECK(off == "rtlsdr://0");
}

TEST_CASE("labels are keyed by a settings-safe serial", "[front-end]")
{
    CHECK(radio_label_key("").empty());
    CHECK(radio_label_key("00000001") == "source/labels/00000001");
    CHECK(radio_label_key("a/b c") == "source/labels/a_b_c");
}

TEST_CASE("a serial for the EEPROM is checked before anything runs", "[front-end]")
{
    CHECK(serial_problem("RTL-HF_2").empty());
    CHECK(!serial_problem("").empty());
    CHECK(!serial_problem("has space").empty());
    CHECK(!serial_problem(std::string(33, 'a')).empty());
}
