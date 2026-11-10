// SPDX-License-Identifier: Apache-2.0
#include "midgard/stats.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include <nlohmann/json.hpp>

namespace midgard::stats {
namespace {

bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::stat:
            return "stat";
        case Kind::score:
            return "score";
        case Kind::counter:
            return "counter";
        case Kind::position:
            return "position";
        case Kind::clock:
            return "clock";
    }
    return "stat";
}

std::optional<Kind> kind_from_name(std::string_view name) {
    if (name == "stat") return Kind::stat;
    if (name == "score") return Kind::score;
    if (name == "counter") return Kind::counter;
    if (name == "position") return Kind::position;
    if (name == "clock") return Kind::clock;
    return std::nullopt;
}

const std::vector<std::string>& clock_parts() {
    static const std::vector<std::string> parts = {"season", "weekday", "day",
                                                   "month",  "year",    "time"};
    return parts;
}

const std::vector<std::string>& clock_default_parts() {
    static const std::vector<std::string> parts = {"season", "day", "month", "year", "time"};
    return parts;
}

StatDef stat_from_json(const std::string& id, const std::string& name, const std::string& text) {
    const std::string who = "Stat '" + (name.empty() ? id : name) + "'";
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) throw StatError(who + " isn't a JSON object");
    StatDef d;
    d.id = id;
    d.name = name.empty() ? id : name;
    const std::string kind = j.value("kind", "stat");
    const auto k = kind_from_name(kind);
    if (!k) throw StatError(who + " has an unknown kind '" + kind + "'");
    d.kind = *k;
    if (j.contains("hud")) {
        if (!j.at("hud").is_boolean()) throw StatError(who + ": 'hud' isn't true or false");
        d.hud = j.at("hud").get<bool>();
    }
    if (d.display()) {  // (no numbers: only what a clock shows)
        d.min = d.max = d.start = 0.0;
        if (d.kind == Kind::clock) {
            if (!j.contains("show")) {
                d.show = clock_default_parts();
            } else {
                const auto& show = j.at("show");
                if (!show.is_array()) throw StatError(who + ": 'show' isn't a list of parts");
                for (const std::string& part : clock_parts())  // (in their own order)
                    for (const auto& v : show)
                        if (v.is_string() && v.get<std::string>() == part) {
                            d.show.push_back(part);
                            break;
                        }
                for (const auto& v : show)
                    if (!v.is_string() || std::find(clock_parts().begin(), clock_parts().end(),
                                                    v.get<std::string>()) == clock_parts().end())
                        throw StatError(who + ": '" +
                                        (v.is_string() ? v.get<std::string>() : v.dump()) +
                                        "' isn't a part a clock shows (season, weekday, day, "
                                        "month, year or time)");
            }
        }
        return d;
    }
    const auto number = [&](const char* key, double fallback) {
        if (!j.contains(key)) return fallback;
        const auto& v = j.at(key);
        if (!v.is_number() || !std::isfinite(v.get<double>()))
            throw StatError(who + ": '" + key + "' isn't a number");
        return v.get<double>();
    };
    const bool stat = d.kind == Kind::stat;
    d.min = number("min", 0.0);
    d.max = number("max", stat ? 100.0 : 0.0);
    d.start = number("start", stat ? d.max : d.min);
    d.regen = stat ? number("regen", 0.0) : 0.0;
    d.delay = stat ? std::max(0.0, number("delay", 0.0)) : 0.0;
    if (stat && d.max <= d.min)
        throw StatError(who + ": its most (" + std::to_string(d.max) + ") isn't above its least (" +
                        std::to_string(d.min) + ")");
    if (d.whole()) {
        d.min = std::round(d.min);
        d.max = std::round(d.max);
    }
    d.start = fit(d, d.start);
    return d;
}

double fit(const StatDef& def, double value) {
    if (!std::isfinite(value)) value = def.start;
    if (def.whole()) value = std::round(value);
    value = std::max(value, def.min);
    if (def.capped()) value = std::min(value, def.max);
    return value;
}

void Stats::define(std::vector<StatDef> defs) {
    const auto by_id = [](const std::vector<StatDef>& list, const std::string& id) {
        const auto it =
            std::find_if(list.begin(), list.end(), [&](const StatDef& o) { return o.id == id; });
        return it == list.end() ? nullptr : &*it;
    };
    // defs_ holds each stat's most as it is now (a script may have raised it); defined_ the
    // designer's, for reset and to notice the design changing.
    std::vector<StatDef> live = defs;
    std::map<std::string, Value> kept;
    for (StatDef& d : live) {
        const StatDef* designed = by_id(defined_, d.id);
        const StatDef* now = by_id(defs_, d.id);
        if (designed && now && designed->max == d.max) d.max = now->max;  // (a script's most)
        Value value;
        if (const auto v = values_.find(d.id); v != values_.end()) {
            value = v->second;
            value.value = fit(d, value.value);
        } else {
            value.value = d.start;
        }
        kept[d.id] = value;
    }
    defined_ = std::move(defs);
    defs_ = std::move(live);
    values_ = std::move(kept);
}

const StatDef* Stats::find(std::string_view id_or_name) const {
    for (const StatDef& d : defs_)
        if (d.id == id_or_name) return &d;
    for (const StatDef& d : defs_)
        if (same_name(d.name, id_or_name)) return &d;
    return nullptr;
}

StatDef* Stats::find_mutable(std::string_view id_or_name) {
    return const_cast<StatDef*>(find(id_or_name));
}

std::optional<double> Stats::get(std::string_view stat) const {
    const StatDef* d = find(stat);
    if (!d || d->display()) return std::nullopt;
    return values_.at(d->id).value;
}

void Stats::changed(const std::string& id, double before, double after) {
    if (before == after) return;
    const auto it =
        std::find_if(changes_.begin(), changes_.end(), [&](const Change& c) { return c.id == id; });
    if (it == changes_.end())
        changes_.push_back({id, before, after});
    else
        it->after = after;
}

std::optional<double> Stats::set(std::string_view stat, double value) {
    const StatDef* d = find(stat);
    if (!d || d->display()) return std::nullopt;
    Value& v = values_.at(d->id);
    const double before = v.value;
    v.value = fit(*d, value);
    if (v.value < before) v.since_drop = 0.0;
    changed(d->id, before, v.value);
    return v.value;
}

std::optional<double> Stats::add(std::string_view stat, double amount) {
    const auto now = get(stat);
    if (!now) return std::nullopt;
    return set(stat, *now + amount);
}

bool Stats::set_max(std::string_view stat, double max) {
    StatDef* d = find_mutable(stat);
    if (!d || d->kind == Kind::score || d->display() || !std::isfinite(max)) return false;
    if (d->whole()) max = std::round(max);
    if (d->kind == Kind::stat && max <= d->min) return false;
    d->max = max;
    Value& v = values_.at(d->id);
    const double before = v.value;
    v.value = fit(*d, v.value);
    changed(d->id, before, v.value);
    return true;
}

bool Stats::reset(std::string_view stat) {
    StatDef* d = find_mutable(stat);
    if (!d || d->display()) return false;
    const auto designed = std::find_if(defined_.begin(), defined_.end(),
                                       [&](const StatDef& o) { return o.id == d->id; });
    if (designed != defined_.end()) d->max = designed->max;
    Value& v = values_.at(d->id);
    const double before = v.value;
    v.value = d->start;
    v.since_drop = 1e9;
    changed(d->id, before, v.value);
    return true;
}

void Stats::reset_all() {
    for (const StatDef& d : std::vector<StatDef>(defs_)) reset(d.id);
}

void Stats::update(double dt) {
    if (dt <= 0.0) return;
    for (const StatDef& d : defs_) {
        Value& v = values_.at(d.id);
        v.since_drop += dt;
        if (d.kind != Kind::stat || d.regen == 0.0 || v.since_drop < d.delay) continue;
        const double before = v.value;
        v.value = fit(d, v.value + d.regen * dt);
        changed(d.id, before, v.value);
    }
}

std::vector<Change> Stats::take_changes() {
    std::vector<Change> out;
    for (Change& c : changes_)
        if (c.before != c.after) out.push_back(std::move(c));
    changes_.clear();
    return out;
}

nlohmann::json Stats::save() const {
    nlohmann::json out = nlohmann::json::object();
    for (const StatDef& d : defs_)
        if (!d.display()) out[d.id] = {{"value", values_.at(d.id).value}, {"max", d.max}};
    return out;
}

void Stats::restore(const nlohmann::json& saved) {
    if (!saved.is_object()) return;
    for (StatDef& d : defs_) {
        if (d.display()) continue;
        const auto it = saved.find(d.id);
        if (it == saved.end() || !it->is_object()) continue;
        if (it->contains("max") && it->at("max").is_number() && d.kind != Kind::score) {
            const double max = it->at("max").get<double>();
            if (std::isfinite(max) && (d.kind != Kind::stat || max > d.min))
                d.max = d.whole() ? std::round(max) : max;
        }
        if (it->contains("value") && it->at("value").is_number())
            values_.at(d.id).value = fit(d, it->at("value").get<double>());
    }
    changes_.clear();  // (as the game starts: not changes)
}

}  // namespace midgard::stats
