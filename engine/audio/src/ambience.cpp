// SPDX-License-Identifier: Apache-2.0
#include "midgard/ambience.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include <nlohmann/json.hpp>

namespace midgard::audio {
namespace {

std::vector<std::string> strings(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    const auto it = j.find(key);
    if (it == j.end()) return out;
    if (!it->is_array()) throw AudioError(std::string(key) + " is a list");
    for (const auto& v : *it)
        if (v.is_string() && !v.get<std::string>().empty()) out.push_back(v.get<std::string>());
    return out;
}

void check_times(const std::vector<std::string>& times) {
    for (const std::string& t : times)
        if (t != "dawn" && t != "day" && t != "dusk" && t != "night")
            throw AudioError("a time of day is dawn, day, dusk or night, not " + t);
}

// A number, or [least, most].
std::pair<float, float> between(const nlohmann::json& j, const char* key, float a, float b) {
    const auto it = j.find(key);
    if (it == j.end()) return {a, b};
    if (it->is_number()) return {it->get<float>(), it->get<float>()};
    if (it->is_array() && it->size() == 2) {
        float x = (*it)[0].get<float>(), y = (*it)[1].get<float>();
        if (x > y) std::swap(x, y);
        return {std::max(x, 0.0f), std::max(y, 0.0f)};
    }
    throw AudioError(std::string(key) + " is a number, or [least, most]");
}

bool now_fits(const std::vector<std::string>& times, const MusicContext& c) {
    return fit({}, {}, {}, times, c).has_value();
}

}  // namespace

AmbienceSet ambience_from_json(const std::string& id, const std::string& name,
                               std::string_view text) {
    AmbienceSet a;
    a.id = id;
    a.name = name;
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (!j.is_object()) throw AudioError("it isn't an object");
        for (const auto& l : j.value("layers", nlohmann::json::array())) {
            AmbienceSet::Layer layer;
            if (l.is_string()) {
                layer.event = l.get<std::string>();
            } else {
                layer.event = l.value("event", std::string());
                layer.times = strings(l, "times");
                layer.volume = std::clamp(l.value("volume", 1.0f), 0.0f, 4.0f);
            }
            check_times(layer.times);
            if (!layer.event.empty()) a.layers.push_back(std::move(layer));
        }
        for (const auto& s : j.value("sprinkles", nlohmann::json::array())) {
            AmbienceSet::Sprinkle sprinkle;
            sprinkle.event = s.value("event", std::string());
            sprinkle.times = strings(s, "times");
            check_times(sprinkle.times);
            std::tie(sprinkle.every_min, sprinkle.every_max) = between(s, "every", 10.0f, 30.0f);
            sprinkle.every_min = std::max(sprinkle.every_min, 0.1f);
            sprinkle.every_max = std::max(sprinkle.every_max, sprinkle.every_min);
            std::tie(sprinkle.distance_min, sprinkle.distance_max) =
                between(s, "distance", 5.0f, 30.0f);
            if (!sprinkle.event.empty()) a.sprinkles.push_back(std::move(sprinkle));
        }
        a.scenes = strings(j, "scenes");
        a.world_types = strings(j, "world_types");
        a.biomes = strings(j, "biomes");
        a.priority = j.value("priority", 0);
        a.crossfade = std::clamp(j.value("crossfade", 4.0f), 0.0f, 60.0f);
    } catch (const nlohmann::json::exception& e) {
        throw AudioError("ambience '" + name + "': " + e.what());
    } catch (const AudioError& e) {
        throw AudioError("ambience '" + name + "': " + e.what());
    }
    return a;
}

AmbienceDirector::AmbienceDirector(SoundPlayer& sounds, std::uint64_t seed)
    : sounds_(sounds), rng_(det::Rng(seed).split(det::tag("ambience"))) {}

void AmbienceDirector::add(AmbienceSet set) {
    const std::string id = set.id;
    sets_[id] = std::move(set);
}

void AmbienceDirector::replace(std::vector<AmbienceSet> sets) {
    std::map<std::string, AmbienceSet> next;
    for (AmbienceSet& a : sets) next[a.id] = std::move(a);
    const auto old = sets_.find(current_);
    const auto now = next.find(current_);
    const bool same = old != sets_.end() && now != next.end() && old->second == now->second;
    sets_ = std::move(next);
    if (!current_.empty() && !same) start(now != sets_.end() ? &now->second : nullptr);
}

void AmbienceDirector::clear() {
    start(nullptr);
    sets_.clear();
}

const AmbienceSet* AmbienceDirector::choose(const MusicContext& c) const {
    const AmbienceSet* best = nullptr;
    std::tuple<int, int> best_rank{-1, 0};
    for (const auto& [id, a] : sets_) {
        const auto named = fit(a.scenes, a.world_types, a.biomes, {}, c);
        if (!named || (a.layers.empty() && a.sprinkles.empty())) continue;
        const std::tuple<int, int> rank{*named, a.priority};
        if (!best || rank > best_rank) {
            best = &a;
            best_rank = rank;
        }
    }
    return best;
}

bool AmbienceDirector::force(const std::string& set) {
    if (set.empty()) {
        forced_ = std::string();
        return true;
    }
    for (const auto& [id, a] : sets_)
        if (id == set || a.name == set) {
            forced_ = id;
            return true;
        }
    return false;
}

void AmbienceDirector::start(const AmbienceSet* set) {
    const auto old = sets_.find(current_);
    const float fade = set ? set->crossfade : old != sets_.end() ? old->second.crossfade : 0.0f;
    for (const VoiceId v : layer_voices_)
        if (v) sounds_.fade_out(v, fade);
    current_ = set ? set->id : std::string();
    layer_voices_.assign(set ? set->layers.size() : 0, 0);
    sprinkle_at_.clear();
    if (set)
        for (const AmbienceSet::Sprinkle& s : set->sprinkles)
            sprinkle_at_.push_back(now_ + s.every_min +
                                   rng_.uniform() * double(s.every_max - s.every_min));
}

void AmbienceDirector::update(double dt) {
    now_ += std::max(dt, 0.0);
    std::string wanted;
    if (forced_) {
        wanted = *forced_;
    } else if (const AmbienceSet* a = choose(context_)) {
        wanted = a->id;
    }
    if (wanted != current_) {
        if (!chosen_once_ || forced_) {
            start(wanted.empty() ? nullptr : &sets_.at(wanted));
            chosen_once_ = true;
        } else if (wanted != candidate_) {
            candidate_ = wanted;
            candidate_since_ = now_;
        } else if (now_ - candidate_since_ >= kSettle) {
            start(wanted.empty() ? nullptr : &sets_.at(wanted));
        }
    } else {
        candidate_ = current_;
        chosen_once_ = true;
    }
    const auto it = sets_.find(current_);
    if (it == sets_.end()) return;
    const AmbienceSet& set = it->second;
    // Each layer in or out by the time of day (fading over the set's crossfade).
    for (size_t i = 0; i < set.layers.size(); ++i) {
        const AmbienceSet::Layer& layer = set.layers[i];
        VoiceId& voice = layer_voices_[i];
        if (voice && !sounds_.playing(voice)) voice = 0;  // (one that doesn't loop: again)
        const bool wanted_now = now_fits(layer.times, context_);
        if (wanted_now && !voice) {
            SoundPlayer::Play play;
            play.volume = layer.volume;
            play.fade_in = set.crossfade;
            voice = sounds_.play(layer.event, play);
        } else if (!wanted_now && voice) {
            sounds_.fade_out(voice, set.crossfade);
            voice = 0;
        }
    }
    // Sprinkles, when due, somewhere around the listener.
    for (size_t i = 0; i < set.sprinkles.size(); ++i) {
        const AmbienceSet::Sprinkle& s = set.sprinkles[i];
        if (now_ < sprinkle_at_[i]) continue;
        sprinkle_at_[i] = now_ + s.every_min + rng_.uniform() * double(s.every_max - s.every_min);
        if (!now_fits(s.times, context_)) continue;
        SoundPlayer::Play play;
        if (const auto& ear = sounds_.listener()) {
            const glm::dvec3 up = glm::normalize(glm::dvec3(ear->up));
            const glm::dvec3 other =
                std::abs(up.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            const glm::dvec3 a = glm::normalize(glm::cross(up, other));
            const glm::dvec3 b = glm::cross(up, a);
            const double turn = rng_.uniform() * 6.283185307179586;
            const double away =
                s.distance_min + rng_.uniform() * double(s.distance_max - s.distance_min);
            play.at = ear->position + (a * std::cos(turn) + b * std::sin(turn)) * away;
        }
        if (sounds_.play(s.event, play)) ++sprinkled_;
    }
}

std::vector<std::string> AmbienceDirector::layers() const {
    std::vector<std::string> out;
    const auto it = sets_.find(current_);
    if (it == sets_.end()) return out;
    for (size_t i = 0; i < layer_voices_.size(); ++i)
        if (layer_voices_[i] && sounds_.playing(layer_voices_[i]))
            out.push_back(it->second.layers[i].event);
    return out;
}

}  // namespace midgard::audio
