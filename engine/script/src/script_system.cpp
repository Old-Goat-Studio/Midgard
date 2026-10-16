// SPDX-License-Identifier: Apache-2.0
#include "midgard/script_system.h"

#include <algorithm>
#include <array>
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
    m.def(
        "time", [] { return g_context ? g_context->time : 0.0f; },
        "Seconds since the game's scripts started.");
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
}

ScriptSystem::~ScriptSystem() {
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
})
)",
             scope);
    return scope["result"].cast<std::string>();
}

}  // namespace midgard::script
