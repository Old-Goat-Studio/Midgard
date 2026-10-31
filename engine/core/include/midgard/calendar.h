// SPDX-License-Identifier: Apache-2.0
// Calendars (roadmap §6; milestone 5.7): dates, seasons, daylight and solar phase from the clock.
//
// The clock counts game seconds, int64, from 0 = year 1, month 1, day 1, 00:00:00. A minute is
// always 60 seconds; a calendar says how many minutes make its hour and how many hours its day,
// so every calendar reads the same clock (the universal clock) its own way. Lighting follows the
// solar phase, not the hour: 0 is sunrise, 0.5 sunset and 1 the next sunrise, whatever the day's
// length or its share of daylight, which blends smoothly through the seasons.
//
// The formulas are specified in Python in tools/reference/calendar.py, and both are checked
// against schema/tests/calendar_*.json. Daylight uses det::cos, so it is the same on every target.
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace midgard {

class CalendarError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Calendar {
    struct Month {
        std::string name_key;
        int days = 30;
    };
    struct Season {
        std::string name_key;
        double share = 0.25;    // of the year
        double daylight = 0.5;  // the day's daylight share at the season's middle
        double temp_min = 0.0;  // degrees Celsius
        double temp_max = 20.0;
    };
    struct Moon {
        std::string name_key;
        double period = 29.5;  // days, full moon to full moon
        double offset = 0.0;   // its phase at the clock's 0 (0: new, 0.5: full)
    };

    std::string id, name_key, era_key;
    bool standard = false;
    int hours_per_day = 24, minutes_per_hour = 60;
    int leap_every = 0;  // years (0: none); a leap year's leap_month gains a day
    int leap_month = 1;  // 1-based
    int start_year = 1, start_month = 1, start_day = 1, start_minute = 360;
    double real_minutes_per_day = 48.0;
    double daylight = 0.6;    // without seasons
    double solar_noon = 0.5;  // the middle of daylight, as a share of the day
    double twilight = 0.04;   // dawn and dusk, as a share of the day
    double day_hours_min = 24.0, day_hours_max = 24.0;  // ranges for body calendars
    int year_days_min = 0, year_days_max = 0;           // 0: the months as they are
    double tilt_min = 0.0, tilt_max = 0.0;              // degrees
    std::vector<Month> months;
    std::vector<std::string> weekdays;  // name keys; day 1 of year 1 is weekday 0
    std::vector<Season> seasons;
    std::vector<Moon> moons;

    std::int64_t seconds_per_hour() const { return std::int64_t(minutes_per_hour) * 60; }
    std::int64_t seconds_per_day() const { return seconds_per_hour() * hours_per_day; }
    bool leap(std::int64_t year) const { return leap_every > 0 && year % leap_every == 0; }
    int month_days(std::int64_t year, int month) const;  // month 1-based
    int year_days(std::int64_t year) const;
    // Game seconds per real second: a day lasts real_minutes_per_day for the player.
    double time_scale() const;
    // Where a new game begins on the clock.
    std::int64_t start_time() const;
    // What makes the calendar unusable (the cook checks), each a sentence. Empty when it's fine.
    std::vector<std::string> problems() const;
};

// A moment as a calendar shows it.
struct Date {
    std::int64_t year = 1;
    int month = 1, day = 1;  // 1-based
    int hour = 0, minute = 0, second = 0;
    int weekday = 0;              // 0-based, into the calendar's weekdays
    int day_of_year = 0;          // 0-based
    int season = -1;              // 0-based, or -1 without seasons
    std::int64_t day_number = 0;  // whole days since the clock's 0
    std::int64_t seconds_into_day = 0;
};

// The date at a moment (times before 0 are taken as 0). Throws CalendarError if the calendar
// has no days or weekdays to count with.
Date date_at(const Calendar& c, std::int64_t time);
// The moment a date starts, plus seconds into its day. Throws CalendarError for dates the
// calendar doesn't have.
std::int64_t time_of(const Calendar& c, std::int64_t year, int month, int day,
                     std::int64_t seconds_into_day = 0);

// The season of a day of the year (by its middle), or -1 without seasons.
int season_on(const Calendar& c, std::int64_t year, int day_of_year);
// The day's share of daylight (0.01 to 0.99): the seasons' daylight blended over the year as
// base + amplitude cos(2 pi (day - longest day) / year length), or the calendar's own without
// seasons.
double daylight_on(const Calendar& c, std::int64_t year, int day_of_year);

// The reference formula (roadmap §6): 0 = sunrise, 0.5 = sunset, 1 = the next sunrise.
double solar_phase(double seconds_into_day, double day_length, double day_fraction,
                   double noon = 0.5);
// The solar phase at a moment, by the calendar.
double solar_phase_at(const Calendar& c, std::int64_t time);
// How high the sun is, in degrees: positive by day, negative at night.
double sun_elevation(double phase, double max_elevation_degrees);

// Sunrise and sunset on the day of a moment, in seconds into that day (sunset may be the next
// day's, past seconds_per_day, when daylight runs over midnight).
struct SunTimes {
    double sunrise = 0.0, sunset = 0.0;
};
SunTimes sun_times(const Calendar& c, std::int64_t time);

// A moon's phase at a moment: 0 new, 0.5 full, back to 1.
double moon_phase(const Calendar& c, size_t moon, std::int64_t time);

// A calendar from JSON with the calendar table's columns as keys, and "months" ([{"name_key",
// "days"}]), "weekdays" (name keys), "seasons" ([{"name_key", "share", "daylight", "temp_min",
// "temp_max"}]) and "moons" ([{"name_key", "period", "offset"}]). Missing keys keep their defaults.
Calendar calendar_from_json(const nlohmann::json& j);

// Every calendar in a project or package database (none before migration 9), standard first.
// Throws CalendarError if the database can't be read.
std::vector<Calendar> load_calendars(const std::filesystem::path& database);
// The problems of a project's calendars together: each calendar's, and that exactly one is the
// standard calendar.
std::vector<std::string> calendar_problems(const std::vector<Calendar>& calendars);

}  // namespace midgard
