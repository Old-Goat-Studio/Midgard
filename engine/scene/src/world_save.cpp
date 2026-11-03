// SPDX-License-Identifier: Apache-2.0
#include "midgard/world_save.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

namespace midgard {
namespace {

// world.db's tables (its user_version is 1).
constexpr const char* kWorldSchema = R"(
CREATE TABLE IF NOT EXISTS clock (
    id        INTEGER PRIMARY KEY CHECK (id = 1),
    game_time INTEGER NOT NULL,   -- game seconds since year 1, month 1, day 1 (midgard/calendar.h)
    scale     REAL NOT NULL       -- game seconds per real second
);
CREATE TABLE IF NOT EXISTS place (  -- where the player was (version 2)
    id INTEGER PRIMARY KEY CHECK (id = 1),
    x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL,  -- metres, double precision
    rx REAL NOT NULL, ry REAL NOT NULL, rz REAL NOT NULL,  -- Euler degrees
    scene TEXT  -- the scene they were in (version 3)
);
CREATE TABLE IF NOT EXISTS state (  -- the game's own state by name, as JSON (version 4)
    name  TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
PRAGMA user_version = 4;
)";

struct Db {
    sqlite3* db = nullptr;
    explicit Db(const std::filesystem::path& file) {
        const std::u8string u8 = file.u8string();
        if (sqlite3_open(std::string(u8.begin(), u8.end()).c_str(), &db) != SQLITE_OK)
            throw WorldError("can't open " + file.string());
    }
    ~Db() { sqlite3_close(db); }
    void exec(const char* sql) const {
        char* err = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string message = err ? err : "?";
            sqlite3_free(err);
            throw WorldError("world.db: " + message);
        }
    }
};

std::string new_id() {
    std::random_device rd;
    char out[33];
    std::snprintf(out, sizeof(out), "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    return out;
}

std::string now_utc() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char out[32];
    std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return out;
}

}  // namespace

WorldSave WorldSave::open(const std::filesystem::path& folder, const std::string& game) {
    WorldSave w;
    w.folder_ = folder;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path file = folder / "world.json";
    if (std::filesystem::exists(file)) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, false);
        if (!j.is_object() || !j.contains("id")) throw WorldError(file.string() + " is damaged");
        w.info_.id = j.value("id", "");
        w.info_.name = j.value("name", w.info_.name);
        w.info_.description = j.value("description", "");
        try {
            w.info_.seed = std::stoull(j.value("seed", std::string("0")));
        } catch (const std::exception&) {
            throw WorldError(file.string() + " is damaged (its seed)");
        }
        w.info_.game = j.value("game", game);
        w.info_.created = j.value("created", "");
    } else {
        std::random_device rd;
        w.info_.id = new_id();
        w.info_.seed = (std::uint64_t(rd()) << 32) ^ rd();
        w.info_.game = game;
        w.info_.created = now_utc();
        const nlohmann::json j = {{"format", 1},
                                  {"id", w.info_.id},
                                  {"name", w.info_.name},
                                  {"description", w.info_.description},
                                  {"seed", std::to_string(w.info_.seed)},  // (JSON numbers are
                                  {"game", w.info_.game},                  // doubles elsewhere)
                                  {"created", w.info_.created}};
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << j.dump(2) << '\n';
        if (!out) throw WorldError("can't write " + file.string());
    }
    Db db(folder / "world.db");
    db.exec(kWorldSchema);
    // A world from before version 3: its place gains the scene (none: the starting one).
    bool has_scene = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.db, "SELECT name FROM pragma_table_info('place')", -1, &st,
                           nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            if (std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))) == "scene")
                has_scene = true;
        sqlite3_finalize(st);
    }
    if (!has_scene) db.exec("ALTER TABLE place ADD COLUMN scene TEXT");
    return w;
}

std::optional<WorldClock> WorldSave::clock() const {
    Db db(folder_ / "world.db");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.db, "SELECT game_time, scale FROM clock WHERE id = 1", -1, &st,
                           nullptr) != SQLITE_OK)
        throw WorldError("world.db has no clock");
    std::optional<WorldClock> out;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = WorldClock{sqlite3_column_int64(st, 0), sqlite3_column_double(st, 1)};
    sqlite3_finalize(st);
    return out;
}

void WorldSave::save_clock(const WorldClock& clock) const {
    Db db(folder_ / "world.db");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.db,
                           "INSERT INTO clock (id, game_time, scale) VALUES (1, ?, ?) "
                           "ON CONFLICT(id) DO UPDATE SET game_time = excluded.game_time, "
                           "scale = excluded.scale",
                           -1, &st, nullptr) != SQLITE_OK)
        throw WorldError("world.db has no clock");
    sqlite3_bind_int64(st, 1, clock.game_time);
    sqlite3_bind_double(st, 2, clock.scale);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) throw WorldError("can't save the clock in world.db");
}

std::optional<WorldPlace> WorldSave::place() const {
    Db db(folder_ / "world.db");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.db, "SELECT x, y, z, rx, ry, rz, scene FROM place WHERE id = 1", -1,
                           &st, nullptr) != SQLITE_OK)
        throw WorldError("world.db has no place");
    std::optional<WorldPlace> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = WorldPlace{};
        for (int i = 0; i < 3; ++i) {
            out->position[i] = sqlite3_column_double(st, i);
            out->rotation[i] = float(sqlite3_column_double(st, 3 + i));
        }
        if (const auto* scene = sqlite3_column_text(st, 6))
            out->scene = reinterpret_cast<const char*>(scene);
    }
    sqlite3_finalize(st);
    return out;
}

std::optional<std::string> WorldSave::value(const std::string& name) const {
    const auto all = values(name);
    const auto it = all.find(name);
    return it == all.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::map<std::string, std::string> WorldSave::values(const std::string& prefix) const {
    Db db(folder_ / "world.db");
    db.exec(kWorldSchema);  // (a world from before version 4)
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db.db, "SELECT name, value FROM state WHERE substr(name, 1, ?) = ?", -1,
                           &st, nullptr) != SQLITE_OK)
        throw WorldError("world.db has no state");
    sqlite3_bind_int(st, 1, int(prefix.size()));
    sqlite3_bind_text(st, 2, prefix.c_str(), -1, SQLITE_TRANSIENT);
    std::map<std::string, std::string> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const auto* n = sqlite3_column_text(st, 0);
        const auto* v = sqlite3_column_text(st, 1);
        if (n && v) out[reinterpret_cast<const char*>(n)] = reinterpret_cast<const char*>(v);
    }
    sqlite3_finalize(st);
    return out;
}

void WorldSave::save_values(const std::map<std::string, std::string>& values) const {
    Db db(folder_ / "world.db");
    db.exec(kWorldSchema);
    db.exec("BEGIN");
    for (const auto& [name, value] : values) {
        sqlite3_stmt* st = nullptr;
        const char* sql = value.empty() ? "DELETE FROM state WHERE name = ?"
                                        : "INSERT INTO state (name, value) VALUES (?, ?) "
                                          "ON CONFLICT(name) DO UPDATE SET value = excluded.value";
        if (sqlite3_prepare_v2(db.db, sql, -1, &st, nullptr) != SQLITE_OK) {
            db.exec("ROLLBACK");
            throw WorldError("world.db has no state");
        }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (!value.empty()) sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            db.exec("ROLLBACK");
            throw WorldError("can't save the game's state in world.db");
        }
    }
    db.exec("COMMIT");
}

void WorldSave::save_place(const WorldPlace& place) const {
    Db db(folder_ / "world.db");
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(
            db.db,
            "INSERT INTO place (id, x, y, z, rx, ry, rz, scene) VALUES (1, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET x = excluded.x, y = excluded.y, z = excluded.z, "
            "rx = excluded.rx, ry = excluded.ry, rz = excluded.rz, scene = excluded.scene",
            -1, &st, nullptr) != SQLITE_OK)
        throw WorldError("world.db has no place");
    for (int i = 0; i < 3; ++i) {
        sqlite3_bind_double(st, 1 + i, place.position[i]);
        sqlite3_bind_double(st, 4 + i, double(place.rotation[i]));
    }
    if (place.scene.empty())
        sqlite3_bind_null(st, 7);
    else
        sqlite3_bind_text(st, 7, place.scene.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) throw WorldError("can't save the player's place in world.db");
}

}  // namespace midgard
