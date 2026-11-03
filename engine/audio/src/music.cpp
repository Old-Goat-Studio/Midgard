// SPDX-License-Identifier: Apache-2.0
#include "midgard/music.h"

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

bool fits(const std::vector<std::string>& wanted, const std::string& value) {
    return wanted.empty() || std::find(wanted.begin(), wanted.end(), value) != wanted.end();
}

}  // namespace

Playlist playlist_from_json(const std::string& id, const std::string& name, std::string_view text) {
    Playlist p;
    p.id = id;
    p.name = name;
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (!j.is_object()) throw AudioError("it isn't an object");
        p.tracks = strings(j, "tracks");
        p.scenes = strings(j, "scenes");
        p.world_types = strings(j, "world_types");
        p.biomes = strings(j, "biomes");
        p.times = strings(j, "times");
        for (const std::string& t : p.times)
            if (t != "dawn" && t != "day" && t != "dusk" && t != "night")
                throw AudioError("a time of day is dawn, day, dusk or night, not " + t);
        const std::string order = j.value("order", std::string("in_order"));
        if (order != "in_order" && order != "shuffle")
            throw AudioError("order is in_order or shuffle, not " + order);
        p.shuffle = order == "shuffle";
        p.priority = j.value("priority", 0);
        p.crossfade = std::clamp(j.value("crossfade", 3.0f), 0.0f, 60.0f);
        if (const auto gap = j.find("gap"); gap != j.end()) {
            if (gap->is_number()) {
                p.gap_min = p.gap_max = gap->get<float>();
            } else if (gap->is_array() && gap->size() == 2) {
                p.gap_min = (*gap)[0].get<float>();
                p.gap_max = (*gap)[1].get<float>();
            } else {
                throw AudioError("gap is seconds, or [least, most]");
            }
            if (p.gap_min > p.gap_max) std::swap(p.gap_min, p.gap_max);
            p.gap_min = std::max(p.gap_min, 0.0f);
            p.gap_max = std::max(p.gap_max, 0.0f);
        }
    } catch (const nlohmann::json::exception& e) {
        throw AudioError("playlist '" + name + "': " + e.what());
    } catch (const AudioError& e) {
        throw AudioError("playlist '" + name + "': " + e.what());
    }
    return p;
}

std::optional<int> fit(const std::vector<std::string>& scenes,
                       const std::vector<std::string>& world_types,
                       const std::vector<std::string>& biomes,
                       const std::vector<std::string>& times, const MusicContext& c) {
    if (!fits(scenes, c.scene) || !fits(world_types, c.world_type) || !fits(biomes, c.biome) ||
        !fits(times, c.time))
        return std::nullopt;
    return int(!scenes.empty()) + int(!world_types.empty()) + int(!biomes.empty()) +
           int(!times.empty());
}

std::string time_of_day(double phase) {
    phase -= std::floor(phase);  // 0 to 1
    const auto close_to = [&](double at) {
        const double d = std::abs(phase - at);
        return std::min(d, 1.0 - d) <= 0.04;
    };
    if (close_to(0.0)) return "dawn";
    if (close_to(0.5)) return "dusk";
    return phase < 0.5 ? "day" : "night";
}

MusicDirector::MusicDirector(SoundPlayer& sounds, std::uint64_t seed)
    : sounds_(sounds), rng_(det::Rng(seed).split(det::tag("music"))) {}

void MusicDirector::add(Playlist playlist) {
    const std::string id = playlist.id;
    playlists_[id] = std::move(playlist);
}

void MusicDirector::clear() {
    start(nullptr);
    playlists_.clear();
}

const Playlist* MusicDirector::choose(const MusicContext& c) const {
    const Playlist* best = nullptr;
    std::tuple<int, int> best_rank{-1, 0};
    for (const auto& [id, p] : playlists_) {  // (by id: the same choice every time)
        const auto named = fit(p.scenes, p.world_types, p.biomes, p.times, c);
        if (p.tracks.empty() || !named) continue;
        const std::tuple<int, int> rank{*named, p.priority};
        if (!best || rank > best_rank) {
            best = &p;
            best_rank = rank;
        }
    }
    return best;
}

void MusicDirector::set_context(const MusicContext& context) {
    context_ = context;
}

bool MusicDirector::force(const std::string& playlist) {
    if (playlist.empty()) {
        forced_ = std::string();
        return true;
    }
    for (const auto& [id, p] : playlists_)
        if (id == playlist || p.name == playlist) {
            forced_ = id;
            return true;
        }
    return false;
}

void MusicDirector::release() {
    forced_.reset();
}

VoiceId MusicDirector::stinger(std::string_view event, float duck) {
    const VoiceId voice = sounds_.play(event);
    if (voice) {
        stingers_.push_back({voice, now_ + sounds_.seconds(voice)});
        duck_ = std::min(duck_, std::clamp(duck, 0.0f, 1.0f));
    }
    return voice;
}

void MusicDirector::start(const Playlist* playlist) {
    // The old track fades out over the new playlist's crossfade (or, into silence, its own).
    const auto old = playlists_.find(current_);
    const float fade = playlist                  ? playlist->crossfade
                       : old != playlists_.end() ? old->second.crossfade
                                                 : 0.0f;
    if (voice_) sounds_.fade_out(voice_, fade);
    voice_ = 0;
    current_ = playlist ? playlist->id : std::string();
    track_.clear();
    next_ = 0;
    gap_until_ = -1.0;
    if (playlist) next_track(playlist->crossfade);
}

void MusicDirector::next_track(float fade_in) {
    const auto it = playlists_.find(current_);
    if (it == playlists_.end() || it->second.tracks.empty()) return;
    const Playlist& p = it->second;
    size_t index = next_ % p.tracks.size();
    if (p.shuffle && p.tracks.size() > 1) {
        // Any but the one just played.
        do {
            index = size_t(rng_.below(p.tracks.size()));
        } while (p.tracks[index] == track_);
    } else {
        next_ = index + 1;
    }
    SoundPlayer::Play play;
    play.fade_in = fade_in;
    play.volume = duck_now_;
    track_ = p.tracks[index];
    voice_ = sounds_.play(track_, play);
    if (!voice_) gap_until_ = now_ + 1.0;  // (it couldn't play: the next, in a second)
}

void MusicDirector::update(double dt) {
    now_ += std::max(dt, 0.0);
    // What should play: a script's choice, else what fits best (a change settles first).
    std::string wanted;
    if (forced_) {
        wanted = *forced_;
    } else if (const Playlist* p = choose(context_)) {
        wanted = p->id;
    }
    if (wanted != current_) {
        if (!chosen_once_ || forced_) {
            start(wanted.empty() ? nullptr : &playlists_.at(wanted));
            chosen_once_ = true;
        } else if (wanted != candidate_) {
            candidate_ = wanted;
            candidate_since_ = now_;
        } else if (now_ - candidate_since_ >= kSettle) {
            start(wanted.empty() ? nullptr : &playlists_.at(wanted));
        }
    } else {
        candidate_ = current_;
        chosen_once_ = true;
    }
    // The next track when one ends (after its gap).
    if (!current_.empty()) {
        if (voice_ && !sounds_.playing(voice_)) {
            voice_ = 0;
            const Playlist& p = playlists_.at(current_);
            gap_until_ = now_ + p.gap_min + rng_.uniform() * double(p.gap_max - p.gap_min);
        }
        if (!voice_ && now_ >= gap_until_) next_track(0.0f);
    }
    // Stingers: the music dips under them (quickly), and comes back after (gently).
    stingers_.erase(std::remove_if(stingers_.begin(), stingers_.end(),
                                   [&](const auto& s) {
                                       return now_ >= s.second || !sounds_.playing(s.first);
                                   }),
                    stingers_.end());
    if (stingers_.empty()) duck_ = 1.0f;
    const double rate = duck_ < duck_now_ ? 1.0 / 0.25 : 1.0 / 0.75;  // (of the whole range)
    const float step = float(std::min(1.0, dt * rate));
    duck_now_ =
        duck_ < duck_now_ ? std::max(duck_, duck_now_ - step) : std::min(duck_, duck_now_ + step);
    if (voice_) sounds_.set_volume(voice_, duck_now_);
}

}  // namespace midgard::audio
