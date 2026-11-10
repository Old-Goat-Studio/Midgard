// SPDX-License-Identifier: Apache-2.0
// The player's stats, scores and counters (milestone 6.0, midgard/stats.h).
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "midgard/stats.h"

using namespace midgard::stats;

namespace {

Stats project_stats() {
    Stats s;
    s.define({stat_from_json("health", "Health",
                             R"({"kind": "stat", "max": 100, "regen": 2, "delay": 3})"),
              stat_from_json("stamina", "Stamina", R"({"max": 50, "regen": 10})"),
              stat_from_json("score", "Score", R"({"kind": "score"})"),
              stat_from_json("keys", "Keys", R"({"kind": "counter", "max": 5})"),
              stat_from_json("coins", "Coins", R"({"kind": "counter"})")});
    return s;
}

}  // namespace

TEST_CASE("stats are read from their table's JSON, with their kind's defaults") {
    const StatDef health = stat_from_json("health", "Health", "{}");
    CHECK(health.kind == Kind::stat);
    CHECK(health.min == 0.0);
    CHECK(health.max == 100.0);
    CHECK(health.start == 100.0);  // (a stat starts full)
    const StatDef score = stat_from_json("score", "Score", R"({"kind": "score"})");
    CHECK(score.start == 0.0);
    CHECK_FALSE(score.capped());
    const StatDef keys = stat_from_json("keys", "Keys", R"({"kind": "counter", "max": 5.4})");
    CHECK(keys.max == 5.0);  // (whole)
    CHECK(keys.capped());
    CHECK_FALSE(stat_from_json("c", "C", R"({"kind": "counter"})").capped());  // (no target)
    // A start outside the range is brought into it.
    CHECK(stat_from_json("m", "Mana", R"({"max": 20, "start": 50})").start == 20.0);
    CHECK_THROWS_WITH_AS(stat_from_json("x", "Luck", R"({"kind": "dice"})"),
                         "Stat 'Luck' has an unknown kind 'dice'", StatError);
    CHECK_THROWS_AS(stat_from_json("x", "Luck", R"({"min": 10, "max": 5})"), StatError);
    CHECK_THROWS_AS(stat_from_json("x", "Luck", R"({"max": "lots"})"), StatError);
    CHECK_THROWS_AS(stat_from_json("x", "Luck", "[1]"), StatError);
}

TEST_CASE("stats stay in their range; scores and counters are whole") {
    Stats s = project_stats();
    CHECK(s.get("health") == 100.0);
    CHECK(s.get("Health") == 100.0);  // (by name too, case ignored)
    CHECK(s.get("HEALTH") == 100.0);
    CHECK_FALSE(s.get("luck"));
    CHECK(s.add("health", -30) == 70.0);
    CHECK(s.add("health", -500) == 0.0);
    CHECK(s.set("health", 1e9) == 100.0);
    CHECK(s.add("score", 10.6) == 11.0);
    CHECK(s.add("score", -100) == 0.0);  // (never below its least)
    CHECK(s.add("score", 1e6) == 1e6);   // (no most)
    CHECK(s.add("keys", 9) == 5.0);      // (up to its target)
    CHECK(s.add("coins", 999) == 999.0);
    CHECK_FALSE(s.set("luck", 1));
}

TEST_CASE("a stat refills after its delay, and a script can raise its most") {
    Stats s = project_stats();
    s.add("health", -50);
    s.update(2.0);
    CHECK(s.get("health") == 50.0);  // (still within its 3 second delay)
    s.update(2.0);                   // 4 s since it dropped: refilling at 2 a second
    CHECK(s.get("health") == doctest::Approx(54.0));
    s.update(100.0);
    CHECK(s.get("health") == 100.0);
    CHECK(s.set_max("health", 150));
    CHECK(s.find("health")->max == 150.0);
    s.update(10.0);
    CHECK(s.get("health") == 120.0);
    CHECK_FALSE(s.set_max("score", 10));   // (a score has no most)
    CHECK_FALSE(s.set_max("health", -5));  // (not below its least)
    CHECK(s.set_max("health", 80));        // (lowered: the value follows)
    CHECK(s.get("health") == 80.0);
    CHECK(s.reset("health"));  // back to its start and its designed most
    CHECK(s.find("health")->max == 100.0);
    CHECK(s.get("health") == 100.0);
}

TEST_CASE("a frame's changes are told once a stat, before and after") {
    Stats s = project_stats();
    s.take_changes();
    s.add("score", 5);
    s.add("score", 5);
    s.add("health", -10);
    s.add("health", 10);  // (back where it was: no change)
    s.set("keys", 0);     // (it was 0)
    const auto changes = s.take_changes();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].id == "score");
    CHECK(changes[0].before == 0.0);
    CHECK(changes[0].after == 10.0);
    CHECK(s.take_changes().empty());
}

TEST_CASE("defining again keeps values, fitted; a saved world brings them back") {
    Stats s = project_stats();
    s.add("health", -40);
    s.add("score", 25);
    s.set_max("stamina", 80);
    // The designer lowers Health's most and removes Coins while the game runs.
    s.define({stat_from_json("health", "Health", R"({"max": 50})"),
              stat_from_json("stamina", "Stamina", R"({"max": 50, "regen": 10})"),
              stat_from_json("score", "Score", R"({"kind": "score"})"),
              stat_from_json("keys", "Keys", R"({"kind": "counter", "max": 5})"),
              stat_from_json("gems", "Gems", R"({"kind": "counter", "start": 3})")});
    CHECK(s.get("health") == 50.0);
    CHECK(s.get("score") == 25.0);
    CHECK(s.find("stamina")->max == 80.0);  // (its design didn't change: the script's most stays)
    CHECK_FALSE(s.get("coins"));
    CHECK(s.get("gems") == 3.0);

    const nlohmann::json saved = s.save();
    Stats fresh = project_stats();
    fresh.restore(saved);
    CHECK(fresh.get("score") == 25.0);
    CHECK(fresh.get("health") == 50.0);
    CHECK(fresh.find("stamina")->max == 80.0);
    CHECK(fresh.get("coins") == 0.0);  // (not in the save: as it starts)
    CHECK(fresh.take_changes().empty());
    fresh.restore(nlohmann::json("nonsense"));  // (a damaged save changes nothing)
    CHECK(fresh.get("score") == 25.0);
}
