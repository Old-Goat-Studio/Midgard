// SPDX-License-Identifier: Apache-2.0
// Mirrors tools/reference/calendar.py: the same steps, with floats combined in the same order.
#include "midgard/calendar.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <sqlite3.h>

#include "midgard/det.h"

namespace midgard {
namespace {

constexpr double kPi = 3.141592653589793;

// Python's x % 1.0 (the result has the sign of the divisor).
double mod1(double x) {
    double m = std::fmod(x, 1.0);
    if (m != 0.0 && m < 0.0) m += 1.0;
    return m == 0.0 ? 0.0 : m;
}

std::int64_t base_days(const Calendar& c) {
    std::int64_t days = 0;
    for (const Calendar::Month& m : c.months) days += m.days;
    return days;
}

// (year, day of year) for a whole-day number (0 or more).
std::pair<std::int64_t, int> year_of(const Calendar& c, std::int64_t day) {
    const std::int64_t base = base_days(c);
    std::int64_t year = 1;
    if (c.leap_every > 0) {
        const std::int64_t cycle = c.leap_every * base + 1;  // a cycle ends with its leap year
        year = 1 + day / cycle * c.leap_every;
        day %= cycle;
    } else {
        year = 1 + day / base;
        day %= base;
    }
    while (day >= c.year_days(year)) {
        day -= c.year_days(year);
        ++year;
    }
    return {year, int(day)};
}

std::string quote(const Calendar& c) {
    return "calendar '" + c.id + "': ";
}

}  // namespace

int Calendar::month_days(std::int64_t year, int month) const {
    return months[size_t(month - 1)].days + (leap(year) && month == leap_month ? 1 : 0);
}

int Calendar::year_days(std::int64_t year) const {
    return int(base_days(*this)) + (leap(year) ? 1 : 0);
}

double Calendar::time_scale() const {
    return double(seconds_per_day()) / (real_minutes_per_day * 60.0);
}

std::int64_t Calendar::start_time() const {
    return time_of(*this, start_year, start_month, start_day, std::int64_t(start_minute) * 60);
}

std::vector<std::string> Calendar::problems() const {
    std::vector<std::string> out;
    const auto bad = [&](const std::string& text) { out.push_back(quote(*this) + text); };
    if (hours_per_day < 1) bad("a day needs at least one hour");
    if (minutes_per_hour < 1) bad("an hour needs at least one minute");
    if (months.empty()) bad("it has no months");
    for (size_t i = 0; i < months.size(); ++i)
        if (months[i].days < 1) bad("month " + std::to_string(i + 1) + " has no days");
    if (weekdays.empty()) bad("it has no weekdays");
    if (leap_every < 0) bad("leap years can't come every " + std::to_string(leap_every) + " years");
    if (leap_every > 0 && (leap_month < 1 || leap_month > int(months.size())))
        bad("the leap day goes in month " + std::to_string(leap_month) + ", which it doesn't have");
    if (!(daylight >= 0.01 && daylight <= 0.99)) bad("daylight must be between 1% and 99%");
    if (!(solar_noon >= 0.0 && solar_noon < 1.0)) bad("solar noon must be within the day");
    if (!(twilight >= 0.0 && twilight <= 0.25)) bad("twilight must be between 0% and 25%");
    if (!(real_minutes_per_day > 0.0)) bad("a day must last some real minutes");
    if (!seasons.empty()) {
        double total = 0.0;
        for (size_t i = 0; i < seasons.size(); ++i) {
            const Season& s = seasons[i];
            total += s.share;
            const std::string which = "season " + std::to_string(i + 1);
            if (!(s.share > 0.0)) bad(which + " has no share of the year");
            if (!(s.daylight >= 0.01 && s.daylight <= 0.99))
                bad(which + "'s daylight must be between 1% and 99%");
            if (!(s.temp_min <= s.temp_max))
                bad(which + "'s temperature range is the wrong way round");
        }
        if (std::abs(total - 1.0) > 1e-6)
            bad("the seasons' shares add up to " + std::to_string(total) + ", not 1");
    }
    for (size_t i = 0; i < moons.size(); ++i)
        if (!(moons[i].period > 0.0)) bad("moon " + std::to_string(i + 1) + " has no period");
    if (!(day_hours_min >= 1.0 && day_hours_min <= day_hours_max))
        bad("the day length range must be at least 1 hour, smallest first");
    if (year_days_min < 0 || year_days_min > year_days_max)
        bad("the year length range must be 0 or more days, smallest first");
    if (!(tilt_min >= 0.0 && tilt_min <= tilt_max && tilt_max <= 90.0))
        bad("the tilt range must be within 0-90 degrees, smallest first");
    if (out.empty()) {
        try {
            if (start_minute < 0 || std::int64_t(start_minute) * 60 >= seconds_per_day())
                throw CalendarError("");
            (void)start_time();
        } catch (const CalendarError&) {
            bad("its start date isn't a date it has");
        }
    }
    return out;
}

Date date_at(const Calendar& c, std::int64_t time) {
    if (base_days(c) < 1 || c.weekdays.empty() || c.seconds_per_day() < 1)
        throw CalendarError(quote(c) + "it has no days, weekdays or hours to count with");
    time = std::max<std::int64_t>(time, 0);
    const std::int64_t spd = c.seconds_per_day();
    Date d;
    d.day_number = time / spd;
    d.seconds_into_day = time % spd;
    const auto [year, day_of_year] = year_of(c, d.day_number);
    d.year = year;
    d.day_of_year = day_of_year;
    int month = 1, rest = day_of_year;
    while (rest >= c.month_days(year, month)) {
        rest -= c.month_days(year, month);
        ++month;
    }
    d.month = month;
    d.day = rest + 1;
    const std::int64_t hour = c.seconds_per_hour();
    d.hour = int(d.seconds_into_day / hour);
    d.minute = int(d.seconds_into_day % hour / 60);
    d.second = int(d.seconds_into_day % 60);
    d.weekday = int(d.day_number % std::int64_t(c.weekdays.size()));
    d.season = season_on(c, year, day_of_year);
    return d;
}

std::int64_t time_of(const Calendar& c, std::int64_t year, int month, int day,
                     std::int64_t seconds_into_day) {
    if (year < 1 || month < 1 || month > int(c.months.size()) || day < 1 ||
        day > c.month_days(year, month) || seconds_into_day < 0 ||
        seconds_into_day >= c.seconds_per_day())
        throw CalendarError(quote(c) + "there is no such date");
    const std::int64_t base = base_days(c);
    std::int64_t days = 0, y = 1;
    if (c.leap_every > 0) {
        const std::int64_t n = (year - 1) / c.leap_every;
        days = n * (c.leap_every * base + 1);
        y = 1 + n * c.leap_every;
    } else {
        days = (year - 1) * base;
        y = year;
    }
    for (; y < year; ++y) days += c.year_days(y);
    for (int m = 1; m < month; ++m) days += c.month_days(year, m);
    return (days + day - 1) * c.seconds_per_day() + seconds_into_day;
}

int season_on(const Calendar& c, std::int64_t year, int day_of_year) {
    if (c.seasons.empty()) return -1;
    const double f = (day_of_year + 0.5) / c.year_days(year);
    double acc = 0.0;
    for (size_t i = 0; i < c.seasons.size(); ++i) {
        acc += c.seasons[i].share;
        if (f < acc) return int(i);
    }
    return int(c.seasons.size()) - 1;
}

namespace {

// The seasons' daylight over the year: base + amplitude cos(2 pi (f - middle)), f the day's
// middle as a fraction of the year. Needs two seasons or more.
struct SeasonBlend {
    double base = 0.0, amplitude = 0.0, middle = 0.0;
};

SeasonBlend season_blend(const Calendar& c) {
    size_t longest = 0;
    double lo = c.seasons[0].daylight;
    for (size_t i = 0; i < c.seasons.size(); ++i) {
        if (c.seasons[i].daylight > c.seasons[longest].daylight) longest = i;
        lo = std::min(lo, c.seasons[i].daylight);
    }
    const double hi = c.seasons[longest].daylight;
    SeasonBlend b;
    b.base = (hi + lo) / 2;
    b.amplitude = (hi - lo) / 2;
    double start = 0.0;
    for (size_t i = 0; i < longest; ++i) start += c.seasons[i].share;
    b.middle = start + c.seasons[longest].share / 2;
    return b;
}

double year_wave(const Calendar& c, const SeasonBlend& b, std::int64_t year, int day_of_year) {
    const double f = (day_of_year + 0.5) / c.year_days(year);
    return det::cos(2 * kPi * (f - b.middle));
}

}  // namespace

double daylight_on(const Calendar& c, std::int64_t year, int day_of_year) {
    double d = c.daylight;
    if (c.seasons.size() == 1) {
        d = c.seasons[0].daylight;
    } else if (c.seasons.size() > 1) {
        const SeasonBlend b = season_blend(c);
        d = b.base + b.amplitude * year_wave(c, b, year, day_of_year);
    }
    return std::min(std::max(d, 0.01), 0.99);
}

double summer_on(const Calendar& c, std::int64_t year, int day_of_year) {
    if (c.seasons.size() < 2) return 0.0;
    const SeasonBlend b = season_blend(c);
    return b.amplitude > 0.0 ? year_wave(c, b, year, day_of_year) : 0.0;
}

double solar_phase(double seconds_into_day, double day_length, double day_fraction, double noon) {
    const double d = std::min(std::max(day_fraction, 0.01), 0.99);
    const double t = mod1(seconds_into_day / day_length - noon + d / 2);  // 0 at sunrise
    if (t < d) return 0.5 * t / d;                                        // day: 0 -> 0.5
    return 0.5 + 0.5 * (t - d) / (1 - d);                                 // night: 0.5 -> 1
}

double solar_phase_at(const Calendar& c, std::int64_t time) {
    const Date d = date_at(c, time);
    return solar_phase(double(d.seconds_into_day), double(c.seconds_per_day()),
                       daylight_on(c, d.year, d.day_of_year), c.solar_noon);
}

double sun_elevation(double phase, double max_elevation_degrees) {
    return max_elevation_degrees * det::sin(2 * kPi * phase);
}

SunTimes sun_times(const Calendar& c, std::int64_t time) {
    const Date d = date_at(c, time);
    const double spd = double(c.seconds_per_day());
    const double share = daylight_on(c, d.year, d.day_of_year);
    const double sunrise = mod1(c.solar_noon - share / 2) * spd;
    return {sunrise, sunrise + share * spd};
}

double moon_phase(const Calendar& c, size_t moon, std::int64_t time) {
    const Date d = date_at(c, time);
    const Calendar::Moon& m = c.moons.at(moon);
    return mod1((double(d.day_number) + double(d.seconds_into_day) / double(c.seconds_per_day())) /
                    m.period +
                m.offset);
}

Calendar roll_body_calendar(const Calendar& tmpl, std::uint64_t seed) {
    const det::Rng rng = det::Rng(seed).split(det::tag("calendar"));
    Calendar out = tmpl;
    out.standard = false;

    const int lo = int(std::ceil(tmpl.day_hours_min));
    const int hi = int(std::floor(tmpl.day_hours_max));
    int hours = 0;
    if (hi < lo) {
        hours = std::max(1, int(tmpl.day_hours_min + 0.5));
    } else {
        det::Rng day = rng.split(det::tag("day"));
        hours = lo + int(day.below(std::uint64_t(hi - lo + 1)));
    }
    out.hours_per_day = hours;
    out.day_hours_min = out.day_hours_max = double(hours);

    if (tmpl.year_days_max > 0) {
        det::Rng year = rng.split(det::tag("year"));
        std::int64_t target =
            tmpl.year_days_min +
            std::int64_t(year.below(std::uint64_t(tmpl.year_days_max - tmpl.year_days_min + 1)));
        target = std::max<std::int64_t>(target, std::int64_t(tmpl.months.size()));
        const std::int64_t base = base_days(tmpl);
        std::int64_t cumulative = 0, previous = 0;
        for (Calendar::Month& m : out.months) {
            cumulative += m.days;
            const std::int64_t boundary = cumulative * target / base;
            m.days = int(boundary - previous);
            previous = boundary;
        }
        for (Calendar::Month& m : out.months) {  // every month keeps a day, from the longest
            if (m.days < 1) {
                Calendar::Month* longest = &out.months[0];
                for (Calendar::Month& x : out.months)
                    if (x.days > longest->days) longest = &x;
                longest->days -= 1 - m.days;
                m.days = 1;
            }
        }
        out.year_days_min = out.year_days_max = int(target);
    }

    det::Rng tilt = rng.split(det::tag("tilt"));
    out.tilt = tmpl.tilt_min + (tmpl.tilt_max - tmpl.tilt_min) * tilt.uniform();
    out.tilt_min = out.tilt_max = out.tilt;
    return out;
}

Calendar calendar_from_json(const nlohmann::json& j) {
    Calendar c;
    c.id = j.value("id", c.id);
    c.name_key = j.value("name_key", c.name_key);
    c.era_key = j.value("era_key", c.era_key);
    c.standard = j.value("standard", 0) != 0;
    c.hours_per_day = j.value("hours_per_day", c.hours_per_day);
    c.minutes_per_hour = j.value("minutes_per_hour", c.minutes_per_hour);
    c.leap_every = j.value("leap_every", c.leap_every);
    c.leap_month = j.value("leap_month", c.leap_month);
    c.start_year = j.value("start_year", c.start_year);
    c.start_month = j.value("start_month", c.start_month);
    c.start_day = j.value("start_day", c.start_day);
    c.start_minute = j.value("start_minute", c.start_minute);
    c.real_minutes_per_day = j.value("real_minutes_per_day", c.real_minutes_per_day);
    c.daylight = j.value("daylight", c.daylight);
    c.solar_noon = j.value("solar_noon", c.solar_noon);
    c.twilight = j.value("twilight", c.twilight);
    c.day_hours_min = j.value("day_hours_min", c.day_hours_min);
    c.day_hours_max = j.value("day_hours_max", c.day_hours_max);
    c.year_days_min = j.value("year_days_min", c.year_days_min);
    c.year_days_max = j.value("year_days_max", c.year_days_max);
    c.tilt_min = j.value("tilt_min", c.tilt_min);
    c.tilt_max = j.value("tilt_max", c.tilt_max);
    c.tilt = j.value("tilt", c.tilt);
    for (const auto& m : j.value("months", nlohmann::json::array()))
        c.months.push_back({m.value("name_key", ""), m.value("days", 30)});
    for (const auto& w : j.value("weekdays", nlohmann::json::array()))
        c.weekdays.push_back(w.is_string() ? w.get<std::string>() : w.value("name_key", ""));
    for (const auto& s : j.value("seasons", nlohmann::json::array()))
        c.seasons.push_back({s.value("name_key", ""), s.value("share", 0.0),
                             s.value("daylight", 0.5), s.value("temp_min", 0.0),
                             s.value("temp_max", 20.0)});
    for (const auto& m : j.value("moons", nlohmann::json::array()))
        c.moons.push_back(
            {m.value("name_key", ""), m.value("period", 29.5), m.value("offset", 0.0)});
    return c;
}

std::vector<std::string> calendar_problems(const std::vector<Calendar>& calendars) {
    std::vector<std::string> out;
    int standard = 0;
    for (const Calendar& c : calendars) {
        for (std::string& p : c.problems()) out.push_back(std::move(p));
        standard += c.standard ? 1 : 0;
    }
    if (!calendars.empty() && standard != 1)
        out.push_back("exactly one calendar must be the standard calendar (there are " +
                      std::to_string(standard) + ")");
    return out;
}

// ---- the database -----------------------------------------------------------------------
namespace {

struct Db {
    sqlite3* db = nullptr;
    ~Db() { sqlite3_close(db); }
};

struct Statement {
    sqlite3_stmt* stmt = nullptr;
    ~Statement() { sqlite3_finalize(stmt); }
};

std::string text(sqlite3_stmt* s, int col) {
    const auto* t = sqlite3_column_text(s, col);
    return t ? reinterpret_cast<const char*>(t) : "";
}

// Runs a query, calling `row` for each row. False if the table isn't there.
template <typename Row>
bool each(sqlite3* db, const char* sql, const std::string& calendar, Row row) {
    Statement st;
    if (sqlite3_prepare_v2(db, sql, -1, &st.stmt, nullptr) != SQLITE_OK) return false;
    if (!calendar.empty()) sqlite3_bind_text(st.stmt, 1, calendar.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st.stmt) == SQLITE_ROW) row(st.stmt);
    return true;
}

}  // namespace

std::vector<Calendar> load_calendars(const std::filesystem::path& database) {
    Db db;
    const std::u8string u8 = database.u8string();
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &db.db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK)
        throw CalendarError("can't open " + database.string());
    std::vector<Calendar> out;
    const bool has_tables = each(
        db.db,
        "SELECT id, name_key, standard, hours_per_day, minutes_per_hour, leap_every, leap_month, "
        "COALESCE(era_key, ''), start_year, start_month, start_day, start_minute, "
        "real_minutes_per_day, daylight, solar_noon, twilight, day_hours_min, day_hours_max, "
        "year_days_min, year_days_max, tilt_min, tilt_max FROM calendar "
        "ORDER BY standard DESC, id",
        "", [&](sqlite3_stmt* s) {
            Calendar c;
            c.id = text(s, 0);
            c.name_key = text(s, 1);
            c.standard = sqlite3_column_int(s, 2) != 0;
            c.hours_per_day = sqlite3_column_int(s, 3);
            c.minutes_per_hour = sqlite3_column_int(s, 4);
            c.leap_every = sqlite3_column_int(s, 5);
            c.leap_month = sqlite3_column_int(s, 6);
            c.era_key = text(s, 7);
            c.start_year = sqlite3_column_int(s, 8);
            c.start_month = sqlite3_column_int(s, 9);
            c.start_day = sqlite3_column_int(s, 10);
            c.start_minute = sqlite3_column_int(s, 11);
            c.real_minutes_per_day = sqlite3_column_double(s, 12);
            c.daylight = sqlite3_column_double(s, 13);
            c.solar_noon = sqlite3_column_double(s, 14);
            c.twilight = sqlite3_column_double(s, 15);
            c.day_hours_min = sqlite3_column_double(s, 16);
            c.day_hours_max = sqlite3_column_double(s, 17);
            c.year_days_min = sqlite3_column_int(s, 18);
            c.year_days_max = sqlite3_column_int(s, 19);
            c.tilt_min = sqlite3_column_double(s, 20);
            c.tilt_max = sqlite3_column_double(s, 21);
            out.push_back(std::move(c));
        });
    if (!has_tables) return {};  // a package from before migration 9
    for (Calendar& c : out) {
        each(db.db, "SELECT name_key, days FROM calendar_month WHERE calendar = ? ORDER BY idx",
             c.id,
             [&](sqlite3_stmt* s) { c.months.push_back({text(s, 0), sqlite3_column_int(s, 1)}); });
        each(db.db, "SELECT name_key FROM calendar_weekday WHERE calendar = ? ORDER BY idx", c.id,
             [&](sqlite3_stmt* s) { c.weekdays.push_back(text(s, 0)); });
        each(db.db,
             "SELECT name_key, share, daylight, temp_min, temp_max FROM calendar_season "
             "WHERE calendar = ? ORDER BY idx",
             c.id, [&](sqlite3_stmt* s) {
                 c.seasons.push_back({text(s, 0), sqlite3_column_double(s, 1),
                                      sqlite3_column_double(s, 2), sqlite3_column_double(s, 3),
                                      sqlite3_column_double(s, 4)});
             });
        each(db.db,
             "SELECT name_key, period, offset FROM calendar_moon WHERE calendar = ? ORDER BY idx",
             c.id, [&](sqlite3_stmt* s) {
                 c.moons.push_back(
                     {text(s, 0), sqlite3_column_double(s, 1), sqlite3_column_double(s, 2)});
             });
    }
    return out;
}

}  // namespace midgard
