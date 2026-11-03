// SPDX-License-Identifier: Apache-2.0
#include "midgard/weather_sim.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <nlohmann/json.hpp>

#include "midgard/det.h"

namespace midgard::weather {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMostHours = 24.0 * 3650.0;

double number(const nlohmann::json& j, const char* key, double fallback, double least,
              double most) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (!it->is_number() || !std::isfinite(it->get<double>()))
        throw WeatherError(std::string(key) + " is a number");
    return std::clamp(it->get<double>(), least, most);
}

// A number, or [least, most], brought into [floor, ceiling].
Range range(const nlohmann::json& j, const char* key, Range fallback, double floor,
            double ceiling) {
    const auto it = j.find(key);
    Range r = fallback;
    if (it != j.end() && !it->is_null()) {
        if (it->is_number()) {
            r = {it->get<double>(), it->get<double>()};
        } else if (it->is_array() && it->size() == 2 && (*it)[0].is_number() &&
                   (*it)[1].is_number()) {
            r = {(*it)[0].get<double>(), (*it)[1].get<double>()};
        } else {
            throw WeatherError(std::string(key) + " is a number, or [least, most]");
        }
    }
    if (!std::isfinite(r.first) || !std::isfinite(r.second))
        throw WeatherError(std::string(key) + " is a number, or [least, most]");
    if (r.first > r.second) std::swap(r.first, r.second);
    return {std::clamp(r.first, floor, ceiling), std::clamp(r.second, floor, ceiling)};
}

std::map<std::string, double> weights(const nlohmann::json& j, const std::string& what) {
    std::map<std::string, double> out;
    if (j.is_null()) return out;
    if (!j.is_object()) throw WeatherError(what + " is weather types with how often each");
    for (const auto& [id, w] : j.items()) {
        if (!w.is_number() || !std::isfinite(w.get<double>()))
            throw WeatherError(what + ": how often " + id + " is, is a number");
        if (w.get<double>() > 0.0) out[id] = w.get<double>();
    }
    return out;
}

std::vector<std::string> names(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return out;
    if (!it->is_array()) throw WeatherError(std::string(key) + " is a list");
    for (const auto& v : *it) {
        if (!v.is_string()) throw WeatherError(std::string(key) + " is a list of names");
        if (!v.get<std::string>().empty()) out.push_back(v.get<std::string>());
    }
    return out;
}

const nlohmann::json& member(const nlohmann::json& j, const char* key) {
    static const nlohmann::json none;
    const auto it = j.find(key);
    return it == j.end() ? none : *it;
}

// Picks from weights (in their order) with a roll in [0, 1); "" when there are none.
std::string pick(const std::vector<std::pair<std::string, double>>& w, double roll) {
    double total = 0.0;
    for (const auto& [id, x] : w) total += x;
    if (w.empty() || total <= 0.0) return {};
    double at = roll * total;
    for (const auto& [id, x] : w) {
        if (at < x) return id;
        at -= x;
    }
    return w.back().first;
}

}  // namespace

WeatherSettings weather_settings_from_json(std::string_view json) {
    WeatherSettings s;
    if (json.find_first_not_of(" \t\r\n") == std::string_view::npos) return s;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception&) {
        throw WeatherError("the weather settings aren't JSON");
    }
    if (j.is_null()) return s;
    try {
        if (!j.is_object()) throw WeatherError("they aren't an object");
        s.cell = number(j, "cell", s.cell, 100.0, 1.0e7);
        const nlohmann::json& tables = member(j, "tables");
        if (!tables.is_null() && !tables.is_array()) throw WeatherError("tables is a list");
        for (const auto& t : tables.is_array() ? tables : nlohmann::json::array()) {
            if (!t.is_object()) throw WeatherError("a table is an object");
            WeatherTable table;
            const auto name = t.find("name");
            if (name != t.end() && name->is_string()) table.name = name->get<std::string>();
            const std::string what = "table '" + table.name + "'";
            table.biomes = names(t, "biomes");
            for (const auto& v :
                 member(t, "seasons").is_array() ? member(t, "seasons") : nlohmann::json::array()) {
                if (!v.is_number_integer() || v.get<int>() < 0)
                    throw WeatherError(what + ": a season is its number, from 0");
                table.seasons.push_back(v.get<int>());
            }
            if (!member(t, "seasons").is_null() && !member(t, "seasons").is_array())
                throw WeatherError(what + ": seasons is a list");
            table.weather = weights(member(t, "weather"), what + "'s weather");
            const nlohmann::json& after = member(t, "after");
            if (!after.is_null()) {
                if (!after.is_object())
                    throw WeatherError(what + "'s after is weather types with what follows each");
                for (const auto& [from, next] : after.items())
                    table.after[from] = weights(next, what + ": after " + from);
            }
            s.tables.push_back(std::move(table));
        }
        const nlohmann::json& lasts = member(j, "lasts");
        if (!lasts.is_null()) {
            if (!lasts.is_object()) throw WeatherError("lasts is weather types with hours each");
            for (const auto& [id, v] : lasts.items()) {
                const nlohmann::json one{{"h", v}};
                const Range r = range(one, "h", s.lasts_default, 0.25, kMostHours);
                (id == "default" ? s.lasts_default : s.lasts[id]) = r;
            }
        }
        const nlohmann::json& fronts = member(j, "fronts");
        if (!fronts.is_null()) {
            if (!fronts.is_object()) throw WeatherError("fronts is an object");
            s.fronts.every = range(fronts, "every", s.fronts.every, 1.0, kMostHours);
            s.fronts.speed = range(fronts, "speed", s.fronts.speed, 0.0, 300.0);
            s.fronts.radius = range(fronts, "radius", s.fronts.radius, 0.5, 1000.0);
            s.fronts.lasts = range(fronts, "lasts", s.fronts.lasts, 0.25, kMostHours);
            s.fronts.weather = weights(member(fronts, "weather"), "the fronts' weather");
        }
        const nlohmann::json& controlled = member(j, "controlled");
        if (!controlled.is_null() && !controlled.is_array())
            throw WeatherError("controlled is a list");
        for (const auto& c : controlled.is_array() ? controlled : nlohmann::json::array()) {
            if (!c.is_object() || !member(c, "weather").is_string())
                throw WeatherError("controlled weather is a list of {weather, hours}");
            s.controlled.push_back(
                {c.at("weather").get<std::string>(), number(c, "hours", 1.0, 0.25, kMostHours)});
        }
    } catch (const WeatherError& e) {
        throw WeatherError(std::string("weather settings: ") + e.what());
    } catch (const nlohmann::json::exception& e) {
        throw WeatherError(std::string("weather settings: ") + e.what());
    }
    return s;
}

std::vector<std::string> weather_settings_problems(const WeatherSettings& s,
                                                   const WeatherTypes& types) {
    std::vector<std::string> out;
    std::set<std::string> told;
    const auto known = [&](const std::string& id, const std::string& where) {
        if (types.count(id) || !told.insert(where + "|" + id).second) return;
        out.push_back(where + " has weather '" + id + "', which isn't one of the weather types.");
    };
    for (const WeatherTable& t : s.tables) {
        const std::string where = "Weather table '" + t.name + "'";
        if (t.weather.empty()) out.push_back(where + " has no weather.");
        for (const auto& [id, w] : t.weather) known(id, where);
        for (const auto& [from, next] : t.after) {
            known(from, where);
            for (const auto& [id, w] : next) known(id, where);
        }
    }
    for (const auto& [id, r] : s.lasts) known(id, "The weather's lasts");
    for (const auto& [id, w] : s.fronts.weather) known(id, "The weather fronts");
    for (const Controlled& c : s.controlled) known(c.weather, "The controlled weather");
    return out;
}

WeatherSim::WeatherSim(const Shape& shape, WeatherSettings settings, WeatherTypes types,
                       std::uint64_t seed, std::optional<Calendar> calendar, BiomeAt biome_at)
    : shape_(shape),
      settings_(std::move(settings)),
      types_(std::move(types)),
      seed_(seed),
      calendar_(std::move(calendar)),
      biome_at_(std::move(biome_at)) {
    if (calendar_) {
        origin_ = calendar_->start_time();
        hour_ = std::max<std::int64_t>(calendar_->seconds_per_hour(), 1);
    }
    const glm::dvec2 size = shape_.face_size();
    // An odd number of cells (the nearest to the size over the cell's), so a face's middle,
    // where a scene's world starts, is a cell's middle and not the corner of four.
    const auto count = [&](double metres) {
        const double n = std::clamp(metres / settings_.cell, 1.0, 1.0e5);
        return 2 * int(std::round((n - 1.0) / 2.0)) + 1;
    };
    cells_u_ = count(size.x);
    cells_v_ = count(size.y);
    const Shape::Kind kind = shape_.kind();
    sphere_ = kind == Shape::Kind::SphereOutside || kind == Shape::Kind::SphereInside;
    wraps_ = kind == Shape::Kind::FlatCylinder || kind == Shape::Kind::CurvedCylinder;
}

// Round a curved cylinder, where a scene's world starts at u = 0 (its bottom), the cells are
// half a cell round, so one is centred there (it goes round: u wraps).
double WeatherSim::u_shift() const {
    return shape_.kind() == Shape::Kind::CurvedCylinder ? 0.5 / cells_u_ : 0.0;
}

Cell WeatherSim::cell_at(const SurfacePoint& p) const {
    const auto index = [](double x, int n) { return std::clamp(int(std::floor(x * n)), 0, n - 1); };
    double u = p.u + u_shift();
    if (u_shift() > 0.0) u -= std::floor(u);
    return {std::clamp(p.face, 0, shape_.faces() - 1), index(u, cells_u_), index(p.v, cells_v_)};
}

SurfacePoint WeatherSim::centre(const Cell& c) const {
    double u = (c.i + 0.5) / cells_u_ - u_shift();
    if (u < 0.0) u += 1.0;
    return {c.face, u, (c.j + 0.5) / cells_v_, 0.0};
}

const std::string& WeatherSim::biome(const Cell& c) {
    auto it = biomes_.find(c);
    if (it == biomes_.end())
        it = biomes_.emplace(c, biome_at_ ? biome_at_(centre(c)) : std::string()).first;
    return it->second;
}

int WeatherSim::season_at(std::int64_t time) const {
    if (!calendar_ || calendar_->seasons.empty()) return -1;
    return date_at(*calendar_, std::max<std::int64_t>(time, 0)).season;
}

const WeatherTable* WeatherSim::table_for(const std::string& biome, int season) const {
    const WeatherTable* best = nullptr;
    int best_score = -1;
    for (const WeatherTable& t : settings_.tables) {
        if (!t.biomes.empty() &&
            std::find(t.biomes.begin(), t.biomes.end(), biome) == t.biomes.end())
            continue;
        if (!t.seasons.empty() &&
            std::find(t.seasons.begin(), t.seasons.end(), season) == t.seasons.end())
            continue;
        const int score = 2 * int(!t.biomes.empty()) + int(!t.seasons.empty());
        if (score > best_score) {
            best = &t;
            best_score = score;
        }
    }
    return best;
}

std::int64_t WeatherSim::hours(const Range& r, double roll) const {
    const double h = r.first + (r.second - r.first) * roll;
    return std::max<std::int64_t>(std::llround(h * double(hour_)), 1);
}

Spell WeatherSim::draw(const Cell& c, std::int64_t k, const std::string& before,
                       std::int64_t start) {
    det::Rng rng =
        det::Rng(det::hash3(seed_ ^ det::tag("weather"), c.face, c.i, c.j)).split(std::uint64_t(k));
    std::vector<std::pair<std::string, double>> w;
    if (const WeatherTable* t = table_for(biome(c), season_at(start))) {
        const auto after = before.empty() ? t->after.end() : t->after.find(before);
        if (after != t->after.end()) {
            for (const auto& [id, x] : after->second)
                if (types_.count(id)) w.emplace_back(id, x);
        } else {
            // How much of the time: drawn as often as that over how long it lasts.
            for (const auto& [id, x] : t->weather) {
                if (!types_.count(id)) continue;
                const auto l = settings_.lasts.find(id);
                const Range r = l == settings_.lasts.end() ? settings_.lasts_default : l->second;
                w.emplace_back(id, x / std::max((r.first + r.second) / 2.0, 0.25));
            }
        }
    }
    Spell s;
    s.weather = pick(w, rng.uniform());
    const auto l = settings_.lasts.find(s.weather);
    s.start = start;
    s.end = start +
            hours(l == settings_.lasts.end() ? settings_.lasts_default : l->second, rng.uniform());
    return s;
}

void WeatherSim::advance(const Cell& c, Run& run, std::int64_t time) {
    const std::int64_t t = std::max(time, origin_);
    if (run.k < 0 || t < run.spell.start) {
        run.k = 0;
        run.spell = draw(c, 0, {}, origin_);
    }
    while (t >= run.spell.end) {
        ++run.k;
        run.spell = draw(c, run.k, run.spell.weather, run.spell.end);
    }
}

Spell WeatherSim::controlled_spell(std::int64_t time) const {
    std::int64_t total = 0;
    for (const Controlled& c : settings_.controlled) total += hours({c.hours, c.hours}, 0.0);
    const std::int64_t t = std::max(time, origin_);
    const std::int64_t into = (t - origin_) % total;
    std::int64_t at = t - into;
    for (const Controlled& c : settings_.controlled) {
        const std::int64_t length = hours({c.hours, c.hours}, 0.0);
        if (t < at + length) return {c.weather, at, at + length, -1};
        at += length;
    }
    return {settings_.controlled.back().weather, at, at + 1, -1};  // (not reached)
}

Spell WeatherSim::own_spell(const Cell& c, std::int64_t time) {
    if (!settings_.controlled.empty()) return controlled_spell(time);
    Run& run = runs_[c];
    advance(c, run, time);
    return run.spell;
}

void WeatherSim::make_fronts_until(std::int64_t time) {
    if (settings_.fronts.weather.empty() || !settings_.controlled.empty()) return;
    const Fronts& f = settings_.fronts;
    const det::Rng fronts = det::Rng(seed_).split(det::tag("weather fronts"));
    while (fronts_.empty() || fronts_.back().start <= time) {
        det::Rng rng = fronts.split(fronts_.size());
        FrontPath p;
        p.start =
            (fronts_.empty() ? origin_ : fronts_.back().start) + hours(f.every, rng.uniform());
        std::vector<std::pair<std::string, double>> w;
        for (const auto& [id, x] : f.weather)
            if (types_.count(id)) w.emplace_back(id, x);
        p.weather = pick(w, rng.uniform());
        p.end = p.start + hours(f.lasts, rng.uniform());
        p.speed = f.speed.first + (f.speed.second - f.speed.first) * rng.uniform();
        p.radius =
            (f.radius.first + (f.radius.second - f.radius.first) * rng.uniform()) * settings_.cell;
        const double a = rng.uniform(), b = rng.uniform(), turn = 2.0 * kPi * rng.uniform();
        if (sphere_) {
            // A point anywhere on the sphere, all equally likely, heading any way.
            const double z = 2.0 * a - 1.0, phi = 2.0 * kPi * b;
            const double r = det::sqrt(std::max(1.0 - z * z, 0.0));
            p.from = {r * det::cos(phi), r * det::sin(phi), z};
            const glm::dvec3 ref = std::abs(z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
            const glm::dvec3 e1 = glm::normalize(glm::cross(p.from, ref));
            const glm::dvec3 e2 = glm::cross(p.from, e1);
            p.heading = e1 * det::cos(turn) + e2 * det::sin(turn);
        } else {
            const glm::dvec2 size = shape_.face_size();
            p.from = {a * size.x, b * size.y, 0.0};
            p.heading = {det::cos(turn), det::sin(turn), 0.0};
        }
        fronts_.push_back(std::move(p));
    }
}

glm::dvec3 WeatherSim::front_position(const FrontPath& f, std::int64_t time) const {
    const double gone = f.speed * double(time - f.start);  // metres
    if (sphere_) {
        const double angle = gone / shape_.radius();
        return f.from * det::cos(angle) + f.heading * det::sin(angle);
    }
    glm::dvec3 p = f.from + f.heading * gone;
    if (wraps_) {
        const double w = shape_.face_size().x;
        p.x = std::fmod(p.x, w);
        if (p.x < 0.0) p.x += w;
    }
    return p;
}

bool WeatherSim::covers(const FrontPath& f, std::int64_t time, const Cell& c) const {
    if (f.weather.empty() || time < f.start || time >= f.end) return false;
    const glm::dvec3 at = front_position(f, time);
    const SurfacePoint m = centre(c);
    if (sphere_) {
        const double angle = f.radius / shape_.radius();
        if (angle >= kPi) return true;
        const glm::dvec3 d = glm::normalize(shape_.position(m));
        return glm::dot(at, d) >= det::cos(angle);
    }
    const glm::dvec2 size = shape_.face_size();
    double dx = std::abs(m.u * size.x - at.x);
    if (wraps_) dx = std::min(dx, size.x - dx);
    const double dy = m.v * size.y - at.y;
    return dx * dx + dy * dy <= f.radius * f.radius;
}

std::optional<int> WeatherSim::front_over(const Cell& c, std::int64_t time) {
    if (settings_.fronts.weather.empty() || !settings_.controlled.empty()) return std::nullopt;
    make_fronts_until(time);
    const std::int64_t longest = hours(settings_.fronts.lasts, 1.0);
    // The latest to start of those over the cell.
    for (int n = int(fronts_.size()) - 1; n >= 0; --n) {
        const FrontPath& f = fronts_[size_t(n)];
        if (f.start > time) continue;
        if (f.start + longest < time) break;
        if (covers(f, time, c)) return n;
    }
    return std::nullopt;
}

Now WeatherSim::at(const Cell& c, std::int64_t time) {
    if (const auto f = front_over(c, time)) return {fronts_[size_t(*f)].weather, *f};
    return {own_spell(c, time).weather, -1};
}

std::vector<Spell> WeatherSim::forecast(const Cell& c, std::int64_t from, std::int64_t until) {
    std::vector<Spell> out;
    const bool fronts = !settings_.fronts.weather.empty() && settings_.controlled.empty();
    const std::int64_t minute = std::max<std::int64_t>(hour_ / 60, 1);
    Run run = runs_.count(c) ? runs_[c] : Run{};  // (a copy: the cell stays where it is)
    for (std::int64_t t = from; t < until;) {
        Spell own;
        if (!settings_.controlled.empty()) {
            own = controlled_spell(t);
        } else {
            advance(c, run, t);
            own = run.spell;
        }
        std::int64_t next = std::min(until, std::max(own.end, t + 1));
        Now now{own.weather, -1};
        if (fronts) {
            const auto front_now = front_over(c, t);
            if (front_now) now = {fronts_[size_t(*front_now)].weather, *front_now};
            next = std::min(next, t + minute);
            // A front arriving or leaving within the minute: the very second it does.
            if (front_over(c, next - 1) != front_now) {
                std::int64_t same = t, changed = next - 1;
                while (changed - same > 1) {
                    const std::int64_t mid = same + (changed - same) / 2;
                    (front_over(c, mid) == front_now ? same : changed) = mid;
                }
                next = changed;
            }
        }
        if (!out.empty() && out.back().end == t && out.back().weather == now.weather &&
            out.back().front == now.front) {
            out.back().end = next;
        } else {
            out.push_back({now.weather, t, next, now.front});
        }
        t = next;
    }
    return out;
}

std::vector<Front> WeatherSim::fronts_at(std::int64_t time) {
    std::vector<Front> out;
    if (settings_.fronts.weather.empty() || !settings_.controlled.empty()) return out;
    make_fronts_until(time);
    for (size_t n = 0; n < fronts_.size(); ++n) {
        const FrontPath& f = fronts_[n];
        if (f.weather.empty() || time < f.start || time >= f.end) continue;
        Front front{int(n), f.weather, f.start, f.end, f.speed, f.radius, {}};
        const glm::dvec3 at = front_position(f, time);
        if (sphere_) {
            front.centre = shape_.surface(at * shape_.radius());
            front.centre.height = 0.0;
        } else {
            const glm::dvec2 size = shape_.face_size();
            front.centre = {0, at.x / size.x, at.y / size.y, 0.0};
        }
        out.push_back(front);
    }
    return out;
}

}  // namespace midgard::weather
