// SPDX-License-Identifier: Apache-2.0
#include "midgard/anim_graph.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace midgard {

namespace {

using json = nlohmann::json;

[[noreturn]] void bad(const std::string& what) {
    throw AnimationError("animation graph: " + what);
}

float number(const json& j, const char* key, float fallback, const std::string& where) {
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    if (it->is_boolean()) return it->get<bool>() ? 1.0f : 0.0f;
    if (!it->is_number()) bad(where + ": '" + key + "' must be a number");
    return it->get<float>();
}

std::string text(const json& j, const char* key, const std::string& where) {
    const auto it = j.find(key);
    if (it == j.end()) return {};
    if (!it->is_string()) bad(where + ": '" + key + "' must be text");
    return it->get<std::string>();
}

AnimCondition condition(const json& c, const std::string& where) {
    if (!c.is_array() || (c.size() != 1 && c.size() != 3) || !c[0].is_string())
        bad(where + ": a condition is [name] or [name, op, number]");
    AnimCondition out;
    out.parameter = c[0].get<std::string>();
    if (c.size() == 1) return out;
    static const std::map<std::string, AnimCondition::Op> ops = {
        {"==", AnimCondition::Op::Eq}, {"!=", AnimCondition::Op::Ne},
        {"<", AnimCondition::Op::Lt},  {"<=", AnimCondition::Op::Le},
        {">", AnimCondition::Op::Gt},  {">=", AnimCondition::Op::Ge}};
    const auto op = c[1].is_string() ? ops.find(c[1].get<std::string>()) : ops.end();
    if (op == ops.end()) bad(where + ": a condition's op is one of == != < <= > >=");
    out.op = op->second;
    if (c[2].is_boolean())
        out.value = c[2].get<bool>() ? 1.0f : 0.0f;
    else if (c[2].is_number())
        out.value = c[2].get<float>();
    else
        bad(where + ": a condition compares with a number");
    return out;
}

// A joint's part in a tree (it, and joints under it): 1, else 0.
std::vector<float> subtree(const Skeleton& skeleton, const std::string& joint) {
    const int n = skeleton.joint_count();
    std::vector<float> out(size_t(n), joint.empty() ? 1.0f : 0.0f);
    if (joint.empty()) return out;
    const int root = skeleton.find(joint);
    if (root < 0) return out;      // (no such joint: the layer moves nothing)
    for (int j = 0; j < n; ++j) {  // (ozz: parents before children)
        const int p = skeleton.parents()[size_t(j)];
        if (j == root || (p >= 0 && out[size_t(p)] > 0.0f)) out[size_t(j)] = 1.0f;
    }
    return out;
}

glm::quat nlerp(const glm::quat& a, glm::quat b, float t) {
    if (glm::dot(a, b) < 0.0f) b = -b;
    return glm::normalize(a * (1.0f - t) + b * t);
}

void mix(std::vector<JointTransform>& into, const std::vector<JointTransform>& from, float t,
         const std::vector<float>* mask = nullptr) {
    for (size_t j = 0; j < into.size(); ++j) {
        const float w = mask ? t * (*mask)[j] : t;
        if (w <= 0.0f) continue;
        JointTransform& a = into[j];
        const JointTransform& b = from[j];
        if (w >= 1.0f) {
            a = b;
            continue;
        }
        a.translation = glm::mix(a.translation, b.translation, w);
        a.rotation = nlerp(a.rotation, b.rotation, w);
        a.scale = glm::mix(a.scale, b.scale, w);
    }
}

}  // namespace

// ---- the graph ----------------------------------------------------------------------------
AnimGraph AnimGraph::parse(const std::string& json_text) {
    json j;
    try {
        j = json::parse(json_text);
    } catch (const json::exception& e) {
        bad(std::string("not JSON: ") + e.what());
    }
    return parse(j);
}

AnimGraph AnimGraph::parse(const json& j) {
    if (!j.is_object()) bad("it must be an object");
    AnimGraph g;
    if (const auto p = j.find("parameters"); p != j.end()) {
        if (!p->is_object()) bad("'parameters' must be an object of names and values");
        for (const auto& [name, value] : p->items()) {
            if (value.is_boolean())
                g.parameters[name] = value.get<bool>() ? 1.0f : 0.0f;
            else if (value.is_number())
                g.parameters[name] = value.get<float>();
            else
                bad("parameter '" + name + "' must start as a number, true or false");
        }
    }
    g.parameters.try_emplace(kSpeedParameter, 0.0f);
    g.parameters.try_emplace(kVerticalSpeedParameter, 0.0f);
    if (const auto t = j.find("triggers"); t != j.end()) {
        if (!t->is_array()) bad("'triggers' must be a list of names");
        for (const json& name : *t) {
            if (!name.is_string()) bad("'triggers' must be a list of names");
            if (g.parameters.contains(name.get<std::string>()))
                bad("'" + name.get<std::string>() + "' is both a parameter and a trigger");
            g.triggers.insert(name.get<std::string>());
        }
    }
    if (const auto r = j.find("root_motion"); r != j.end()) {
        if (!r->is_boolean()) bad("'root_motion' must be true or false");
        g.root_motion = r->get<bool>();
    }
    const auto parameter = [&](const std::string& name, const std::string& where) {
        if (!name.empty() && !g.parameters.contains(name))
            bad(where + ": there is no parameter '" + name + "'");
    };
    const auto layers = j.find("layers");
    if (layers == j.end() || !layers->is_array() || layers->empty())
        bad("it needs 'layers', a list of at least one");
    for (const json& lj : *layers) {
        if (!lj.is_object()) bad("each layer must be an object");
        AnimLayer layer;
        layer.name = text(lj, "name", "a layer");
        const std::string where = "layer '" + layer.name + "'";
        layer.mask = text(lj, "mask", where);
        if (const auto w = lj.find("weight"); w != lj.end() && w->is_string()) {
            layer.weight_parameter = w->get<std::string>();
            parameter(layer.weight_parameter, where);
        } else {
            layer.weight = std::clamp(number(lj, "weight", 1.0f, where), 0.0f, 1.0f);
        }
        const auto states = lj.find("states");
        if (states == lj.end() || !states->is_object() || states->empty())
            bad(where + " needs 'states', an object of at least one");
        std::map<std::string, int> index;
        for (const auto& [name, sj] : states->items()) {
            const std::string at = where + ", state '" + name + "'";
            if (!sj.is_object()) bad(at + " must be an object");
            AnimState s;
            s.name = name;
            s.clip = text(sj, "clip", at);
            s.blend = text(sj, "blend", at);
            if (s.clip.empty() == s.blend.empty())
                bad(at + " plays a 'clip', or a 'blend' of clips by a parameter");
            if (!s.blend.empty()) {
                parameter(s.blend, at);
                const auto clips = sj.find("clips");
                if (clips == sj.end() || !clips->is_array() || clips->empty())
                    bad(at + ": a blend needs 'clips', a list of [clip, where]");
                for (const json& c : *clips) {
                    if (!c.is_array() || c.size() != 2 || !c[0].is_string() || !c[1].is_number())
                        bad(at + ": a blend's clips are [clip, where]");
                    s.clips.emplace_back(c[0].get<std::string>(), c[1].get<float>());
                }
                std::stable_sort(s.clips.begin(), s.clips.end(),
                                 [](const auto& a, const auto& b) { return a.second < b.second; });
            }
            if (const auto sp = sj.find("speed"); sp != sj.end() && sp->is_string()) {
                s.speed_parameter = sp->get<std::string>();
                parameter(s.speed_parameter, at);
            } else {
                s.speed = number(sj, "speed", 1.0f, at);
            }
            s.loop = number(sj, "loop", 1.0f, at) != 0.0f;
            if (const auto ev = sj.find("events"); ev != sj.end()) {
                if (!ev->is_array()) bad(at + ": 'events' is a list of [point, name]");
                for (const json& e : *ev) {
                    if (!e.is_array() || e.size() != 2 || !e[0].is_number() || !e[1].is_string())
                        bad(at + ": an event is [point in the cycle (0 to 1), name]");
                    s.events.emplace_back(std::clamp(e[0].get<float>(), 0.0f, 1.0f),
                                          e[1].get<std::string>());
                }
            }
            index[name] = int(layer.states.size());
            layer.states.push_back(std::move(s));
        }
        const auto state = [&](const std::string& name, const std::string& at) {
            const auto it = index.find(name);
            if (it == index.end()) bad(at + ": there is no state '" + name + "'");
            return it->second;
        };
        const std::string start = text(lj, "start", where);
        if (start.empty() && layer.states.size() > 1) bad(where + " needs a 'start' state");
        layer.start = start.empty() ? 0 : state(start, where);
        if (const auto ts = lj.find("transitions"); ts != lj.end()) {
            if (!ts->is_array()) bad(where + ": 'transitions' must be a list");
            for (const json& tj : *ts) {
                if (!tj.is_object()) bad(where + ": each transition must be an object");
                AnimTransition t;
                const std::string from = text(tj, "from", where);
                const std::string at =
                    where + ", transition from '" + from + "' to '" + text(tj, "to", where) + "'";
                t.from = from == "*" || from.empty() ? -1 : state(from, at);
                t.to = state(text(tj, "to", where), at);
                t.blend = std::max(0.0f, number(tj, "blend", 0.2f, at));
                if (tj.contains("after")) t.after = std::max(0.0f, number(tj, "after", 0.0f, at));
                if (const auto w = tj.find("when"); w != tj.end()) {
                    if (!w->is_array()) bad(at + ": 'when' is a list of conditions");
                    for (const json& c : *w) {
                        AnimCondition cond = condition(c, at);
                        if (g.triggers.contains(cond.parameter)) {
                            if (cond.op != AnimCondition::Op::Set)
                                bad(at + ": trigger '" + cond.parameter + "' is only [name]");
                        } else {
                            parameter(cond.parameter, at);
                        }
                        t.when.push_back(std::move(cond));
                    }
                }
                if (t.when.empty() && !t.after)
                    bad(at + " needs a condition ('when') or an 'after'");
                layer.transitions.push_back(std::move(t));
            }
        }
        g.layers.push_back(std::move(layer));
    }
    return g;
}

std::set<std::string> AnimGraph::clips() const {
    std::set<std::string> out;
    for (const AnimLayer& l : layers)
        for (const AnimState& s : l.states) {
            if (!s.clip.empty()) out.insert(s.clip);
            for (const auto& [clip, at] : s.clips) out.insert(clip);
        }
    return out;
}

// ---- playing it ---------------------------------------------------------------------------
AnimPlayer::AnimPlayer(std::shared_ptr<const AnimGraph> graph, const Skeleton& skeleton,
                       FindClip find, int motion_joint, const glm::mat4& armature)
    : graph_(std::move(graph)),
      skeleton_(&skeleton),
      find_(std::move(find)),
      motion_joint_(motion_joint),
      armature_(armature),
      values_(graph_->parameters) {
    for (const AnimLayer& l : graph_->layers) {
        LayerState s;
        s.now.state = l.start;
        s.mask = subtree(skeleton, l.mask);
        layers_.push_back(std::move(s));
    }
}

void AnimPlayer::set(const std::string& parameter, float value) {
    const auto it = values_.find(parameter);
    if (it == values_.end()) {
        if (graph_->triggers.contains(parameter)) {
            if (value != 0.0f)
                set_triggers_.insert(parameter);
            else
                set_triggers_.erase(parameter);
            return;
        }
        throw AnimationError("the animation graph has no parameter '" + parameter + "'");
    }
    it->second = value;
}

void AnimPlayer::trigger(const std::string& name) {
    if (!graph_->triggers.contains(name))
        throw AnimationError("the animation graph has no trigger '" + name + "'");
    set_triggers_.insert(name);
}

float AnimPlayer::get(const std::string& parameter) const {
    if (const auto it = values_.find(parameter); it != values_.end()) return it->second;
    if (graph_->triggers.contains(parameter)) return set_triggers_.contains(parameter) ? 1.f : 0.f;
    throw AnimationError("the animation graph has no parameter '" + parameter + "'");
}

const std::string& AnimPlayer::state(size_t layer) const {
    static const std::string none;
    if (layer >= layers_.size()) return none;
    return graph_->layers[layer].states[size_t(layers_[layer].now.state)].name;
}

bool AnimPlayer::holds(const AnimCondition& c) const {
    if (graph_->triggers.contains(c.parameter)) return set_triggers_.contains(c.parameter);
    const float v = values_.at(c.parameter);
    switch (c.op) {
        case AnimCondition::Op::Set:
            return v != 0.0f;
        case AnimCondition::Op::Eq:
            return v == c.value;
        case AnimCondition::Op::Ne:
            return v != c.value;
        case AnimCondition::Op::Lt:
            return v < c.value;
        case AnimCondition::Op::Le:
            return v <= c.value;
        case AnimCondition::Op::Gt:
            return v > c.value;
        case AnimCondition::Op::Ge:
            return v >= c.value;
    }
    return false;
}

std::vector<std::pair<const Clip*, float>> AnimPlayer::blend_weights(const AnimState& s) const {
    if (!s.clip.empty()) {
        const Clip* c = find_(s.clip);
        return c ? std::vector<std::pair<const Clip*, float>>{{c, 1.0f}}
                 : std::vector<std::pair<const Clip*, float>>{};
    }
    // The two either side of the parameter's value (or the end one beyond them).
    std::vector<std::pair<const Clip*, float>> points;  // (clip, where), those there are
    for (const auto& [name, at] : s.clips)
        if (const Clip* c = find_(name)) points.emplace_back(c, at);
    if (points.empty()) return {};
    const float v = values_.at(s.blend);
    if (v <= points.front().second) return {{points.front().first, 1.0f}};
    if (v >= points.back().second) return {{points.back().first, 1.0f}};
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& [a, at_a] = points[i];
        const auto& [b, at_b] = points[i + 1];
        if (v > at_b) continue;
        const float t = at_b > at_a ? (v - at_a) / (at_b - at_a) : 1.0f;
        return {{a, 1.0f - t}, {b, t}};
    }
    return {{points.back().first, 1.0f}};
}

float AnimPlayer::cycle_time(const AnimState& s) const {
    float d = 0.0f;
    for (const auto& [clip, w] : blend_weights(s)) d += w * clip->duration();
    return d;
}

float AnimPlayer::state_speed(const AnimState& s) const {
    return s.speed_parameter.empty() ? s.speed : values_.at(s.speed_parameter);
}

float AnimPlayer::clip_time(const AnimState& s, const Clip& c, double cycles) const {
    const double point = s.loop ? cycles - std::floor(cycles) : std::min(cycles, 1.0);
    return float(point) * c.duration();
}

void AnimPlayer::sample(const AnimState& s, double cycles, Pose& pose,
                        std::vector<JointTransform>& out) {
    const auto weights = blend_weights(s);
    if (weights.empty()) {  // (its clips are missing: at rest)
        out = pose.rest_local();
        return;
    }
    // In step: each clip at the same point in its own cycle.
    pose.sample_local(*weights[0].first, clip_time(s, *weights[0].first, cycles), false, out);
    for (size_t i = 1; i < weights.size(); ++i) {
        pose.sample_local(*weights[i].first, clip_time(s, *weights[i].first, cycles), false, b_);
        mix(out, b_, weights[i].second);
    }
}

glm::vec3 AnimPlayer::RootTrack::offset(float time) const {
    if (at.empty()) return glm::vec3(0.0f);
    const float f = std::max(0.0f, time / step);
    const size_t i = std::min(size_t(f), at.size() - 1);
    const size_t k = std::min(i + 1, at.size() - 1);
    return glm::mix(at[i], at[k], std::clamp(f - float(i), 0.0f, 1.0f));
}

const AnimPlayer::RootTrack& AnimPlayer::root_track(const Clip& clip, Pose& pose) {
    if (const auto it = roots_.find(&clip); it != roots_.end()) return it->second;
    RootTrack t;
    const int frames = std::max(1, int(std::ceil(clip.duration() / t.step)));
    t.step = clip.duration() / float(frames);
    glm::vec3 first(0.0f);
    std::vector<JointTransform> joints;
    for (int f = 0; f <= frames; ++f) {
        pose.sample_local(clip, float(f) * t.step, false, joints);
        pose.set_local(joints);
        glm::vec3 p = glm::vec3(armature_ * pose.model()[size_t(motion_joint_)][3]);
        p.y = 0.0f;  // (horizontally)
        if (f == 0) first = p;
        t.at.push_back(p - first);
    }
    return roots_.emplace(&clip, std::move(t)).first->second;
}

glm::vec3 AnimPlayer::root_offset(const AnimState& s, double cycles, Pose& pose) {
    glm::vec3 out(0.0f);
    for (const auto& [clip, w] : blend_weights(s))
        out += w * root_track(*clip, pose).offset(clip_time(s, *clip, cycles));
    return out;
}

glm::vec3 AnimPlayer::root_moved(const AnimState& s, double from, double to, Pose& pose) {
    glm::vec3 out(0.0f);
    for (const auto& [clip, w] : blend_weights(s)) {
        const RootTrack& t = root_track(*clip, pose);
        glm::vec3 moved = t.offset(clip_time(s, *clip, to)) - t.offset(clip_time(s, *clip, from));
        if (s.loop) moved += float(std::floor(to) - std::floor(from)) * t.cycle();
        out += w * moved;
    }
    return out;
}

AnimPlayer::Step AnimPlayer::update(float dt, Pose& pose) {
    Step step;
    const bool roots = graph_->root_motion && motion_joint_ >= 0;
    glm::vec3 offset(0.0f);
    for (size_t li = 0; li < layers_.size(); ++li) {
        const AnimLayer& layer = graph_->layers[li];
        LayerState& ls = layers_[li];
        // A transition whose conditions hold (the first listed).
        for (const AnimTransition& t : layer.transitions) {
            if (t.from >= 0 ? t.from != ls.now.state : t.to == ls.now.state) continue;
            if (t.after && ls.now.cycles < double(*t.after)) continue;
            if (!std::all_of(t.when.begin(), t.when.end(),
                             [&](const AnimCondition& c) { return holds(c); }))
                continue;
            for (const AnimCondition& c : t.when) set_triggers_.erase(c.parameter);  // used
            ls.before = ls.now;
            ls.now = Playing{t.to, 0.0};
            ls.fade = t.blend > 0.0f ? 0.0f : 1.0f;
            ls.fade_time = t.blend;
            break;
        }
        // Time moves on (a state's cycles at its own speed), passing its events; what its
        // clips' roots moved (the first layer's).
        const auto advance = [&](Playing& p, bool events) {
            const AnimState& s = layer.states[size_t(p.state)];
            const float cycle = cycle_time(s);
            const double from = p.cycles;
            if (cycle > 0.0f) p.cycles += double(dt * state_speed(s) / cycle);
            if (events)
                for (const auto& [point, name] : s.events) {
                    // Passed: some k + point in [from, to) (once, if it doesn't loop).
                    const double first = s.loop ? std::ceil(from - point) + point : point;
                    for (double x = first; x < p.cycles; x += 1.0) {
                        if (x >= from) step.events.push_back(name);
                        if (!s.loop) break;
                    }
                }
            return roots && li == 0 ? root_moved(s, from, p.cycles, pose) : glm::vec3(0.0f);
        };
        const bool fading = ls.before.state >= 0;
        if (fading) {  // (both move it, by their shares)
            const glm::vec3 before = advance(ls.before, false);
            step.motion += glm::mix(before, advance(ls.now, true), ls.fade);
        } else {
            step.motion += advance(ls.now, true);
        }
        if (fading) {
            ls.fade = ls.fade_time > 0.0f ? std::min(1.0f, ls.fade + dt / ls.fade_time) : 1.0f;
            if (ls.fade >= 1.0f) ls.before = Playing{};
        }
        // The layer's pose: the state (fading in over the one before).
        std::vector<JointTransform>& out = li == 0 ? result_ : layer_;
        sample(layer.states[size_t(ls.now.state)], ls.now.cycles, pose, out);
        if (ls.before.state >= 0) {
            sample(layer.states[size_t(ls.before.state)], ls.before.cycles, pose, a_);
            mix(a_, out, ls.fade);
            out.swap(a_);
        }
        if (roots && li == 0) {
            offset = root_offset(layer.states[size_t(ls.now.state)], ls.now.cycles, pose);
            if (ls.before.state >= 0)
                offset = glm::mix(
                    root_offset(layer.states[size_t(ls.before.state)], ls.before.cycles, pose),
                    offset, ls.fade);
        }
        if (li > 0) {
            const float w = layer.weight_parameter.empty()
                                ? layer.weight
                                : std::clamp(values_.at(layer.weight_parameter), 0.0f, 1.0f);
            mix(result_, layer_, w, &ls.mask);
        }
    }
    pose.set_local(result_);
    if (roots)  // (the motion is the character's: kept out of its pose)
        pose.shift(glm::inverse(glm::mat3(armature_)) * -offset);
    return step;
}

}  // namespace midgard
