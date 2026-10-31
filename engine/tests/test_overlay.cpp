// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "midgard/render/overlay.h"

using midgard::render::layout_overlay;
using midgard::render::OverlayLine;
using midgard::render::OverlayVertex;

namespace {

bool inside(const std::vector<OverlayVertex>& vs, float w, float h) {
    for (const auto& v : vs)
        if (v.position.x < 0 || v.position.y < 0 || v.position.x > w || v.position.y > h)
            return false;
    return true;
}

}  // namespace

TEST_CASE("no lines, no overlay") {
    CHECK(layout_overlay({}, 1280, 720).empty());
    CHECK(layout_overlay({{"hi"}}, 0, 0).empty());
}

TEST_CASE("text becomes triangles over a panel, inside the screen") {
    const auto vs =
        layout_overlay({{"Frame 16.7 ms", {1, 1, 0, 1}}, {"PASS", {0, 1, 0, 1}, 2}}, 1280, 720);
    REQUIRE(vs.size() > 6);
    CHECK(vs.size() % 3 == 0);                     // whole triangles
    CHECK(vs[0].color.a == doctest::Approx(0.6));  // the panel comes first, behind the text
    CHECK(vs[0].position == glm::vec2(0, 0));
    CHECK(vs[6].color == glm::vec4(1, 1, 0, 1));  // then the first line's colour
    CHECK(vs.back().color == glm::vec4(0, 1, 0, 1));
    CHECK(inside(vs, 1280, 720));
}

TEST_CASE("bigger screens get bigger text") {
    CHECK(midgard::render::overlay_scale(720) == 1.0f);
    CHECK(midgard::render::overlay_scale(2160) == 4.0f);  // a 4K screen or a phone held sideways
    const auto small = layout_overlay({{"A"}}, 1280, 720);
    const auto large = layout_overlay({{"A"}}, 3840, 2160);
    CHECK(large[2].position.y > small[2].position.y * 3);  // the panel is taller
}

TEST_CASE("characters outside printable ASCII are shown, not crashed on") {
    const std::string odd = "caf\xc3\xa9 \t\x01 \x7f";
    const auto vs = layout_overlay({{odd}}, 1280, 720);
    CHECK(vs.size() > 6);
    CHECK(inside(vs, 1280, 720));
}

TEST_CASE("lines past the bottom of the screen are cut off") {
    std::vector<OverlayLine> many(500, OverlayLine{"line"});
    const auto vs = layout_overlay(many, 640, 360);
    CHECK(inside(vs, 640, 360));
    const auto few = layout_overlay(std::vector<OverlayLine>(10, OverlayLine{"line"}), 640, 360);
    CHECK(vs.size() > few.size());
}

TEST_CASE("a top-right panel sits against the right edge, its lines right-aligned") {
    using midgard::render::OverlayCorner;
    const std::vector<OverlayLine> lines{{"Time   06:00"}, {"Year   1"}};
    const auto left = layout_overlay(lines, 1280, 720);
    const auto right = layout_overlay(lines, 1280, 720, OverlayCorner::TopRight);
    REQUIRE(right.size() == left.size());  // the same text, just moved
    CHECK(inside(right, 1280, 720));
    CHECK(right[0].position.y == 0.0f);
    CHECK(right[2].position.x == 1280.0f);  // the panel's far corner is the screen's edge
    const float panel_width = left[2].position.x;
    CHECK(right[0].position.x == doctest::Approx(1280.0f - panel_width));
    float widest = 0.0f, rightmost = 0.0f;  // the glyphs end one margin short of the edge
    for (size_t i = 6; i < right.size(); ++i) {
        widest = std::max(widest, left[i].position.x);
        rightmost = std::max(rightmost, right[i].position.x);
    }
    CHECK(1280.0f - rightmost == doctest::Approx(panel_width - widest));
}
