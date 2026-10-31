// SPDX-License-Identifier: Apache-2.0
// Calendars (midgard/calendar.h): against the shared table (schema/tests/calendar_dates.json,
// which tools/reference/calendar.py is checked against too), the cook checks, and the standard
// calendar every project gets from migration 9.
#include <doctest/doctest.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include "midgard/calendar.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string bits(double x) {
    std::uint64_t b = 0;
    std::memcpy(&b, &x, sizeof(b));
    char out[17];
    std::snprintf(out, sizeof(out), "%016" PRIx64, b);
    return out;
}

json shared_table() {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "calendar_dates.json");
    REQUIRE(in);
    return json::parse(in);
}

// A database with every migration applied: the standard calendar is in it.
fs::path migrated_database() {
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("midgard_calendar_" + std::to_string(rd()));
    fs::create_directories(dir);
    const fs::path file = dir / "Game.db";
    sqlite3* db = nullptr;
    const std::u8string u8 = file.u8string();
    sqlite3_open(std::string(u8.begin(), u8.end()).c_str(), &db);
    std::vector<fs::path> migrations;
    for (const auto& e : fs::directory_iterator(fs::path(MIDGARD_SCHEMA_DIR) / "migrations"))
        migrations.push_back(e.path());
    std::sort(migrations.begin(), migrations.end());
    for (const auto& m : migrations) {
        std::ifstream in(m);
        std::stringstream sql;
        sql << in.rdbuf();
        char* err = nullptr;
        REQUIRE_MESSAGE(sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, &err) == SQLITE_OK,
                        (err ? err : ""));
    }
    sqlite3_close(db);
    return file;
}

}  // namespace

TEST_CASE("calendar dates, daylight, solar phase and moons match the shared table") {
    const json table = shared_table();
    std::map<std::string, midgard::Calendar> calendars;
    for (const auto& [name, c] : table.at("calendars").items()) {
        calendars[name] = midgard::calendar_from_json(c);
        calendars[name].id = name;
    }
    REQUIRE(table.at("cases").size() > 1000);
    for (const json& c : table.at("cases")) {
        const midgard::Calendar& cal = calendars.at(c.at("calendar").get<std::string>());
        const std::int64_t t = c.at("time").get<std::int64_t>();
        const midgard::Date d = midgard::date_at(cal, t);
        CAPTURE(c.dump());
        CHECK(d.year == c.at("year").get<std::int64_t>());
        CHECK(d.month == c.at("month").get<int>());
        CHECK(d.day == c.at("day").get<int>());
        CHECK(d.hour == c.at("hour").get<int>());
        CHECK(d.minute == c.at("minute").get<int>());
        CHECK(d.second == c.at("second").get<int>());
        CHECK(d.weekday == c.at("weekday").get<int>());
        CHECK(d.day_of_year == c.at("day_of_year").get<int>());
        CHECK(d.season == c.at("season").get<int>());
        CHECK(bits(midgard::daylight_on(cal, d.year, d.day_of_year)) ==
              c.at("daylight").get<std::string>());
        CHECK(bits(midgard::solar_phase_at(cal, t)) == c.at("phase").get<std::string>());
        const midgard::SunTimes sun = midgard::sun_times(cal, t);
        CHECK(bits(sun.sunrise) == c.at("sunrise").get<std::string>());
        CHECK(bits(sun.sunset) == c.at("sunset").get<std::string>());
        for (size_t i = 0; i < cal.moons.size(); ++i)
            CHECK(bits(midgard::moon_phase(cal, i, t)) == c.at("moons").at(i).get<std::string>());
        CHECK(midgard::time_of(cal, d.year, d.month, d.day, d.seconds_into_day) == t);
    }
}

TEST_CASE("a 20-hour day with 70% daylight rises at 03:00 and sets at 17:00") {
    const midgard::Calendar twenty =
        midgard::calendar_from_json(shared_table().at("calendars").at("twenty"));
    const midgard::SunTimes sun = midgard::sun_times(twenty, 0);
    CHECK(sun.sunrise == doctest::Approx(3 * 3600.0));
    CHECK(sun.sunset == doctest::Approx(17 * 3600.0));
    CHECK(midgard::solar_phase_at(twenty, 3 * 3600) == 0.0);
    CHECK(midgard::solar_phase_at(twenty, 10 * 3600) == 0.25);  // solar noon
    CHECK(midgard::solar_phase_at(twenty, 17 * 3600) == 0.5);
    CHECK(midgard::sun_elevation(0.25, 60.0) == doctest::Approx(60.0));  // highest at noon
    CHECK(midgard::sun_elevation(0.75, 60.0) == doctest::Approx(-60.0));
}

TEST_CASE("every project starts with a usable standard calendar") {
    const fs::path db = migrated_database();
    const std::vector<midgard::Calendar> calendars = midgard::load_calendars(db);
    REQUIRE(calendars.size() == 1);
    const midgard::Calendar& c = calendars[0];
    CHECK(c.standard);
    CHECK(c.id == "cal_standard");
    CHECK(c.months.size() == 12);
    CHECK(c.weekdays.size() == 7);
    CHECK(c.seasons.size() == 4);
    CHECK(c.moons.size() == 1);
    CHECK(midgard::calendar_problems(calendars).empty());
    CHECK(c.year_days(1) == 365);
    CHECK(c.year_days(4) == 366);                    // February 29th
    CHECK(c.time_scale() == doctest::Approx(30.0));  // a day in 48 real minutes
    const midgard::Date start = midgard::date_at(c, c.start_time());
    CHECK(start.year == 1);
    CHECK(start.month == 4);
    CHECK(start.day == 1);
    CHECK(start.hour == 6);

    // A database from before migration 9 has none; a file that isn't there is an error.
    const fs::path old = db.parent_path() / "old.db";
    sqlite3* raw = nullptr;
    sqlite3_open(old.string().c_str(), &raw);
    sqlite3_exec(raw, "CREATE TABLE meta (key TEXT)", nullptr, nullptr, nullptr);
    sqlite3_close(raw);
    CHECK(midgard::load_calendars(old).empty());
    CHECK_THROWS_AS(midgard::load_calendars(db.parent_path() / "missing.db"),
                    midgard::CalendarError);
    std::error_code ec;
    fs::remove_all(db.parent_path(), ec);
}

TEST_CASE("the cook checks find calendars that can't work") {
    const json table = shared_table();
    midgard::Calendar good = midgard::calendar_from_json(table.at("calendars").at("standard"));
    good.id = "cal_good";
    good.standard = true;
    good.start_month = 4;
    CHECK(good.problems().empty());

    const auto problems_of = [&](auto change) {
        midgard::Calendar c = good;
        change(c);
        return c.problems();
    };
    const auto mentions = [](const std::vector<std::string>& problems, const char* text) {
        for (const std::string& p : problems)
            if (p.find(text) != std::string::npos && p.find("cal_good") != std::string::npos)
                return true;
        return false;
    };
    CHECK(mentions(problems_of([](auto& c) { c.months[3].days = 0; }), "month 4 has no days"));
    CHECK(mentions(problems_of([](auto& c) { c.months.clear(); }), "no months"));
    CHECK(mentions(problems_of([](auto& c) { c.weekdays.clear(); }), "no weekdays"));
    CHECK(mentions(problems_of([](auto& c) { c.seasons[0].share = 0.3; }), "add up to"));
    CHECK(mentions(problems_of([](auto& c) { c.seasons[2].daylight = 1.0; }), "1% and 99%"));
    CHECK(mentions(problems_of([](auto& c) { c.daylight = 0.0; }), "1% and 99%"));
    CHECK(mentions(problems_of([](auto& c) { c.hours_per_day = 0; }), "at least one hour"));
    CHECK(mentions(problems_of([](auto& c) { c.leap_month = 13; }), "leap day"));
    CHECK(mentions(problems_of([](auto& c) { c.day_hours_min = 40; }), "day length range"));
    CHECK(mentions(problems_of([](auto& c) { c.year_days_max = -1; }), "year length range"));
    CHECK(mentions(problems_of([](auto& c) { c.tilt_max = 100; }), "tilt range"));
    CHECK(mentions(problems_of([](auto& c) { c.moons[0].period = 0; }), "moon 1"));
    CHECK(mentions(problems_of([](auto& c) { c.start_day = 31; }), "start date"));  // April 31st
    CHECK(mentions(problems_of([](auto& c) { c.start_minute = 24 * 60; }), "start date"));

    // Exactly one standard calendar.
    midgard::Calendar second = good;
    second.id = "cal_second";
    CHECK(midgard::calendar_problems({good}).empty());
    CHECK_FALSE(midgard::calendar_problems({good, second}).empty());  // two
    second.standard = good.standard = false;
    CHECK_FALSE(midgard::calendar_problems({good, second}).empty());  // none
    CHECK(midgard::calendar_problems({}).empty());  // no calendars at all: nothing to check

    CHECK_THROWS_AS(midgard::time_of(good, 1, 2, 29), midgard::CalendarError);  // not a leap year
    CHECK(midgard::time_of(good, 4, 2, 29) > 0);
    midgard::Calendar empty;
    CHECK_THROWS_AS(midgard::date_at(empty, 0), midgard::CalendarError);
}
