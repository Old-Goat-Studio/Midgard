// SPDX-License-Identifier: Apache-2.0
// _midgard: the engine as a Python extension module for the editor (roadmap §2, channel 3).
// Anything that must match the game exactly is done here rather than re-implemented in Python.
//
//   import _midgard
//   _midgard.version()            -> {"engine": "0.1.0", "engine_abi": 0, ...}
//   _midgard.validate(package)    -> {"ok": bool, "errors": [...], "warnings": [...], ...}
//   _midgard.Viewport()           the editor's 3D view (see viewport.h)
//   _midgard.Body(settings_json)  a world body's shape: positions, up and gravity, exactly as
//                                 the game computes them (see midgard/shape.h)
//   _midgard.import_model(path, settings_json)
//                                 a glTF model cooked for a package: its parts as .glb bytes,
//                                 its textures as KTX2 bytes, its materials (midgard/model.h)
//   _midgard.encode_texture(path, srgb)
//                                 an image file (PNG, JPEG) as KTX2 bytes with mipmaps
#include <array>
#include <fstream>
#include <iterator>
#include <span>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <nlohmann/json.hpp>

#include "midgard/body.h"
#include "midgard/calendar.h"
#include "midgard/environment.h"
#include "midgard/gltf.h"
#include "midgard/link.h"
#include "midgard/model.h"
#include "midgard/pak.h"
#include "midgard/render/ktx.h"
#include "midgard/render/mesh_data.h"
#include "midgard/terrain.h"
#include "midgard/validate.h"
#include "midgard/version.h"
#include "viewport.h"

namespace py = pybind11;
using midgard::editor::Viewport;

namespace {

glm::vec3 vec3(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

}  // namespace

PYBIND11_MODULE(_midgard, m) {
    m.doc() = "The Midgard engine, for the Midgard editor.";

    m.def(
        "version",
        [] {
            py::dict v;
            v["engine"] = midgard::kVersion;
            v["engine_abi"] = midgard::kEngineAbi;
            v["format_major"] = midgard::kFormatMajor;
            v["schema_min"] = midgard::kSchemaMin;
            v["schema_max"] = midgard::kSchemaMax;
            v["live_link_protocol"] = midgard::link::kProtocol;
            return v;
        },
        "The engine's version and the package formats it reads.");

    m.def(
        "encode_texture",
        [](const std::filesystem::path& path, bool srgb) {
            std::vector<uint8_t> ktx2;
            {
                py::gil_scoped_release release;
                std::ifstream in(path, std::ios::binary);
                if (!in) throw midgard::AssetError("can't open " + path.filename().string());
                const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                                 std::istreambuf_iterator<char>());
                ktx2 = midgard::render::encode_ktx2(midgard::load_image(bytes, srgb));
            }
            return py::bytes(reinterpret_cast<const char*>(ktx2.data()), ktx2.size());
        },
        py::arg("path"), py::arg("srgb") = true,
        "An image file (PNG or JPEG) as KTX2 bytes with mipmaps, sRGB for colours. Raises "
        "RuntimeError for one that can't be read.");

    m.def(
        "import_model",
        [](const std::filesystem::path& path, const std::string& settings_json) {
            const nlohmann::json s = nlohmann::json::parse(settings_json);
            midgard::ImportSettings settings;
            settings.scale = s.value("scale", 1.0f);
            settings.z_up = s.value("z_up", false);
            midgard::Model model;
            std::vector<std::vector<uint8_t>> glbs, ktx2s;
            {
                py::gil_scoped_release release;  // importing and encoding take a while
                model = midgard::load_model(path, settings);
                for (const auto& part : model.parts) glbs.push_back(midgard::save_glb(part.mesh));
                for (const auto& texture : model.textures)
                    ktx2s.push_back(midgard::render::encode_ktx2(texture.data));
            }
            const auto bytes = [](const std::vector<uint8_t>& b) {
                return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
            };
            py::list parts, materials, textures;
            for (size_t i = 0; i < model.parts.size(); ++i) {
                py::dict p;
                p["name"] = model.parts[i].name;
                p["glb"] = bytes(glbs[i]);
                p["material"] = model.parts[i].material;
                parts.append(p);
            }
            for (const auto& mat : model.materials) {
                py::dict d;
                d["name"] = mat.name;
                d["color"] = std::vector<float>{mat.base_color.r, mat.base_color.g,
                                                mat.base_color.b, mat.base_color.a};
                d["metallic"] = mat.metallic;
                d["roughness"] = mat.roughness;
                d["emissive"] = std::vector<float>{mat.emissive.r, mat.emissive.g, mat.emissive.b};
                d["emissive_strength"] = mat.emissive_strength;
                d["normal_scale"] = mat.normal_scale;
                d["occlusion_strength"] = mat.occlusion_strength;
                py::dict slots;
                const std::pair<const char*, int> texture_slots[] = {
                    {"base_color", mat.base_color_texture},
                    {"metallic_roughness", mat.metallic_roughness_texture},
                    {"normal", mat.normal_texture},
                    {"occlusion", mat.occlusion_texture},
                    {"emissive", mat.emissive_texture}};
                for (const auto& [slot, index] : texture_slots)
                    if (index >= 0) slots[slot] = index;
                d["textures"] = slots;
                materials.append(d);
            }
            for (size_t i = 0; i < model.textures.size(); ++i) {
                py::dict t;
                t["name"] = model.textures[i].name;
                t["ktx2"] = bytes(ktx2s[i]);
                textures.append(t);
            }
            py::dict out;
            out["parts"] = parts;
            out["materials"] = materials;
            out["textures"] = textures;
            out["bounds"] =
                std::vector<float>{model.bounds_min.x, model.bounds_min.y, model.bounds_min.z,
                                   model.bounds_max.x, model.bounds_max.y, model.bounds_max.z};
            out["warnings"] = model.warnings;
            return out;
        },
        py::arg("path"), py::arg("settings_json") = "{}",
        "A glTF model cooked for a package: {parts: [{name, glb, material}], materials: [...], "
        "textures: [{name, ktx2}], bounds: [min xyz, max xyz], warnings}. Raises RuntimeError "
        "for a file that can't be imported.");

    m.def(
        "validate",
        [](const std::filesystem::path& package_dir) {
            midgard::ValidationReport report;
            {
                py::gil_scoped_release release;  // reading meshes can take a while
                report = midgard::validate_package(package_dir);
            }
            py::dict out;
            out["ok"] = report.ok();
            out["errors"] = report.errors;
            out["warnings"] = report.warnings;
            out["entities"] = report.entities;
            out["assets"] = report.assets;
            return out;
        },
        py::arg("package_dir"),
        "Checks a cooked package the way the runtime loads it (no window or GPU needed).");

    py::register_exception<midgard::BodyError>(m, "BodyError", PyExc_ValueError);
    const auto xyz = [](const glm::dvec3& v) { return py::make_tuple(v.x, v.y, v.z); };
    py::class_<midgard::Body>(m, "Body",
                              "A world body: its shape, size, seed and gravity (midgard/body.h).")
        .def(
            py::init([](const std::string& settings) {
                nlohmann::json j;
                try {
                    j = nlohmann::json::parse(settings);
                } catch (const nlohmann::json::exception& e) {
                    throw midgard::BodyError(std::string("body settings aren't JSON: ") + e.what());
                }
                return midgard::body_from_json(j);
            }),
            py::arg("settings_json"))
        .def_readonly("id", &midgard::Body::id)
        .def_readonly("seed", &midgard::Body::seed)
        .def_property_readonly(
            "shape",
            [](const midgard::Body& b) { return std::string(midgard::shape_name(b.shape.kind())); })
        .def_property_readonly("faces", [](const midgard::Body& b) { return b.shape.faces(); })
        .def_property_readonly("radius", [](const midgard::Body& b) { return b.shape.radius(); })
        .def_property_readonly("face_size",
                               [](const midgard::Body& b) {
                                   const glm::dvec2 s = b.shape.face_size();
                                   return py::make_tuple(s.x, s.y);
                               })
        .def(
            "position",
            [xyz](const midgard::Body& b, int face, double u, double v, double height) {
                if (face < 0 || face >= b.shape.faces())
                    throw py::value_error("no face " + std::to_string(face));
                return xyz(b.position({face, u, v, height}));
            },
            py::arg("face"), py::arg("u"), py::arg("v"), py::arg("height") = 0.0,
            "The 3D position of a surface point.")
        .def(
            "surface",
            [](const midgard::Body& b, double x, double y, double z) {
                const midgard::SurfacePoint s = b.surface({x, y, z});
                return py::make_tuple(s.face, s.u, s.v, s.height);
            },
            py::arg("x"), py::arg("y"), py::arg("z"), "The surface point (face, u, v, height).")
        .def(
            "up",
            [xyz](const midgard::Body& b, double x, double y, double z) {
                return xyz(b.up({x, y, z}));
            },
            py::arg("x"), py::arg("y"), py::arg("z"))
        .def(
            "gravity",
            [xyz](const midgard::Body& b, double x, double y, double z) {
                return xyz(b.gravity({x, y, z}));
            },
            py::arg("x"), py::arg("y"), py::arg("z"))
        .def(
            "frame",
            [xyz](const midgard::Body& b, double x, double y, double z) {
                const midgard::SurfaceFrame f = b.frame({x, y, z});
                return py::make_tuple(xyz(f.up), xyz(f.east), xyz(f.north));
            },
            py::arg("x"), py::arg("y"), py::arg("z"), "(up, east, north) at a point.");
    m.def("shape_fingerprint", &midgard::shape_fingerprint,
          "The shapes' fingerprint (equal to schema/tests/shape_fingerprint.json everywhere).");
    m.def(
        "terrain_problems",
        [](const std::string& settings_json) {
            try {
                return midgard::terrain_problems(
                    midgard::terrain_from_json(nlohmann::json::parse(settings_json)));
            } catch (const std::exception& e) {
                return std::vector<std::string>{e.what()};
            }
        },
        py::arg("settings_json"),
        "What the cook refuses in a scene's terrain settings (midgard/terrain.h).");
    m.def(
        "stamp_problems",
        [](const std::string& stamp_json) {
            try {
                return midgard::stamp_problems(
                    midgard::stamp_from_json(nlohmann::json::parse(stamp_json)));
            } catch (const std::exception& e) {
                return std::vector<std::string>{e.what()};
            }
        },
        py::arg("stamp_json"), "What the cook refuses in a terrain stamp (midgard/terrain.h).");
    m.def("terrain_fingerprint", &midgard::terrain_fingerprint,
          "The terrain's fingerprint (equal to schema/tests/terrain_chunks.json everywhere).");

    // Calendars (midgard/calendar.h), for the Calendar dock: the engine's own maths.
    py::class_<midgard::EnvironmentDef>(
        m, "Environment",
        "An environment (midgard/environment.h): curves over the solar phase, and settings.")
        .def(py::init([](const std::string& curves_json, const std::string& settings_json) {
                 try {
                     return midgard::environment_from_json("environment", "Environment",
                                                           curves_json, settings_json);
                 } catch (const std::exception& e) {
                     throw py::value_error(e.what());
                 }
             }),
             py::arg("curves_json") = "{}", py::arg("settings_json") = "{}")
        .def_static(
            "curve_names",
            []() {
                py::list out;
                for (const midgard::CurveName& n : midgard::environment_curve_names())
                    out.append(py::make_tuple(n.name, n.width));
                return out;
            },
            "The curves an environment may set: (name, 1 for a number or 3 for a colour).")
        .def(
            "problems", [](const midgard::EnvironmentDef& e) { return environment_problems(e); },
            "What the cook refuses in it.")
        .def(
            "evaluate",
            [](const midgard::EnvironmentDef& e, double phase, float sun_elevation, double season) {
                const midgard::EnvironmentLook look =
                    midgard::evaluate(e, phase, sun_elevation, season);
                const auto rgb = [](const glm::vec3& v) { return py::make_tuple(v.x, v.y, v.z); };
                const midgard::render::SkyColors& s = look.sky;
                py::dict out;
                out["zenith"] = rgb(s.zenith);
                out["horizon"] = rgb(s.horizon);
                out["horizon_away"] = rgb(s.horizon_away);
                out["ground"] = rgb(s.ground);
                out["sky_brightness"] = s.brightness;
                out["glow"] = rgb(s.glow);
                out["glow_brightness"] = s.glow_brightness;
                out["sun_disc"] = rgb(s.sun);
                out["sun_disc_brightness"] = s.sun_brightness;
                out["sunlight"] = rgb(look.sunlight);
                out["sunlight_intensity"] = look.sunlight_intensity;
                out["ambient"] = look.ambient ? py::object(rgb(*look.ambient)) : py::none();
                out["ambient_ground"] =
                    look.ambient_ground ? py::object(rgb(*look.ambient_ground)) : py::none();
                out["fog_density"] = look.fog_density;
                out["fog_color"] = look.fog_color ? py::object(rgb(*look.fog_color)) : py::none();
                out["ambient_tint"] = rgb(look.ambient_tint);
                out["water_shallow"] = rgb(look.water.shallow);
                out["water_deep"] = rgb(look.water.deep);
                out["water_clarity"] = look.water.clarity;
                return out;
            },
            py::arg("phase"), py::arg("sun_elevation"), py::arg("season") = 0.0,
            "Every curve's value at a solar phase, with the sun this high (radians), this far into "
            "summer (1) or winter (-1): those not set from the analytic sky (ambient and fog "
            "colour: None, the scene's own), with the season's tint applied (ambient_tint: for "
            "the ambient light).")
        .def(
            "season_tint",
            [](const midgard::EnvironmentDef& e, double season) {
                const midgard::SeasonTint t = midgard::season_tint(e, season);
                const auto rgb = [](const glm::vec3& v) { return py::make_tuple(v.x, v.y, v.z); };
                py::dict out;
                out["sunlight"] = rgb(t.sunlight);
                out["ambient"] = rgb(t.ambient);
                out["sky"] = rgb(t.sky);
                return out;
            },
            py::arg("season"), "The season's tints: 1 summer, -1 winter, 0 none.");

    py::class_<midgard::Calendar>(m, "Calendar",
                                  "A calendar, from JSON with the calendar table's columns and "
                                  "months, weekdays, seasons and moons.")
        .def(py::init([](const std::string& json_text) {
                 const nlohmann::json j = nlohmann::json::parse(json_text, nullptr, false);
                 if (!j.is_object()) throw py::value_error("a calendar is a JSON object");
                 return midgard::calendar_from_json(j);
             }),
             py::arg("json"))
        .def_property_readonly("seconds_per_day", &midgard::Calendar::seconds_per_day)
        .def_property_readonly("seconds_per_hour", &midgard::Calendar::seconds_per_hour)
        .def_property_readonly("hours_per_day",
                               [](const midgard::Calendar& c) { return c.hours_per_day; })
        .def_property_readonly("months",
                               [](const midgard::Calendar& c) {
                                   std::vector<int> days;
                                   for (const auto& month : c.months) days.push_back(month.days);
                                   return days;
                               })
        .def_property_readonly("tilt", [](const midgard::Calendar& c) { return c.tilt; })
        .def_property_readonly("time_scale", &midgard::Calendar::time_scale)
        .def("year_days", &midgard::Calendar::year_days, py::arg("year"))
        .def("problems", &midgard::Calendar::problems, "The cook checks' complaints.")
        .def("start_time",
             [](const midgard::Calendar& c) {
                 try {
                     return c.start_time();
                 } catch (const midgard::CalendarError& e) {
                     throw py::value_error(e.what());
                 }
             })
        .def(
            "date",
            [](const midgard::Calendar& c, std::int64_t time) {
                try {
                    const midgard::Date d = midgard::date_at(c, time);
                    py::dict out;
                    out["year"] = d.year;
                    out["month"] = d.month;
                    out["day"] = d.day;
                    out["hour"] = d.hour;
                    out["minute"] = d.minute;
                    out["second"] = d.second;
                    out["weekday"] = d.weekday;
                    out["day_of_year"] = d.day_of_year;
                    out["season"] = d.season;
                    out["seconds_into_day"] = d.seconds_into_day;
                    return out;
                } catch (const midgard::CalendarError& e) {
                    throw py::value_error(e.what());
                }
            },
            py::arg("time"), "The date at a moment: a dict of year, month, day, hour, ...")
        .def(
            "time_of",
            [](const midgard::Calendar& c, std::int64_t year, int month, int day,
               std::int64_t seconds) {
                try {
                    return midgard::time_of(c, year, month, day, seconds);
                } catch (const midgard::CalendarError& e) {
                    throw py::value_error(e.what());
                }
            },
            py::arg("year"), py::arg("month"), py::arg("day"), py::arg("seconds_into_day") = 0)
        .def("season_on", &midgard::season_on, py::arg("year"), py::arg("day_of_year"))
        .def("daylight", &midgard::daylight_on, py::arg("year"), py::arg("day_of_year"),
             "The day's share of daylight (0.01 to 0.99).")
        .def("summer", &midgard::summer_on, py::arg("year"), py::arg("day_of_year"),
             "How far into summer: 1 on the longest day, -1 on the shortest.")
        .def(
            "solar_phase",
            [](const midgard::Calendar& c, std::int64_t year, int day_of_year, double seconds) {
                return midgard::solar_phase(seconds, double(c.seconds_per_day()),
                                            midgard::daylight_on(c, year, day_of_year),
                                            c.solar_noon);
            },
            py::arg("year"), py::arg("day_of_year"), py::arg("seconds_into_day"),
            "0 sunrise, 0.25 noon, 0.5 sunset, back to 1.")
        .def(
            "sun_times",
            [](const midgard::Calendar& c, std::int64_t time) {
                const midgard::SunTimes s = midgard::sun_times(c, time);
                return py::make_tuple(s.sunrise, s.sunset);
            },
            py::arg("time"), "(sunrise, sunset) in seconds into the day of the moment.")
        .def(
            "roll",
            [](const midgard::Calendar& c, std::uint64_t seed) {
                return midgard::roll_body_calendar(c, seed);
            },
            py::arg("seed"), "A body's calendar rolled from this one's ranges and a seed.");
    m.def(
        "calendar_problems",
        [](const std::vector<std::string>& calendars) {
            std::vector<midgard::Calendar> all;
            for (const std::string& j : calendars)
                all.push_back(midgard::calendar_from_json(nlohmann::json::parse(j)));
            return midgard::calendar_problems(all);
        },
        py::arg("calendars"),
        "The cook checks of a project's calendars together (JSON each): each calendar's, and "
        "that exactly one is the standard calendar.");

    py::class_<Viewport>(m, "Viewport", "The editor's 3D view of the scene being edited.")
        .def(py::init<>())
        .def("put_entity", &Viewport::put_entity, py::arg("entity_json"),
             "Adds or replaces an entity: JSON {id, name, parent, components}.")
        .def("remove_entity", &Viewport::remove_entity, py::arg("entity_id"))
        .def("clear", &Viewport::clear, "Removes every entity.")
        .def("set_settings", &Viewport::set_settings, py::arg("settings_json"),
             "The scene settings: JSON {sky, ambient, body}.")
        .def_property("show_colliders", &Viewport::show_colliders, &Viewport::set_show_colliders,
                      "Outlines of colliders, triggers and the player over the scene.")
        .def(
            "set_mesh",
            [](Viewport& v, const std::string& asset, const py::bytes& glb) {
                const std::string_view data = glb;
                v.set_mesh(asset,
                           std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
            },
            py::arg("asset"), py::arg("glb"), "A mesh (glTF binary) for a mesh asset id.")
        .def(
            "set_texture",
            [](Viewport& v, const std::string& texture, const py::bytes& ktx2) {
                const std::string_view data = ktx2;
                v.set_texture(
                    texture, std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
            },
            py::arg("texture"), py::arg("ktx2"), "A KTX2 texture for a texture content id.")
        .def("set_object", &Viewport::set_object, py::arg("object_id"), py::arg("parts_json"),
             "An object's template (JSON list of {name, components}); its instances follow.")
        .def("set_environment", &Viewport::set_environment, py::arg("id"), py::arg("name"),
             py::arg("curves_json"), py::arg("settings_json"),
             "An environment (a row of the environment table) for the Sky to link to.")
        .def(
            "set_camera",
            [](Viewport& v, const std::array<float, 3>& eye, const std::array<float, 3>& target,
               float fov) { v.set_camera(vec3(eye), vec3(target), fov); },
            py::arg("eye"), py::arg("target"), py::arg("fov") = 55.0f)
        .def_property(
            "solar_phase", &Viewport::solar_phase, &Viewport::set_solar_phase,
            "The time of day for suns that follow the clock (0 sunrise, 0.25 noon, 0.5 sunset), "
            "or None: suns stay where they were placed.")
        .def(
            "set_terrain_stamps",
            [](Viewport& v, const std::string& stamps_json) {
                try {
                    v.set_terrain_stamps(stamps_json);
                } catch (const std::exception& e) {
                    throw py::value_error(e.what());
                }
            },
            py::arg("stamps_json"), "The terrain's sculpting and painting: a JSON list of stamps.")
        .def(
            "terrain_ground",
            [](const Viewport& v, const std::array<double, 3>& p) -> std::optional<py::tuple> {
                const auto g = v.terrain_ground({p[0], p[1], p[2]});
                if (!g) return std::nullopt;
                return py::make_tuple(g->above,
                                      py::make_tuple(g->ground.x, g->ground.y, g->ground.z),
                                      py::make_tuple(g->up.x, g->up.y, g->up.z));
            },
            py::arg("point"),
            "The terrain at a point: (metres above the ground, the ground under it, up there), or "
            "None without terrain.")
        .def(
            "terrain_hit",
            [](const Viewport& v, const std::array<double, 3>& origin,
               const std::array<double, 3>& direction, double distance,
               std::optional<int> face) -> std::optional<py::tuple> {
                const auto hit =
                    v.terrain_hit({origin[0], origin[1], origin[2]},
                                  {direction[0], direction[1], direction[2]}, distance, face);
                if (!hit) return std::nullopt;
                return py::make_tuple(hit->face, hit->u, hit->v,
                                      py::make_tuple(hit->point.x, hit->point.y, hit->point.z),
                                      hit->height);
            },
            py::arg("origin"), py::arg("direction"), py::arg("distance") = 5000.0,
            py::arg("face") = py::none(),
            "Where a ray first meets the terrain: (face, u, v, (x, y, z), height), or None. On "
            "a sphere, `face` gives u and v in that face's terms, carried on past its edges.")
        .def_property("season", &Viewport::season, &Viewport::set_season,
                      "How far into summer (1) or winter (-1), for the environment's season "
                      "tints.")
        .def(
            "preview_camera",
            [](Viewport& v, const std::string& view) -> std::optional<py::tuple> {
                const auto which = midgard::view_from_name(view);
                if (!which) throw py::value_error("no camera view '" + view + "'");
                const auto pose = v.preview_camera(*which);
                if (!pose) return std::nullopt;
                const auto list = [](const auto& p) {
                    return std::array{double(p.x), double(p.y), double(p.z)};
                };
                return py::make_tuple(list(pose->position), list(pose->forward), list(pose->up),
                                      pose->fov);
            },
            py::arg("view"),
            "Where the game's camera would be in a view (first, third or top): (eye, forward, "
            "up, fov), or None without a player and a camera. Walls don't stop it here.")
        .def("attach_window", &Viewport::attach_window, py::arg("platform"), py::arg("handle"),
             py::arg("width"), py::arg("height"),
             "Draws into a native window: platform 'windows' (HWND) or 'xcb' (X11 window id).")
        .def("attach_offscreen", &Viewport::attach_offscreen, py::arg("width"), py::arg("height"))
        .def("detach", &Viewport::detach, py::call_guard<py::gil_scoped_release>(),
             "Lets go of the window (before the window system destroys it).")
        .def_property_readonly("attached", &Viewport::attached)
        .def("resize", &Viewport::resize, py::arg("width"), py::arg("height"))
        .def("render", &Viewport::render, py::call_guard<py::gil_scoped_release>(),
             "Draws one frame. False if there is nothing to draw into.")
        .def(
            "read_pixels",
            [](Viewport& v) {
                const std::vector<uint8_t> pixels = v.read_pixels();
                return py::bytes(reinterpret_cast<const char*>(pixels.data()), pixels.size());
            },
            "Offscreen only: the last frame as RGBA8, top row first.")
        .def_property_readonly("device_name", &Viewport::device_name)
        .def_property_readonly("entity_count", &Viewport::entity_count);
}
