// SPDX-License-Identifier: Apache-2.0
#include "midgard/hud.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <nlohmann/json.hpp>

namespace midgard::hud {

float Note::opacity() const {
    const double left = life - age;
    return float(std::clamp(left, 0.0, 1.0));
}

void HudState::notify(const std::string& text, double seconds) {
    if (text.empty()) return;
    notes_.push_back({text, 0.0, std::max(seconds, 0.5)});
    while (notes_.size() > kMaxNotes) notes_.pop_front();
}

void HudState::start_timer(const std::string& id, double seconds, bool down,
                           const std::string& label) {
    stop_timer(id);
    Timer t;
    t.id = id;
    t.label = label;
    t.down = down;
    t.value = down ? std::max(seconds, 0.0) : 0.0;
    t.limit = down ? 0.0 : std::max(seconds, 0.0);
    timers_.push_back(std::move(t));
}

bool HudState::stop_timer(const std::string& id) {
    return std::erase_if(timers_, [&](const Timer& t) { return t.id == id; }) > 0;
}

std::optional<double> HudState::timer(const std::string& id) const {
    for (const Timer& t : timers_)
        if (t.id == id) return t.value;
    return std::nullopt;
}

void HudState::update(double dt) {
    if (dt <= 0.0) return;
    for (Note& n : notes_) n.age += dt;
    std::erase_if(notes_, [](const Note& n) { return n.age >= n.life; });
    for (Timer& t : timers_) {
        if (t.down) {
            t.value = std::max(0.0, t.value - dt);
            if (t.value <= 0.0) finished_.push_back(t.id);
        } else {
            t.value += dt;
            if (t.limit > 0.0 && t.value >= t.limit) {
                t.value = t.limit;
                finished_.push_back(t.id);
            }
        }
    }
    std::erase_if(timers_, [&](const Timer& t) {
        return std::find(finished_.begin(), finished_.end(), t.id) != finished_.end();
    });
}

std::vector<std::string> HudState::take_finished() {
    return std::exchange(finished_, {});
}

nlohmann::json HudState::dump() const {
    nlohmann::json notes = nlohmann::json::array();
    for (const Note& n : notes_) notes.push_back(n.text);
    nlohmann::json timers = nlohmann::json::object();
    for (const Timer& t : timers_) timers[t.id] = t.value;
    return {{"visible", visible},
            {"compass", compass},
            {"minimap", minimap},
            {"marks", marks},
            {"crosshair", crosshair ? nlohmann::json(*crosshair) : nlohmann::json()},
            {"prompt", prompt_},
            {"notes", notes},
            {"timers", timers}};
}

}  // namespace midgard::hud
