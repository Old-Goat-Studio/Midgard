// SPDX-License-Identifier: Apache-2.0
// Sound events (milestone 5.14): the named sounds a game plays.
//
// An event holds one or more sounds (its variations: one is picked each time, by weight), a bus,
// and ranges for its volume and pitch (a value is picked within each every time it plays, so a
// footstep heard a hundred times never sounds mechanical). It may loop, wait a `cooldown` before
// it plays again, and hold at most `max_voices` at once (the oldest stops for a new one). Picks
// come from a seeded stream (midgard/det.h): the same game makes the same picks every run.
//
// The sounds are content ids ("snd:<path>"), read on first use through a callback (the game's
// package); one that can't be read is reported once, and the event plays its other sounds.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/audio.h"
#include "midgard/det.h"

namespace midgard::audio {

struct SoundEvent {
    std::string id, name;
    struct Variant {
        std::string sound;  // a content id ("snd:<path>")
        float weight = 1.0f;
    };
    std::vector<Variant> sounds;
    Bus bus = Bus::Effects;
    float volume_min = 1.0f, volume_max = 1.0f;  // linear
    float pitch_min = 1.0f, pitch_max = 1.0f;    // 2: an octave up
    bool loop = false;
    float cooldown = 0.0f;  // seconds before it plays again (0: any time)
    int max_voices = 0;     // at once (0: no limit)
    // Where it is heard from (step 4: 3D sound): placed sounds fade between these distances.
    bool spatial = true;
    float min_distance = 1.0f, max_distance = 40.0f;
};

// An event from its row (sound_event: id, name, event JSON). AudioError if it isn't one.
SoundEvent sound_event_from_json(const std::string& id, const std::string& name,
                                 std::string_view json);

class SoundPlayer {
public:
    using Bytes = std::function<std::vector<std::uint8_t>(const std::string& content_id)>;

    SoundPlayer(Mixer& mixer, Bytes bytes, std::uint64_t seed = 0);

    void add(SoundEvent event);  // (replaces one with its id)
    void remove(const std::string& id);
    void clear();
    // By id, else by name.
    const SoundEvent* find(std::string_view id_or_name) const;
    const std::map<std::string, SoundEvent>& events() const { return events_; }

    struct Play {
        float volume = 1.0f;  // times the event's
        // Where it's heard from (a placed event; set_position moves it). None, or an event
        // that isn't spatial: heard the same everywhere.
        std::optional<glm::dvec3> at;
        float fade_in = 0.0f;  // seconds rising from silence
    };

    // Who hears (step 4): where, and which way they face. Placed sounds fade with distance
    // (distance_gain, by the event's near and far), pan by direction, and are muffled while
    // `blocked` says something solid is between them and the listener (asked about ten times a
    // second for each; it eases over about 0.15 s).
    struct Listener {
        glm::dvec3 position{0.0};
        glm::vec3 forward{0.0f, 0.0f, -1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
    };
    using Blocked =
        std::function<bool(VoiceId voice, const glm::dvec3& from, const glm::dvec3& to)>;
    void set_listener(const Listener& listener);
    void set_blocked(Blocked blocked) { blocked_ = std::move(blocked); }
    void set_position(VoiceId voice, const glm::dvec3& at);
    // How muffled a placed voice is now (0 clear, 1 behind a wall), and how loud for its
    // distance: for tests and the debug overlay.
    float occlusion(VoiceId voice) const;
    float distance_gain(VoiceId voice) const;
    // Plays an event (by id or name). 0 when it doesn't: unknown, cooling down, or none of its
    // sounds can be read (problems() says which).
    VoiceId play(std::string_view event, const Play& play);
    // (Not a default argument: Play isn't complete until the class is, for clang and GCC.)
    VoiceId play(std::string_view event) { return play(event, Play{}); }
    void stop(VoiceId voice);
    void stop_event(const std::string& id);
    void stop_all();
    bool playing(VoiceId voice) const;
    void set_volume(VoiceId voice, float volume);  // times the event's pick
    void fade_out(VoiceId voice, float seconds);   // then it stops
    double seconds(VoiceId voice) const;           // how long its sound lasts
    // The events playing now (by id), one entry a voice.
    std::vector<std::string> playing_events() const;
    struct VoiceInfo {
        VoiceId voice = 0;
        std::string event;
        bool spatial = false;
        float gain = 1.0f, occlusion = 0.0f;
    };
    std::vector<VoiceInfo> voices() const;

    // Time passes: cooldowns, and voices that have finished let go.
    void update(double dt);
    // Problems since last asked (a sound that can't be read, an unknown event), once each.
    std::vector<std::string> take_problems();

    Mixer& mixer() { return mixer_; }

private:
    std::optional<SoundId> sound(const std::string& content_id);

    Mixer& mixer_;
    Bytes bytes_;
    det::Rng rng_;
    std::map<std::string, SoundEvent> events_;
    std::map<std::string, double> ready_at_;  // event id -> when its cooldown is over
    double now_ = 0.0;
    struct Voice {
        std::string event;
        float picked = 1.0f;  // the volume picked for it
        float volume = 1.0f;  // the player's (Play::volume, set_volume)
        bool spatial = false;
        glm::dvec3 at{0.0};
        float nearest = 1.0f, farthest = 40.0f;
        float gain = 1.0f;                       // for its distance
        float occlusion = 0.0f, blocked = 0.0f;  // now, and where it's easing to
        double check_at = 0.0;
    };
    void place(VoiceId id, Voice& v, double dt);
    std::optional<Listener> listener_;
    Blocked blocked_;
    std::map<VoiceId, Voice> voices_;
    std::map<std::string, std::string> failed_;  // content id -> why
    std::vector<std::string> problems_;
};

}  // namespace midgard::audio
