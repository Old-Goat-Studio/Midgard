// SPDX-License-Identifier: Apache-2.0
#include "midgard/world_type.h"

#include "midgard/hash.h"
#include "midgard/worldgen.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

namespace midgard {

namespace {

constexpr Shape::Kind kKinds[] = {Shape::Kind::BoundedArea, Shape::Kind::FlatCylinder,
                                  Shape::Kind::CurvedCylinder, Shape::Kind::SphereOutside,
                                  Shape::Kind::SphereInside};

std::optional<Shape::Kind> kind_named(const std::string& name) {
    for (const Shape::Kind k : kKinds)
        if (name == shape_name(k)) return k;
    return std::nullopt;
}

}  // namespace

double world_size(const Body& body) {
    switch (body.shape.kind()) {
        case Shape::Kind::BoundedArea:
        case Shape::Kind::FlatCylinder:
            return body.shape.face_size().x;
        default:
            return body.shape.radius();
    }
}

WorldRecipe recipe_from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("a world type's recipe is a JSON object");
    WorldRecipe r;
    if (j.contains("shapes")) {
        if (!j.at("shapes").is_array())
            throw std::runtime_error("world type: \"shapes\" is a list of shape names");
        for (const nlohmann::json& s : j.at("shapes")) {
            const auto kind = s.is_string() ? kind_named(s.get<std::string>()) : std::nullopt;
            if (!kind) throw std::runtime_error("world type: no shape named " + s.dump());
            r.shapes.push_back(*kind);
        }
    }
    if (j.contains("size")) {
        const nlohmann::json& s = j.at("size");
        if (!s.is_array() || s.size() != 2 || !s[0].is_number() || !s[1].is_number())
            throw std::runtime_error("world type: \"size\" is [smallest, largest] in metres");
        r.size_min = s[0].get<double>();
        r.size_max = s[1].get<double>();
    }
    if (j.contains("gravity")) {
        if (!j.at("gravity").is_number())
            throw std::runtime_error("world type: \"gravity\" is a number (m/s^2)");
        r.gravity = j.at("gravity").get<double>();
    }
    const auto text = [&](const char* key, std::string& into) {
        if (!j.contains(key)) return;
        if (!j.at(key).is_string())
            throw std::runtime_error(std::string("world type: \"") + key + "\" is an id");
        into = j.at(key).get<std::string>();
    };
    text("atmosphere", r.atmosphere);
    text("calendar", r.calendar);
    if (j.contains("graph") && !j.at("graph").is_null()) r.graph = j.at("graph").dump();
    if (j.contains("terrain")) {
        if (j.at("terrain").is_object() && j.at("terrain").contains("seed"))
            throw std::runtime_error("world type: the seed is the world's, not the recipe's");
        r.terrain = terrain_from_json(j.at("terrain"));
    }
    return r;
}

nlohmann::json recipe_to_json(const WorldRecipe& r) {
    nlohmann::json j = nlohmann::json::object();
    if (!r.shapes.empty()) {
        j["shapes"] = nlohmann::json::array();
        for (const Shape::Kind k : r.shapes) j["shapes"].push_back(shape_name(k));
    }
    if (r.size_min && r.size_max) j["size"] = {*r.size_min, *r.size_max};
    if (r.gravity) j["gravity"] = *r.gravity;
    if (!r.atmosphere.empty()) j["atmosphere"] = r.atmosphere;
    if (!r.calendar.empty()) j["calendar"] = r.calendar;
    j["terrain"] = terrain_to_json(r.terrain);
    if (!r.graph.empty()) j["graph"] = nlohmann::json::parse(r.graph);
    return j;
}

std::vector<std::string> recipe_problems(const WorldRecipe& r) {
    std::vector<std::string> out;
    if (r.size_min && r.size_max &&
        !(*r.size_min > 0.0 && *r.size_min <= *r.size_max && std::isfinite(*r.size_max)))
        out.push_back("world type: the size range is from more than 0 up to at least as much");
    if (r.gravity && !(*r.gravity > 0.0 && *r.gravity <= 100.0))
        out.push_back("world type: gravity is above 0 and at most 100 m/s^2");
    for (std::string& p : terrain_problems(r.terrain)) out.push_back(std::move(p));
    if (!r.graph.empty())
        for (const std::string& p : graph_problems(nlohmann::json::parse(r.graph)))
            out.push_back("world type's graph: " + p);
    return out;
}

std::vector<std::string> recipe_misfits(const WorldRecipe& r, const Body& body) {
    std::vector<std::string> out;
    const Shape::Kind kind = body.shape.kind();
    bool suits = r.shapes.empty();
    for (const Shape::Kind k : r.shapes) suits = suits || k == kind;
    if (!suits)
        out.push_back(std::string("the world type isn't for a ") + shape_name(kind) +
                      " (its shapes are set in the World Type editor)");
    const double size = world_size(body);
    if (r.size_min && r.size_max && (size < *r.size_min || size > *r.size_max))
        out.push_back("the world's size, " + std::to_string(int(std::lround(size))) +
                      " m, is outside the world type's " +
                      std::to_string(int(std::lround(*r.size_min))) + " to " +
                      std::to_string(int(std::lround(*r.size_max))) + " m");
    return out;
}

bool is_world_ref(const nlohmann::json& j) {
    return j.is_object() && j.contains("world_type");
}

WorldRef world_ref_from_json(const nlohmann::json& j) {
    if (!is_world_ref(j) || !j.at("world_type").is_string())
        throw std::runtime_error("terrain: \"world_type\" is a world type's id");
    WorldRef r;
    r.world_type = j.at("world_type").get<std::string>();
    if (j.contains("version")) {
        if (!j.at("version").is_number_integer() || j.at("version").get<int>() < 1)
            throw std::runtime_error("terrain: \"version\" is a whole number from 1");
        r.version = j.at("version").get<int>();
    }
    if (j.contains("seed")) {
        if (!j.at("seed").is_number_integer())
            throw std::runtime_error("terrain: the seed is a whole number");
        r.seed = j.at("seed").get<std::uint64_t>();
    }
    for (const auto& [key, value] : j.items())
        if (key != "world_type" && key != "version" && key != "seed")
            throw std::runtime_error("terrain: \"" + key +
                                     "\" belongs in the world type's recipe, not the scene");
    return r;
}

nlohmann::json world_ref_to_json(const WorldRef& r) {
    nlohmann::json j = {{"world_type", r.world_type}, {"version", r.version}};
    if (r.seed) j["seed"] = *r.seed;
    return j;
}

std::vector<WorldTypeVersion> load_world_type_versions(const std::filesystem::path& database) {
    sqlite3* db = nullptr;
    const std::u8string u8 = database.u8string();
    if (sqlite3_open_v2(std::string(u8.begin(), u8.end()).c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("can't open " + database.string());
    }
    std::vector<WorldTypeVersion> out;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT world_type, version, recipe, shipped FROM world_type_version "
                           "ORDER BY world_type, version",
                           -1, &s, nullptr) == SQLITE_OK) {  // (none before migration 12)
        while (sqlite3_step(s) == SQLITE_ROW) {
            WorldTypeVersion v;
            v.world_type = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
            v.version = sqlite3_column_int(s, 1);
            v.recipe = reinterpret_cast<const char*>(sqlite3_column_text(s, 2));
            v.shipped = sqlite3_column_int(s, 3) != 0;
            out.push_back(std::move(v));
        }
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    return out;
}

std::vector<std::string> world_type_problems(const std::vector<WorldTypeVersion>& versions) {
    std::vector<std::string> out;
    for (const WorldTypeVersion& v : versions) {
        const std::string who =
            "world type '" + v.world_type + "' version " + std::to_string(v.version) + ": ";
        try {
            for (const std::string& p :
                 recipe_problems(recipe_from_json(nlohmann::json::parse(v.recipe))))
                out.push_back(who + p);
        } catch (const std::exception& e) {
            out.push_back(who + e.what());
        }
    }
    return out;
}

TerrainSettings world_terrain(const WorldRecipe& recipe, const WorldRef& world) {
    TerrainSettings t = recipe.terrain;
    t.seed = world.seed;
    if (!recipe.graph.empty()) t.graph = recipe.graph;
    return t;
}

namespace {

void add_number(Fnv1a64& h, double value) {
    value = value == 0.0 ? 0.0 : value;  // -0 as 0
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, 8);
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(bits >> (8 * i));
    h.add(b, 8);
}

void add_bits(Fnv1a64& h, std::uint64_t bits) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(bits >> (8 * i));
    h.add(b, 8);
}

}  // namespace

std::uint64_t world_hash(const Terrain& terrain) {
    const Shape::Kind kind = terrain.body().shape.kind();
    const int faces =
        (kind == Shape::Kind::SphereOutside || kind == Shape::Kind::SphereInside) ? 6 : 1;
    Fnv1a64 h;
    add_number(h, terrain.settings().sea_level);  // where the water is
    for (int face = 0; face < faces; ++face) {
        const int low = terrain.min_level();
        const int last_u = int(terrain.chunks_u(low)) - 1, last_v = int(terrain.chunks_v(low)) - 1;
        add_bits(h, terrain.chunk_hash({face, low, 0, 0}));
        add_bits(h, terrain.chunk_hash({face, low, last_u, last_v}));
        add_bits(h, terrain.chunk_hash(
                        {face, 2, int(terrain.chunks_u(2) / 2), int(terrain.chunks_v(2) / 2)}));
        for (int j = 0; j <= 16; ++j)
            for (int i = 0; i <= 16; ++i) {
                const double u = (i + 0.5) / 17.5, v = (j + 0.5) / 17.5;
                const double height = terrain.height(face, u, v);
                add_number(h, terrain.temperature_at(face, u, v, height));
                add_number(h, double(terrain.biome_index(face, u, v, height)));
            }
    }
    const int level = terrain.placement_level();
    const int mx = int(terrain.chunks_u(level) / 2), my = int(terrain.chunks_v(level) / 2);
    for (const ChunkKey tile : {ChunkKey{0, level, mx, my}, ChunkKey{0, level, mx + 1, my},
                                ChunkKey{0, level, mx, my + 1}})
        for (const TerrainPlacement& p : terrain.placements_in(tile)) {
            add_number(h, double(p.rule));
            for (const double x :
                 {p.position.x, p.position.y, p.position.z, p.up.x, p.up.y, p.up.z, p.yaw, p.scale})
                add_number(h, x);
        }
    return h.value();
}

const std::vector<SampleWorldType>& sample_world_types() {
    static const std::vector<SampleWorldType> samples = {
        {"wt_sample_temperate_planet", "Temperate Planet",
         R"({"shapes": ["sphere_outside"], "size": [1000, 20000],
             "terrain": {"sea_level": 0, "snow_line": 400,
               "climate": {"temperature": 16, "latitude_cooling": 45, "lapse_rate": 6.5,
                           "variation": 3, "season_swing": 10},
               "biomes": [
                 {"name": "Tundra", "temperature": [-100, 2], "moisture": [0, 1],
                  "color": [0.42, 0.45, 0.36], "dry_color": [0.55, 0.53, 0.45]},
                 {"name": "Desert", "temperature": [22, 100], "moisture": [0, 0.4],
                  "color": [0.70, 0.62, 0.40], "dry_color": [0.80, 0.70, 0.45]},
                 {"name": "Forest", "temperature": [2, 22], "moisture": [0.5, 1],
                  "color": [0.16, 0.32, 0.12], "dry_color": [0.30, 0.38, 0.18]},
                 {"name": "Grassland", "temperature": [2, 100], "moisture": [0, 1],
                  "color": [0.32, 0.48, 0.18], "dry_color": [0.58, 0.54, 0.30]}]},
             "graph": {"output": "height", "nodes": {
               "continents": {"op": "noise", "size": 1500, "octaves": 5, "salt": "continents"},
               "mountains": {"op": "ridged", "size": 600, "octaves": 5, "salt": "mountains"},
               "land": {"op": "smoothstep", "in": ["continents"], "from": -0.1, "to": 0.4},
               "lowland": {"op": "multiply", "in": [70, "continents"]},
               "peaks": {"op": "multiply", "in": [90, "land", "mountains"]},
               "height": {"op": "add", "in": [-10, "lowland", "peaks"]}}}})",
         R"({"id": "planet", "shape": "sphere_outside", "radius": 2000, "seed": 21})"},
        {"wt_sample_habitat", "Habitat Farmland",
         R"({"shapes": ["curved_cylinder", "flat_cylinder"],
             "terrain": {"sea_level": 0, "wall": "barrier", "end_ramp": 300,
                         "wall_height": 150},
             "graph": {"output": "height", "nodes": {
               "hills": {"op": "noise", "size": 400, "octaves": 4, "salt": "fields"},
               "scaled": {"op": "multiply", "in": [25, "hills"]},
               "fields": {"op": "terrace", "in": ["scaled"], "step": 4, "sharpness": 0.6},
               "end": {"op": "input", "name": "end_distance"},
               "away": {"op": "smoothstep", "in": ["end"], "from": 0, "to": 600},
               "shaped": {"op": "multiply", "in": ["fields", "away"]},
               "height": {"op": "add", "in": [6, "shaped"]}}}})",
         R"({"id": "habitat", "shape": "curved_cylinder", "radius": 1000, "length": 5000,
             "seed": 22})"},
        {"wt_sample_archipelago", "Archipelago",
         R"({"shapes": ["bounded_area"],
             "terrain": {"sea_level": 0, "ocean_band": 300, "ocean_depth": 40},
             "graph": {"output": "height", "nodes": {
               "islands": {"op": "ridged", "size": 700, "octaves": 5, "salt": "islands"},
               "edge": {"op": "input", "name": "edge_distance"},
               "inland": {"op": "smoothstep", "in": ["edge"], "from": 100, "to": 900},
               "land": {"op": "multiply", "in": [60, "islands", "inland"]},
               "height": {"op": "add", "in": [-20, "land"]}}}})",
         R"({"id": "area", "shape": "bounded_area", "width": 3000, "length": 3000,
             "seed": 23})"},
        {"wt_sample_hollow", "Hollow World",
         R"({"shapes": ["sphere_inside"],
             "terrain": {"sea_level": 0,
               "climate": {"temperature": 18, "latitude_cooling": 10, "lapse_rate": 6.5,
                           "variation": 2, "season_swing": 0}},
             "graph": {"output": "height", "nodes": {
               "base": {"op": "noise", "size": 800, "octaves": 5, "salt": "hollow"},
               "scaled": {"op": "multiply", "in": [50, "base"]},
               "smoothed": {"op": "smooth", "in": ["scaled"], "radius": 15},
               "height": {"op": "add", "in": [8, "smoothed"]}}}})",
         R"({"id": "hollow", "shape": "sphere_inside", "radius": 2500, "seed": 24})"},
    };
    return samples;
}

std::uint64_t sample_world_types_fingerprint() {
    Fnv1a64 all;
    for (const SampleWorldType& s : sample_world_types()) {
        const nlohmann::json body = nlohmann::json::parse(s.body);
        WorldRef world;
        world.world_type = s.id;
        world.seed = body.at("seed").get<std::uint64_t>();
        const Terrain t(body_from_json(body),
                        world_terrain(recipe_from_json(nlohmann::json::parse(s.recipe)), world));
        add_bits(all, world_hash(t));
    }
    return all.value();
}

}  // namespace midgard
