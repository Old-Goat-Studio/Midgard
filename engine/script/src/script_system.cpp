// SPDX-License-Identifier: Apache-2.0
#include "midgard/script_system.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <glm/gtc/quaternion.hpp>

namespace py = pybind11;

namespace midgard::script {
namespace {

// What the midgard module works on while a ScriptSystem is updating.
struct Context {
    Game* game = nullptr;
    const Actions* actions = nullptr;
    float time = 0.0f;  // seconds since the scripts started
    float dt = 0.0f;
    std::function<void(const Report&)> report;
    // midgard.time.on(signal, callback), in the order they were made.
    std::vector<std::pair<TimeSignal, pybind11::object>> time_callbacks;
};
Context* g_context = nullptr;

Game& current_game() {
    if (!g_context || !g_context->game) throw std::runtime_error("midgard: no game is running");
    return *g_context->game;
}

entt::registry& registry() {
    return current_game().scene().registry;
}

[[noreturn]] void raise_reference_error(const std::string& message) {
    PyErr_SetString(PyExc_ReferenceError, message.c_str());
    throw py::error_already_set();
}

// midgard.Entity: a handle on an entity of the running scene, by id.
struct RoomsHandle {};  // midgard.rooms
struct AudioHandle {};  // midgard.audio
struct SoundHandle {    // a sound playing (midgard.audio.play)
    audio::VoiceId voice = 0;
};

// Plays a sound event for a script, from an entity (following it), a place, or everywhere:
// None when the game is silent, the event unknown (a warning in the log, once) or cooling down.
py::object play_sound(const std::string& event, float volume, entt::entity from,
                      std::optional<glm::dvec3> at) {
    const audio::VoiceId voice = current_game().play_sound(event, volume, from, at);
    return voice ? py::cast(SoundHandle{voice}) : py::none();
}
struct NavHandle {};  // midgard.nav

struct EntityHandle {
    entt::entity entity = entt::null;
    std::string id;

    bool exists() const {
        const entt::registry& r = registry();
        return r.valid(entity) && r.all_of<EntityInfo>(entity) &&
               r.get<EntityInfo>(entity).id == id;
    }
    entt::entity resolve() const {
        if (!exists()) raise_reference_error("entity '" + id + "' is no longer in the scene");
        return entity;
    }
    Transform& transform() const { return registry().get_or_emplace<Transform>(resolve()); }
};

EntityHandle handle(entt::entity e) {
    return {e, registry().get<EntityInfo>(e).id};
}

py::tuple tuple3(const glm::vec3& v) {
    return py::make_tuple(v.x, v.y, v.z);
}

py::tuple tuple3(const glm::dvec3& v) {  // positions: Python floats are doubles too
    return py::make_tuple(v.x, v.y, v.z);
}

glm::vec3 vec3(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

std::string join(const py::args& args) {
    std::string text;
    for (const py::handle& a : args) {
        if (!text.empty()) text += ' ';
        text += py::str(a).cast<std::string>();
    }
    return text;
}

void emit(const std::string& level, const std::string& text) {
    if (g_context && g_context->report) g_context->report({level, text, "", "", 0});
}

// ---- physics ------------------------------------------------------------------------------
ScenePhysics& scene_physics() {
    return current_game().physics();
}

// The entity a physics body belongs to, if it is still in the scene.
std::optional<EntityHandle> entity_of(std::uint64_t user) {
    const auto e = static_cast<entt::entity>(user);
    if (!registry().valid(e) || !registry().all_of<EntityInfo>(e)) return std::nullopt;
    return handle(e);
}

// An entity's body for forces and velocity, or a ValueError saying why it has none.
physics::BodyId moving_body(const EntityHandle& h) {
    const entt::entity e = h.resolve();
    if (e == scene_physics().player())
        throw py::value_error("the player walks as a character: move it to move it");
    const auto* rb = registry().try_get<RigidBody>(e);
    const physics::BodyId id = scene_physics().body(e);
    if (!rb || rb->motion != physics::Motion::Dynamic || id == physics::kNoBody)
        throw py::value_error("entity '" + h.id + "' has no dynamic rigid body");
    return id;
}

glm::dvec3 dvec3(const std::array<double, 3>& v) {
    return {v[0], v[1], v[2]};
}

// midgard.Hit: what a raycast met.
struct Hit {
    std::optional<EntityHandle> entity;
    py::tuple point, normal;
    double distance = 0.0;
};

std::vector<EntityHandle> entities_of(const std::vector<std::uint64_t>& users) {
    std::vector<EntityHandle> out;
    for (std::uint64_t u : users)
        if (auto h = entity_of(u)) out.push_back(*h);
    std::sort(out.begin(), out.end(),
              [](const EntityHandle& a, const EntityHandle& b) { return a.id < b.id; });
    return out;
}

physics::Touching touching(bool triggers) {
    return triggers ? physics::Touching::All : physics::Touching::Solid;
}

// ---- the camera ---------------------------------------------------------------------------
// midgard.camera: the player's camera (midgard/camera_rig.h). It holds nothing itself, so it
// always reaches the rig the game has now (an edit while playing makes a new one).
struct CameraHandle {};

CameraRig& rig() {
    return current_game().camera_rig();
}

CameraView view_named(const std::string& name) {
    if (const auto v = view_from_name(name)) return *v;
    throw py::value_error("no camera view '" + name + "' (first, third or top)");
}

// ---- time ---------------------------------------------------------------------------------
// midgard.time: the game's clock (midgard/time_service.h), and called, the seconds since the
// scripts started (API v1).
struct TimeHandle {};

// midgard.GameTime: a moment as a calendar shows it, with its names in English.
struct GameTime {
    std::int64_t year = 1;
    int month = 1, day = 1, hour = 0, minute = 0, second = 0, day_of_year = 0;
    std::string weekday, month_name, season;
    std::int64_t time = 0;  // the clock, game seconds
};

GameTime game_time(const Calendar& c, std::int64_t time) {
    const Game& game = current_game();
    const Date d = date_at(c, time);
    GameTime g{d.year, d.month, d.day, d.hour, d.minute, d.second, d.day_of_year, "", "", "", time};
    if (size_t(d.weekday) < c.weekdays.size()) g.weekday = game.text(c.weekdays[size_t(d.weekday)]);
    if (d.month >= 1 && size_t(d.month) <= c.months.size())
        g.month_name = game.text(c.months[size_t(d.month - 1)].name_key);
    if (d.season >= 0) g.season = game.text(c.seasons[size_t(d.season)].name_key);
    return g;
}

TimeSignal signal_named(const std::string& name) {
    for (TimeSignal s : {TimeSignal::Hour, TimeSignal::Day, TimeSignal::Month, TimeSignal::Season,
                         TimeSignal::Year, TimeSignal::Dawn, TimeSignal::Dusk})
        if (name == signal_name(s)) return s;
    throw py::value_error("no time signal '" + name +
                          "' (hour, day, month, season, year, dawn or dusk)");
}

template <typename Component>
std::optional<EntityHandle> first_with() {
    entt::entity found = entt::null;
    for (entt::entity e : registry().view<const Component>())
        if (found == entt::null || entt::to_integral(e) < entt::to_integral(found)) found = e;
    if (found == entt::null) return std::nullopt;
    return handle(found);
}

}  // namespace

PYBIND11_EMBEDDED_MODULE(midgard, m) {
    m.doc() = "The Midgard engine, for game scripts (API v1).";

    py::class_<EntityHandle>(m, "Entity", "An entity of the running scene.")
        .def_property_readonly("id", [](const EntityHandle& h) { return h.id; })
        .def_property(
            "name",
            [](const EntityHandle& h) { return registry().get<EntityInfo>(h.resolve()).name; },
            [](const EntityHandle& h, const std::string& name) {
                registry().get<EntityInfo>(h.resolve()).name = name;
            })
        .def_property_readonly("exists", &EntityHandle::exists)
        .def_property(
            "position", [](const EntityHandle& h) { return tuple3(h.transform().position); },
            [](const EntityHandle& h, const std::array<double, 3>& v) {
                h.transform().position = glm::dvec3(v[0], v[1], v[2]);
            })
        .def_property(
            "rotation", [](const EntityHandle& h) { return tuple3(h.transform().rotation); },
            [](const EntityHandle& h, const std::array<float, 3>& v) {
                h.transform().rotation = vec3(v);
            })
        .def_property(
            "scale", [](const EntityHandle& h) { return tuple3(h.transform().scale); },
            [](const EntityHandle& h, const std::array<float, 3>& v) {
                h.transform().scale = vec3(v);
            })
        .def_property(
            "color",
            [](const EntityHandle& h) {
                const Material* mat = registry().try_get<Material>(h.resolve());
                const glm::vec4 c = mat ? mat->color : glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
                return py::make_tuple(c.r, c.g, c.b, c.a);
            },
            [](const EntityHandle& h, const std::vector<float>& c) {
                if (c.size() != 3 && c.size() != 4)
                    throw py::value_error("color needs 3 or 4 numbers (r, g, b[, a])");
                registry().get_or_emplace<Material>(h.resolve()).color =
                    glm::vec4(c[0], c[1], c[2], c.size() == 4 ? c[3] : 1.0f);
            })
        .def(
            "move",
            [](const EntityHandle& h, double x, double y, double z) {
                h.transform().position += glm::dvec3(x, y, z);
            },
            py::arg("x"), py::arg("y"), py::arg("z"), "Moves the entity by (x, y, z) metres.")
        .def_property(
            "velocity",
            [](const EntityHandle& h) {
                const entt::entity e = h.resolve();
                ScenePhysics& p = scene_physics();
                if (e == p.player() && p.character()) return tuple3(p.character()->velocity());
                const physics::BodyId id = p.body(e);
                if (id == physics::kNoBody) return tuple3(glm::vec3(0.0f));
                return tuple3(p.world().state(id).velocity);
            },
            [](const EntityHandle& h, const std::array<float, 3>& v) {
                scene_physics().world().set_velocity(moving_body(h), vec3(v));
            })
        .def_property_readonly("on_ground",
                               [](const EntityHandle& h) {
                                   ScenePhysics& p = scene_physics();
                                   return h.resolve() == p.player() && p.character() &&
                                          p.character()->on_ground();
                               })
        .def(
            "add_force",
            [](const EntityHandle& h, float x, float y, float z) {
                scene_physics().world().add_force(moving_body(h), {x, y, z});
            },
            py::arg("x"), py::arg("y"), py::arg("z"),
            "Pushes a dynamic body with a force in newtons, for the next physics step.")
        .def(
            "add_impulse",
            [](const EntityHandle& h, float x, float y, float z) {
                scene_physics().world().add_impulse(moving_body(h), {x, y, z});
            },
            py::arg("x"), py::arg("y"), py::arg("z"),
            "Gives a dynamic body a sudden push (kg m/s): its speed changes by this over its mass.")
        .def(
            "set_animation",
            [](const EntityHandle& h, const std::string& name, float value) {
                try {
                    current_game().characters().set_parameter(h.resolve(), name, value);
                } catch (const AnimationError& e) {
                    throw py::value_error(e.what());
                }
            },
            py::arg("name"), py::arg("value"),
            "Sets a parameter of the character's animation graph (True and False are 1 and 0).")
        .def(
            "play_sound",
            [](const EntityHandle& h, const std::string& event, float volume) {
                return play_sound(event, volume, h.resolve(), std::nullopt);
            },
            py::arg("event"), py::arg("volume") = 1.0f,
            "Plays a sound event (by name or id) from this entity, following it as it moves, at "
            "volume times the event's. The Sound playing, or None (a silent game, an unknown "
            "event, one cooling down).")
        .def(
            "trigger_animation",
            [](const EntityHandle& h, const std::string& name) {
                try {
                    current_game().characters().trigger(h.resolve(), name);
                } catch (const AnimationError& e) {
                    throw py::value_error(e.what());
                }
            },
            py::arg("name"),
            "Sets a trigger of the character's animation graph (used up by the transition it "
            "fires).")
        .def(
            "animation",
            [](const EntityHandle& h, const std::string& name) {
                try {
                    return current_game().characters().parameter(h.resolve(), name);
                } catch (const AnimationError& e) {
                    throw py::value_error(e.what());
                }
            },
            py::arg("name"), "A parameter of the character's animation graph.")
        .def(
            "animation_state",
            [](const EntityHandle& h, int layer) {
                return current_game().characters().state_of(h.resolve(),
                                                            size_t(std::max(0, layer)));
            },
            py::arg("layer") = 0,
            "The state a layer of the character's animation graph is in ('' without a graph).")
        .def(
            "look_at",
            [](const EntityHandle& h, const py::object& target) {
                const entt::entity e = h.resolve();
                Characters& c = current_game().characters();
                if (target.is_none())
                    c.look_at(e, std::nullopt);
                else if (py::isinstance<EntityHandle>(target))
                    c.look_at(e, std::nullopt, target.cast<EntityHandle>().resolve());
                else
                    c.look_at(e, dvec3(target.cast<std::array<double, 3>>()));
            },
            py::arg("target"),
            "Turns the character's head to a point (x, y, z) or an entity (a character's head), "
            "within its look limit; None: back to what its component says.")
        .def(
            "reach",
            [](const EntityHandle& h, const std::string& hand, const py::object& target,
               float weight) {
                if (hand != "left" && hand != "right")
                    throw py::value_error("a hand is 'left' or 'right'");
                const entt::entity e = h.resolve();
                Characters& c = current_game().characters();
                std::optional<glm::dvec3> point;
                if (py::isinstance<EntityHandle>(target))
                    point = registry().all_of<Transform>(target.cast<EntityHandle>().resolve())
                                ? std::optional(current_game().scene().world_position(
                                      target.cast<EntityHandle>().resolve()))
                                : std::nullopt;
                else if (!target.is_none())
                    point = dvec3(target.cast<std::array<double, 3>>());
                c.reach(e, hand == "right", point, weight);
            },
            py::arg("hand"), py::arg("target"), py::arg("weight") = 1.0f,
            "Reaches a hand ('left' or 'right') to a point (x, y, z) or where an entity is, by a "
            "weight (0 to 1); None: lets it go.")
        .def("__eq__", [](const EntityHandle& a, const EntityHandle& b) { return a.id == b.id; })
        .def("__hash__", [](const EntityHandle& h) { return py::hash(py::str(h.id)); })
        .def("__repr__", [](const EntityHandle& h) {
            return "<midgard.Entity " + h.id + (h.exists() ? "" : " (gone)") + ">";
        });

    py::class_<Hit>(m, "Hit", "What a raycast met.")
        .def_readonly("entity", &Hit::entity)
        .def_readonly("point", &Hit::point)
        .def_readonly("normal", &Hit::normal)
        .def_readonly("distance", &Hit::distance)
        .def("__repr__", [](const Hit& h) {
            return "<midgard.Hit " + (h.entity ? h.entity->id : std::string("?")) + " at " +
                   std::to_string(h.distance) + " m>";
        });

    py::class_<CameraHandle>(m, "Camera", "The player's camera (midgard.camera).")
        .def_property_readonly("active", [](const CameraHandle&) { return rig().active(); })
        .def_property(
            "view", [](const CameraHandle&) { return std::string(view_name(rig().view())); },
            [](const CameraHandle&, const std::string& name) {
                rig().set_view(current_game().scene(), view_named(name));
            })
        .def(
            "set_view",
            [](const CameraHandle&, const std::string& name, bool blend) {
                rig().set_view(current_game().scene(), view_named(name), blend);
            },
            py::arg("view"), py::arg("blend") = true,
            "Changes to a view (first, third or top), blended or at once.")
        .def_property_readonly("position",
                               [](const CameraHandle&) { return tuple3(rig().pose().position); })
        .def_property_readonly("forward",
                               [](const CameraHandle&) { return tuple3(rig().pose().forward); })
        .def_property_readonly("up", [](const CameraHandle&) { return tuple3(rig().pose().up); })
        .def_property_readonly("fov", [](const CameraHandle&) { return rig().pose().fov; })
        .def_property_readonly("blending", [](const CameraHandle&) { return rig().blending(); })
        .def_property_readonly("scripted", [](const CameraHandle&) { return rig().scripted(); })
        .def(
            "shake",
            [](const CameraHandle&, float strength, float seconds) {
                if (!(strength >= 0.0f) || !(seconds >= 0.0f))
                    throw py::value_error("shake needs a strength and seconds of 0 or more");
                rig().shake(strength, seconds);
            },
            py::arg("strength") = 0.2f, py::arg("seconds") = 0.5f,
            "Shakes the camera, fading out over the seconds (strength in metres).")
        .def(
            "look",
            [](const CameraHandle&, const std::array<double, 3>& position,
               const std::array<double, 3>& target, float seconds) {
                if (dvec3(position) == dvec3(target))
                    throw py::value_error("the camera can't look at the point it is at");
                if (!(seconds >= 0.0f)) throw py::value_error("seconds must be 0 or more");
                rig().look(dvec3(position), dvec3(target), seconds);
            },
            py::arg("position"), py::arg("target"), py::arg("seconds") = 3.0f,
            "Takes the camera to look from position at target, then gives the view back.")
        .def(
            "release", [](const CameraHandle&) { rig().release(); },
            "Ends a scripted look early; the player's view comes back.")
        .def("__repr__", [](const CameraHandle&) {
            return "<midgard.camera " + std::string(view_name(rig().view())) + ">";
        });
    m.attr("camera") = CameraHandle{};

    m.def(
        "raycast",
        [](const std::array<double, 3>& origin, const std::array<double, 3>& direction,
           double distance, std::optional<EntityHandle> ignore,
           bool triggers) -> std::optional<Hit> {
            ScenePhysics& p = scene_physics();
            const physics::BodyId skip = ignore ? p.body(ignore->resolve()) : physics::kNoBody;
            const auto hit = p.world().raycast(dvec3(origin), glm::vec3(dvec3(direction)), distance,
                                               touching(triggers), skip);
            if (!hit) return std::nullopt;
            return Hit{entity_of(hit->user), tuple3(hit->point), tuple3(hit->normal),
                       hit->distance};
        },
        py::arg("origin"), py::arg("direction"), py::arg("distance") = 1000.0, py::kw_only(),
        py::arg("ignore") = py::none(), py::arg("triggers") = false,
        "The first thing a ray meets, or None.");
    m.def(
        "overlap_sphere",
        [](const std::array<double, 3>& centre, float radius, bool triggers) {
            physics::Collider sphere;
            sphere.kind = physics::Collider::Kind::Sphere;
            sphere.radius = radius;
            return entities_of(scene_physics().world().overlaps(
                sphere, dvec3(centre), glm::quat(1, 0, 0, 0), glm::vec3(1.0f), touching(triggers)));
        },
        py::arg("centre"), py::arg("radius"), py::kw_only(), py::arg("triggers") = false,
        "Every entity whose collider is inside a sphere.");
    m.def(
        "overlap_box",
        [](const std::array<double, 3>& centre, const std::array<float, 3>& size,
           const std::array<float, 3>& rotation, bool triggers) {
            physics::Collider box;
            box.size = vec3(size);
            const glm::quat turn = glm::quat_cast(glm::mat3(rotation_matrix(vec3(rotation))));
            return entities_of(scene_physics().world().overlaps(
                box, dvec3(centre), turn, glm::vec3(1.0f), touching(triggers)));
        },
        py::arg("centre"), py::arg("size"), py::arg("rotation") = std::array<float, 3>{0, 0, 0},
        py::kw_only(), py::arg("triggers") = false,
        "Every entity whose collider is inside a box (full size, turned by rotation in degrees).");
    m.def(
        "gravity",
        [](const std::array<double, 3>& position) {
            return tuple3(scene_physics().world().gravity(dvec3(position)));
        },
        py::arg("position"), "Gravity at a point (m/s^2), from the world's shape.");
    m.def(
        "up",
        [](const std::array<double, 3>& position) {
            const glm::dvec3 g = scene_physics().world().gravity(dvec3(position));
            glm::dvec3 up = glm::length(g) > 0.0 ? -g / glm::length(g) : glm::dvec3(0, 1, 0);
            for (int i = 0; i < 3; ++i) up[i] = up[i] == 0.0 ? 0.0 : up[i];  // no -0.0
            return tuple3(up);
        },
        py::arg("position"), "Which way is up at a point, from the world's shape.");

    m.def(
        "log", [](const py::args& args) { emit("info", join(args)); },
        "Writes a line to the game's log (the editor's Log panel while playing).");
    m.def(
        "warn", [](const py::args& args) { emit("warn", join(args)); },
        "Writes a warning to the game's log.");
    m.def("_emit", &emit);
    m.def(
        "entity",
        [](const std::string& id) -> std::optional<EntityHandle> {
            const entt::entity e = current_game().scene().find(id);
            if (e == entt::null) return std::nullopt;
            return handle(e);
        },
        py::arg("id"), "The entity with this id, or None.");
    m.def(
        "find",
        [](const std::string& name) -> std::optional<EntityHandle> {
            for (entt::entity e : registry().view<const EntityInfo>())
                if (registry().get<EntityInfo>(e).name == name) return handle(e);
            return std::nullopt;
        },
        py::arg("name"), "The first entity with this name, or None.");
    m.def(
        "entities",
        [] {
            std::vector<EntityHandle> out;
            for (entt::entity e : registry().view<const EntityInfo>()) out.push_back(handle(e));
            return out;
        },
        "Every entity in the scene.");
    m.def(
        "player", [] { return first_with<Player>(); }, "The entity the player controls, or None.");
    py::class_<GameTime>(m, "GameTime", "A moment as a calendar shows it.")
        .def_readonly("year", &GameTime::year)
        .def_readonly("month", &GameTime::month)
        .def_readonly("day", &GameTime::day)
        .def_readonly("hour", &GameTime::hour)
        .def_readonly("minute", &GameTime::minute)
        .def_readonly("second", &GameTime::second)
        .def_readonly("day_of_year", &GameTime::day_of_year)
        .def_readonly("weekday", &GameTime::weekday)
        .def_readonly("month_name", &GameTime::month_name)
        .def_readonly("season", &GameTime::season)
        .def_readonly("time", &GameTime::time)
        .def("__repr__", [](const GameTime& g) {
            char clock[16];
            std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d", g.hour, g.minute, g.second);
            return "GameTime(year=" + std::to_string(g.year) +
                   ", month=" + std::to_string(g.month) + ", day=" + std::to_string(g.day) + ", " +
                   clock + ", weekday=" + py::repr(py::str(g.weekday)).cast<std::string>() +
                   ", season=" + py::repr(py::str(g.season)).cast<std::string>() + ")";
        });

    py::class_<TimeHandle>(m, "Time", "The game's clock (midgard.time).")
        .def("__call__", [](const TimeHandle&) { return g_context ? g_context->time : 0.0f; })
        .def("now",
             [](const TimeHandle&) {
                 const Game& g = current_game();
                 return game_time(g.local_calendar(), g.time().now());
             })
        .def("standard",
             [](const TimeHandle&) {
                 const Game& g = current_game();
                 return game_time(g.standard_calendar(), g.time().now());
             })
        .def("season",
             [](const TimeHandle&) {
                 const Game& g = current_game();
                 return game_time(g.local_calendar(), g.time().now()).season;
             })
        .def("solar_phase",
             [](const TimeHandle&) {
                 const Game& g = current_game();
                 return solar_phase_at(g.local_calendar(), g.time().now());
             })
        .def("is_day",
             [](const TimeHandle&) {
                 const Game& g = current_game();
                 return solar_phase_at(g.local_calendar(), g.time().now()) < 0.5;
             })
        .def_property_readonly("seconds",
                               [](const TimeHandle&) { return current_game().time().now(); })
        .def_property_readonly("scale",
                               [](const TimeHandle&) { return current_game().time().scale(); })
        .def(
            "set_scale",
            [](const TimeHandle&, double scale) {
                if (!(scale >= 0.0)) throw py::value_error("time can't run backwards");
                current_game().time().set_scale(scale);
            },
            py::arg("scale"))
        .def(
            "advance",
            [](const TimeHandle&, double days, double hours, double minutes, double seconds,
               const std::string& skip) {
                const Calendar& c = current_game().local_calendar();
                const double total = days * double(c.seconds_per_day()) +
                                     hours * double(c.seconds_per_hour()) + minutes * 60.0 +
                                     seconds;
                if (!(total >= 0.0)) throw py::value_error("time can only be advanced forwards");
                const SkipPolicy policy =
                    skip == "all"      ? SkipPolicy::All
                    : skip == "latest" ? SkipPolicy::Latest
                    : skip == "none"   ? SkipPolicy::None
                                       : throw py::value_error("skip is all, latest or none");
                current_game().advance_time(std::int64_t(std::llround(total)), policy);
            },
            py::kw_only(), py::arg("days") = 0.0, py::arg("hours") = 0.0, py::arg("minutes") = 0.0,
            py::arg("seconds") = 0.0, py::arg("skip") = "all")
        .def(
            "on",
            [](const TimeHandle&, const std::string& signal, const py::object& callback) {
                if (!PyCallable_Check(callback.ptr()))
                    throw py::type_error("midgard.time.on needs a function to call");
                if (!g_context) throw std::runtime_error("midgard: no game is running");
                g_context->time_callbacks.emplace_back(signal_named(signal), callback);
            },
            py::arg("signal"), py::arg("callback"))
        .def("__repr__", [](const TimeHandle&) { return std::string("<midgard.time>"); });
    m.attr("time") = TimeHandle{};

    py::class_<RoomsHandle>(m, "Rooms",
                            "Moving between scenes (midgard.rooms; milestone 5.11a): the player "
                            "is in one scene at a time.")
        .def(
            "change",
            [](const RoomsHandle&, const std::string& scene, const std::string& arrival,
               float fade) {
                Game& g = current_game();
                if (!g.has_scene(scene))
                    throw py::value_error("there is no scene '" + scene + "' in the game");
                g.change_scene(scene, arrival, fade);
            },
            py::arg("scene"), py::arg("arrival") = "", py::arg("fade") = 0.5f,
            "Takes the player to a scene (its id), at an arrival (an entity id there; none: where "
            "the scene puts the player), fading out and in over `fade` seconds. Ignored while "
            "already fading out for another; while fading in (on_room_enter), it goes on.")
        .def(
            "take_exit",
            [](const RoomsHandle&, const EntityHandle& exit) {
                if (!current_game().take_exit(exit.resolve()))
                    throw py::value_error("entity '" + exit.id + "' is not a scene exit");
            },
            py::arg("exit"),
            "Takes a scene exit (an entity with a scene_exit), whatever its activation: the way "
            "an exit set to 'script' is taken.")
        .def_property_readonly(
            "current", [](const RoomsHandle&) { return current_game().scene().id; },
            "The scene the player is in (its id).")
        .def_property_readonly(
            "previous", [](const RoomsHandle&) { return current_game().previous_scene(); },
            "The scene the player came from (empty until the first change).")
        .def_property_readonly(
            "changing", [](const RoomsHandle&) { return current_game().changing_scene(); },
            "Whether a change is under way (fading out or in).")
        .def("__repr__", [](const RoomsHandle&) { return std::string("<midgard.rooms>"); });
    m.attr("rooms") = RoomsHandle{};

    py::class_<SoundHandle>(m, "Sound", "A sound playing (midgard.audio.play).")
        .def(
            "stop",
            [](const SoundHandle& s) {
                if (auto* sounds = current_game().sounds()) sounds->stop(s.voice);
            },
            "Stops it.")
        .def_property_readonly(
            "playing",
            [](const SoundHandle& s) {
                const auto* sounds = current_game().sounds();
                return sounds && sounds->playing(s.voice);
            },
            "Whether it's still playing (False once it has finished or been stopped).")
        .def(
            "set_volume",
            [](const SoundHandle& s, float volume) {
                if (auto* sounds = current_game().sounds()) sounds->set_volume(s.voice, volume);
            },
            py::arg("volume"), "Its volume, times the event's (1 as it started).")
        .def("__repr__", [](const SoundHandle& s) {
            return "<midgard.Sound " + std::to_string(s.voice) + ">";
        });

    const auto bus_of = [](const std::string& name) {
        const auto bus = audio::bus_from_name(name);
        if (!bus)
            throw py::value_error("bus '" + name +
                                  "' isn't master, music, effects, voice, ambience or interface");
        return *bus;
    };
    py::class_<AudioHandle>(m, "Audio", "Sound (midgard.audio; milestone 5.14).")
        .def(
            "play",
            [](const AudioHandle&, const std::string& event, float volume, py::object at) {
                if (at.is_none()) return play_sound(event, volume, entt::null, std::nullopt);
                if (py::isinstance<EntityHandle>(at))
                    return play_sound(event, volume, at.cast<EntityHandle>().resolve(),
                                      std::nullopt);
                const auto p = at.cast<std::array<double, 3>>();
                return play_sound(event, volume, entt::null, glm::dvec3(p[0], p[1], p[2]));
            },
            py::arg("event"), py::arg("volume") = 1.0f, py::arg("at") = py::none(),
            "Plays a sound event (by name or id), at volume times the event's: from an entity "
            "(following it) or a place (x, y, z) if `at` says (and the event is placed in the "
            "world), else heard the same everywhere. The Sound playing, or None (a silent game, "
            "an unknown event, one cooling down).")
        .def(
            "stop_all",
            [](const AudioHandle&) {
                if (auto* sounds = current_game().sounds()) sounds->stop_all();
            },
            "Stops every sound playing.")
        .def(
            "set_volume",
            [bus_of](const AudioHandle&, const std::string& bus, float volume) {
                const audio::Bus b = bus_of(bus);
                if (auto* sounds = current_game().sounds()) sounds->mixer().set_volume(b, volume);
            },
            py::arg("bus"), py::arg("volume"),
            "A bus's volume (linear: 0.5 is half): master, music, effects, voice, ambience or "
            "interface.")
        .def(
            "volume",
            [bus_of](const AudioHandle&, const std::string& bus) {
                const audio::Bus b = bus_of(bus);
                auto* sounds = current_game().sounds();
                return sounds ? sounds->mixer().volume(b) : 1.0f;
            },
            py::arg("bus"), "A bus's volume.")
        .def_property_readonly(
            "playing",
            [](const AudioHandle&) {
                auto* sounds = current_game().sounds();
                return sounds ? sounds->playing_events() : std::vector<std::string>();
            },
            "The sound events playing now, by id (one for each sound playing).")
        .def_property_readonly(
            "enabled", [](const AudioHandle&) { return current_game().sounds() != nullptr; },
            "Whether the game makes sound (False on a server, or without a mixer).")
        .def("__repr__", [](const AudioHandle&) { return std::string("<midgard.audio>"); });
    m.attr("audio") = AudioHandle{};

    py::class_<NavHandle>(m, "Nav",
                          "Where agents can walk (midgard.nav; milestone 5.12): paths over the "
                          "scene's navigation mesh, made round the player.")
        .def(
            "path",
            [](const NavHandle&, const std::array<double, 3>& start,
               const std::array<double, 3>& end, bool partial) -> std::optional<py::list> {
                const auto path = current_game().find_path(dvec3(start), dvec3(end));
                if (!path || (!path->complete && !partial)) return std::nullopt;
                py::list out;
                for (const glm::vec3& p : path->points) out.append(tuple3(glm::dvec3(p)));
                return out;
            },
            py::arg("start"), py::arg("end"), py::kw_only(), py::arg("partial") = false,
            "The way from one place to another as points a few metres apart on the ground (the "
            "first at the start), or None if there is none (either place too far from where "
            "agents can walk, or no way between them; with partial=True, the way as near the "
            "end as it gets).")
        .def(
            "nearest",
            [](const NavHandle&, const std::array<double, 3>& point,
               double reach) -> std::optional<py::tuple> {
                // (a path to itself: the point on the mesh nearest it)
                const auto path = current_game().find_path(dvec3(point), dvec3(point));
                if (!path || path->points.empty()) return std::nullopt;
                const glm::dvec3 at(path->points.front());
                if (glm::distance(at, dvec3(point)) > reach) return std::nullopt;
                return tuple3(at);
            },
            py::arg("point"), py::arg("reach") = 2.0,
            "The nearest place agents can walk to a point (within `reach` metres), or None.")
        .def_property_readonly(
            "ready",
            [](const NavHandle&) {
                const Game& g = current_game();
                if (const NavWorld* w = g.nav_world()) return w->stats().complete;
                return g.navmesh() != nullptr;
            },
            "Whether the navigation round the player is all made (it is made as they go).")
        .def("__repr__", [](const NavHandle&) { return std::string("<midgard.nav>"); });
    m.attr("nav") = NavHandle{};
    m.def(
        "action",
        [](const std::string& name) {
            return g_context && g_context->actions ? g_context->actions->axis(name, g_context->dt)
                                                   : 0.0f;
        },
        py::arg("name"), "An input action's value this frame (-1 to 1 for most actions).");
    m.def(
        "down",
        [](const std::string& name) {
            return g_context && g_context->actions && g_context->actions->down(name);
        },
        py::arg("name"), "True while an input action is held.");
    m.def(
        "pressed",
        [](const std::string& name) {
            return g_context && g_context->actions && g_context->actions->pressed(name);
        },
        py::arg("name"), "True in the frame an input action goes down.");
    m.def(
        "sky", [] { return tuple3(current_game().scene().settings.sky); },
        "The sky colour (r, g, b).");
    m.def(
        "set_sky",
        [](float r, float g, float b) { current_game().scene().settings.sky = {r, g, b}; },
        py::arg("r"), py::arg("g"), py::arg("b"), "Changes the sky colour.");
}

// print() in a script goes to the game's log, line by line.
constexpr const char* kRedirectOutput = R"(
import sys, midgard

class _Log:
    def __init__(self, level):
        self.level, self.pending = level, ""
    def write(self, text):
        self.pending += text
        while "\n" in self.pending:
            line, self.pending = self.pending.split("\n", 1)
            midgard._emit(self.level, line)
        return len(text)
    def flush(self):
        pass

sys.stdout, sys.stderr = _Log("info"), _Log("warn")
)";

struct ScriptSystem::Impl {
    Context context;
    std::filesystem::path scripts_dir;
    std::map<std::string, py::object> modules;
    std::set<std::string> paused;  // raised an error; waiting for a reload
    std::set<std::pair<entt::entity, std::string>> started;
    std::string reported_change_error;  // (each failed change reported once)

    // Reports a Python error from a module and pauses the module.
    void fail(const std::string& module, py::error_already_set& e, const std::string& during) {
        Report r{"error", "", module, "", 0};
        try {
            py::module_ traceback = py::module_::import("traceback");
            py::list lines = traceback.attr("format_exception")(e.type(), e.value(), e.trace());
            std::string text;
            for (const py::handle& line : lines) text += line.cast<std::string>();
            r.text = text;
            for (const py::handle& frame : traceback.attr("extract_tb")(e.trace())) {
                const std::filesystem::path file(frame.attr("filename").cast<std::string>());
                std::error_code ec;
                const auto relative = std::filesystem::relative(file, scripts_dir, ec);
                if (!ec && !relative.empty() && relative.native()[0] != '.') {
                    r.file = relative.generic_string();
                    r.line = frame.attr("lineno").cast<int>();
                }
            }
            if (r.file.empty()) {  // a syntax error has no frame of its own
                if (py::isinstance(e.value(),
                                   py::module_::import("builtins").attr("SyntaxError"))) {
                    const py::object filename = e.value().attr("filename");
                    if (!filename.is_none()) {
                        r.file =
                            std::filesystem::relative(filename.cast<std::string>(), scripts_dir)
                                .generic_string();
                        r.line = e.value().attr("lineno").cast<int>();
                    }
                }
            }
        } catch (const std::exception&) {
            r.text = e.what();
        }
        r.text = "script '" + module + "' (" + during + "): " + r.text +
                 "The script is paused until it is saved again.";
        paused.insert(module);
        if (context.report) context.report(r);
    }

    py::object load(const std::string& module) {
        if (paused.contains(module)) return py::none();
        if (auto it = modules.find(module); it != modules.end()) return it->second;
        try {
            py::object m = py::module_::import(module.c_str());
            modules.emplace(module, m);
            return m;
        } catch (py::error_already_set& e) {
            fail(module, e, "import");
            return py::none();
        }
    }

    template <typename... Args>
    void call(const std::string& module, const char* function, Args&&... args) {
        if (paused.contains(module)) return;
        const auto it = modules.find(module);
        if (it == modules.end()) return;
        try {
            py::object f = py::getattr(it->second, function, py::none());
            if (!f.is_none()) f(std::forward<Args>(args)...);
        } catch (py::error_already_set& e) {
            fail(module, e, function);
        }
    }
};

ScriptSystem::ScriptSystem(Game& game, std::function<void(const Report&)> report)
    : impl_(std::make_unique<Impl>()) {
    impl_->context.game = &game;
    impl_->context.report = std::move(report);
    impl_->scripts_dir = game.package().root / "scripts";
    g_context = &impl_->context;
    py::exec(kRedirectOutput);
    // Leaving a scene: its scripts hear on_room_exit, then stop (the next scene's start).
    game.on_leaving_scene([this](const std::string& to) {
        Impl& s = *impl_;
        g_context = &s.context;
        entt::registry& reg = s.context.game->scene().registry;
        for (const auto& [e, module] : std::vector(s.started.begin(), s.started.end()))
            if (reg.valid(e)) s.call(module, "on_room_exit", handle(e), to);
        s.started.clear();
        s.context.time_callbacks.clear();  // (made by the scene's scripts)
    });
}

ScriptSystem::~ScriptSystem() {
    impl_->context.game->on_leaving_scene(nullptr);
    if (g_context == &impl_->context) g_context = nullptr;
}

bool ScriptSystem::needed(const Game& game) {
    return !game.scene().registry.view<const ScriptRef>().empty();
}

void ScriptSystem::update(const Actions& actions, float dt) {
    Impl& s = *impl_;
    g_context = &s.context;
    s.context.actions = &actions;
    s.context.dt = dt;
    s.context.time += dt;
    entt::registry& reg = s.context.game->scene().registry;

    // Scripts that are new, or whose entity now names another module, start.
    std::vector<std::pair<entt::entity, std::string>> scripted;
    for (entt::entity e : reg.view<const ScriptRef>())
        scripted.emplace_back(e, reg.get<ScriptRef>(e).module);
    std::erase_if(s.started, [&](const auto& key) {
        return !reg.valid(key.first) || !reg.all_of<ScriptRef>(key.first) ||
               reg.get<ScriptRef>(key.first).module != key.second;
    });
    for (const auto& key : scripted) {
        if (s.started.contains(key) || s.load(key.second).is_none()) continue;
        s.started.insert(key);
        s.call(key.second, "on_start", handle(key.first));
    }
    // A new scene: its scripts (just started) hear where the player came from.
    if (s.context.game->scene_changed()) {
        const std::string from = s.context.game->previous_scene();
        for (const auto& [e, module] : std::vector(s.started.begin(), s.started.end()))
            if (reg.valid(e)) s.call(module, "on_room_enter", handle(e), from);
    }
    if (const std::string& error = s.context.game->scene_change_error();
        !error.empty() && error != s.reported_change_error) {
        s.reported_change_error = error;
        if (s.context.report) s.context.report({"warn", "midgard.rooms: " + error, "", "", 0});
    }

    for (const TriggerEvent& ev : s.context.game->trigger_events()) {
        const char* function = ev.entered ? "on_trigger_enter" : "on_trigger_exit";
        const std::array<std::pair<entt::entity, entt::entity>, 2> sides{
            std::pair{ev.trigger, ev.other}, std::pair{ev.other, ev.trigger}};
        for (const auto& [self, other] : sides) {
            if (!reg.valid(self) || !reg.valid(other) || !reg.all_of<ScriptRef>(self)) continue;
            const std::string module = reg.get<ScriptRef>(self).module;
            if (s.started.contains({self, module}))
                s.call(module, function, handle(self), handle(other));
        }
    }

    // Its character's animation events (a footstep), in the entity's own script.
    for (const Characters::Event& ev : s.context.game->characters().events()) {
        if (!reg.valid(ev.entity) || !reg.all_of<ScriptRef>(ev.entity)) continue;
        const std::string module = reg.get<ScriptRef>(ev.entity).module;
        if (s.started.contains({ev.entity, module}))
            s.call(module, "on_animation_event", handle(ev.entity), ev.name);
    }

    // The clock's signals on the player's calendar (midgard.time.on).
    for (const TimeEvent& ev : s.context.game->time_events()) {
        if (ev.calendar != 0) continue;
        for (const auto& [signal, callback] : std::vector(s.context.time_callbacks)) {
            if (signal != ev.signal) continue;
            const std::string module = py::str(py::getattr(callback, "__module__", py::str("")));
            if (s.paused.contains(module)) continue;
            try {
                callback();
            } catch (py::error_already_set& e) {
                s.fail(module, e, std::string("midgard.time.on '") + signal_name(signal) + "'");
            }
        }
    }

    for (const auto& [e, module] : std::vector(s.started.begin(), s.started.end()))
        if (reg.valid(e)) s.call(module, "on_update", handle(e), dt);
}

void ScriptSystem::reload(const std::string& module) {
    Impl& s = *impl_;
    g_context = &s.context;
    py::module_::import("importlib").attr("invalidate_caches")();
    s.paused.erase(module);
    if (auto it = s.modules.find(module); it != s.modules.end()) {
        try {
            it->second = py::module_::import("importlib").attr("reload")(it->second);
        } catch (py::error_already_set& e) {
            s.fail(module, e, "reload");
            return;
        }
    } else if (s.load(module).is_none()) {
        return;  // still broken; reported
    }
    if (const auto it = s.modules.find(module); it != s.modules.end()) {
        for (auto& [signal, callback] : s.context.time_callbacks) {
            if (py::str(py::getattr(callback, "__module__", py::str(""))).cast<std::string>() !=
                module)
                continue;
            const py::object renewed =
                py::getattr(it->second, py::getattr(callback, "__name__", py::str("")), py::none());
            if (!renewed.is_none()) callback = renewed;  // the edited function from now on
        }
    }
    if (s.context.report)
        s.context.report({"info", "Reloaded script '" + module + "'", module, "", 0});
    entt::registry& reg = s.context.game->scene().registry;
    for (const auto& [e, name] : std::vector(s.started.begin(), s.started.end()))
        if (name == module && reg.valid(e)) s.call(module, "on_reload", handle(e));
}

std::string api_names() {
    py::dict scope;
    py::exec(R"(
import json, midgard
result = json.dumps({
    "module": sorted(n for n in dir(midgard) if not n.startswith("_")),
    "Entity": sorted(n for n in dir(midgard.Entity) if not n.startswith("_")),
    "Camera": sorted(n for n in dir(midgard.Camera) if not n.startswith("_")),
    "Time": sorted(n for n in dir(midgard.Time) if not n.startswith("_")),
    "GameTime": sorted(n for n in dir(midgard.GameTime) if not n.startswith("_")),
    "Rooms": sorted(n for n in dir(midgard.Rooms) if not n.startswith("_")),
    "Nav": sorted(n for n in dir(midgard.Nav) if not n.startswith("_")),
    "Audio": sorted(n for n in dir(midgard.Audio) if not n.startswith("_")),
    "Sound": sorted(n for n in dir(midgard.Sound) if not n.startswith("_")),
})
)",
             scope);
    return scope["result"].cast<std::string>();
}

}  // namespace midgard::script
