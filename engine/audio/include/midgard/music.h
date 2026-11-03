// SPDX-License-Identifier: Apache-2.0
// Music (milestone 5.14): playlists chosen by where the player is, crossfaded.
//
// A playlist (the music_playlist table, migration 15) is a list of tracks (sound events, played
// on their own bus, music's) and when it plays: in which scenes, world types and biomes, and at
// which times of day ("dawn", "day", "dusk", "night"); an empty list is anywhere, any time. The
// director plays the playlist that fits best: the one naming the most of those that all hold,
// then the highest priority. A new choice must hold for a couple of seconds before the music
// changes (walking along a biome's edge doesn't flip it), then the old track fades out as the
// new one fades in over the new playlist's crossfade (exactly: miniaudio's fades, on the audio
// thread). Tracks follow each other in order or shuffled, with a gap of silence between if it
// says. Scripts can choose a playlist themselves, stop the music, and play stingers: short
// pieces over the music, which dips under them and comes back after.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "midgard/det.h"
#include "midgard/sound_events.h"

namespace midgard::audio {

struct Playlist {
    bool operator==(const Playlist&) const = default;
    std::string id, name;
    std::vector<std::string> tracks;  // sound events' ids (or names)
    bool shuffle = false;
    // When it fits (empty: any).
    std::vector<std::string> scenes, world_types, biomes, times;
    int priority = 0;
    float crossfade = 3.0f;                // seconds
    float gap_min = 0.0f, gap_max = 0.0f;  // seconds of silence between tracks
};

// A playlist from its row (music_playlist: id, name, playlist JSON). AudioError if it isn't one.
Playlist playlist_from_json(const std::string& id, const std::string& name, std::string_view json);

// Where the player is, for choosing music.
struct MusicContext {
    std::string scene, world_type, biome, time;  // time: dawn, day, dusk or night
    bool operator==(const MusicContext&) const = default;
};

// How well conditions fit where the player is (music's, ambience's): how many of the lists
// name something (an empty one is any), or none when one of them doesn't hold.
std::optional<int> fit(const std::vector<std::string>& scenes,
                       const std::vector<std::string>& world_types,
                       const std::vector<std::string>& biomes,
                       const std::vector<std::string>& times, const MusicContext& context);

// The time of day for a solar phase (0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight): "dawn"
// and "dusk" within 0.04 of sunrise and sunset (about an hour of a 24 hour day), "day" between,
// "night" the rest.
std::string time_of_day(double solar_phase);

class MusicDirector {
public:
    MusicDirector(SoundPlayer& sounds, std::uint64_t seed = 0);

    void add(Playlist playlist);
    void clear();
    // All of them at once (live editing): the one playing plays on if it's unchanged.
    void replace(std::vector<Playlist> playlists);
    const std::map<std::string, Playlist>& playlists() const { return playlists_; }
    // The playlist that fits a context best (none fits: none).
    const Playlist* choose(const MusicContext& context) const;

    void set_context(const MusicContext& context);
    const MusicContext& context() const { return context_; }
    // A script's choice (by id or name; empty: silence) over the context's, until release().
    // False for an unknown playlist.
    bool force(const std::string& playlist);
    void release();
    // A stinger: a sound event played over the music, which dips to `duck` of its level under
    // it and comes back after.
    VoiceId stinger(std::string_view event, float duck = 0.35f);

    void update(double dt);

    const std::string& playing() const { return current_; }  // the playlist (empty: none)
    const std::string& track() const { return track_; }      // the event of the track
    float duck_level() const { return duck_now_; }           // the music under stingers now

    static constexpr double kSettle = 2.0;  // seconds a new choice must hold

private:
    void start(const Playlist* playlist);
    void next_track(float fade_in);

    SoundPlayer& sounds_;
    det::Rng rng_;
    std::map<std::string, Playlist> playlists_;
    MusicContext context_;
    std::optional<std::string> forced_;
    std::string current_;    // the playlist playing
    std::string candidate_;  // the one that fits now, waiting to settle
    double candidate_since_ = 0.0;
    bool chosen_once_ = false;
    std::string track_;
    VoiceId voice_ = 0;
    size_t next_ = 0;          // the next track (in order)
    double gap_until_ = -1.0;  // waiting between tracks until then
    double now_ = 0.0;
    // Stingers playing, and the music's dip under them.
    std::vector<std::pair<VoiceId, double>> stingers_;  // and when each ends
    float duck_ = 1.0f, duck_now_ = 1.0f;
};

}  // namespace midgard::audio
