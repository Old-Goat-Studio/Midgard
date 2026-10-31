// SPDX-License-Identifier: Apache-2.0
#include "midgard/player_profile.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

namespace midgard {
namespace {

std::optional<float> distance_in(const nlohmann::json& camera, const char* key) {
    const auto it = camera.find(key);
    if (it == camera.end() || !it->is_number()) return std::nullopt;
    const float d = it->get<float>();
    if (!std::isfinite(d) || d < 0.0f) return std::nullopt;
    return d;
}

}  // namespace

PlayerProfile PlayerProfile::load(const std::filesystem::path& file) {
    PlayerProfile p;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return p;
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ProfileError("can't read the player profile " + file.string());
    std::stringstream text;
    text << in.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, false);
    if (!j.is_object()) throw ProfileError("the player profile " + file.string() + " is damaged");
    p.other = j;
    if (const auto camera = j.find("camera"); camera != j.end() && camera->is_object()) {
        if (const auto view = camera->find("view"); view != camera->end() && view->is_string())
            p.view = view_from_name(view->get<std::string>());
        p.third_distance = distance_in(*camera, "third_distance");
        p.top_distance = distance_in(*camera, "top_distance");
        p.other.erase("camera");
    }
    return p;
}

void PlayerProfile::save(const std::filesystem::path& file) const {
    nlohmann::json j = other;
    nlohmann::json camera = nlohmann::json::object();
    if (view) camera["view"] = view_name(*view);
    if (third_distance) camera["third_distance"] = *third_distance;
    if (top_distance) camera["top_distance"] = *top_distance;
    j["camera"] = camera;
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << j.dump(2) << '\n';
        if (!out) throw ProfileError("can't write the player profile " + file.string());
    }
    std::filesystem::rename(temp, file, ec);  // replaces the old file in one step
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw ProfileError("can't write the player profile " + file.string());
    }
}

}  // namespace midgard
