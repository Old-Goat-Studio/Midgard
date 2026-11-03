// SPDX-License-Identifier: Apache-2.0
#include "midgard/sound_events.h"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

namespace midgard::audio {

namespace {

std::pair<float, float> range(const nlohmann::json& j, const char* key, float low, float high,
                              float fallback) {
    const auto it = j.find(key);
    if (it == j.end()) return {fallback, fallback};
    float a = fallback, b = fallback;
    if (it->is_number()) {
        a = b = it->get<float>();
    } else if (it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
        a = (*it)[0].get<float>();
        b = (*it)[1].get<float>();
    } else {
        throw AudioError(std::string(key) + " is a number or [lowest, highest]");
    }
    if (a > b) std::swap(a, b);
    return {std::clamp(a, low, high), std::clamp(b, low, high)};
}

}  // namespace

SoundEvent sound_event_from_json(const std::string& id, const std::string& name,
                                 std::string_view text) {
    SoundEvent e;
    e.id = id;
    e.name = name;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& ex) {
        throw AudioError("sound event '" + name + "': " + ex.what());
    }
    if (!j.is_object()) throw AudioError("sound event '" + name + "' isn't an object");
    try {
        for (const auto& v : j.value("sounds", nlohmann::json::array())) {
            SoundEvent::Variant variant;
            if (v.is_string()) {
                variant.sound = v.get<std::string>();
            } else {
                variant.sound = v.value("sound", std::string());
                variant.weight = std::max(v.value("weight", 1.0f), 0.0f);
            }
            if (!variant.sound.empty()) e.sounds.push_back(std::move(variant));
        }
        const std::string bus = j.value("bus", std::string("effects"));
        const auto chosen = bus_from_name(bus);
        if (!chosen || *chosen == Bus::Master)
            throw AudioError("bus '" + bus +
                             "' isn't music, effects, voice, ambience or interface");
        e.bus = *chosen;
        std::tie(e.volume_min, e.volume_max) = range(j, "volume", 0.0f, 4.0f, 1.0f);
        std::tie(e.pitch_min, e.pitch_max) = range(j, "pitch", 0.1f, 4.0f, 1.0f);
        e.loop = j.value("loop", false);
        e.cooldown = std::max(j.value("cooldown", 0.0f), 0.0f);
        e.max_voices = std::max(j.value("max_voices", 0), 0);
        e.spatial = j.value("spatial", true);
        e.min_distance = std::max(j.value("min_distance", 1.0f), 0.01f);
        e.max_distance = std::max(j.value("max_distance", 40.0f), e.min_distance);
    } catch (const nlohmann::json::exception& ex) {
        throw AudioError("sound event '" + name + "': " + ex.what());
    } catch (const AudioError& ex) {
        throw AudioError("sound event '" + name + "': " + ex.what());
    }
    return e;
}

SoundPlayer::SoundPlayer(Mixer& mixer, Bytes bytes, std::uint64_t seed)
    : mixer_(mixer), bytes_(std::move(bytes)), rng_(det::Rng(seed).split(det::tag("sound"))) {}

void SoundPlayer::add(SoundEvent event) {
    const std::string id = event.id;
    events_[id] = std::move(event);
}

void SoundPlayer::remove(const std::string& id) {
    stop_event(id);
    events_.erase(id);
}

void SoundPlayer::clear() {
    stop_all();
    events_.clear();
    ready_at_.clear();
}

const SoundEvent* SoundPlayer::find(std::string_view id_or_name) const {
    if (const auto it = events_.find(std::string(id_or_name)); it != events_.end())
        return &it->second;
    for (const auto& [id, e] : events_)
        if (e.name == id_or_name) return &e;
    return nullptr;
}

std::optional<SoundId> SoundPlayer::sound(const std::string& content_id) {
    if (const auto known = mixer_.find(content_id)) return known;
    if (failed_.contains(content_id)) return std::nullopt;
    try {
        const std::vector<std::uint8_t> data =
            bytes_ ? bytes_(content_id) : std::vector<std::uint8_t>();
        if (data.empty()) throw AudioError("not in the game");
        return mixer_.load(content_id, data);
    } catch (const std::exception& e) {
        failed_[content_id] = e.what();
        problems_.push_back("sound '" + content_id + "': " + e.what());
        return std::nullopt;
    }
}

VoiceId SoundPlayer::play(std::string_view name, const Play& play) {
    const SoundEvent* e = find(name);
    if (!e) {
        const std::string problem = "no sound event '" + std::string(name) + "'";
        if (std::find(problems_.begin(), problems_.end(), problem) == problems_.end())
            problems_.push_back(problem);
        return 0;
    }
    if (const auto it = ready_at_.find(e->id); it != ready_at_.end() && now_ < it->second)
        return 0;  // (cooling down)
    // The sounds that can be read, by weight.
    std::vector<std::pair<SoundId, float>> choices;
    float total = 0.0f;
    for (const SoundEvent::Variant& v : e->sounds)
        if (v.weight > 0.0f)
            if (const auto id = sound(v.sound)) {
                choices.emplace_back(*id, v.weight);
                total += v.weight;
            }
    if (choices.empty()) return 0;
    float pick = float(rng_.uniform()) * total;
    SoundId chosen = choices.back().first;
    for (const auto& [id, weight] : choices) {
        if (pick < weight) {
            chosen = id;
            break;
        }
        pick -= weight;
    }
    const auto between = [&](float a, float b) { return a + float(rng_.uniform()) * (b - a); };
    const float volume = between(e->volume_min, e->volume_max);
    PlayOptions options;
    options.bus = e->bus;
    options.volume = volume * std::max(play.volume, 0.0f);
    options.pitch = between(e->pitch_min, e->pitch_max);
    options.loop = e->loop;
    // At most max_voices of it: the oldest goes for the new one.
    if (e->max_voices > 0) {
        std::vector<VoiceId> mine;
        for (const auto& [voice, v] : voices_)
            if (v.event == e->id && mixer_.playing(voice)) mine.push_back(voice);
        while (int(mine.size()) >= e->max_voices) {
            stop(mine.front());
            mine.erase(mine.begin());
        }
    }
    const VoiceId voice = mixer_.play(chosen, options);
    voices_[voice] = {e->id, volume};
    if (e->cooldown > 0.0f) ready_at_[e->id] = now_ + e->cooldown;
    return voice;
}

void SoundPlayer::stop(VoiceId voice) {
    mixer_.stop(voice);
    voices_.erase(voice);
}

void SoundPlayer::stop_event(const std::string& id) {
    for (auto it = voices_.begin(); it != voices_.end();) {
        if (it->second.event == id) {
            mixer_.stop(it->first);
            it = voices_.erase(it);
        } else {
            ++it;
        }
    }
}

void SoundPlayer::stop_all() {
    for (const auto& [voice, v] : voices_) mixer_.stop(voice);
    voices_.clear();
}

bool SoundPlayer::playing(VoiceId voice) const {
    return voices_.contains(voice) && mixer_.playing(voice);
}

void SoundPlayer::set_volume(VoiceId voice, float volume) {
    if (const auto it = voices_.find(voice); it != voices_.end())
        mixer_.set_voice_volume(voice, it->second.picked * std::max(volume, 0.0f));
}

std::vector<std::string> SoundPlayer::playing_events() const {
    std::vector<std::string> out;
    for (const auto& [voice, v] : voices_)
        if (mixer_.playing(voice)) out.push_back(v.event);
    return out;
}

void SoundPlayer::update(double dt) {
    now_ += std::max(dt, 0.0);
    for (auto it = voices_.begin(); it != voices_.end();)
        it = mixer_.playing(it->first) ? std::next(it) : voices_.erase(it);
    mixer_.update();
}

std::vector<std::string> SoundPlayer::take_problems() {
    return std::exchange(problems_, {});
}

}  // namespace midgard::audio
