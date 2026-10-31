// SPDX-License-Identifier: Apache-2.0
#include "midgard/time_service.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace midgard {

const char* signal_name(TimeSignal signal) {
    switch (signal) {
        case TimeSignal::Hour:
            return "hour";
        case TimeSignal::Day:
            return "day";
        case TimeSignal::Month:
            return "month";
        case TimeSignal::Season:
            return "season";
        case TimeSignal::Year:
            return "year";
        case TimeSignal::Dawn:
            return "dawn";
        default:
            return "dusk";
    }
}

size_t TimeService::follow(const Calendar& calendar) {
    calendars_.push_back(calendar);
    return calendars_.size() - 1;
}

void TimeService::set_scale(double scale) {
    scale_ = std::isfinite(scale) ? std::max(scale, 0.0) : 0.0;
}

void TimeService::boundaries(const Calendar& c, size_t index, std::int64_t from, std::int64_t to,
                             std::vector<TimeEvent>& out) {
    if (to <= from) return;
    const std::int64_t spd = c.seconds_per_day(), sph = c.seconds_per_hour();
    const size_t first = out.size();
    const auto add = [&](TimeSignal s, double t) {
        if (t > double(from) && t <= double(to)) out.push_back({s, index, t});
    };
    // From the day before `from`: its dusk can fall after midnight, in the span.
    const std::int64_t last_day = to / spd;
    for (std::int64_t day = std::max<std::int64_t>(from / spd - 1, 0); day <= last_day; ++day) {
        const std::int64_t start = day * spd;
        if (start > from && start <= to) {
            const Date d = date_at(c, start);
            add(TimeSignal::Day, double(start));
            if (d.day == 1) add(TimeSignal::Month, double(start));
            if (d.day_of_year == 0) add(TimeSignal::Year, double(start));
            if (day > 0 && date_at(c, start - spd).season != d.season)
                add(TimeSignal::Season, double(start));
        }
        for (std::int64_t h = 0; h < c.hours_per_day; ++h) {
            const std::int64_t t = start + h * sph;
            if (t > to) break;
            add(TimeSignal::Hour, double(t));
        }
        const SunTimes sun = sun_times(c, start);
        add(TimeSignal::Dawn, double(start) + sun.sunrise);
        add(TimeSignal::Dusk, double(start) + sun.sunset);
    }
    std::stable_sort(out.begin() + std::ptrdiff_t(first), out.end(),
                     [](const TimeEvent& a, const TimeEvent& b) {
                         return a.time < b.time || (a.time == b.time && a.signal < b.signal);
                     });
}

void TimeService::collect(std::int64_t from, std::int64_t to, SkipPolicy policy) {
    events_.clear();
    if (policy == SkipPolicy::None || to <= from) return;
    for (size_t i = 0; i < calendars_.size(); ++i) boundaries(calendars_[i], i, from, to, events_);
    std::stable_sort(events_.begin(), events_.end(), [](const TimeEvent& a, const TimeEvent& b) {
        return a.time < b.time ||
               (a.time == b.time &&
                (a.calendar < b.calendar || (a.calendar == b.calendar && a.signal < b.signal)));
    });
    if (policy == SkipPolicy::Latest) {
        std::map<std::pair<size_t, TimeSignal>, TimeEvent> latest;
        for (const TimeEvent& e : events_) latest[{e.calendar, e.signal}] = e;
        events_.clear();
        for (const auto& [key, e] : latest) events_.push_back(e);
        std::stable_sort(events_.begin(), events_.end(),
                         [](const TimeEvent& a, const TimeEvent& b) { return a.time < b.time; });
    }
}

const std::vector<TimeEvent>& TimeService::update(double real_seconds) {
    if (!(real_seconds > 0.0) || scale_ == 0.0) {
        events_.clear();
        return events_;
    }
    // Whole seconds go on the clock; the rest is carried, so nothing is lost or added.
    const double total = carry_ + real_seconds * scale_;
    const double whole = std::floor(total);
    carry_ = total - whole;
    const std::int64_t from = now_;
    now_ += std::int64_t(whole);
    collect(from, now_, SkipPolicy::All);
    return events_;
}

const std::vector<TimeEvent>& TimeService::advance(std::int64_t game_seconds, SkipPolicy policy) {
    const std::int64_t from = now_;
    if (game_seconds > 0) now_ += game_seconds;
    collect(from, now_, policy);
    return events_;
}

void TimeService::set_time(std::int64_t time) {
    now_ = std::max<std::int64_t>(time, 0);
    carry_ = 0.0;
    events_.clear();
}

}  // namespace midgard
