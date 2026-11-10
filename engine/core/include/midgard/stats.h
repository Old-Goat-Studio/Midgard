// SPDX-License-Identifier: Apache-2.0
// The player's stats, scores and counters (milestone 6.0): numbers the designer names.
//
// A stat (the stat table, migration 19) is one of three kinds:
//   stat     a value between a least and a most (health, stamina, mana) that can refill (or
//            drain) by itself, so many a second, once it has gone that long without dropping
//   score    a whole number that never goes below its least (points)
//   counter  a whole number from its least up to a target if it has one (keys 3 of 5; coins)
// A project starts with Health, Stamina and Score. Scripts read and change them
// (midgard.stats), the HUD shows them, and the world's save keeps them.
#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace midgard::stats {

class StatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Kind { stat, score, counter };
const char* kind_name(Kind k);
std::optional<Kind> kind_from_name(std::string_view name);

struct StatDef {
    bool operator==(const StatDef&) const = default;
    std::string id, name;
    Kind kind = Kind::stat;
    double min = 0.0;    // the least it can be
    double max = 100.0;  // the most (a stat's); a counter's target (0: none); a score has none
    double start = 100.0;
    double regen = 0.0;  // a stat's change a second by itself (negative: it drains)
    double delay = 0.0;  // seconds after it last dropped before it refills
    // Whole numbers: scores and counters.
    bool whole() const { return kind != Kind::stat; }
    // Bounded above: stats, and counters with a target.
    bool capped() const { return kind == Kind::stat || (kind == Kind::counter && max > min); }
};

// A stat from its table row ("stat" is the JSON its column holds:
// {"kind": "stat", "min": 0, "max": 100, "start": 100, "regen": 0, "delay": 0}). Throws
// StatError, naming it, if it isn't one: an unknown kind, a most below the least.
StatDef stat_from_json(const std::string& id, const std::string& name, const std::string& json);

// A value made fit for a stat: clamped to its range, and whole for scores and counters.
double fit(const StatDef& def, double value);

// One stat's change over a frame: its value before the first change and after the last.
struct Change {
    std::string id;
    double before = 0.0, after = 0.0;
};

class Stats {
public:
    // The stats there are. Ones already known keep their values (fitted to their new range);
    // new ones start at their start; ones no longer defined go.
    void define(std::vector<StatDef> defs);
    const std::vector<StatDef>& defs() const { return defs_; }
    // A stat by id, or by name (case ignored); nullptr if there is none.
    const StatDef* find(std::string_view id_or_name) const;

    std::optional<double> get(std::string_view stat) const;
    // Sets, or adds to, a stat (fitted); its new value, or none if there is no such stat.
    std::optional<double> set(std::string_view stat, double value);
    std::optional<double> add(std::string_view stat, double amount);
    // A stat's most (a stat's; a counter's target), the value fitted to it. False if there is
    // no such stat, or it is a score.
    bool set_max(std::string_view stat, double max);
    // Back to its start (and its most as defined); every stat with no argument.
    bool reset(std::string_view stat);
    void reset_all();

    // Stats refill (or drain) by themselves.
    void update(double dt);

    // The changes since the last call, a stat once each, in the order they first changed.
    std::vector<Change> take_changes();

    // Each stat's value and most (as JSON, for the world's save) and back. Restoring keeps
    // only stats still defined, fitted to their range.
    nlohmann::json save() const;
    void restore(const nlohmann::json& saved);

private:
    struct Value {
        double value = 0.0;
        double since_drop = 1e9;  // seconds since it last went down
    };
    StatDef* find_mutable(std::string_view id_or_name);
    void changed(const std::string& id, double before, double after);

    std::vector<StatDef> defs_;  // (their max is the current one: set_max changes it)
    std::vector<StatDef> defined_;
    std::map<std::string, Value> values_;
    std::vector<Change> changes_;
};

}  // namespace midgard::stats
