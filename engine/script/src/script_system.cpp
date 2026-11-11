// SPDX-License-Identifier: Apache-2.0
#include "midgard/script_system.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
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
    // midgard.weather.on_change(callback), likewise.
    std::vector<pybind11::object> weather_callbacks;
    // midgard.stats.on_change(callback), likewise.
    std::vector<pybind11::object> stat_callbacks;
    // midgard.ui.timer(..., on_done=callback): a timer's id -> its callback.
    std::map<std::string, pybind11::object> timer_callbacks;
    // midgard.ui.on_click(screen, element, callback) and on_change: by "screen/element".
    std::map<std::string, std::vector<pybind11::object>> click_callbacks, change_callbacks;
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
struct RoomsHandle {};     // midgard.rooms
struct AudioHandle {};     // midgard.audio
struct MusicHandle {};     // midgard.music
struct AmbienceHandle {};  // midgard.ambience

// midgard.stats (milestone 6.0): the player's stats, scores and counters.
struct StatsHandle {};
// midgard.ui (milestone 6.0): the HUD's notes, prompt, timers and switches.
struct UiHandle {};

const stats::StatDef& stat_def(const std::string& stat) {
    const stats::StatDef* d = current_game().stats().find(stat);
    if (!d) throw py::key_error("there is no stat '" + stat + "'");
    if (d->display())
        throw py::key_error("'" + d->name + "' only shows on the HUD (the " +
                            stats::kind_name(d->kind) + "): it has no value to read or change");
    return *d;
}

// A screen's element, for midgard.ui (KeyError if there's no such screen or element, or
// ValueError if it isn't of the kind needed: "" for any).
const ScreenElement& check_element(const std::string& screen, const std::string& element,
                                   const std::string& kind) {
    const ScreenDef* s = current_game().screen(screen);
    if (!s) throw py::key_error("there is no screen '" + screen + "'");
    const ScreenElement* e = s->find(element);
    if (!e) throw py::key_error("screen '" + screen + "' has no element '" + element + "'");
    if (!kind.empty() && e->kind != kind)
        throw py::value_error("'" + element + "' on screen '" + screen + "' is a " + e->kind +
                              ", not a " + kind);
    return *e;
}

// A stat's value as Python has it: an int for scores and counters.
py::object stat_value(const stats::StatDef& d, double v) {
    if (d.whole()) return py::int_(static_cast<long long>(v));
    return py::float_(v);
}

// midgard.weather's view of the weather.
struct WeatherHandle {};
struct ScriptWeather {
    std::string id, name, falls = "none";
    bool forced = false, front = false;
    float clouds = 0, darkness = 0, visibility = 0, amount = 0, lightning = 0, wind = 0, gusts = 0,
          wetness = 0, temperature = 0;
};
struct ScriptSpell {
    std::string id, name;
    std::int64_t start = 0, end = 0;
    double in_hours = 0.0, hours = 0.0;
    bool forced = false, front = false;
};

std::string weather_name(const Game& game, const std::string& id) {
    const auto it = game.scene().weather_types.find(id);
    return it == game.scene().weather_types.end() ? std::string() : it->second.name;
}

ScriptWeather script_weather(const Game& game, const weather::Now& now) {
    ScriptWeather w;
    w.id = now.weather;
    w.forced = now.forced;
    w.front = now.front >= 0;
    const auto it = game.scene().weather_types.find(now.weather);
    if (it == game.scene().weather_types.end()) return w;
    const weather::WeatherType& t = it->second;
    w.name = t.name;
    w.falls = weather::falls_name(t.falls);
    w.clouds = t.clouds;
    w.darkness = t.darkness;
    w.visibility = t.visibility;
    w.amount = t.amount;
    w.lightning = t.lightning;
    w.wind = t.wind;
    w.gusts = t.gusts;
    w.wetness = t.wetness;
    w.temperature = t.temperature;
    return w;
}

ScriptWeather script_weather(const Game& game) {
    return script_weather(game, game.weather_now());
}
struct SoundHandle {  // a sound playing (midgard.audio.play)
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
            "flying",
            [](const EntityHandle& h) {
                const Player* p = registry().try_get<Player>(h.resolve());
                return p != nullptr && p->fly;
            },
            [](const EntityHandle& h, bool fly) {
                const entt::entity e = h.resolve();
                if (!registry().all_of<Player>(e))
                    throw py::value_error("entity '" + h.id + "' isn't the player: only it flies");
                current_game().set_flying(e, fly);
            },
            "The player flies (a jetpack, a ship): where the camera looks, up with jump, down "
            "with descend, with no gravity; False: it walks again. Only the player.")
        .def_property(
            "speed",
            [](const EntityHandle& h) {
                const Player* p = registry().try_get<Player>(h.resolve());
                if (p == nullptr) throw py::value_error("entity '" + h.id + "' isn't the player");
                return p->speed;
            },
            [](const EntityHandle& h, float speed) {
                Player* p = registry().try_get<Player>(h.resolve());
                if (p == nullptr) throw py::value_error("entity '" + h.id + "' isn't the player");
                if (!(speed > 0.0f) || speed > 1000.0f)
                    throw py::value_error("a speed is more than 0 and at most 1000 m/s");
                p->speed = speed;
            },
            "The player's walking speed, metres a second (running multiplies it). Only the "
            "player.")
        .def(
            "follow",
            [](const EntityHandle& h, const std::vector<std::array<double, 3>>& points) {
                const entt::entity e = h.resolve();
                if (!registry().all_of<Player>(e))
                    throw py::value_error("entity '" + h.id +
                                          "' isn't the player: only it "
                                          "follows a route");
                std::vector<glm::dvec3> route;
                route.reserve(points.size());
                for (const auto& p : points) route.emplace_back(p[0], p[1], p[2]);
                current_game().follow_path(std::move(route));
            },
            py::arg("points"),
            "The player walks through these points (in the scene's frame) at its speed, as if "
            "steered, instead of following the controls, until it reaches the last (a cutscene, "
            "click-to-move, a guided tour); [] stops it. Only the player.")
        .def_property_readonly(
            "following",
            [](const EntityHandle& h) {
                return registry().all_of<Player>(h.resolve()) && current_game().following_path();
            },
            "True while the player is walking a route given to follow().")
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
        .def_property_readonly(
            "swimming",
            [](const EntityHandle& h) {
                ScenePhysics& p = scene_physics();
                return h.resolve() == p.player() && p.character() && p.character()->swimming();
            },
            "Whether the player is swimming: in the sea deeper than its shoulders, floating at "
            "the surface.")
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
        .def(
            "enter_space",
            [](const RoomsHandle&, float fade) {
                Game& g = current_game();
                if (const std::string problem = g.arrival_problem(Game::kSystemSpace, "");
                    !problem.empty())
                    throw py::value_error(problem);
                g.enter_space(fade);
            },
            py::arg("fade") = 1.0f,
            "Into space (milestone 5.16): above the planet the player is on, or outside the "
            "habitat or hollow world. ValueError on a world with no outside.")
        .def_property_readonly(
            "in_space", [](const RoomsHandle&) { return current_game().in_space(); },
            "Whether the player is in space.")
        .def_property_readonly(
            "space_from", [](const RoomsHandle&) { return current_game().space_from(); },
            "In space: the scene left for it (empty when not in space).")
        .def_property_readonly(
            "ceiling", [](const RoomsHandle&) { return current_game().ceiling(); },
            "A planet's atmosphere ceiling, metres above its sea level: fly above it and you're "
            "in space (0 off a planet).")
        .def_property_readonly(
            "altitude", [](const RoomsHandle&) { return current_game().altitude(); },
            "How high the player is above the world's sea level (its surface), metres.")
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

    const auto music = []() -> audio::MusicDirector* { return current_game().music(); };
    py::class_<MusicHandle>(m, "Music",
                            "Music (midgard.music; milestone 5.14): playlists chosen by where "
                            "the player is, or by a script.")
        .def(
            "play",
            [music](const MusicHandle&, const std::string& playlist) {
                if (auto* d = music(); d && !d->force(playlist))
                    throw py::value_error("there is no playlist '" + playlist + "'");
            },
            py::arg("playlist"),
            "Plays a playlist (by name or id), crossfading, whatever fits where the player is, "
            "until release(). ValueError for one the game hasn't.")
        .def(
            "release",
            [music](const MusicHandle&) {
                if (auto* d = music()) d->release();
            },
            "Back to the playlist that fits where the player is.")
        .def(
            "stop",
            [music](const MusicHandle&) {
                if (auto* d = music()) d->force("");
            },
            "Fades the music out, until play() or release().")
        .def(
            "stinger",
            [music](const MusicHandle&, const std::string& event, float duck) {
                auto* d = music();
                const audio::VoiceId voice = d ? d->stinger(event, duck) : 0;
                return voice ? py::cast(SoundHandle{voice}) : py::none();
            },
            py::arg("event"), py::arg("duck") = 0.35f,
            "Plays a sound event over the music (a fanfare, a discovery), which dips to `duck` "
            "of its level under it and comes back after. The Sound, or None.")
        .def_property_readonly(
            "playlist",
            [music](const MusicHandle&) {
                auto* d = music();
                return d ? d->playing() : std::string();
            },
            "The playlist playing, by id (empty: none).")
        .def_property_readonly(
            "track",
            [music](const MusicHandle&) {
                auto* d = music();
                return d ? d->track() : std::string();
            },
            "The sound event of the track playing (empty: none).")
        .def("__repr__", [](const MusicHandle&) { return std::string("<midgard.music>"); });
    m.attr("music") = MusicHandle{};

    const auto ambience = []() -> audio::AmbienceDirector* { return current_game().ambience(); };
    py::class_<AmbienceHandle>(m, "Ambience",
                               "Ambience (midgard.ambience; milestone 5.14): the background "
                               "sound of where the player is, or of a script's choosing.")
        .def(
            "play",
            [ambience](const AmbienceHandle&, const std::string& set) {
                if (auto* d = ambience(); d && !d->force(set))
                    throw py::value_error("there is no ambience '" + set + "'");
            },
            py::arg("set"),
            "Plays an ambience set (by name or id), crossfading, until release(). ValueError "
            "for one the game hasn't.")
        .def(
            "release",
            [ambience](const AmbienceHandle&) {
                if (auto* d = ambience()) d->release();
            },
            "Back to the ambience that fits where the player is.")
        .def(
            "stop",
            [ambience](const AmbienceHandle&) {
                if (auto* d = ambience()) d->force("");
            },
            "Fades the ambience out, until play() or release().")
        .def_property_readonly(
            "playing",
            [ambience](const AmbienceHandle&) {
                auto* d = ambience();
                return d ? d->playing() : std::string();
            },
            "The ambience set playing, by id (empty: none).")
        .def_property_readonly(
            "layers",
            [ambience](const AmbienceHandle&) {
                auto* d = ambience();
                return d ? d->layers() : std::vector<std::string>();
            },
            "Its layers sounding now (their sound events).")
        .def("__repr__", [](const AmbienceHandle&) { return std::string("<midgard.ambience>"); });
    m.attr("ambience") = AmbienceHandle{};

    py::class_<ScriptWeather>(m, "Weather",
                              "The weather somewhere (midgard.weather.current()): its type's id "
                              "and name, and what it does.")
        .def_readonly("id", &ScriptWeather::id, "Its weather type's id (\"\": no weather).")
        .def_readonly("name", &ScriptWeather::name)
        .def_readonly("forced", &ScriptWeather::forced, "A script's (midgard.weather.force).")
        .def_readonly("front", &ScriptWeather::front, "A front passing over.")
        .def_readonly("clouds", &ScriptWeather::clouds, "0 a clear sky .. 1 overcast.")
        .def_readonly("darkness", &ScriptWeather::darkness, "The daylight it takes, 0 .. 1.")
        .def_readonly("visibility", &ScriptWeather::visibility,
                      "Metres you can see through it (0: no limit).")
        .def_readonly("falls", &ScriptWeather::falls, "none, rain, snow, ash or sand.")
        .def_readonly("amount", &ScriptWeather::amount, "How much falls, 0 .. 1.")
        .def_readonly("lightning", &ScriptWeather::lightning, "Flashes a minute.")
        .def_readonly("wind", &ScriptWeather::wind, "Metres a second.")
        .def_readonly("gusts", &ScriptWeather::gusts, "0 steady .. 1 very gusty.")
        .def_readonly("wetness", &ScriptWeather::wetness, "How wet the open gets, 0 .. 1.")
        .def_readonly("temperature", &ScriptWeather::temperature,
                      "Degrees Celsius it adds to the climate's.")
        .def("__eq__", [](const ScriptWeather& a, const ScriptWeather& b) { return a.id == b.id; })
        .def("__eq__", [](const ScriptWeather& a, const std::string& id) { return a.id == id; })
        .def("__hash__", [](const ScriptWeather& w) { return py::hash(py::str(w.id)); })
        .def("__repr__", [](const ScriptWeather& w) {
            return "Weather(" + py::repr(py::str(w.id)).cast<std::string>() +
                   (w.forced ? ", forced" : "") + (w.front ? ", front" : "") + ")";
        });
    py::class_<ScriptSpell>(m, "WeatherSpell",
                            "A stretch of the same weather in a forecast "
                            "(midgard.weather.forecast()).")
        .def_readonly("id", &ScriptSpell::id)
        .def_readonly("name", &ScriptSpell::name)
        .def_readonly("start", &ScriptSpell::start, "When it starts, as midgard.time.now().time.")
        .def_readonly("end", &ScriptSpell::end, "When the next starts.")
        .def_readonly("in_hours", &ScriptSpell::in_hours, "Hours from now until it starts.")
        .def_readonly("hours", &ScriptSpell::hours, "How many hours it lasts.")
        .def_readonly("forced", &ScriptSpell::forced)
        .def_readonly("front", &ScriptSpell::front)
        .def("__repr__", [](const ScriptSpell& s) {
            char text[96];
            std::snprintf(text, sizeof(text), ", in %.1f h for %.1f h", s.in_hours, s.hours);
            return "WeatherSpell(" + py::repr(py::str(s.id)).cast<std::string>() + text + ")";
        });
    py::class_<Game::Exposure>(m, "Exposure",
                               "How the weather treats the player (midgard.weather.exposure()).")
        .def_readonly("air", &Game::Exposure::air, "The air's temperature, degrees Celsius.")
        .def_readonly("feels_like", &Game::Exposure::feels_like,
                      "How warm it feels, degrees Celsius: the air, the wind, being wet, and "
                      "heat sources near.")
        .def_readonly("wind", &Game::Exposure::wind,
                      "The wind where the player is, metres a second (walls cut it).")
        .def_readonly("heat", &Game::Exposure::heat, "Degrees of warmth from heat sources near.")
        .def_readonly("wetness", &Game::Exposure::wetness,
                      "How wet the player is, 0 dry .. 1 soaked.")
        .def_readonly("roof", &Game::Exposure::roof,
                      "How much of the sky above is covered, 0 .. 1.")
        .def_readonly("walls", &Game::Exposure::walls, "How much is walled in round about, 0 .. 1.")
        .def_readonly("sheltered", &Game::Exposure::sheltered,
                      "Under a roof: rain and snow don't reach the player.")
        .def("__repr__", [](const Game::Exposure& e) {
            char text[160];
            std::snprintf(text, sizeof(text),
                          "Exposure(air=%.1f, feels_like=%.1f, wind=%.1f, heat=%.1f, "
                          "wetness=%.2f, sheltered=%s)",
                          e.air, e.feels_like, e.wind, e.heat, double(e.wetness),
                          e.sheltered ? "True" : "False");
            return std::string(text);
        });
    py::class_<WeatherHandle>(m, "WeatherApi",
                              "The weather (midgard.weather; milestone 5.15): what it is where "
                              "the player is, the forecast, and a script's own weather.")
        .def(
            "current", [](const WeatherHandle&) { return script_weather(current_game()); },
            "The weather where the player is now.")
        .def(
            "forecast",
            [](const WeatherHandle&, double hours) {
                if (!(hours > 0.0)) throw py::value_error("a forecast is for some hours ahead");
                Game& game = current_game();
                const double hour = double(game.local_calendar().seconds_per_hour());
                const std::int64_t now = game.time().now();
                std::vector<ScriptSpell> out;
                for (const weather::Spell& s : game.weather_forecast(hours)) {
                    ScriptSpell spell;
                    spell.id = s.weather;
                    spell.name = weather_name(game, s.weather);
                    spell.start = s.start;
                    spell.end = s.end;
                    spell.in_hours = double(s.start - now) / hour;
                    spell.hours = double(s.end - s.start) / hour;
                    spell.forced = s.forced;
                    spell.front = s.front >= 0;
                    out.push_back(std::move(spell));
                }
                return out;
            },
            py::arg("hours") = 24.0,
            "The weather where the player is over the next so many hours, as it will come: a "
            "list of WeatherSpell, the first starting now.")
        .def(
            "on_change",
            [](const WeatherHandle&, const py::object& callback) {
                if (!PyCallable_Check(callback.ptr()))
                    throw py::type_error("midgard.weather.on_change needs a function to call");
                if (!g_context) throw std::runtime_error("midgard: no game is running");
                g_context->weather_callbacks.push_back(callback);
            },
            py::arg("callback"),
            "Calls callback(before, now) with two Weather whenever the weather where the player "
            "is changes (by itself, a front, the player going somewhere else, or a script).")
        .def(
            "force",
            [](const WeatherHandle&, const std::string& weather, double hours) {
                if (!current_game().force_weather(weather, hours))
                    throw py::value_error("there is no weather type '" + weather + "'");
            },
            py::arg("weather"), py::arg("hours") = 0.0,
            "Makes it this weather (a weather type's id) everywhere, from now: for so many hours, "
            "then the weather there would have been (0: until release()). ValueError if there "
            "is no such weather type.")
        .def(
            "release", [](const WeatherHandle&) { current_game().release_weather(); },
            "Back to the weather there would have been.")
        .def_property(
            "wetness", [](const WeatherHandle&) { return current_game().ground_cover().wetness; },
            [](const WeatherHandle&, float wetness) {
                render::GroundCover g = current_game().ground_cover();
                g.wetness = std::clamp(wetness, 0.0f, 1.0f);
                current_game().set_ground_cover(g);
            },
            "How wet the ground is where the player is, 0 dry .. 1 soaked (settable).")
        .def_property(
            "snow", [](const WeatherHandle&) { return current_game().ground_cover().snow; },
            [](const WeatherHandle&, float snow) {
                render::GroundCover g = current_game().ground_cover();
                g.snow = std::clamp(snow, 0.0f, 1.0f);
                current_game().set_ground_cover(g);
            },
            "How much snow has settled where the player is, 0 none .. 1 covered (settable).")
        .def(
            "exposure", [](const WeatherHandle&) { return current_game().exposure(); },
            "How the weather treats the player now: sheltered or not, the wind reaching them, "
            "how wet they are, the warmth of fires near, and how warm it feels.")
        .def(
            "set_player_wetness",
            [](const WeatherHandle&, float wetness) { current_game().set_player_wetness(wetness); },
            py::arg("wetness"), "How wet the player is now, 0 .. 1 (dried off, or fell in).")
        .def_property_readonly(
            "temperature", [](const WeatherHandle&) { return current_game().temperature_here(); },
            "The temperature where the player is, degrees Celsius: the climate's (with the "
            "season) and the weather's.")
        .def_property_readonly(
            "types",
            [](const WeatherHandle&) {
                std::vector<std::string> out;
                for (const auto& [id, t] : current_game().scene().weather_types) out.push_back(id);
                return out;
            },
            "The weather types' ids.")
        .def("__repr__", [](const WeatherHandle&) { return std::string("<midgard.weather>"); });
    m.attr("weather") = WeatherHandle{};
    py::class_<StatsHandle>(m, "StatsApi",
                            "The player's stats, scores and counters (midgard.stats; milestone "
                            "6.0), as the Stats panel defines them: by id or name. A stat stays "
                            "between its least and most and may refill by itself; scores and "
                            "counters are whole numbers (a counter up to its target, if any).")
        .def(
            "get",
            [](const StatsHandle&, const std::string& stat) {
                const stats::StatDef& d = stat_def(stat);
                return stat_value(d, *current_game().stats().get(d.id));
            },
            py::arg("stat"), "A stat's value. KeyError if there is no such stat.")
        .def(
            "set",
            [](const StatsHandle&, const std::string& stat, double value) {
                const stats::StatDef& d = stat_def(stat);
                return stat_value(d, *current_game().stats().set(d.id, value));
            },
            py::arg("stat"), py::arg("value"),
            "Sets a stat (kept within its range, whole for scores and counters) and gives its "
            "new value.")
        .def(
            "add",
            [](const StatsHandle&, const std::string& stat, double amount) {
                const stats::StatDef& d = stat_def(stat);
                return stat_value(d, *current_game().stats().add(d.id, amount));
            },
            py::arg("stat"), py::arg("amount") = 1.0,
            "Adds to a stat (a negative amount takes away) and gives its new value.")
        .def(
            "max",
            [](const StatsHandle&, const std::string& stat) -> py::object {
                const stats::StatDef& d = stat_def(stat);
                if (!d.capped()) return py::none();
                return stat_value(d, d.max);
            },
            py::arg("stat"), "A stat's most, or a counter's target (None: it has none).")
        .def(
            "min",
            [](const StatsHandle&, const std::string& stat) {
                const stats::StatDef& d = stat_def(stat);
                return stat_value(d, d.min);
            },
            py::arg("stat"), "A stat's least.")
        .def(
            "set_max",
            [](const StatsHandle&, const std::string& stat, double max) {
                const stats::StatDef& d = stat_def(stat);
                if (!current_game().stats().set_max(d.id, max))
                    throw py::value_error("'" + d.name + "' can't have a most of " +
                                          std::to_string(max));
            },
            py::arg("stat"), py::arg("max"),
            "Changes a stat's most (more health as the player grows) or a counter's target; the "
            "value is kept within it. ValueError for a score, or a most not above the least.")
        .def(
            "reset",
            [](const StatsHandle&, const std::optional<std::string>& stat) {
                if (!stat) {
                    current_game().stats().reset_all();
                    return;
                }
                current_game().stats().reset(stat_def(*stat).id);
            },
            py::arg("stat") = py::none(),
            "A stat back to its start and its most as designed (every stat with none given).")
        .def(
            "name", [](const StatsHandle&, const std::string& stat) { return stat_def(stat).name; },
            py::arg("stat"), "A stat's name, as the player sees it.")
        .def(
            "kind",
            [](const StatsHandle&, const std::string& stat) {
                return std::string(stats::kind_name(stat_def(stat).kind));
            },
            py::arg("stat"), "\"stat\", \"score\" or \"counter\".")
        .def(
            "on_change",
            [](const StatsHandle&, const py::object& callback) {
                if (!PyCallable_Check(callback.ptr()))
                    throw py::type_error("midgard.stats.on_change needs a function to call");
                if (!g_context) throw std::runtime_error("midgard: no game is running");
                g_context->stat_callbacks.push_back(callback);
            },
            py::arg("callback"),
            "Calls callback(stat, before, now) after a frame in which a stat changed (by itself "
            "or by a script): its id and its value before and after.")
        .def_property_readonly(
            "ids",
            [](const StatsHandle&) {
                std::vector<std::string> out;
                for (const stats::StatDef& d : current_game().stats().defs())
                    if (!d.display()) out.push_back(d.id);
                return out;
            },
            "The stats' ids, in the Stats panel's order (not positions or clocks, which only "
            "show on the HUD).")
        .def(
            "__getitem__",
            [](const StatsHandle&, const std::string& stat) {
                const stats::StatDef& d = stat_def(stat);
                return stat_value(d, *current_game().stats().get(d.id));
            },
            py::arg("stat"))
        .def(
            "__setitem__",
            [](const StatsHandle&, const std::string& stat, double value) {
                current_game().stats().set(stat_def(stat).id, value);
            },
            py::arg("stat"), py::arg("value"))
        .def("__contains__",
             [](const StatsHandle&, const std::string& stat) {
                 const stats::StatDef* d = current_game().stats().find(stat);
                 return d != nullptr && !d->display();
             })
        .def("__repr__", [](const StatsHandle&) { return std::string("<midgard.stats>"); });
    m.attr("stats") = StatsHandle{};
    py::class_<UiHandle>(m, "UiApi",
                         "The HUD (midgard.ui; milestone 6.0): short notes that fade, a prompt, "
                         "timers, and whether the HUD and its crosshair show. The stats show "
                         "on it by themselves (midgard.stats).")
        .def(
            "notify",
            [](const UiHandle&, const std::string& text, double seconds) {
                current_game().hud().notify(text, seconds);
            },
            py::arg("text"), py::arg("seconds") = 4.0,
            "A short note on the HUD (\"Picked up a key\") for so many seconds; it fades as it "
            "goes. Five show at most: the oldest goes first.")
        .def(
            "prompt",
            [](const UiHandle&, const std::optional<std::string>& text) {
                current_game().hud().set_prompt(text.value_or(""));
            },
            py::arg("text") = py::none(),
            "The prompt at the bottom of the screen (\"Press E to open\") until another, or "
            "none with no text.")
        .def(
            "timer",
            [](const UiHandle&, const std::string& id, double seconds, const std::string& label,
               const std::string& count, const py::object& on_done) {
                if (count != "down" && count != "up")
                    throw py::value_error("a timer counts \"down\" or \"up\", not \"" + count +
                                          "\"");
                if (!on_done.is_none() && !PyCallable_Check(on_done.ptr()))
                    throw py::type_error("midgard.ui.timer's on_done needs a function to call");
                current_game().hud().start_timer(id, seconds, count == "down", label);
                if (g_context) {
                    g_context->timer_callbacks.erase(id);
                    if (!on_done.is_none()) g_context->timer_callbacks[id] = on_done;
                }
            },
            py::arg("id"), py::arg("seconds"), py::arg("label") = "", py::arg("count") = "down",
            py::arg("on_done") = py::none(),
            "A timer on the HUD, by id (one with the same id starts over): counting \"down\" "
            "from seconds to 0, or \"up\" from 0 (to seconds, or for ever with 0). "
            "on_done() is called when it ends.")
        .def(
            "stop_timer",
            [](const UiHandle&, const std::string& id) {
                if (g_context) g_context->timer_callbacks.erase(id);
                return current_game().hud().stop_timer(id);
            },
            py::arg("id"),
            "Takes a timer away (its on_done isn't called). False if there wasn't one.")
        .def(
            "timer_value",
            [](const UiHandle&, const std::string& id) { return current_game().hud().timer(id); },
            py::arg("id"),
            "A timer's seconds: left (counting down) or passed (counting up); None if there is no "
            "such timer.")
        .def_property(
            "hud", [](const UiHandle&) { return current_game().hud().visible; },
            [](const UiHandle&, bool on) { current_game().hud().visible = on; },
            "Whether the HUD shows (False hides it all, as for a cut scene).")
        .def_property(
            "crosshair",
            [](const UiHandle&) -> py::object {
                const auto& c = current_game().hud().crosshair;
                return c ? py::cast(*c) : py::none();
            },
            [](const UiHandle&, const std::optional<bool>& on) {
                current_game().hud().crosshair = on;
            },
            "The crosshair: True shows it, False hides it, None (as it starts) shows it in "
            "first person only.")
        .def(
            "show",
            [](const UiHandle&, const std::string& screen) {
                if (!current_game().screen(screen))
                    throw py::key_error("there is no screen '" + screen + "'");
                current_game().hud().show_screen(screen);
            },
            py::arg("screen"),
            "Shows a screen made in the Menu editor (by its id), on top of any others.")
        .def(
            "hide",
            [](const UiHandle&, const std::string& screen) {
                return current_game().hud().hide_screen(screen);
            },
            py::arg("screen"), "Takes a screen away. False if it wasn't shown.")
        .def(
            "shown",
            [](const UiHandle&, const std::string& screen) {
                return current_game().hud().screen_shown(screen);
            },
            py::arg("screen"), "Whether a screen is shown.")
        .def_property_readonly(
            "screens", [](const UiHandle&) { return current_game().hud().screens(); },
            "The screens shown now, bottom to top.")
        .def(
            "on_click",
            [](const UiHandle&, const std::string& screen, const std::string& element,
               py::object callback) {
                check_element(screen, element, "button");
                if (!PyCallable_Check(callback.ptr()))
                    throw py::type_error("midgard.ui.on_click needs a function to call");
                if (g_context)
                    g_context->click_callbacks[screen + "/" + element].push_back(callback);
            },
            py::arg("screen"), py::arg("element"), py::arg("callback"),
            "Calls callback() when the player clicks a button on a screen.")
        .def(
            "on_change",
            [](const UiHandle&, const std::string& screen, const std::string& element,
               py::object callback) {
                check_element(screen, element, "");
                if (!PyCallable_Check(callback.ptr()))
                    throw py::type_error("midgard.ui.on_change needs a function to call");
                if (g_context)
                    g_context->change_callbacks[screen + "/" + element].push_back(callback);
            },
            py::arg("screen"), py::arg("element"), py::arg("callback"),
            "Calls callback(value) when the player moves a slider (its value) or ticks a check "
            "(True or False) on a screen.")
        .def(
            "click",
            [](const UiHandle&, const std::string& screen, const std::string& element) {
                check_element(screen, element, "button");
                current_game().ui_click(screen, element);
            },
            py::arg("screen"), py::arg("element"),
            "Presses a button as the player would: it does what it says (but doesn't quit the "
            "game), and on_click hears it at the next update.")
        .def(
            "set_text",
            [](const UiHandle&, const std::string& screen, const std::string& element,
               const std::string& text) {
                check_element(screen, element, "");
                current_game().hud().texts[screen + "/" + element] = text;
            },
            py::arg("screen"), py::arg("element"), py::arg("text"),
            "Changes the text of an element (text, a button, a check) on a screen.")
        .def(
            "value",
            [](const UiHandle&, const std::string& screen,
               const std::string& element) -> py::object {
                const ScreenElement& e = check_element(screen, element, "");
                const auto& values = current_game().hud().values;
                const auto it = values.find(screen + "/" + element);
                const double v = it != values.end() ? it->second : e.value;
                if (e.kind == "check" || e.kind == "option") return py::bool_(v != 0.0);
                return py::float_(v);
            },
            py::arg("screen"), py::arg("element"),
            "A slider's value, or whether a check is ticked or an option chosen, on a screen.")
        .def(
            "set_value",
            [](const UiHandle&, const std::string& screen, const std::string& element,
               double value) {
                const ScreenElement& e = check_element(screen, element, "");
                if (e.kind == "slider") value = std::clamp(value, e.min, e.max);
                if (e.kind == "check" || e.kind == "option") value = value != 0.0 ? 1.0 : 0.0;
                current_game().hud().values[screen + "/" + element] = value;
                if (e.kind == "option" && value != 0.0)  // (the others in its group: not)
                    for (const ScreenElement& other : current_game().screen(screen)->elements)
                        if (other.kind == "option" && other.group == e.group && other.id != e.id)
                            current_game().hud().values[screen + "/" + other.id] = 0.0;
            },
            py::arg("screen"), py::arg("element"), py::arg("value"),
            "Sets a slider's value (or ticks a check, or chooses an option: True or False) on a "
            "screen.")
        .def(
            "set_enabled",
            [](const UiHandle&, const std::string& screen, const std::string& element, bool on) {
                check_element(screen, element, "");
                current_game().hud().enabled[screen + "/" + element] = on;
            },
            py::arg("screen"), py::arg("element"), py::arg("enabled"),
            "Lets the player use an element (a button, slider, check or option), or not: "
            "disabled, it shows its disabled picture (or greyed) and can't be used.")
        .def(
            "enabled",
            [](const UiHandle&, const std::string& screen, const std::string& element) {
                const ScreenElement& e = check_element(screen, element, "");
                const auto& on = current_game().hud().enabled;
                const auto it = on.find(screen + "/" + element);
                return it != on.end() ? it->second : e.enabled;
            },
            py::arg("screen"), py::arg("element"), "Whether the player can use an element.")
        .def_property(
            "compass", [](const UiHandle&) { return current_game().hud().compass; },
            [](const UiHandle&, bool on) { current_game().hud().compass = on; },
            "Whether the compass shows at the top of the screen.")
        .def_property(
            "minimap", [](const UiHandle&) { return current_game().hud().minimap; },
            [](const UiHandle&, bool on) { current_game().hud().minimap = on; },
            "Whether the minimap shows at the bottom right.")
        .def_property_readonly(
            "heading", [](const UiHandle&) { return current_game().heading(); },
            "The way the camera faces, in degrees from north (0 north, 90 east, 180 south, 270 "
            "west), as the compass shows it on the world's shape.")
        .def(
            "mark",
            [](const UiHandle&, const EntityHandle& entity, const std::string& label) {
                if (!entity.exists()) raise_reference_error("that entity is gone");
                current_game().hud().marks[entity.id] = label;
            },
            py::arg("entity"), py::arg("label") = "",
            "Marks an entity on the minimap (a quest's goal, a lost friend), with a label if "
            "given, until unmark().")
        .def(
            "unmark",
            [](const UiHandle&, const EntityHandle& entity) {
                return current_game().hud().marks.erase(entity.id) > 0;
            },
            py::arg("entity"), "Takes an entity's mark off the minimap. False if it had none.")
        .def("__repr__", [](const UiHandle&) { return std::string("<midgard.ui>"); });
    m.attr("ui") = UiHandle{};

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
        "input_context", [] { return current_game().input_context(); },
        "The input context now: 'on_foot', 'flying', 'menu', 'vehicle' (actions of others "
        "read nothing).");
    m.def(
        "set_input_context",
        [](std::optional<std::string> context) {
            if (context && !context->empty() &&
                std::find(std::begin(kContexts), std::end(kContexts), *context) ==
                    std::end(kContexts))
                throw py::value_error("no input context '" + *context +
                                      "' (on_foot, flying, menu or vehicle)");
            current_game().set_input_context(context.value_or(""));
        },
        py::arg("context"),
        "Chooses the input context ('vehicle' in a car, say); None: the game's own again.");
    m.def(
        "sky", [] { return tuple3(current_game().scene().settings.sky); },
        "The sky colour (r, g, b).");
    m.def(
        "set_sky",
        [](float r, float g, float b) {
            current_game().scene().settings.sky = {r, g, b};
        },
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
        s.context.weather_callbacks.clear();
        s.context.stat_callbacks.clear();
        s.context.timer_callbacks.clear();  // (the timers carry on, telling no one)
        s.context.click_callbacks.clear();
        s.context.change_callbacks.clear();
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
    // A pausing screen shows: the scripts' time stands still with the world's, and on_update
    // isn't called (so nothing a script moves moves); its buttons are still heard.
    const bool frozen = s.context.game->frozen();
    if (frozen) dt = 0.0f;
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

    // The weather's changes where the player is (midgard.weather.on_change).
    for (const Game::WeatherChange& change : s.context.game->weather_changes()) {
        for (const py::object& callback : std::vector(s.context.weather_callbacks)) {
            const std::string module = py::str(py::getattr(callback, "__module__", py::str("")));
            if (s.paused.contains(module)) continue;
            try {
                callback(script_weather(*s.context.game, change.from),
                         script_weather(*s.context.game, change.to));
            } catch (py::error_already_set& e) {
                s.fail(module, e, "midgard.weather.on_change");
            }
        }
    }

    // The stats that changed (midgard.stats.on_change).
    for (const stats::Change& change : s.context.game->stat_changes()) {
        const stats::StatDef* d = s.context.game->stats().find(change.id);
        if (!d) continue;  // (gone since)
        for (const py::object& callback : std::vector(s.context.stat_callbacks)) {
            const std::string module = py::str(py::getattr(callback, "__module__", py::str("")));
            if (s.paused.contains(module)) continue;
            try {
                callback(change.id, stat_value(*d, change.before), stat_value(*d, change.after));
            } catch (py::error_already_set& e) {
                s.fail(module, e, "midgard.stats.on_change");
            }
        }
    }

    // The player's clicks and changes on screens (midgard.ui.on_click, on_change).
    for (const hud::UiEvent& ev : s.context.game->ui_events()) {
        const auto& table =
            ev.kind == "click" ? s.context.click_callbacks : s.context.change_callbacks;
        const auto it = table.find(ev.screen + "/" + ev.element);
        if (it == table.end()) continue;
        for (const py::object& callback : std::vector(it->second)) {
            const std::string module = py::str(py::getattr(callback, "__module__", py::str("")));
            if (s.paused.contains(module)) continue;
            try {
                if (ev.kind == "click")
                    callback();
                else
                    callback(ev.value);
            } catch (py::error_already_set& e) {
                s.fail(module, e,
                       "midgard.ui.on_" + ev.kind + " '" + ev.screen + "/" + ev.element + "'");
            }
        }
    }

    // Timers that ended (midgard.ui.timer's on_done).
    for (const std::string& id : s.context.game->finished_timers()) {
        const auto it = s.context.timer_callbacks.find(id);
        if (it == s.context.timer_callbacks.end()) continue;
        const py::object callback = it->second;
        s.context.timer_callbacks.erase(it);
        const std::string module = py::str(py::getattr(callback, "__module__", py::str("")));
        if (s.paused.contains(module)) continue;
        try {
            callback();
        } catch (py::error_already_set& e) {
            s.fail(module, e, "midgard.ui.timer '" + id + "'");
        }
    }

    if (s.context.game->frozen()) return;  // (a script may have shown a pausing screen)
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
        for (auto* callbacks : {&s.context.weather_callbacks, &s.context.stat_callbacks}) {
            for (py::object& callback : *callbacks) {
                if (py::str(py::getattr(callback, "__module__", py::str(""))).cast<std::string>() !=
                    module)
                    continue;
                const py::object renewed = py::getattr(
                    it->second, py::getattr(callback, "__name__", py::str("")), py::none());
                if (!renewed.is_none()) callback = renewed;
            }
        }
        for (auto& [id, callback] : s.context.timer_callbacks) {
            if (py::str(py::getattr(callback, "__module__", py::str(""))).cast<std::string>() !=
                module)
                continue;
            const py::object renewed =
                py::getattr(it->second, py::getattr(callback, "__name__", py::str("")), py::none());
            if (!renewed.is_none()) callback = renewed;
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
    "Music": sorted(n for n in dir(midgard.Music) if not n.startswith("_")),
    "Ambience": sorted(n for n in dir(midgard.Ambience) if not n.startswith("_")),
    "Weather": sorted(n for n in dir(midgard.Weather) if not n.startswith("_")),
    "WeatherSpell": sorted(n for n in dir(midgard.WeatherSpell) if not n.startswith("_")),
    "WeatherApi": sorted(n for n in dir(midgard.WeatherApi) if not n.startswith("_")),
    "Exposure": sorted(n for n in dir(midgard.Exposure) if not n.startswith("_")),
    "StatsApi": sorted(n for n in dir(midgard.StatsApi) if not n.startswith("_")),
    "UiApi": sorted(n for n in dir(midgard.UiApi) if not n.startswith("_")),
})
)",
             scope);
    return scope["result"].cast<std::string>();
}

}  // namespace midgard::script
