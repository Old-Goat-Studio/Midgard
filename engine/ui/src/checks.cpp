// SPDX-License-Identifier: Apache-2.0
// Screen checks (Phase6_Roadmap.md 2.1): every control reachable with a gamepad, big enough for
// a finger and inside the safe area, and text that fits its box.
#include <algorithm>
#include <cmath>
#include <deque>
#include <set>
#include <string>

#include <RmlUi/Core.h>

#include "midgard/ui/ui.h"

namespace midgard::ui {
namespace {

bool focusable(Rml::Element* e) {
    if (!e->IsVisible(true)) return false;
    if (e->GetComputedValues().tab_index() != Rml::Style::TabIndex::Auto) return false;
    if (e->HasAttribute("disabled")) return false;
    const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
    return size.x > 0.0f && size.y > 0.0f;
}

void collect(Rml::Element* e, std::vector<Rml::Element*>& controls,
             std::vector<Rml::Element*>& all) {
    if (!e->IsVisible(true)) return;
    all.push_back(e);
    if (focusable(e)) controls.push_back(e);
    for (int i = 0; i < e->GetNumChildren(); ++i) collect(e->GetChild(i), controls, all);
}

// The control an element belongs to (itself, or the nearest focusable parent).
Rml::Element* control_of(Rml::Element* e, const std::set<Rml::Element*>& controls) {
    for (; e; e = e->GetParentNode())
        if (controls.count(e)) return e;
    return nullptr;
}

}  // namespace

std::vector<Problem> Ui::check(Rml::ElementDocument* document, bool touch) {
    std::vector<Problem> problems;
    if (!document) return problems;
    Rml::Context& c = *context();
    c.Update();  // (laid out as it will be drawn)
    std::vector<Rml::Element*> controls, all;
    collect(document, controls, all);
    const std::set<Rml::Element*> known(controls.begin(), controls.end());

    // Reachable: from the first control, every other by the four directions (as a gamepad's
    // d-pad moves; Ui turns it into arrow keys).
    if (!controls.empty()) {
        Rml::Element* was = c.GetFocusElement();
        std::set<Rml::Element*> reached{controls.front()};
        std::deque<Rml::Element*> todo{controls.front()};
        while (!todo.empty()) {
            Rml::Element* from = todo.front();
            todo.pop_front();
            for (const auto key : {Rml::Input::KI_UP, Rml::Input::KI_DOWN, Rml::Input::KI_LEFT,
                                   Rml::Input::KI_RIGHT}) {
                from->Focus(true);
                navigate(key);
                Rml::Element* to = control_of(c.GetFocusElement(), known);
                if (to && reached.insert(to).second) todo.push_back(to);
            }
        }
        for (Rml::Element* e : controls)
            if (!reached.count(e))
                problems.push_back({"unreachable", describe(e),
                                    "a gamepad can't reach it from " + describe(controls.front()) +
                                        " (check the screen's nav properties)"});
        if (was) was->Focus();
        c.Update();
    }
    // Working the controls may have changed the screen (a status line rewritten, a part shown
    // or hidden): what was collected before may be gone. Collect it again.
    controls.clear();
    all.clear();
    collect(document, controls, all);

    // Text that doesn't fit: an element whose content is wider than its box and isn't scrolled.
    for (Rml::Element* e : all) {
        if (e == document) continue;
        const auto& v = e->GetComputedValues();
        if (v.overflow_x() != Rml::Style::Overflow::Visible) continue;
        bool text = false;
        for (int i = 0; i < e->GetNumChildren(); ++i)
            if (e->GetChild(i)->GetTagName() == "#text") text = true;
        if (!text) continue;
        const float inside = e->GetClientWidth();
        if (inside > 0.0f && e->GetScrollWidth() > inside + 1.0f)
            problems.push_back({"text_overflow", describe(e),
                                "its text is " + std::to_string(int(e->GetScrollWidth())) +
                                    " pixels wide in a box " + std::to_string(int(inside)) +
                                    " wide"});
    }

    // Off the screen with no way to scroll to it (any player); with touch, also too small for a
    // finger, or under the notch or a rounded corner.
    const float min_px = kMinTouchTarget * dp_ratio() - 0.5f;
    const Insets& s = safe_area();
    const float w = float(width()), h = float(height());
    for (Rml::Element* e : controls) {
        const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
        const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        if (touch && (size.x < min_px || size.y < min_px))
            problems.push_back({"small_target", describe(e),
                                "it is " + std::to_string(int(std::round(size.x / dp_ratio()))) +
                                    " x " + std::to_string(int(std::round(size.y / dp_ratio()))) +
                                    " dp; a finger needs " + std::to_string(int(kMinTouchTarget)) +
                                    " x " + std::to_string(int(kMinTouchTarget))});
        // (a control scrolled out of view, in a list or a screen that scrolls, is reached by
        // scrolling: not a problem)
        bool scrolled = false;
        for (Rml::Element* p = e->GetParentNode(); p; p = p->GetParentNode()) {
            if (p->GetComputedValues().overflow_y() != Rml::Style::Overflow::Visible)
                scrolled = true;
            if (p == document) break;
        }
        if (scrolled) continue;
        const auto outside = [&](float left, float top, float right, float bottom) {
            return at.x < left - 0.5f || at.y < top - 0.5f || at.x + size.x > right + 0.5f ||
                   at.y + size.y > bottom + 0.5f;
        };
        if (outside(0.0f, 0.0f, w, h))
            problems.push_back({"off_screen", describe(e),
                                "part of it is off the screen, and the screen doesn't scroll "
                                "(give it, or the part holding it, overflow-y: auto)"});
        else if (touch && outside(s.left, s.top, w - s.right, h - s.bottom))
            problems.push_back({"outside_safe_area", describe(e),
                                "part of it is under the notch, a rounded corner or the "
                                "system's bars"});
    }
    return problems;
}

}  // namespace midgard::ui
