// The trunking panel's alias table and talkgroup lists, as typed.

#include <catch2/catch_test_macros.hpp>

#include "models/trunk_aliases.h"
#include "models/trunk_rules.h"

using namespace revenant::ui;

TEST_CASE("trunk rules: the three texts make one rule set", "[trunk_aliases]") {
    const auto parsed = parse_trunk_rules("100, 200", "300\nnope", "500=5, 600=x");
    CHECK(parsed.rules.allow == std::vector<std::uint32_t>{100, 200});
    CHECK(parsed.rules.deny == std::vector<std::uint32_t>{300});
    REQUIRE(parsed.rules.priorities.size() == 1);
    CHECK(parsed.rules.priorities.front().talkgroup == 500);
    CHECK(parsed.skipped == 2);
}

TEST_CASE("trunk rules: compared regardless of order", "[trunk_aliases]") {
    const auto a = parse_trunk_rules("100, 200", "", "500=5, 600=3").rules;
    auto b = parse_trunk_rules("200, 100", "", "600=3, 500=5").rules;
    CHECK(same_trunk_rules(a, b));
    b.priorities.front().priority = 9;
    CHECK_FALSE(same_trunk_rules(a, b));
}

TEST_CASE("trunk aliases: parse, skip and format", "[trunk_aliases]") {
    const auto parsed = parse_trunk_aliases(
        "# local system\n100 = Fire Dispatch\r\n200=EMS, north\n\nnonsense\n300 =\n");
    CHECK(parsed.skipped == 2);
    REQUIRE(parsed.value.size() == 2);
    CHECK(parsed.value.at(100) == "Fire Dispatch");
    CHECK(parsed.value.at(200) == "EMS, north");
    CHECK(format_trunk_aliases(parsed.value) == "100 = Fire Dispatch\n200 = EMS, north\n");
    CHECK(trunk_group_label(parsed.value, 100) == "Fire Dispatch");
    CHECK(trunk_group_label(parsed.value, 7) == "TG 7");
}

TEST_CASE("trunk aliases: group lists and priorities", "[trunk_aliases]") {
    const auto groups = parse_trunk_groups("100, 200\n3501,x, 4294967296");
    CHECK(groups.value == std::vector<std::uint32_t>{100, 200, 3501});
    CHECK(groups.skipped == 2);

    const auto prio = parse_trunk_priorities("500=5, 600 = -1, 700");
    REQUIRE(prio.value.size() == 2);
    CHECK(prio.value[0] == std::pair<std::uint32_t, std::int32_t>{500, 5});
    CHECK(prio.value[1] == std::pair<std::uint32_t, std::int32_t>{600, -1});
    CHECK(prio.skipped == 1);
}
