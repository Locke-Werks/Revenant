// The constellation and eye panel's arithmetic, render/symbol_scope_map.h.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

#include "render/symbol_scope_map.h"

using namespace revenant;
using Catch::Matchers::WithinAbs;

TEST_CASE("the extent leaves a level of room past the outer ideal", "[symbols]")
{
    rpc::SymbolScope c4fm;
    c4fm.kind = rpc::SymbolKind::Levels;
    c4fm.ideal = {-3.0F, -1.0F, 1.0F, 3.0F};
    CHECK(ui::symbol_extent(c4fm) == 4.0);

    rpc::SymbolScope gmsk;
    gmsk.kind = rpc::SymbolKind::Levels;
    gmsk.ideal = {-1.0F, 1.0F};
    CHECK(ui::symbol_extent(gmsk) == 2.0);

    rpc::SymbolScope psk;
    psk.kind = rpc::SymbolKind::Constellation;
    CHECK(ui::symbol_extent(psk) == 1.5);
}

TEST_CASE("values map onto the square with y upward and clamp at its edges", "[symbols]")
{
    CHECK_THAT(ui::symbol_x(0.0, 2.0, 100.0), WithinAbs(50.0, 1e-9));
    CHECK_THAT(ui::symbol_x(2.0, 2.0, 100.0), WithinAbs(100.0, 1e-9));
    CHECK_THAT(ui::symbol_y(2.0, 2.0, 100.0), WithinAbs(0.0, 1e-9));
    CHECK_THAT(ui::symbol_y(-1.0, 2.0, 100.0), WithinAbs(75.0, 1e-9));
    CHECK_THAT(ui::symbol_y(-9.0, 2.0, 100.0), WithinAbs(100.0, 1e-9));
}

TEST_CASE("dots fade from the newest to a fifth for the oldest", "[symbols]")
{
    CHECK(ui::symbol_dot_alpha(9, 10) == 1.0F);
    CHECK_THAT(ui::symbol_dot_alpha(0, 10), WithinAbs(0.2, 1e-6));
    CHECK(ui::symbol_dot_alpha(0, 1) == 1.0F);
    CHECK(ui::symbol_dot_alpha(4, 10) < ui::symbol_dot_alpha(5, 10));
}

TEST_CASE("the level histogram peaks at one where the levels are", "[symbols]")
{
    const std::vector<float> levels{-3.0F, -3.0F, 1.0F, 1.0F, 1.0F, 3.0F, 9.0F};
    const auto bins = ui::symbol_level_histogram(levels, 4.0, 8);
    REQUIRE(bins.size() == 8);
    // 1.0 lands in bin 5 of [-4, 4] in eighths, three times.
    CHECK(bins[5] == 1.0F);
    CHECK_THAT(bins[1], WithinAbs(2.0 / 3.0, 1e-6));
    // 3.0 and the overdriven 9.0 share the top bin.
    CHECK_THAT(bins[7], WithinAbs(2.0 / 3.0, 1e-6));
    CHECK(bins[3] == 0.0F);

    CHECK(ui::symbol_level_histogram({}, 4.0, 4) == std::vector<float>(4, 0.0F));
}

TEST_CASE("eye frames fade out over the persistence", "[symbols]")
{
    CHECK(ui::symbol_eye_alpha(0, 4) == 1.0F);
    CHECK(ui::symbol_eye_alpha(2, 4) == 0.5F);
    CHECK(ui::symbol_eye_alpha(4, 4) == 0.0F);
    CHECK(ui::symbol_eye_alpha(0, 0) == 0.0F);
}
