// SPDX-License-Identifier: Apache-2.0
// What the HUD shows besides the stats (milestone 6.0, midgard/hud.h).
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "midgard/hud.h"
#include "midgard/stats.h"

using namespace midgard::hud;

TEST_CASE("notes fade over their last second and go; five at most") {
    HudState h;
    h.notify("Picked up a key", 3.0);
    h.notify("");  // (nothing to say)
    REQUIRE(h.notes().size() == 1);
    CHECK(h.notes()[0].opacity() == 1.0f);
    h.update(2.5);
    CHECK(h.notes()[0].opacity() == doctest::Approx(0.5f));
    h.update(0.6);
    CHECK(h.notes().empty());
    for (int i = 0; i < 7; ++i) h.notify("note " + std::to_string(i));
    REQUIRE(h.notes().size() == kMaxNotes);
    CHECK(h.notes().front().text == "note 2");  // (the oldest went)
    CHECK(h.notes().back().text == "note 6");
}

TEST_CASE("timers count down or up, end, and are told once") {
    HudState h;
    h.start_timer("bomb", 10.0, true, "Bomb");
    h.start_timer("lap", 0.0, false, "Lap");    // (up for ever)
    h.start_timer("race", 5.0, false, "Race");  // (up to 5)
    h.update(4.0);
    CHECK(h.timer("bomb") == doctest::Approx(6.0));
    CHECK(h.timer("lap") == doctest::Approx(4.0));
    CHECK(h.take_finished().empty());
    h.update(2.0);
    CHECK(h.take_finished() == std::vector<std::string>{"race"});
    CHECK_FALSE(h.timer("race"));
    h.update(10.0);
    CHECK(h.take_finished() == std::vector<std::string>{"bomb"});
    CHECK(h.timer("lap") == doctest::Approx(16.0));
    CHECK(h.take_finished().empty());
    // Started again with the same id, it starts over; stopped, it ends untold.
    h.start_timer("lap", 3.0, true);
    CHECK(h.timers().size() == 1);
    CHECK(h.timer("lap") == 3.0);
    CHECK(h.stop_timer("lap"));
    CHECK_FALSE(h.stop_timer("lap"));
    h.update(5.0);
    CHECK(h.take_finished().empty());
}

TEST_CASE("the HUD's state is dumped for tests") {
    HudState h;
    h.set_prompt("Press E to open");
    h.crosshair = false;
    h.notify("Hello");
    h.start_timer("t", 5.0);
    const nlohmann::json d = h.dump();
    CHECK(d["prompt"] == "Press E to open");
    CHECK(d["crosshair"] == false);
    CHECK(d["notes"] == nlohmann::json::array({"Hello"}));
    CHECK(d["timers"]["t"] == 5.0);
    CHECK(d["visible"] == true);
}

TEST_CASE("a stat can be kept off the HUD") {
    using midgard::stats::stat_from_json;
    CHECK(stat_from_json("h", "Health", "{}").hud);
    CHECK_FALSE(stat_from_json("x", "Secret", R"({"hud": false})").hud);
    CHECK_THROWS_AS(stat_from_json("x", "X", R"({"hud": "yes"})"), midgard::stats::StatError);
}
