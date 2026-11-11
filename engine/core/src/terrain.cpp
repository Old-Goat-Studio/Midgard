// SPDX-License-Identifier: Apache-2.0
#include "midgard/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "midgard/det.h"
#include "midgard/hash.h"

namespace midgard {

namespace {

constexpr double kTwoPi = 6.283185307179586;

double wrap01(double u) {
    return u - std::floor(u);
}

double smooth01(double t) {
    t = std::min(std::max(t, 0.0), 1.0);
    return t * t * (3.0 - 2.0 * t);
}

bool wraps(Shape::Kind kind) {
    return kind == Shape::Kind::FlatCylinder || kind == Shape::Kind::CurvedCylinder;
}

bool sphere(Shape::Kind kind) {
    return kind == Shape::Kind::SphereOutside || kind == Shape::Kind::SphereInside;
}

int roots_along(double size) {
    return std::max(1, int(std::lround(size / kRootSize)));
}

// A sphere's faces have a power of two of roots a side, the nearest to kRootSize metres each
// (in proportion), so every grid point is a power-of-two fraction of the face and the points on
// face edges come out bit-identical from both faces.
// Positions on a grid of 2^-16 m: a chunk's points less than 256 m from its middle (also on
// it) are then exactly floats relative to it, so a point that two chunks share comes out the
// same from both, to the bit (no hairline gaps for rays or the player along chunk edges).
glm::dvec3 on_grid(const glm::dvec3& p) {
    constexpr double kGrid = 65536.0;
    return {std::round(p.x * kGrid) / kGrid, std::round(p.y * kGrid) / kGrid,
            std::round(p.z * kGrid) / kGrid};
}

// 3D helpers for stamps on spheres, in a fixed order of operations (tools/reference/terrain.py
// does the same, so both give the same results to the bit).
double dot3(const glm::dvec3& a, const glm::dvec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
glm::dvec3 cross3(const glm::dvec3& a, const glm::dvec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
glm::dvec3 unit3(const glm::dvec3& a) {
    const double l = det::sqrt(dot3(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

// How much a stroke through `points` (3D) applies at q: 1 on its line, falling to 0 at
// `radius` metres (straight-line distances: a sphere's strokes are short beside its size).
double stroke_weight(const std::vector<glm::dvec3>& points, double radius, const glm::dvec3& q) {
    if (points.empty() || !(radius > 0.0)) return 0.0;
    glm::dvec3 a = points[0] - q;
    double best = dot3(a, a);
    for (size_t i = 1; i < points.size(); ++i) {
        const glm::dvec3 b = points[i] - q;
        const glm::dvec3 e = b - a;
        const double len2 = dot3(e, e);
        double t = len2 > 0.0 ? -dot3(a, e) / len2 : 0.0;
        t = std::min(std::max(t, 0.0), 1.0);
        const glm::dvec3 c(a.x + e.x * t, a.y + e.y * t, a.z + e.z * t);
        best = std::min(best, dot3(c, c));
        a = b;
    }
    return smooth01(1.0 - det::sqrt(best) / radius);
}

int sphere_roots(double size) {
    constexpr double kSqrt2 = 1.4142135623730951;
    int roots = 1;
    while (roots < (1 << 16) && 2.0 * roots <= size / kRootSize * kSqrt2) roots *= 2;
    return roots;
}

}  // namespace

const char* wall_look_name(WallLook look) {
    switch (look) {
        case WallLook::Cliff:
            return "cliff";
        case WallLook::Fortification:
            return "fortification";
        case WallLook::Barrier:
            return "barrier";
    }
    return "cliff";
}

namespace {
// A ground layer's textures: {"textures": {slot: "tex:..."}, "tile": metres}.
GroundTexture ground_texture_from_json(const nlohmann::json& j) {
    if (!j.is_object())
        throw std::runtime_error("a ground texture is {\"textures\": {...}, \"tile\": metres}");
    GroundTexture g;
    if (j.contains("tile")) {
        if (!j.at("tile").is_number()) throw std::runtime_error("ground: \"tile\" is metres");
        g.tile = j.at("tile").get<double>();
    }
    if (j.contains("name")) {
        if (!j.at("name").is_string()) throw std::runtime_error("ground: \"name\" is text");
        g.name = j.at("name").get<std::string>();
    }
    if (j.contains("textures")) {
        const nlohmann::json& m = j.at("textures");
        if (!m.is_object())
            throw std::runtime_error("ground: \"textures\" is an object of maps by slot");
        for (const auto& [slot, id] : m.items()) {
            if (!id.is_string()) throw std::runtime_error("ground: each map is a content id");
            std::string* into = slot == "base_color"           ? &g.base_color
                                : slot == "normal"             ? &g.normal
                                : slot == "metallic_roughness" ? &g.metallic_roughness
                                : slot == "occlusion"          ? &g.occlusion
                                : slot == "emissive"           ? &g.emissive
                                                               : nullptr;
            if (!into)
                throw std::runtime_error("ground: '" + slot +
                                         "' isn't a map (base_color, normal, "
                                         "metallic_roughness, occlusion or emissive)");
            *into = id.get<std::string>();
        }
    }
    if (j.contains("off")) {
        if (!j.at("off").is_array()) throw std::runtime_error("ground: \"off\" is a list of maps");
        for (const nlohmann::json& slot : j.at("off")) {
            const std::string name = slot.is_string() ? slot.get<std::string>() : "";
            if (std::find(std::begin(kGroundSlots), std::end(kGroundSlots), name) ==
                std::end(kGroundSlots))
                throw std::runtime_error(
                    "ground: \"off\" names maps (base_color, normal, "
                    "metallic_roughness, occlusion or emissive)");
            g.off.push_back(name);
        }
    }
    return g;
}

nlohmann::json ground_texture_to_json(const GroundTexture& g) {
    nlohmann::json maps = nlohmann::json::object();
    const std::string* values[] = {&g.base_color, &g.normal, &g.metallic_roughness, &g.occlusion,
                                   &g.emissive};
    for (size_t i = 0; i < std::size(kGroundSlots); ++i)
        if (!values[i]->empty()) maps[kGroundSlots[i]] = *values[i];
    nlohmann::json out{{"textures", maps}, {"tile", g.tile}};
    if (!g.off.empty()) out["off"] = g.off;
    if (!g.name.empty()) out["name"] = g.name;
    return out;
}
}  // namespace

TerrainSettings terrain_from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("terrain settings are a JSON object");
    TerrainSettings t;
    const auto number = [&](const char* key, double& into) {
        if (!j.contains(key)) return;
        if (!j.at(key).is_number())
            throw std::runtime_error(std::string("terrain: \"") + key + "\" is a number");
        into = j.at(key).get<double>();
    };
    if (j.contains("seed")) {
        if (!j.at("seed").is_number_integer())
            throw std::runtime_error("terrain: the seed is a whole number");
        t.seed = j.at("seed").get<std::uint64_t>();
    }
    number("base", t.base);
    number("height_scale", t.height_scale);
    number("feature_size", t.feature_size);
    if (j.contains("octaves")) {
        if (!j.at("octaves").is_number_integer())
            throw std::runtime_error("terrain: \"octaves\" is a whole number");
        t.octaves = j.at("octaves").get<int>();
    }
    number("sea_level", t.sea_level);
    number("ocean_band", t.ocean_band);
    number("ocean_depth", t.ocean_depth);
    number("end_ramp", t.end_ramp);
    number("wall_height", t.wall_height);
    number("spacing", t.spacing);
    number("snow_line", t.snow_line);
    if (j.contains("wall")) {
        const std::string wall = j.at("wall").is_string() ? j.at("wall").get<std::string>() : "";
        if (wall == "cliff")
            t.wall = WallLook::Cliff;
        else if (wall == "fortification")
            t.wall = WallLook::Fortification;
        else if (wall == "barrier")
            t.wall = WallLook::Barrier;
        else
            throw std::runtime_error(
                "terrain: \"wall\" is \"cliff\", \"fortification\" or \"barrier\"");
    }
    if (j.contains("graph") && !j.at("graph").is_null()) {
        if (!j.at("graph").is_object())
            throw std::runtime_error("terrain: \"graph\" is a world graph (an object)");
        t.graph = j.at("graph").dump();
    }
    if (j.contains("climate") && !j.at("climate").is_null()) {
        const nlohmann::json& c = j.at("climate");
        if (!c.is_object()) throw std::runtime_error("terrain: \"climate\" is an object");
        Climate climate;
        const auto get = [&](const char* key, double& into) {
            if (!c.contains(key)) return;
            if (!c.at(key).is_number())
                throw std::runtime_error(std::string("climate: \"") + key + "\" is a number");
            into = c.at(key).get<double>();
        };
        get("temperature", climate.temperature);
        get("latitude_cooling", climate.latitude_cooling);
        get("lapse_rate", climate.lapse_rate);
        get("variation", climate.variation);
        get("season_swing", climate.season_swing);
        t.climate = climate;
    }
    if (j.contains("biomes")) {
        if (!j.at("biomes").is_array()) throw std::runtime_error("terrain: \"biomes\" is a list");
        for (const nlohmann::json& b : j.at("biomes")) {
            if (!b.is_object()) throw std::runtime_error("biomes: each is an object");
            Biome biome;
            biome.name = b.value("name", std::string());
            const auto range = [&](const char* key, double& lo, double& hi) {
                if (!b.contains(key)) return;
                const nlohmann::json& r = b.at(key);
                if (!r.is_array() || r.size() != 2 || !r[0].is_number() || !r[1].is_number())
                    throw std::runtime_error(std::string("biome: \"") + key +
                                             "\" is [lowest, highest]");
                lo = r[0].get<double>();
                hi = r[1].get<double>();
            };
            range("temperature", biome.temperature_min, biome.temperature_max);
            range("moisture", biome.moisture_min, biome.moisture_max);
            const auto colour = [&](const char* key, glm::vec3& into) {
                if (!b.contains(key)) return;
                const nlohmann::json& r = b.at(key);
                if (!r.is_array() || r.size() != 3)
                    throw std::runtime_error(std::string("biome: \"") + key + "\" is [r, g, b]");
                into = {r[0].get<float>(), r[1].get<float>(), r[2].get<float>()};
            };
            colour("color", biome.color);
            colour("dry_color", biome.dry_color);
            if (b.contains("texture")) biome.texture = ground_texture_from_json(b.at("texture"));
            if (b.contains("dry_texture"))
                biome.dry_texture = ground_texture_from_json(b.at("dry_texture"));
            t.biomes.push_back(biome);
        }
    }
    if (j.contains("placements")) {
        if (!j.at("placements").is_array())
            throw std::runtime_error("terrain: \"placements\" is a list");
        for (const nlohmann::json& r : j.at("placements")) {
            if (!r.is_object()) throw std::runtime_error("placements: each is an object");
            PlacementRule rule;
            rule.object = r.value("object", std::string());
            const auto number = [&](const char* key, double& into) {
                if (!r.contains(key)) return;
                if (!r.at(key).is_number())
                    throw std::runtime_error(std::string("placement: \"") + key + "\" is a number");
                into = r.at(key).get<double>();
            };
            const auto range = [&](const char* key, double& lo, double& hi) {
                if (!r.contains(key)) return;
                const nlohmann::json& v = r.at(key);
                if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
                    throw std::runtime_error(std::string("placement: \"") + key +
                                             "\" is [lowest, highest]");
                lo = v[0].get<double>();
                hi = v[1].get<double>();
            };
            number("spacing", rule.spacing);
            number("chance", rule.chance);
            range("slope", rule.slope_min, rule.slope_max);
            range("height", rule.height_min, rule.height_max);
            range("scale", rule.scale_min, rule.scale_max);
            number("align", rule.align);
            number("distance", rule.distance);
            rule.salt = r.value("salt", std::string());
            if (r.contains("biomes")) {
                if (!r.at("biomes").is_array())
                    throw std::runtime_error("placement: \"biomes\" is a list of names");
                for (const nlohmann::json& b : r.at("biomes"))
                    rule.biomes.push_back(b.get<std::string>());
            }
            t.placements.push_back(rule);
        }
    }
    if (j.contains("ground")) {
        if (!j.at("ground").is_object())
            throw std::runtime_error("terrain: \"ground\" is an object of layers");
        for (const auto& [layer, value] : j.at("ground").items())
            t.ground[layer] = ground_texture_from_json(value);
    }
    return t;
}

nlohmann::json terrain_to_json(const TerrainSettings& t) {
    nlohmann::json j{{"base", t.base},
                     {"height_scale", t.height_scale},
                     {"feature_size", t.feature_size},
                     {"octaves", t.octaves},
                     {"sea_level", t.sea_level},
                     {"ocean_band", t.ocean_band},
                     {"ocean_depth", t.ocean_depth},
                     {"end_ramp", t.end_ramp},
                     {"wall_height", t.wall_height},
                     {"wall", wall_look_name(t.wall)},
                     {"spacing", t.spacing},
                     {"snow_line", t.snow_line}};
    if (t.seed) j["seed"] = *t.seed;
    if (!t.graph.empty()) j["graph"] = nlohmann::json::parse(t.graph);
    if (t.climate)
        j["climate"] = {{"temperature", t.climate->temperature},
                        {"latitude_cooling", t.climate->latitude_cooling},
                        {"lapse_rate", t.climate->lapse_rate},
                        {"variation", t.climate->variation},
                        {"season_swing", t.climate->season_swing}};
    if (!t.placements.empty()) {
        j["placements"] = nlohmann::json::array();
        for (const PlacementRule& r : t.placements)
            j["placements"].push_back({{"object", r.object},
                                       {"spacing", r.spacing},
                                       {"chance", r.chance},
                                       {"slope", {r.slope_min, r.slope_max}},
                                       {"height", {r.height_min, r.height_max}},
                                       {"biomes", r.biomes},
                                       {"scale", {r.scale_min, r.scale_max}},
                                       {"align", r.align},
                                       {"salt", r.salt},
                                       {"distance", r.distance}});
    }
    if (!t.biomes.empty()) {
        j["biomes"] = nlohmann::json::array();
        for (const Biome& b : t.biomes) {
            nlohmann::json biome{{"name", b.name},
                                 {"temperature", {b.temperature_min, b.temperature_max}},
                                 {"moisture", {b.moisture_min, b.moisture_max}},
                                 {"color", {b.color.r, b.color.g, b.color.b}},
                                 {"dry_color", {b.dry_color.r, b.dry_color.g, b.dry_color.b}}};
            if (!b.texture.empty()) biome["texture"] = ground_texture_to_json(b.texture);
            if (!b.dry_texture.empty())
                biome["dry_texture"] = ground_texture_to_json(b.dry_texture);
            j["biomes"].push_back(biome);
        }
    }
    if (!t.ground.empty()) {
        j["ground"] = nlohmann::json::object();
        for (const auto& [layer, g] : t.ground) j["ground"][layer] = ground_texture_to_json(g);
    }
    return j;
}

std::string GroundTexture::map(const std::string& slot) const {
    if (std::find(off.begin(), off.end(), slot) != off.end()) return "";
    return slot == "base_color"           ? base_color
           : slot == "normal"             ? normal
           : slot == "metallic_roughness" ? metallic_roughness
           : slot == "occlusion"          ? occlusion
           : slot == "emissive"           ? emissive
                                          : "";
}

std::vector<std::string> GroundTexture::textures() const {
    std::vector<std::string> out;
    for (const char* slot : kGroundSlots)
        if (std::string id = map(slot); !id.empty()) out.push_back(std::move(id));
    return out;
}

std::vector<std::string> ground_textures(const TerrainSettings& t) {
    std::vector<std::string> out;
    const auto add = [&](const GroundTexture& g) {
        for (const std::string& id : g.textures())
            if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };
    for (const auto& [layer, g] : t.ground) add(g);
    for (const Biome& b : t.biomes) {
        add(b.texture);
        add(b.dry_texture);
    }
    return out;
}

std::vector<std::string> terrain_problems(const TerrainSettings& t) {
    std::vector<std::string> out;
    const auto finite = [](double v) { return std::isfinite(v); };
    if (!finite(t.base) || !finite(t.height_scale) || !finite(t.sea_level) ||
        !finite(t.ocean_depth) || !finite(t.wall_height) || !finite(t.snow_line))
        out.push_back("terrain: heights are numbers");
    if (!(t.height_scale >= 0.0 && t.height_scale <= 5000.0))
        out.push_back("terrain: the height scale is from 0 to 5000 m");
    if (!(t.feature_size >= 1.0 && t.feature_size <= 100000.0))
        out.push_back("terrain: the feature size is from 1 to 100,000 m");
    if (t.octaves < 1 || t.octaves > 12) out.push_back("terrain: octaves are from 1 to 12");
    if (!(t.ocean_band >= 0.0)) out.push_back("terrain: the ocean band can't be negative");
    if (!(t.ocean_depth >= 0.0)) out.push_back("terrain: the ocean's depth can't be negative");
    if (!(t.end_ramp >= 0.0)) out.push_back("terrain: the end ramp can't be negative");
    if (!(t.spacing >= 0.25 && t.spacing <= 64.0))
        out.push_back("terrain: the grid spacing is from 0.25 to 64 m");
    if (!t.graph.empty())
        for (const std::string& p : graph_problems(nlohmann::json::parse(t.graph)))
            out.push_back("terrain's graph: " + p);
    if (t.climate) {
        const Climate& c = *t.climate;
        if (!(c.temperature >= -150.0 && c.temperature <= 150.0))
            out.push_back("climate: the temperature is from -150 to 150 degrees");
        if (!(c.latitude_cooling >= 0.0 && c.latitude_cooling <= 200.0))
            out.push_back("climate: the poles' cooling is from 0 to 200 degrees");
        if (!(c.lapse_rate >= 0.0 && c.lapse_rate <= 50.0))
            out.push_back("climate: the cooling with height is from 0 to 50 degrees a km");
        if (!(c.variation >= 0.0 && c.variation <= 50.0))
            out.push_back("climate: the variation is from 0 to 50 degrees");
        if (!(c.season_swing >= 0.0 && c.season_swing <= 100.0))
            out.push_back("climate: the seasons' swing is from 0 to 100 degrees");
    }
    if (int(t.placements.size()) > kMaxPlacementRules)
        out.push_back("placements: at most " + std::to_string(kMaxPlacementRules) + " rules");
    for (const PlacementRule& r : t.placements) {
        const std::string who = "placing '" + r.object + "': ";
        if (r.object.empty()) out.push_back("placing: which object it places is needed");
        if (!(r.spacing >= 0.5 && r.spacing <= 1000.0))
            out.push_back(who + "the spacing is from 0.5 to 1000 m");
        if (!(r.chance >= 0.0 && r.chance <= 1.0)) out.push_back(who + "the chance is 0 to 1");
        if (!(r.slope_min >= 0.0 && r.slope_min <= r.slope_max && r.slope_max <= 90.0))
            out.push_back(who + "slopes run from lowest to highest, within 0 to 90 degrees");
        if (!(r.height_min <= r.height_max))
            out.push_back(who + "heights run from lowest to highest");
        if (!(r.scale_min > 0.0 && r.scale_min <= r.scale_max && r.scale_max <= 100.0))
            out.push_back(who + "sizes run from lowest to highest, above 0 and at most 100");
        if (!(r.align >= 0.0 && r.align <= 1.0)) out.push_back(who + "align is 0 to 1");
        if (!(r.distance >= 10.0 && r.distance <= 2000.0))
            out.push_back(who + "the distance is from 10 to 2000 m");
    }
    // The ground's textures (6.0b): known layers, sensible tiles, content ids.
    const auto texture_problems = [&](const std::string& who, const GroundTexture& g) {
        if (!(g.tile >= 0.1 && g.tile <= 1000.0))
            out.push_back(who + "its texture's tile is from 0.1 to 1000 m");
        for (const std::string& id : g.textures())
            if (id.find(':') == std::string::npos)
                out.push_back(who + "'" + id + "' isn't a texture's content id");
    };
    for (const auto& [layer, g] : t.ground) {
        if (std::find_if(std::begin(kGroundLayers), std::end(kGroundLayers),
                         [&](const char* n) { return layer == n; }) == std::end(kGroundLayers))
            out.push_back("ground: '" + layer +
                          "' isn't a layer (grass, dry_grass, sand, rock, snow, paint_1 or "
                          "paint_2)");
        texture_problems("ground '" + layer + "': ", g);
    }
    for (const Biome& b : t.biomes) {
        texture_problems("biome '" + b.name + "': ", b.texture);
        texture_problems("biome '" + b.name + "' (dry): ", b.dry_texture);
    }
    if (int(t.biomes.size()) > kMaxBiomes)
        out.push_back("biomes: at most " + std::to_string(kMaxBiomes));
    for (const Biome& b : t.biomes) {
        const std::string who = "biome '" + b.name + "': ";
        if (!(b.temperature_min <= b.temperature_max))
            out.push_back(who + "its temperatures run from lowest to highest");
        if (!(b.moisture_min <= b.moisture_max && b.moisture_min >= 0.0 && b.moisture_max <= 1.0))
            out.push_back(who + "its moisture runs from lowest to highest, within 0 to 1");
        for (const glm::vec3 c : {b.color, b.dry_color})
            if (!(c.r >= 0 && c.r <= 1 && c.g >= 0 && c.g <= 1 && c.b >= 0 && c.b <= 1)) {
                out.push_back(who + "colours are 0 to 1");
                break;
            }
    }
    return out;
}

const char* stamp_tool_name(StampTool tool) {
    switch (tool) {
        case StampTool::Raise:
            return "raise";
        case StampTool::Lower:
            return "lower";
        case StampTool::Smooth:
            return "smooth";
        case StampTool::Flatten:
            return "flatten";
        case StampTool::Paint:
            return "paint";
    }
    return "raise";
}

const char* paint_layer_name(PaintLayer layer) {
    switch (layer) {
        case PaintLayer::Clear:
            return "clear";
        case PaintLayer::Lush:
            return "lush";
        case PaintLayer::Dry:
            return "dry";
        case PaintLayer::Sand:
            return "sand";
        case PaintLayer::Rock:
            return "rock";
        case PaintLayer::Paint1:
            return "paint_1";
        case PaintLayer::Paint2:
            return "paint_2";
    }
    return "clear";
}

TerrainStamp stamp_from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("a terrain stamp is a JSON object");
    TerrainStamp s;
    const std::string tool = j.value("tool", std::string("raise"));
    bool known = false;
    for (StampTool t : {StampTool::Raise, StampTool::Lower, StampTool::Smooth, StampTool::Flatten,
                        StampTool::Paint})
        if (tool == stamp_tool_name(t)) {
            s.tool = t;
            known = true;
        }
    if (!known) throw std::runtime_error("terrain stamp: no tool '" + tool + "'");
    const std::string paint = j.value("paint", std::string("lush"));
    known = false;
    for (PaintLayer l : {PaintLayer::Clear, PaintLayer::Lush, PaintLayer::Dry, PaintLayer::Sand,
                         PaintLayer::Rock, PaintLayer::Paint1, PaintLayer::Paint2})
        if (paint == paint_layer_name(l)) {
            s.paint = l;
            known = true;
        }
    if (!known) throw std::runtime_error("terrain stamp: no paint '" + paint + "'");
    const auto number = [&](const char* key, double& into) {
        if (!j.contains(key)) return;
        if (!j.at(key).is_number())
            throw std::runtime_error(std::string("terrain stamp: '") + key + "' is a number");
        into = j.at(key).get<double>();
    };
    number("radius", s.radius);
    number("strength", s.strength);
    number("height", s.height);
    s.face = j.value("face", 0);
    if (!j.contains("points") || !j.at("points").is_array())
        throw std::runtime_error("terrain stamp: 'points' is a list of [u, v]");
    for (const nlohmann::json& p : j.at("points")) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
            throw std::runtime_error("terrain stamp: a point is [u, v]");
        s.points.emplace_back(p[0].get<double>(), p[1].get<double>());
    }
    return s;
}

nlohmann::json stamp_to_json(const TerrainStamp& s) {
    nlohmann::json points = nlohmann::json::array();
    for (const glm::dvec2& p : s.points) points.push_back({p.x, p.y});
    nlohmann::json j{{"tool", stamp_tool_name(s.tool)},
                     {"radius", s.radius},
                     {"strength", s.strength},
                     {"face", s.face},
                     {"points", points}};
    if (s.tool == StampTool::Paint) j["paint"] = paint_layer_name(s.paint);
    if (s.tool == StampTool::Flatten) j["height"] = s.height;
    return j;
}

std::vector<std::string> stamp_problems(const TerrainStamp& s) {
    std::vector<std::string> out;
    if (!(s.radius > 0.0 && s.radius <= 2000.0))
        out.push_back("terrain stamp: the radius is above 0 and at most 2000 m");
    const bool metres = s.tool == StampTool::Raise || s.tool == StampTool::Lower;
    if (metres ? !(s.strength >= 0.0 && s.strength <= 1000.0)
               : !(s.strength >= 0.0 && s.strength <= 1.0))
        out.push_back(metres ? "terrain stamp: raising and lowering is 0 to 1000 m"
                             : "terrain stamp: the strength is 0 to 1");
    if (s.points.empty()) out.push_back("terrain stamp: a stroke has a point at least");
    for (const glm::dvec2& p : s.points)
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            out.push_back("terrain stamp: points are numbers");
            break;
        }
    if (!std::isfinite(s.height)) out.push_back("terrain stamp: the height is a number");
    return out;
}

Terrain::Terrain(const Body& body, const TerrainSettings& settings,
                 std::vector<TerrainStamp> stamps)
    : body_(body),
      settings_(settings),
      stamps_(std::move(stamps)),
      seed_(settings.seed.value_or(body.seed)) {
    if (!settings_.graph.empty())
        graph_ = std::make_shared<const WorldGraph>(
            WorldGraph::from_json(nlohmann::json::parse(settings_.graph)));
    const glm::dvec2 size = body_.shape.face_size();
    if (sphere(body_.shape.kind())) {
        roots_u_ = roots_v_ = sphere_roots(size.x);
        // Coarser than the roots, up to one chunk a face (the far side of a hollow world).
        while ((1 << -min_level_) < roots_u_) --min_level_;
    } else {
        roots_u_ = roots_along(size.x);
        roots_v_ = roots_along(size.y);
    }
    const double root = std::max(size.x / roots_u_, size.y / roots_v_);
    const double spacing = std::max(settings_.spacing, 0.25);
    while (max_level_ < kMaxTerrainLevel &&
           root / (double(std::int64_t(1) << max_level_) * kChunkQuads) > spacing)
        ++max_level_;
    for (const TerrainStamp& s : stamps_) {
        bounds_.push_back(stamp_bounds(s));
        if (sphere(body_.shape.kind())) {
            strokes_.push_back(stroke_points(s));
            double reach = 0.0;
            for (const glm::dvec3& q : strokes_.back())
                reach = std::max(reach, glm::length(q - strokes_.back().front()));
            reach_.push_back(reach + s.radius);
        }
    }
}

std::int64_t Terrain::chunks_u(int level) const {
    return level >= 0 ? std::int64_t(roots_u_) << level : std::int64_t(roots_u_) >> -level;
}

std::int64_t Terrain::chunks_v(int level) const {
    return level >= 0 ? std::int64_t(roots_v_) << level : std::int64_t(roots_v_) >> -level;
}

std::int64_t Terrain::points_u(int level) const {
    return chunks_u(level) * kChunkQuads;
}

std::int64_t Terrain::points_v(int level) const {
    return chunks_v(level) * kChunkQuads;
}

bool Terrain::valid(const ChunkKey& k) const {
    return k.face >= 0 && k.face < body_.shape.faces() && k.level >= min_level_ &&
           k.level <= max_level_ && k.x >= 0 && k.y >= 0 && k.x < chunks_u(k.level) &&
           k.y < chunks_v(k.level);
}

std::array<ChunkKey, 4> Terrain::children(const ChunkKey& k) const {
    const int l = k.level + 1, x = k.x * 2, y = k.y * 2;
    return {ChunkKey{k.face, l, x, y}, ChunkKey{k.face, l, x + 1, y}, ChunkKey{k.face, l, x, y + 1},
            ChunkKey{k.face, l, x + 1, y + 1}};
}

ChunkKey Terrain::parent(const ChunkKey& k) const {
    if (k.level <= min_level_) return k;
    return {k.face, k.level - 1, k.x / 2, k.y / 2};
}

std::optional<ChunkKey> Terrain::neighbour(const ChunkKey& k, ChunkEdge edge) const {
    ChunkKey n = k;
    const int across_u = int(chunks_u(k.level)), across_v = int(chunks_v(k.level));
    switch (edge) {
        case kWest:
            n.x -= 1;
            break;
        case kEast:
            n.x += 1;
            break;
        case kSouth:
            n.y -= 1;
            break;
        case kNorth:
            n.y += 1;
            break;
    }
    const bool off = n.x < 0 || n.x >= across_u || n.y < 0 || n.y >= across_v;
    if (off && sphere(body_.shape.kind())) {
        // Onto the face across that edge (square faces: across_u == across_v).
        const CubeLink link = cube_link(k.face, int(edge));
        int along = edge < kSouth ? k.y : k.x;
        if (link.reversed) along = across_u - 1 - along;
        const int in = link.edge % 2 == 0 ? 0 : across_u - 1;
        n.face = link.face;
        n.x = link.edge < kSouth ? in : along;
        n.y = link.edge < kSouth ? along : in;
        return n;
    }
    if (n.x < 0 || n.x >= across_u) {
        if (!wraps(body_.shape.kind())) return std::nullopt;
        n.x = (n.x + across_u) % across_u;
    }
    if (n.y < 0 || n.y >= across_v) return std::nullopt;
    return n;
}

glm::dvec3 Terrain::noise_position(int face, double u, double v) const {
    const Shape& s = body_.shape;
    if (s.kind() == Shape::Kind::FlatCylinder) {
        // On the circle it would be if it were curved, so its edges meet.
        const glm::dvec2 size = s.face_size();
        const double r = size.x / kTwoPi;
        const double angle = kTwoPi * wrap01(u);
        return {r * det::sin(angle), -(r * det::cos(angle)), (v - 0.5) * size.y};
    }
    return s.position({face, u, v, 0.0});
}

std::vector<glm::dvec3> Terrain::stroke_points(const TerrainStamp& s) const {
    // On the sphere at height 0 (the noise positions' frame), from the stamp's face's u and v,
    // which may run a little past its edges for a stroke that crosses one.
    std::vector<glm::dvec3> out;
    out.reserve(s.points.size());
    for (const glm::dvec2& p : s.points)
        out.push_back(body_.shape.position({s.face, p.x, p.y, 0.0}));
    return out;
}

double Terrain::noise_height(const glm::dvec3& q) const {
    const TerrainSettings& t = settings_;
    const double f = t.feature_size;
    const det::Fixed n = det::fbm(seed_, q.x / f, q.y / f, q.z / f, t.octaves);
    return t.base + t.height_scale * (double(n) / 65536.0);
}

double Terrain::smooth_average(const glm::dvec3& q, double step) const {
    // Four points `step` metres away along a frame from q's direction alone (not its face's
    // u and v, which turn at an edge), back on the sphere: the same from every face.
    const glm::dvec3 r = unit3(q);
    const double radius = body_.shape.radius();
    const glm::dvec3 axis =
        std::abs(r.y) < 0.9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 east = unit3(cross3(axis, r));
    const glm::dvec3 north = cross3(r, east);
    const glm::dvec3 steps[4] = {{east.x * step, east.y * step, east.z * step},
                                 {-(east.x * step), -(east.y * step), -(east.z * step)},
                                 {north.x * step, north.y * step, north.z * step},
                                 {-(north.x * step), -(north.y * step), -(north.z * step)}};
    double sum = 0.0;
    for (const glm::dvec3& d : steps) {
        const glm::dvec3 p = unit3(glm::dvec3(q.x + d.x, q.y + d.y, q.z + d.z));
        sum = sum + noise_height(glm::dvec3(p.x * radius, p.y * radius, p.z * radius));
    }
    return sum * 0.25;
}

Terrain::StampBounds Terrain::stamp_bounds(const TerrainStamp& s) const {
    const glm::dvec2 size = body_.shape.face_size();
    StampBounds b{1e300, -1e300, 1e300, -1e300};
    for (const glm::dvec2& p : s.points) {
        b.u0 = std::min(b.u0, p.x);
        b.u1 = std::max(b.u1, p.x);
        b.v0 = std::min(b.v0, p.y);
        b.v1 = std::max(b.v1, p.y);
    }
    const double ru = s.radius / size.x, rv = s.radius / size.y;
    b.u0 -= ru;
    b.u1 += ru;
    b.v0 -= rv;
    b.v1 += rv;
    if (wraps(body_.shape.kind())) {  // round the cylinder: any u
        b.u0 = -1e300;
        b.u1 = 1e300;
    }
    return b;
}

bool Terrain::touches(const ChunkKey& k, const TerrainStamp& s) const {
    if (sphere(body_.shape.kind())) {  // by distance: a stroke can reach across face edges
        const std::vector<glm::dvec3> points = stroke_points(s);
        if (points.empty()) return false;
        double reach = s.radius;
        for (const glm::dvec3& q : points)
            reach = std::max(reach, glm::length(q - points.front()) + s.radius);
        const glm::dvec3 middle =
            noise_position(k.face, u_at(k, kChunkQuads / 2), v_at(k, kChunkQuads / 2));
        const double side = body_.shape.face_size().x / double(chunks_u(k.level));
        return glm::length(middle - points.front()) <= reach + side;  // (a corner: 0.71 sides)
    }
    if (s.face != k.face) return false;
    const StampBounds b = stamp_bounds(s);
    const double u0 = u_at(k, -1), u1 = u_at(k, kChunkQuads + 1);
    const double v0 = v_at(k, -1), v1 = v_at(k, kChunkQuads + 1);
    return b.u0 <= u1 && b.u1 >= u0 && b.v0 <= v1 && b.v1 >= v0;
}

double Terrain::stamp_weight(const TerrainStamp& s, int face, double u, double v) const {
    if (sphere(body_.shape.kind()))
        return stroke_weight(stroke_points(s), s.radius, noise_position(face, u, v));
    if (s.face != face || s.points.empty() || !(s.radius > 0.0)) return 0.0;
    const glm::dvec2 size = body_.shape.face_size();
    const bool round = wraps(body_.shape.kind());
    // The stroke's points in metres from (u, v) (round a cylinder, the nearer way).
    const auto metres = [&](const glm::dvec2& p) {
        double du = p.x - u;
        if (round) du = du - std::floor(du + 0.5);
        return glm::dvec2(du * size.x, (p.y - v) * size.y);
    };
    glm::dvec2 a = metres(s.points[0]);
    double best = a.x * a.x + a.y * a.y;  // the distance squared to the stroke's line
    for (size_t i = 1; i < s.points.size(); ++i) {
        const glm::dvec2 b = metres(s.points[i]);
        const double ex = b.x - a.x, ey = b.y - a.y;
        const double len2 = ex * ex + ey * ey;
        double t = len2 > 0.0 ? -(a.x * ex + a.y * ey) / len2 : 0.0;
        t = std::min(std::max(t, 0.0), 1.0);
        const double cx = a.x + ex * t, cy = a.y + ey * t;
        best = std::min(best, cx * cx + cy * cy);
        a = b;
    }
    return smooth01(1.0 - det::sqrt(best) / s.radius);
}

double Terrain::height(int face, double u, double v) const {
    double h = base_height(face, u, v);
    const bool round = !strokes_.empty();  // a sphere with stamps
    const glm::dvec3 q = round ? noise_position(face, u, v) : glm::dvec3(0.0);
    for (size_t i = 0; i < stamps_.size(); ++i) {
        const TerrainStamp& s = stamps_[i];
        double w = 0.0;
        if (round) {  // by 3D distance, across face edges too
            if (s.tool == StampTool::Paint || s.points.empty()) continue;
            const glm::dvec3 d = q - strokes_[i].front();
            if (dot3(d, d) > reach_[i] * reach_[i]) continue;
            w = stroke_weight(strokes_[i], s.radius, q);
        } else {
            const StampBounds& b = bounds_[i];
            if (s.tool == StampTool::Paint || s.face != face || u < b.u0 || u > b.u1 || v < b.v0 ||
                v > b.v1)
                continue;
            w = stamp_weight(s, face, u, v);
        }
        if (w <= 0.0) continue;
        switch (s.tool) {
            case StampTool::Raise:
                h = h + s.strength * w;
                break;
            case StampTool::Lower:
                h = h - s.strength * w;
                break;
            case StampTool::Flatten:
                h = h + (s.height - h) * (w * s.strength);
                break;
            case StampTool::Smooth: {
                if (round) {
                    h = h + (smooth_average(q, 0.5 * s.radius) - h) * (w * s.strength);
                    break;
                }
                const glm::dvec2 size = body_.shape.face_size();
                const double du = 0.5 * s.radius / size.x, dv = 0.5 * s.radius / size.y;
                const double average =
                    (base_height(face, u + du, v) + base_height(face, u - du, v) +
                     base_height(face, u, v + dv) + base_height(face, u, v - dv)) *
                    0.25;
                h = h + (average - h) * (w * s.strength);
                break;
            }
            case StampTool::Paint:
                break;
        }
    }
    return h;
}

std::optional<glm::dvec3> Terrain::raycast(const glm::dvec3& origin, const glm::dvec3& direction,
                                           double distance) const {
    const glm::dvec3 d = glm::normalize(direction);
    if (above_ground(origin) < 0.0) return std::nullopt;  // starting under the ground
    double t = 0.0;
    while (t < distance) {
        const double next = std::min(t + std::max(0.5, t * 0.01), distance);
        if (above_ground(origin + d * next) < 0.0) {  // crossed: narrow it down
            double lo = t, hi = next;
            for (int i = 0; i < 30; ++i) {
                const double mid = 0.5 * (lo + hi);
                (above_ground(origin + d * mid) < 0.0 ? hi : lo) = mid;
            }
            return origin + d * hi;
        }
        t = next;
    }
    return std::nullopt;
}

double Terrain::base_height(int face, double u, double v) const {
    const TerrainSettings& t = settings_;
    // A world type's graph, or the 5.9 hills; then the shape's edges shape it.
    double h = graph_ ? graph_->evaluate(body_.shape, seed_, {face, u, v})
                      : noise_height(noise_position(face, u, v));
    const Shape::Kind kind = body_.shape.kind();
    const glm::dvec2 size = body_.shape.face_size();
    if (kind == Shape::Kind::BoundedArea) {
        if (t.ocean_band > 0.0) {
            const double d = std::min(std::min(u * size.x, (1.0 - u) * size.x),
                                      std::min(v * size.y, (1.0 - v) * size.y));
            const double s = smooth01((d - t.ocean_band) / t.ocean_band);
            h = -t.ocean_depth + (h + t.ocean_depth) * s;
        }
    } else if (wraps(kind)) {
        if (t.end_ramp > 0.0) {
            const double e = std::min(v * size.y, (1.0 - v) * size.y);
            const double s = smooth01(1.0 - e / t.end_ramp);
            h = h + (t.wall_height - h) * s;
        }
    }
    return h;
}

glm::dvec3 Terrain::ground(const glm::dvec3& p) const {
    const SurfacePoint s = body_.surface(p);
    glm::dvec3 g = position(s.face, s.u, s.v, height(s.face, s.u, s.v));
    if (body_.shape.kind() == Shape::Kind::FlatCylinder) {  // the copy nearest the point
        const double c = body_.shape.face_size().x;
        g.x += std::round((p.x - g.x) / c) * c;
    }
    return g;
}

double Terrain::above_ground(const glm::dvec3& p) const {
    const SurfacePoint s = body_.surface(p);
    return s.height - height(s.face, s.u, s.v);
}

glm::dvec3 Terrain::position(int face, double u, double v, double h) const {
    const Shape& s = body_.shape;
    if (s.kind() == Shape::Kind::BoundedArea || s.kind() == Shape::Kind::FlatCylinder) {
        const glm::dvec2 size = s.face_size();
        return body_.origin + glm::dvec3((u - 0.5) * size.x, h, (v - 0.5) * size.y);
    }
    return body_.position({face, u, v, h});
}

double Terrain::u_at(const ChunkKey& k, int i) const {
    return double(std::int64_t(k.x) * kChunkQuads + i) / double(points_u(k.level));
}

double Terrain::v_at(const ChunkKey& k, int j) const {
    return double(std::int64_t(k.y) * kChunkQuads + j) / double(points_v(k.level));
}

double Terrain::temperature_at(int face, double u, double v, double height) const {
    if (!settings_.climate) return 0.0;
    const Climate& c = *settings_.climate;
    double latitude = 0.0;  // degrees
    if (sphere(body_.shape.kind())) {
        const glm::dvec3 d = cube_direction(face, u, v);
        latitude = std::abs(det::atan2(d.y, det::sqrt(d.x * d.x + d.z * d.z))) * 57.29577951308232;
    }
    const glm::dvec3 q = noise_position(face, u, v);
    const double noise = double(det::fbm(seed_ ^ det::tag("temperature"), q.x / 3000.0,
                                         q.y / 3000.0, q.z / 3000.0, 3)) /
                         65536.0;
    return c.temperature - c.latitude_cooling * latitude / 90.0 -
           c.lapse_rate * std::max(height, 0.0) / 1000.0 + c.variation * noise;
}

int Terrain::biome_index(int face, double u, double v, double height) const {
    if (settings_.biomes.empty()) return -1;
    const double t = temperature_at(face, u, v, height);
    const double m = biome_at(face, u, v).x;
    for (size_t i = 0; i < settings_.biomes.size(); ++i) {
        const Biome& b = settings_.biomes[i];
        if (t >= b.temperature_min && t <= b.temperature_max && m >= b.moisture_min &&
            m <= b.moisture_max)
            return int(i);
    }
    return -1;
}

int Terrain::placement_level() const {
    const double width = body_.shape.face_size().x;
    int level = max_level_;
    while (level > min_level_ && width / double(chunks_u(level)) < 64.0) --level;
    return level;
}

std::vector<TerrainPlacement> Terrain::placements_in(const ChunkKey& tile) const {
    std::vector<TerrainPlacement> out;
    if (settings_.placements.empty()) return out;
    const glm::dvec2 size = body_.shape.face_size();
    const double cu = double(chunks_u(tile.level)), cv = double(chunks_v(tile.level));
    const double u0 = double(tile.x) / cu, u1 = double(tile.x + 1) / cu;
    const double v0 = double(tile.y) / cv, v1 = double(tile.y + 1) / cv;
    for (size_t r = 0; r < settings_.placements.size(); ++r) {
        const PlacementRule& rule = settings_.placements[r];
        const std::uint64_t rule_seed =
            seed_ ^ det::tag("placement") ^ det::mix64(r + 1) ^ det::tag(rule.salt.c_str());
        const auto first = [&](double t, double extent) {
            return std::int64_t(std::floor(t * extent / rule.spacing));
        };
        for (std::int64_t cy = first(v0, size.y); cy <= first(v1, size.y); ++cy)
            for (std::int64_t cx = first(u0, size.x); cx <= first(u1, size.x); ++cx) {
                // Every random number drawn, in this order, whatever is kept.
                det::Rng rng(det::hash3(rule_seed, cx, cy, tile.face));
                const double ju = rng.uniform(), jv = rng.uniform(), keep = rng.uniform();
                const double size_t01 = rng.uniform(), yaw = rng.uniform() * 360.0;
                const double u = (double(cx) + ju) * rule.spacing / size.x;
                const double v = (double(cy) + jv) * rule.spacing / size.y;
                if (!(u >= u0 && u < u1 && v >= v0 && v < v1) || u < 0.0 || u >= 1.0 || v < 0.0 ||
                    v >= 1.0 || !(keep < rule.chance))
                    continue;
                const double h = height(tile.face, u, v);
                const double above = h - settings_.sea_level;
                if (above < rule.height_min || above > rule.height_max) continue;
                // The slope, from the heights half a metre either way.
                const double d = 0.5;
                const double gx =
                    (height(tile.face, u + d / size.x, v) - height(tile.face, u - d / size.x, v)) /
                    (2.0 * d);
                const double gy =
                    (height(tile.face, u, v + d / size.y) - height(tile.face, u, v - d / size.y)) /
                    (2.0 * d);
                const double slope = det::atan(det::sqrt(gx * gx + gy * gy)) * 57.29577951308232;
                if (slope < rule.slope_min || slope > rule.slope_max) continue;
                if (!rule.biomes.empty()) {
                    const int b = biome_index(tile.face, u, v, h);
                    if (b < 0 || std::find(rule.biomes.begin(), rule.biomes.end(),
                                           settings_.biomes[size_t(b)].name) == rule.biomes.end())
                        continue;
                }
                TerrainPlacement p;
                p.rule = int(r);
                p.face = tile.face;
                p.u = u;
                p.v = v;
                p.position = position(tile.face, u, v, h);
                const glm::dvec3 up = body_.up(p.position);
                if (rule.align > 0.0) {
                    const SurfaceFrame f = body_.frame(p.position);
                    const glm::dvec3 normal = glm::normalize(f.up - f.east * gx - f.north * gy);
                    p.up = glm::normalize(up + (normal - up) * rule.align);
                } else {
                    p.up = up;
                }
                p.yaw = yaw;
                p.scale = rule.scale_min + (rule.scale_max - rule.scale_min) * size_t01;
                out.push_back(p);
            }
    }
    return out;
}

glm::vec4 Terrain::biome_at(int face, double u, double v) const {
    const glm::dvec3 q = noise_position(face, u, v);
    const double moist = double(det::fbm(seed_ ^ det::tag("moisture"), q.x / 1500.0, q.y / 1500.0,
                                         q.z / 1500.0, 4)) /
                         65536.0;
    const double detail =
        double(det::gradient_noise(seed_ ^ det::tag("detail"), q.x / 7.0, q.y / 7.0, q.z / 7.0)) /
        65536.0;
    const double natural = std::clamp(0.5 + 0.7 * moist, 0.0, 1.0);
    double moisture = natural, ground = 0.0, layers = 0.0;  // (layers: the paint layers)
    for (size_t i = 0; i < stamps_.size(); ++i) {
        const TerrainStamp& s = stamps_[i];
        double w = 0.0;
        if (!strokes_.empty()) {  // a sphere: by 3D distance, across face edges too
            if (s.tool != StampTool::Paint || s.points.empty()) continue;
            const glm::dvec3 d = q - strokes_[i].front();
            if (dot3(d, d) > reach_[i] * reach_[i]) continue;
            w = stroke_weight(strokes_[i], s.radius, q);
        } else {
            const StampBounds& b = bounds_[i];
            if (s.tool != StampTool::Paint || s.face != face || u < b.u0 || u > b.u1 || v < b.v0 ||
                v > b.v1)
                continue;
            w = stamp_weight(s, face, u, v);
        }
        w = w * std::clamp(s.strength, 0.0, 1.0);
        if (w <= 0.0) continue;
        switch (s.paint) {
            case PaintLayer::Lush:
                moisture += (1.0 - moisture) * w;
                break;
            case PaintLayer::Dry:
                moisture -= moisture * w;
                break;
            case PaintLayer::Sand:  // (over a paint layer: it goes)
                ground += (-1.0 - ground) * w;
                layers -= layers * w;
                break;
            case PaintLayer::Rock:
                ground += (1.0 - ground) * w;
                layers -= layers * w;
                break;
            case PaintLayer::Paint1:  // (over sand or rock: they go)
                layers += (1.0 - layers) * w;
                ground -= ground * w;
                break;
            case PaintLayer::Paint2:
                layers += (-1.0 - layers) * w;
                ground -= ground * w;
                break;
            case PaintLayer::Clear:
                moisture += (natural - moisture) * w;
                ground -= ground * w;
                layers -= layers * w;
                break;
        }
    }
    return {float(moisture), float(std::clamp(detail, -1.0, 1.0)), float(ground), float(layers)};
}

bool Terrain::flipped(const ChunkKey& k) const {
    // Across the chunk's middle: from v's step to u's, crossed, against up there.
    const double u = u_at(k, kChunkQuads / 2), v = v_at(k, kChunkQuads / 2);
    const double du = 1.0 / double(points_u(k.level)), dv = 1.0 / double(points_v(k.level));
    const glm::dvec3 p = position(k.face, u, v, 0.0);
    const glm::dvec3 c =
        glm::cross(position(k.face, u, v + dv, 0.0) - p, position(k.face, u + du, v, 0.0) - p);
    return glm::dot(c, body_.up(p)) < 0.0;
}

glm::dvec3 Terrain::chunk_origin(const ChunkKey& k) const {
    return on_grid(position(k.face, u_at(k, kChunkQuads / 2), v_at(k, kChunkQuads / 2), 0.0));
}

ChunkMesh Terrain::mesh(const ChunkKey& k, const std::array<bool, 4>& coarser) const {
    constexpr int n = kChunkQuads;
    constexpr int g = n + 3;  // with a border of one point all round, for normals
    std::vector<glm::dvec3> p(size_t(g) * g);
    ChunkMesh out;
    out.heights.resize(size_t(n + 1) * (n + 1));
    const bool round = sphere(body_.shape.kind());
    for (int j = -1; j <= n + 1; ++j) {
        const double v = v_at(k, j);
        for (int i = -1; i <= n + 1; ++i) {
            const double u = u_at(k, i);
            // On a sphere, the border past a face's edge is the face across it (the same grid
            // points, so normals match there). A cube corner's border point is never used.
            SurfacePoint s{k.face, u, v, 0.0};
            if (round && (u < 0.0 || u > 1.0) != (v < 0.0 || v > 1.0)) s = cube_across(s);
            const double h = height(s.face, s.u, s.v);
            p[size_t(j + 1) * g + size_t(i + 1)] = position(s.face, s.u, s.v, h);
            if (i >= 0 && i <= n && j >= 0 && j <= n) out.heights[size_t(j) * (n + 1) + i] = h;
        }
    }
    const auto at = [&](int i, int j) -> glm::dvec3& { return p[size_t(j + 1) * g + (i + 1)]; };
    // Which way round the grid turns, seen from above: u then v turn clockwise on a flat map
    // (x then z, seen from +y), but the other way on the outside of a sphere.
    const bool flip = flipped(k);
    std::vector<glm::dvec3> normal(size_t(n + 1) * (n + 1));
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const glm::dvec3 c =
                glm::cross(at(i, j + 1) - at(i, j - 1), at(i + 1, j) - at(i - 1, j));
            normal[size_t(j) * (n + 1) + i] = glm::normalize(flip ? -c : c);
        }
    // Onto a coarser neighbour's edge: the points between its points move to the midpoint.
    const auto stitch = [&](int i0, int j0, int di, int dj) {
        for (int s = 1; s < n; s += 2) {
            const int i = i0 + di * s, j = j0 + dj * s;
            at(i, j) = (at(i - di, j - dj) + at(i + di, j + dj)) * 0.5;
            glm::dvec3& nm = normal[size_t(j) * (n + 1) + i];
            nm = glm::normalize(normal[size_t(j - dj) * (n + 1) + (i - di)] +
                                normal[size_t(j + dj) * (n + 1) + (i + di)]);
        }
    };
    if (coarser[kWest]) stitch(0, 0, 0, 1);
    if (coarser[kEast]) stitch(n, 0, 0, 1);
    if (coarser[kSouth]) stitch(0, 0, 1, 0);
    if (coarser[kNorth]) stitch(0, n, 1, 0);

    out.origin = chunk_origin(k);
    out.positions.reserve(size_t(n + 1) * (n + 1));
    out.normals.reserve(size_t(n + 1) * (n + 1));
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            out.positions.push_back(glm::vec3(on_grid(at(i, j)) - out.origin));
            out.normals.push_back(glm::vec3(normal[size_t(j) * (n + 1) + i]));
            out.ups.push_back(glm::vec3(body_.up(at(i, j))));
            out.biome.push_back(biome_at(k.face, u_at(k, i), v_at(k, j)));
            if (settings_.climate)
                out.temperatures.push_back(float(temperature_at(
                    k.face, u_at(k, i), v_at(k, j), out.heights[size_t(j) * (n + 1) + i])));
            const glm::dvec2 size = body_.shape.face_size();
            out.uvs.emplace_back(float(u_at(k, i) * size.x / 16.0),
                                 float(v_at(k, j) * size.y / 16.0));
        }
    out.indices.reserve(size_t(n) * n * 6);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = std::uint32_t(j * (n + 1) + i), b = a + (n + 1), c = a + 1,
                                d = b + 1;
            if (flip)
                out.indices.insert(out.indices.end(), {a, d, b, a, c, d});
            else
                out.indices.insert(out.indices.end(), {a, b, d, a, d, c});
        }
    return out;
}

std::optional<ChunkMesh> Terrain::water_mesh(const ChunkKey& k,
                                             const std::array<bool, 4>& coarser) const {
    constexpr int n = kChunkQuads;
    const double sea = settings_.sea_level;
    ChunkMesh out;
    out.origin = chunk_origin(k);
    std::vector<glm::dvec3> p(size_t(n + 1) * (n + 1));
    out.heights.resize(p.size());
    bool wet = false;
    const glm::dvec2 size = body_.shape.face_size();
    for (int j = 0; j <= n; ++j) {
        const double v = v_at(k, j);
        for (int i = 0; i <= n; ++i) {
            const double u = u_at(k, i);
            const size_t at = size_t(j) * (n + 1) + i;
            out.heights[at] = sea - height(k.face, u, v);
            wet = wet || out.heights[at] > 0.0;
            p[at] = position(k.face, u, v, sea);
            out.uvs.emplace_back(float(u * size.x / 16.0), float(v * size.y / 16.0));
        }
    }
    if (!wet) return std::nullopt;
    const bool flip = flipped(k);
    const auto stitch = [&](int i0, int j0, int di, int dj) {
        for (int s = 1; s < n; s += 2) {
            const size_t at = size_t(j0 + dj * s) * (n + 1) + (i0 + di * s);
            const size_t lo = size_t(j0 + dj * (s - 1)) * (n + 1) + (i0 + di * (s - 1));
            const size_t hi = size_t(j0 + dj * (s + 1)) * (n + 1) + (i0 + di * (s + 1));
            p[at] = (p[lo] + p[hi]) * 0.5;
            out.heights[at] = (out.heights[lo] + out.heights[hi]) * 0.5;
        }
    };
    if (coarser[kWest]) stitch(0, 0, 0, 1);
    if (coarser[kEast]) stitch(n, 0, 0, 1);
    if (coarser[kSouth]) stitch(0, 0, 1, 0);
    if (coarser[kNorth]) stitch(0, n, 1, 0);
    for (const glm::dvec3& q : p) {
        out.positions.push_back(glm::vec3(on_grid(q) - out.origin));
        out.normals.push_back(glm::vec3(body_.up(q)));
    }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = std::uint32_t(j * (n + 1) + i), b = a + (n + 1), c = a + 1,
                                d = b + 1;
            if (flip)
                out.indices.insert(out.indices.end(), {a, d, b, a, c, d});
            else
                out.indices.insert(out.indices.end(), {a, b, d, a, d, c});
        }
    return out;
}

std::uint64_t Terrain::chunk_hash(const ChunkKey& k) const {
    constexpr int n = kChunkQuads;
    Fnv1a64 h;
    for (const std::int32_t v :
         {std::int32_t(k.face), std::int32_t(k.level), std::int32_t(k.x), std::int32_t(k.y)}) {
        unsigned char b[4];
        for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>(std::uint32_t(v) >> (8 * i));
        h.add(b, 4);
    }
    for (int j = 0; j <= n; ++j) {
        const double v = v_at(k, j);
        for (int i = 0; i <= n; ++i) {
            const double height_here = height(k.face, u_at(k, i), v);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &height_here, 8);
            unsigned char b[8];
            for (int c = 0; c < 8; ++c) b[c] = static_cast<unsigned char>(bits >> (8 * c));
            h.add(b, 8);
        }
    }
    return h.value();
}

std::uint64_t sphere_terrain_fingerprint() {
    // tools/reference/terrain.py's SPHERES and SPHERE_STAMPS.
    const Body world = body_from_json(nlohmann::json::parse(
        R"({"id": "world", "shape": "sphere_outside", "radius": 3000, "seed": 11})"));
    const Body hollow = body_from_json(nlohmann::json::parse(
        R"({"id": "hollow", "shape": "sphere_inside", "radius": 2500, "seed": 12})"));
    std::vector<TerrainStamp> stamps;
    for (const nlohmann::json& s : nlohmann::json::parse(R"([
        {"tool": "raise", "radius": 60.0, "strength": 25.0, "face": 0,
         "points": [[0.97, 0.40], [0.995, 0.41], [1.02, 0.42], [1.05, 0.43]]},
        {"tool": "lower", "radius": 30.0, "strength": 8.0, "face": 5, "points": [[0.008, 0.52]]},
        {"tool": "flatten", "radius": 45.0, "strength": 0.8, "height": 20.0, "face": 0,
         "points": [[0.99, 0.60], [1.01, 0.61]]},
        {"tool": "smooth", "radius": 50.0, "strength": 0.9, "face": 0,
         "points": [[0.985, 0.45], [1.01, 0.46]]}])"))
        stamps.push_back(stamp_from_json(s));
    Fnv1a64 all;
    const auto add = [&](std::uint64_t c) {
        unsigned char bytes[8];
        for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(c >> (8 * i));
        all.add(bytes, 8);
    };
    for (const Body& b : {world, hollow}) {
        const Terrain t(b, TerrainSettings{});
        const int last = int(t.chunks_u(2)) - 1;
        for (int face = 0; face < 6; ++face) {
            add(t.chunk_hash({face, t.min_level(), 0, 0}));
            add(t.chunk_hash({face, 2, last, 5}));
        }
    }
    const Terrain sculpted(world, TerrainSettings{}, stamps);
    const int last = int(sculpted.chunks_u(2)) - 1;
    add(sculpted.chunk_hash({0, 2, last, 6}));
    add(sculpted.chunk_hash({5, 2, 0, 6}));
    return all.value();
}

std::uint64_t placement_fingerprint() {
    TerrainSettings t;
    PlacementRule trees;
    trees.object = "obj_tree";
    trees.spacing = 9.0;
    trees.chance = 0.7;
    trees.slope_max = 25.0;
    trees.height_min = 1.0;
    trees.scale_min = 0.8;
    trees.scale_max = 1.3;
    trees.salt = "trees";
    PlacementRule rocks;
    rocks.object = "obj_rock";
    rocks.spacing = 20.0;
    rocks.chance = 0.5;
    rocks.slope_min = 5.0;
    rocks.slope_max = 70.0;
    rocks.align = 1.0;
    t.placements = {trees, rocks};
    const Terrain terrain(
        body_from_json(nlohmann::json::parse(
            R"({"id": "area", "shape": "bounded_area", "width": 3000, "length": 2000, "seed": 7})")),
        t);
    Fnv1a64 h;
    const auto add = [&](double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, 8);
        unsigned char b[8];
        for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(bits >> (8 * i));
        h.add(b, 8);
    };
    const int level = terrain.placement_level();
    for (const ChunkKey k :
         {ChunkKey{0, level, 12, 9}, ChunkKey{0, level, 13, 9}, ChunkKey{0, level, 20, 14}})
        for (const TerrainPlacement& p : terrain.placements_in(k)) {
            add(double(p.rule));
            add(p.position.x);
            add(p.position.y);
            add(p.position.z);
            add(p.up.x);
            add(p.up.y);
            add(p.up.z);
            add(p.yaw);
            add(p.scale);
        }
    return h.value();
}

std::uint64_t terrain_fingerprint() {
    // The shapes and chunks of tools/reference/terrain.py's fingerprint().
    const auto body = [](const char* json_text) {
        return body_from_json(nlohmann::json::parse(json_text));
    };
    const Body bodies[] = {
        body(R"({"id": "area", "shape": "bounded_area", "width": 3000, "length": 2000,
                 "seed": 7})"),
        body(R"({"id": "strip", "shape": "flat_cylinder", "circumference": 6000,
                 "length": 5000, "seed": 8})"),
        body(R"({"id": "habitat", "shape": "curved_cylinder", "radius": 1000, "length": 5000,
                 "seed": 9})"),
        body(R"({"id": "planet", "shape": "sphere_outside", "radius": 1000, "seed": 10})"),
    };
    Fnv1a64 all;
    for (const Body& b : bodies) {
        const Terrain t(b, TerrainSettings{});
        for (const ChunkKey& k : {ChunkKey{0, 0, 0, 0}, ChunkKey{0, 2, 3, 1}}) {
            const std::uint64_t c = t.chunk_hash(k);
            unsigned char bytes[8];
            for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(c >> (8 * i));
            all.add(bytes, 8);
        }
    }
    return all.value();
}

}  // namespace midgard
