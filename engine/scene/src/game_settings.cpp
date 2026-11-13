// SPDX-License-Identifier: Apache-2.0
#include "midgard/game_settings.h"

#include <algorithm>
#include <cctype>
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
    if (j.contains("touch")) s.touch = touch_layout_from_json(j["touch"]);
    if (j.contains("window") && j["window"].is_string() &&
        (j["window"] == "windowed" || j["window"] == "fullscreen"))
        s.window = j["window"].get<std::string>();
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

std::vector<std::pair<std::string, std::vector<uint8_t>>> package_fonts(
    const std::vector<ContentEntry>& content, const std::string& folder,
    const std::function<std::vector<uint8_t>(const std::string&)>& read) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    for (const ContentEntry& c : content) {
        if (!c.id.starts_with(folder + "/")) continue;
        std::string lower = c.id;
        for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (!lower.ends_with(".ttf") && !lower.ends_with(".otf")) continue;
        try {
            out.emplace_back(c.id.substr(c.id.rfind('/') + 1), read(c.id));
        } catch (const std::exception&) {
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::vector<uint8_t>>> read_package_fonts(
    const std::filesystem::path& package_dir, const std::string& folder) {
    try {
        const Manifest m = load_manifest(package_dir / "manifest.json");
        const Pak pak(package_dir / "game.pak");
        return package_fonts(m.content, folder, [&](const std::string& id) {
            for (const ContentEntry& c : m.content)
                if (c.id == id) return pak.read(c.path);
            return std::vector<uint8_t>{};
        });
    } catch (const std::exception&) {
        return {};
    }
}

std::optional<ScreenDef> load_splash_screen(const std::filesystem::path& database) {
    sqlite3* raw = nullptr;
    const std::u8string u8 = database.u8string();
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &raw, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(raw);
        return std::nullopt;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), "SELECT id, name, screen FROM screen ORDER BY position, name",
                           -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;  // (a package from before screens)
    std::optional<ScreenDef> found;
    while (!found && sqlite3_step(stmt) == SQLITE_ROW) {
        const auto text = [&](int col) {
            const auto* t = sqlite3_column_text(stmt, col);
            return std::string(t ? reinterpret_cast<const char*>(t) : "");
        };
        try {
            ScreenDef s = screen_from_json(text(0), text(1), text(2));
            if (s.role == "splash") found = std::move(s);
        } catch (const ScreenError&) {  // (the cook refused it: not here)
        }
    }
    sqlite3_finalize(stmt);
    return found;
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
