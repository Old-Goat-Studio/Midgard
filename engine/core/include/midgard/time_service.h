// SPDX-License-Identifier: Apache-2.0
// The game's clock (roadmap §6, "Engine: TimeService"; milestone 5.7).
//
// One clock per universe: game seconds since year 1, month 1, day 1 (int64), advanced by real
// time times the scale. The part of a second not yet counted is carried to the next step, so the
// clock never drifts however small the steps. Each calendar the service follows (the standard
// calendar and the bodies' own) reads the same clock, and reports the boundaries it passed as
// signals: hour, day, month, season, year, dawn (solar phase passing 0) and dusk (passing 0.5).
// Every boundary is reported exactly once, however the time between was covered: in many small
// steps, or one big skip.
#pragma once

#include <cstdint>
#include <vector>

#include "midgard/calendar.h"

namespace midgard {

enum class TimeSignal { Hour, Day, Month, Season, Year, Dawn, Dusk };
const char* signal_name(TimeSignal signal);  // "hour", "day", ..., "dawn", "dusk"

struct TimeEvent {
    TimeSignal signal = TimeSignal::Hour;
    size_t calendar = 0;  // which followed calendar (in the order follow() was called)
    double time = 0.0;    // when, in game seconds (dawn and dusk fall between seconds)
};

// What a skip (sleeping, waiting, fast travel) reports of the boundaries it passes.
enum class SkipPolicy { All, Latest, None };  // every one, the last of each kind, or none

class TimeService {
public:
    explicit TimeService(std::int64_t start = 0) : now_(start) {}

    // A calendar to report signals for. Returns its number in TimeEvent::calendar.
    size_t follow(const Calendar& calendar);
    const Calendar& calendar(size_t i) const { return calendars_.at(i); }
    size_t calendars() const { return calendars_.size(); }

    // Game seconds per real second (0 stops time; never negative).
    void set_scale(double scale);
    double scale() const { return scale_; }

    // One frame: real seconds times the scale. Returns the boundaries passed, in time order.
    const std::vector<TimeEvent>& update(double real_seconds);
    // Moves the clock on by game seconds at once (sleeping, waiting), reporting what it passes by
    // the policy. Returns the boundaries reported.
    const std::vector<TimeEvent>& advance(std::int64_t game_seconds,
                                          SkipPolicy policy = SkipPolicy::All);
    // Puts the clock at a moment without reporting anything (loading a save, the editor's
    // scrubber). The part of a second carried is dropped.
    void set_time(std::int64_t time);

    std::int64_t now() const { return now_; }
    // The clock with the part of a second not yet counted (for smooth lighting).
    double precise() const { return double(now_) + carry_; }
    // The boundaries of the last update or advance.
    const std::vector<TimeEvent>& events() const { return events_; }

    // Every boundary a calendar passes in the time span (from, to]: the same whichever steps
    // make up the span. Appended to `out`, in time order.
    static void boundaries(const Calendar& c, size_t index, std::int64_t from, std::int64_t to,
                           std::vector<TimeEvent>& out);

private:
    void collect(std::int64_t from, std::int64_t to, SkipPolicy policy);

    std::int64_t now_ = 0;
    double carry_ = 0.0;  // [0, 1): game seconds passed but not yet on the clock
    double scale_ = 1.0;
    std::vector<Calendar> calendars_;
    std::vector<TimeEvent> events_;
};

}  // namespace midgard
