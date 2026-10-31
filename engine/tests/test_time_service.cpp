// SPDX-License-Identifier: Apache-2.0
// The game's clock (midgard/time_service.h) and body calendars rolled from a seed.
#include <doctest/doctest.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "midgard/det.h"
#include "midgard/time_service.h"

namespace fs = std::filesystem;
using midgard::TimeEvent;
using midgard::TimeSignal;
using nlohmann::json;

namespace {

json table(const char* name) {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / name);
    REQUIRE(in);
    return json::parse(in);
}

midgard::Calendar shared_calendar(const char* name) {
    midgard::Calendar c =
        midgard::calendar_from_json(table("calendar_dates.json").at("calendars").at(name));
    c.id = name;
    return c;
}

std::string bits(double x) {
    std::uint64_t b = 0;
    std::memcpy(&b, &x, sizeof(b));
    char out[17];
    std::snprintf(out, sizeof(out), "%016" PRIx64, b);
    return out;
}

// (signal, the whole second it falls in) for each event: what a per-second scan can see.
using Seen = std::multiset<std::tuple<int, std::int64_t>>;

Seen seen(const std::vector<TimeEvent>& events) {
    Seen out;
    for (const TimeEvent& e : events) out.insert({int(e.signal), std::int64_t(std::ceil(e.time))});
    return out;
}

// Every boundary in (from, to], found the slow way: second by second, by what changed.
Seen scan(const midgard::Calendar& c, std::int64_t from, std::int64_t to) {
    Seen out;
    midgard::Date before = midgard::date_at(c, from);
    double phase_before = midgard::solar_phase_at(c, from);
    for (std::int64_t t = from + 1; t <= to; ++t) {
        const midgard::Date d = midgard::date_at(c, t);
        const double phase = midgard::solar_phase_at(c, t);
        const bool new_day = d.day_number != before.day_number;
        if (new_day || d.hour != before.hour) out.insert({int(TimeSignal::Hour), t});
        if (new_day) out.insert({int(TimeSignal::Day), t});
        if (d.month != before.month || d.year != before.year)
            out.insert({int(TimeSignal::Month), t});
        if (d.season != before.season) out.insert({int(TimeSignal::Season), t});
        if (d.year != before.year) out.insert({int(TimeSignal::Year), t});
        // Dawn wraps the phase from near 1 to near 0 (a new day's daylight can move it a little).
        if (phase + 0.5 < phase_before) out.insert({int(TimeSignal::Dawn), t});
        if (phase_before < 0.5 && phase >= 0.5) out.insert({int(TimeSignal::Dusk), t});
        before = d;
        phase_before = phase;
    }
    return out;
}

}  // namespace

TEST_CASE("the clock never drifts: 10 million frames") {
    midgard::TimeService exact;  // 1/60 s at 1x: not a whole number of seconds a frame
    for (int i = 0; i < 10'000'000; ++i) exact.update(1.0 / 60.0);
    CHECK(exact.now() == 166'666);
    CHECK(exact.precise() == doctest::Approx(10'000'000.0 / 60.0).epsilon(1e-12));

    midgard::TimeService fast(1000);  // an uneven frame at 30x, from a start time
    fast.set_scale(30.0);
    for (int i = 0; i < 10'000'000; ++i) fast.update(0.0173);
    CHECK(std::abs(fast.precise() - (1000.0 + 10'000'000.0 * 0.0173 * 30.0)) < 1e-3);

    midgard::TimeService stopped(500);
    stopped.set_scale(0.0);
    stopped.update(10.0);
    stopped.set_scale(-5.0);  // never backwards
    stopped.update(10.0);
    CHECK(stopped.now() == 500);
    CHECK(stopped.scale() == 0.0);
}

TEST_CASE("every signal fires exactly once at its boundary, in small steps or one skip") {
    // Spans over a leap day and a year's end, a season change, 20-hour days with 70% daylight,
    // and 30-hour days of 50-minute hours whose noon is off-centre.
    struct Span {
        const char* calendar;
        std::int64_t year;
        int month, day, days;
    };
    for (const Span& s : {Span{"standard", 4, 2, 27, 4}, Span{"standard", 4, 12, 30, 3},
                          Span{"standard", 1, 3, 30, 3}, Span{"twenty", 3, 3, 5, 6},
                          Span{"odd", 2, 1, 1, 4}, Span{"odd", 1, 4, 1, 3}}) {
        CAPTURE(std::string(s.calendar));
        CAPTURE(s.year);
        const midgard::Calendar c = shared_calendar(s.calendar);
        const std::int64_t from = midgard::time_of(c, s.year, s.month, s.day) - 7;
        const std::int64_t to = from + s.days * c.seconds_per_day();
        const Seen expected = scan(c, from, to);
        CHECK(expected.size() > std::size_t(s.days) * (c.hours_per_day + 3));

        midgard::TimeService steps(from);  // frames of uneven length at 600x
        steps.follow(c);
        steps.set_scale(600.0);
        std::vector<TimeEvent> stepped;
        midgard::det::Rng rng(42);
        while (steps.now() < to) {
            const double frame = 0.002 + 0.03 * rng.uniform();
            if (steps.now() + std::int64_t(frame * 600.0) + 2 > to) {
                for (const TimeEvent& e : steps.advance(to - steps.now())) stepped.push_back(e);
                break;
            }
            for (const TimeEvent& e : steps.update(frame)) stepped.push_back(e);
        }
        CHECK(steps.now() == to);
        CHECK(seen(stepped) == expected);

        midgard::TimeService skip(from);  // the same span at once
        skip.follow(c);
        const std::vector<TimeEvent> skipped = skip.advance(to - from);
        CHECK(seen(skipped) == expected);
        REQUIRE(skipped.size() == stepped.size());
        for (size_t i = 0; i < skipped.size(); ++i) {
            CHECK(skipped[i].signal == stepped[i].signal);
            CHECK(skipped[i].time == stepped[i].time);
        }
        for (size_t i = 1; i < skipped.size(); ++i) CHECK(skipped[i - 1].time <= skipped[i].time);
    }
}

TEST_CASE("big skips: every boundary of ten years, or the latest of each, or none") {
    const midgard::Calendar c = shared_calendar("standard");
    const std::int64_t start = c.start_time();
    const std::int64_t ten_years =
        midgard::time_of(c, 11, 1, 1, 6 * 3600) - start;  // (it starts on January 1st)

    midgard::TimeService all(start);
    all.follow(c);
    std::map<TimeSignal, int> count;
    for (const TimeEvent& e : all.advance(ten_years)) ++count[e.signal];
    const int days = int(ten_years / c.seconds_per_day());  // (3,652: two leap days)
    CHECK(days == 3652);
    CHECK(count[TimeSignal::Day] == days);
    CHECK(count[TimeSignal::Hour] == days * 24);
    CHECK(count[TimeSignal::Dawn] == days);
    CHECK(count[TimeSignal::Dusk] == days);
    CHECK(count[TimeSignal::Month] == 120);
    CHECK(count[TimeSignal::Year] == 10);
    CHECK(count[TimeSignal::Season] == 40);

    midgard::TimeService latest(start);
    latest.follow(c);
    const std::vector<TimeEvent> last = latest.advance(ten_years, midgard::SkipPolicy::Latest);
    CHECK(last.size() == 7);  // one of each
    const std::vector<TimeEvent>& every = all.events();
    for (const TimeEvent& e : last) {
        const auto it = std::find_if(every.rbegin(), every.rend(),
                                     [&](const TimeEvent& x) { return x.signal == e.signal; });
        CHECK(it->time == e.time);
    }

    midgard::TimeService none(start);
    none.follow(c);
    CHECK(none.advance(ten_years, midgard::SkipPolicy::None).empty());
    CHECK(none.now() == start + ten_years);  // the clock moved all the same

    none.set_time(5);  // loading a save, or the scrubber: no signals
    CHECK(none.now() == 5);
    CHECK(none.events().empty());
}

TEST_CASE("each followed calendar reports its own boundaries") {
    midgard::TimeService time;
    const size_t standard = time.follow(shared_calendar("standard"));
    const size_t twenty = time.follow(shared_calendar("twenty"));
    std::map<size_t, int> days;
    for (const TimeEvent& e : time.advance(3'600'000))  // 1000 standard hours
        if (e.signal == TimeSignal::Day) ++days[e.calendar];
    CHECK(days[standard] == 41);  // 24-hour days
    CHECK(days[twenty] == 50);    // 20-hour days
    CHECK(std::string(midgard::signal_name(TimeSignal::Dusk)) == "dusk");
}

TEST_CASE("body calendars are the same from the same seed, on every target") {
    const json data = table("calendar_bodies.json");
    std::map<std::string, midgard::Calendar> templates;
    for (const auto& [name, t] : data.at("templates").items()) {
        templates[name] = midgard::calendar_from_json(t);
        templates[name].id = name;
        templates[name].weekdays = {"w"};
    }
    REQUIRE(data.at("cases").size() > 80);
    for (const json& c : data.at("cases")) {
        CAPTURE(c.dump());
        const midgard::Calendar body =
            midgard::roll_body_calendar(templates.at(c.at("template").get<std::string>()),
                                        std::stoull(c.at("seed").get<std::string>()));
        CHECK(body.hours_per_day == c.at("hours_per_day").get<int>());
        std::vector<int> months;
        for (const auto& m : body.months) months.push_back(m.days);
        CHECK(months == c.at("months").get<std::vector<int>>());
        CHECK(bits(body.tilt) == c.at("tilt").get<std::string>());
        CHECK_FALSE(body.standard);
        CHECK(body.problems().empty());  // a usable calendar, its ranges now the values drawn
    }
}
