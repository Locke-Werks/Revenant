// The constellation and eye panel across the wire (#17): the receiver's flag,
// and a passband frame's symbol scope from the engine's struct to the
// client's.

#include <catch2/catch_test_macros.hpp>

#include <capnp/message.h>

#include <vector>

#include "core/engine/engine.h"
#include "core/engine/symbol_tap.h"
#include "core/rpc/convert.h"
#include "core/rpc/revenant.capnp.h"
#include "core/rpc/symbol_wire.h"
#include "core/rpc/types.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

TEST_CASE("the symbols flag survives the engine's writer and reader", "[rpc][symbol_tap]") {
    engine::VrxParams sent;
    sent.demod = engine::Demod::P25p1;
    sent.display_symbols = true;

    capnp::MallocMessageBuilder message;
    auto out = message.initRoot<rpc::schema::VrxParams>();
    rpc::write_vrx_params(out, sent);
    const auto read = rpc::read_vrx_params(out.asReader());
    INFO(test::message_of(read));
    REQUIRE(read.has_value());
    CHECK(read->display_symbols);

    // An older peer sends nothing and gets the panel off.
    capnp::MallocMessageBuilder bare;
    auto plain = bare.initRoot<rpc::schema::VrxParams>();
    plain.setDemod(rpc::schema::Demod::NFM);
    const auto off = rpc::read_vrx_params(plain.asReader());
    REQUIRE(off.has_value());
    CHECK_FALSE(off->display_symbols);
}

TEST_CASE("a passband frame's symbol scope crosses the wire", "[rpc][symbol_tap]") {
    std::vector<float> power{-90.0F, -80.0F};

    engine::SymbolScope scope;
    scope.kind = engine::SymbolKind::Constellation;
    scope.symbol_rate_hz = 18'000.0;
    scope.points = {0.7F, 0.71F, -0.69F, 0.7F};
    scope.ideal = {0.7071F, 0.7071F, -0.7071F, 0.7071F};
    scope.eye = {0.1F, 0.2F, 0.3F};
    scope.eye_trace_length = 3;
    scope.error_rms = 0.042F;
    scope.timing_offset = -0.125F;
    scope.rate_error_ppm = 12.5F;

    engine::PassbandFrame in;
    in.vrx = engine::VrxId{5};
    in.power_db = power;
    in.symbols = &scope;

    capnp::MallocMessageBuilder message;
    auto out = message.initRoot<rpc::schema::PassbandFrame>();
    rpc::write_passband_frame(out, in);
    const auto reader = out.asReader();
    REQUIRE(reader.hasSymbols());

    rpc::SymbolScope read;
    rpc::read_symbol_scope(read, reader.getSymbols());
    CHECK(read.kind == rpc::SymbolKind::Constellation);
    CHECK(read.symbol_rate_hz == 18'000.0);
    CHECK(read.points == scope.points);
    CHECK(read.ideal == scope.ideal);
    CHECK(read.eye == scope.eye);
    CHECK(read.eye_trace_length == 3);
    CHECK(read.error_rms == 0.042F);
    CHECK(read.timing_offset == -0.125F);
    CHECK(read.rate_error_ppm == 12.5F);

    // Levels map to levels.
    scope.kind = engine::SymbolKind::Levels;
    capnp::MallocMessageBuilder levels;
    auto level_out = levels.initRoot<rpc::schema::PassbandFrame>();
    rpc::write_passband_frame(level_out, in);
    CHECK(rpc::read_symbol_kind(level_out.asReader().getSymbols().getKind()) ==
          rpc::SymbolKind::Levels);

    // Without a scope nothing of it is written.
    engine::PassbandFrame none;
    none.power_db = power;
    capnp::MallocMessageBuilder empty;
    auto blank = empty.initRoot<rpc::schema::PassbandFrame>();
    rpc::write_passband_frame(blank, none);
    CHECK_FALSE(blank.asReader().hasSymbols());
}
