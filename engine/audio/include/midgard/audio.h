// SPDX-License-Identifier: Apache-2.0
// Sound (milestone 5.14): the mixer, built on miniaudio's engine.
//
// Every sound plays on a bus: music, effects, voice, ambience or interface, each with its own
// volume, all mixed into the master bus (whose volume is the whole game's). The mixer plays
// through the system's audio device (WASAPI, PulseAudio or ALSA, AAudio or OpenSL ES), or, with
// none (a server, a CI machine, Options::device false), mixes only when asked: mix() fills a
// buffer, which is how the tests measure what would be heard.
//
// Sounds are files as they come (WAV, FLAC, MP3, Ogg Vorbis, Ogg Opus: games get Vorbis and
// Opus from the cook, midgard/audio_encode.h), given as bytes and decoded once
// to the mixer's rate; any number of voices may play one at once, each with its own volume,
// pitch and pan. Call update() each frame to reclaim the voices that have finished.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace midgard::audio {

enum class Bus : std::uint8_t { Master, Music, Effects, Voice, Ambience, Interface };
inline constexpr int kBusCount = 6;
const char* bus_name(Bus bus);  // "master", "music", ...
std::optional<Bus> bus_from_name(std::string_view name);

struct AudioError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct MixerOptions {
    bool device = true;                 // play through the system's audio device, if it has one
    std::uint32_t sample_rate = 48000;  // frames a second (the device's own when it has one)
    std::uint32_t channels = 2;         // stereo
};

struct PlayOptions {
    Bus bus = Bus::Effects;  // (not Master: a sound plays on one of the others)
    float volume = 1.0f;     // linear: 0.5 is 6 dB quieter
    float pitch = 1.0f;      // 2 an octave up (and twice as fast), 0.5 an octave down
    float pan = 0.0f;        // -1 left, 0 the middle, 1 right
    bool loop = false;
};

using SoundId = std::uint32_t;  // a loaded sound (0: none)
using VoiceId = std::uint64_t;  // a sound playing (0: none)

class Mixer {
public:
    explicit Mixer(const MixerOptions& options = {});
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // Whether it plays through a device (else only mix() makes sound), and which.
    bool has_device() const;
    std::string backend() const;  // "WASAPI", "PulseAudio", "AAudio"...; "none" without one
    std::uint32_t sample_rate() const;
    std::uint32_t channels() const;

    // A sound from a file's bytes (decoded now). `name` must be unique (a content id).
    // AudioError if it can't be decoded.
    SoundId load(const std::string& name, std::span<const std::uint8_t> encoded);
    void unload(SoundId sound);           // (its voices stop)
    double seconds(SoundId sound) const;  // how long it lasts
    std::optional<SoundId> find(const std::string& name) const;

    // Plays a sound (AudioError for an unknown one, or the master bus).
    VoiceId play(SoundId sound, const PlayOptions& options = {});
    void stop(VoiceId voice);
    void stop_all();
    bool playing(VoiceId voice) const;
    std::size_t voice_count() const;  // playing now
    void set_voice_volume(VoiceId voice, float volume);

    // Bus volumes (linear, 0 and up): Master is the whole game's.
    void set_volume(Bus bus, float volume);
    float volume(Bus bus) const;

    // Without a device: mixes the next frames into `out` (interleaved, channels() a frame).
    // AudioError with a device (it mixes by itself).
    void mix(std::span<float> out);

    // Reclaims finished voices (each frame).
    void update();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// What a sound file holds, read with the game's own decoders (AudioError if they can't).
struct SoundInfo {
    double seconds = 0.0;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
};
SoundInfo sound_info(std::span<const std::uint8_t> encoded);

// A WAV file's bytes (32-bit float, the given channels): for tests and generated sounds.
std::vector<std::uint8_t> wav_bytes(std::span<const float> interleaved, std::uint32_t channels,
                                    std::uint32_t sample_rate);

}  // namespace midgard::audio
