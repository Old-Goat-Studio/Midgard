// SPDX-License-Identifier: Apache-2.0
// Ambience (milestone 5.14): the background sound of a place, layered by the time of day.
//
// An ambience set (the ambience table, migration 16) is for somewhere: scenes, world types and
// biomes (empty: anywhere), chosen as music is (the one naming the most that hold, then the
// highest priority; a change settles for a couple of seconds, then crossfades). It has layers,
// sound events that loop under everything, each at its own times of day ("dawn", "day", "dusk",
// "night"; none: always) and volume: birdsong by day, crickets and an owl's wind by night, each
// fading in and out over the set's crossfade as dawn and dusk come. And sprinkles: one-shot
// events now and then (every so many seconds, between two numbers), at their times of day,
// played at a random spot around the listener (between two distances), so a woodpecker is heard
// somewhere off to the left, then later behind.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "midgard/det.h"
#include "midgard/music.h"
#include "midgard/sound_events.h"

namespace midgard::audio {

struct AmbienceSet {
    std::string id, name;
    struct Layer {
        std::string event;
        std::vector<std::string> times;  // empty: always
        float volume = 1.0f;
    };
    std::vector<Layer> layers;
    struct Sprinkle {
        std::string event;
        std::vector<std::string> times;
        float every_min = 10.0f, every_max = 30.0f;       // seconds between
        float distance_min = 5.0f, distance_max = 30.0f;  // metres from the listener
    };
    std::vector<Sprinkle> sprinkles;
    std::vector<std::string> scenes, world_types, biomes;
    int priority = 0;
    float crossfade = 4.0f;  // seconds
};

// A set from its row (ambience: id, name, ambience JSON). AudioError if it isn't one.
AmbienceSet ambience_from_json(const std::string& id, const std::string& name,
                               std::string_view json);

class AmbienceDirector {
public:
    AmbienceDirector(SoundPlayer& sounds, std::uint64_t seed = 0);

    void add(AmbienceSet set);
    void clear();
    const std::map<std::string, AmbienceSet>& sets() const { return sets_; }
    const AmbienceSet* choose(const MusicContext& context) const;

    void set_context(const MusicContext& context) { context_ = context; }
    // A script's choice (by id or name; empty: silence) until release(). False if unknown.
    bool force(const std::string& set);
    void release() { forced_.reset(); }

    void update(double dt);

    const std::string& playing() const { return current_; }  // the set (empty: none)
    // The layers sounding now (their events, in the set's order).
    std::vector<std::string> layers() const;
    int sprinkled() const { return sprinkled_; }  // sprinkles played so far

    static constexpr double kSettle = 2.0;

private:
    void start(const AmbienceSet* set);

    SoundPlayer& sounds_;
    det::Rng rng_;
    std::map<std::string, AmbienceSet> sets_;
    MusicContext context_;
    std::optional<std::string> forced_;
    std::string current_, candidate_;
    double candidate_since_ = 0.0;
    bool chosen_once_ = false;
    double now_ = 0.0;
    std::vector<VoiceId> layer_voices_;  // a voice for each layer of the set (0: silent now)
    std::vector<double> sprinkle_at_;    // when each sprinkle is due next
    int sprinkled_ = 0;
};

}  // namespace midgard::audio
