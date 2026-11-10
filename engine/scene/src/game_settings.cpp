// SPDX-License-Identifier: Apache-2.0
#include "midgard/game_settings.h"

#include <algorithm>
#include <memory>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include "midgard/manifest.h"
#include "midgard/pak.h"

namespace midgard {

GameSettings game_settings_from_json(const std::string& text) {
    GameSettings s;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return s;
    const auto picture = [&](const char* key, std::string& out) {
        if (j.contains(key) && j[key].is_string() && j[key].get<std::string>().starts_with("ui:"))
            out = j[key].get<std::string>();
    };
    picture("splash", s.splash);
    picture("crosshair", s.crosshair);
    picture("pointer", s.pointer);
    if (j.contains("crosshair_views") && j["crosshair_views"].is_string()) {
        const std::string v = j["crosshair_views"].get<std::string>();
        if (v == "first" || v == "all" || v == "none") s.crosshair_views = v;
    }
    if (j.contains("crosshair_size") && j["crosshair_size"].is_number())
        s.crosshair_size = std::clamp(j["crosshair_size"].get<double>(), 4.0, 256.0);
    if (j.contains("menu_music") && j["menu_music"].is_string())
        s.menu_music = j["menu_music"].get<std::string>();
    return s;
}

GameSettings load_game_settings(const std::filesystem::path& database) {
    sqlite3* raw = nullptr;
    const std::u8string u8 = database.u8string();
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &raw, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(raw);
        return {};
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), "SELECT value FROM meta WHERE key = ?", -1, &stmt, nullptr) !=
        SQLITE_OK)
        return {};
    sqlite3_bind_text(stmt, 1, kGameSettingsKey, -1, SQLITE_STATIC);
    GameSettings s;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* t = sqlite3_column_text(stmt, 0);
        if (t) s = game_settings_from_json(reinterpret_cast<const char*>(t));
    }
    sqlite3_finalize(stmt);
    return s;
}

std::vector<uint8_t> read_package_content(const std::filesystem::path& package_dir,
                                          const std::string& content) {
    if (content.empty()) return {};
    try {
        const Manifest m = load_manifest(package_dir / "manifest.json");
        for (const ContentEntry& c : m.content)
            if (c.id == content) return Pak(package_dir / "game.pak").read(c.path);
    } catch (const std::exception&) {
    }
    return {};
}

}  // namespace midgard
