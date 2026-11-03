// SPDX-License-Identifier: Apache-2.0
// The mixer (milestone 5.14 step 1), mixed offline without a device and measured.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "midgard/audio.h"
#include "midgard/audio_encode.h"

namespace {

using midgard::audio::Bus;
using midgard::audio::Mixer;
using midgard::audio::MixerOptions;
using midgard::audio::PlayOptions;

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = 48000;

// A mono sine tone's WAV.
std::vector<std::uint8_t> tone(double hz, double seconds, float amplitude,
                               std::uint32_t rate = kRate) {
    std::vector<float> s(size_t(seconds * rate));
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = amplitude * float(std::sin(2.0 * kPi * hz * double(i) / rate));
    return midgard::audio::wav_bytes(s, 1, rate);
}

Mixer offline() {
    MixerOptions o;
    o.device = false;
    return Mixer(o);
}

// The next `seconds` mixed (interleaved stereo).
std::vector<float> mix(Mixer& m, double seconds) {
    std::vector<float> out(size_t(seconds * m.sample_rate()) * m.channels());
    m.mix(out);
    return out;
}

double rms(const std::vector<float>& s, int channel, int channels = 2) {
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = size_t(channel); i < s.size(); i += size_t(channels), ++n) sum += s[i] * s[i];
    return n ? std::sqrt(sum / double(n)) : 0.0;
}

float peak(const std::vector<float>& s) {
    float p = 0.0f;
    for (const float v : s) p = std::max(p, std::abs(v));
    return p;
}

// The left channel's frequency, from how often it crosses zero.
double frequency(const std::vector<float>& s, std::uint32_t rate) {
    int crossings = 0;
    for (size_t i = 2; i < s.size(); i += 2)
        if ((s[i - 2] < 0.0f) != (s[i] < 0.0f)) ++crossings;
    return crossings / 2.0 / (double(s.size() / 2) / rate);
}

}  // namespace

TEST_CASE("the mixer plays a sound at its level, on its bus, under the master") {
    Mixer m = offline();
    CHECK_FALSE(m.has_device());
    CHECK(m.backend() == "none");
    CHECK(m.sample_rate() == kRate);
    CHECK(m.channels() == 2);
    const auto sound = m.load("tone", tone(440.0, 2.0, 0.5f));
    CHECK(m.seconds(sound) == doctest::Approx(2.0).epsilon(0.001));
    CHECK(m.find("tone") == sound);
    m.play(sound);
    const double full = 0.5 / std::sqrt(2.0);  // a sine's RMS, in both channels (mono)
    auto out = mix(m, 0.25);
    CHECK(rms(out, 0) == doctest::Approx(full).epsilon(0.01));
    CHECK(rms(out, 1) == doctest::Approx(full).epsilon(0.01));
    CHECK(peak(out) <= 0.5001f);
    CHECK(frequency(out, kRate) == doctest::Approx(440.0).epsilon(0.01));
    // The effects bus at half: half as loud; the master at half too: a quarter.
    m.set_volume(Bus::Effects, 0.5f);
    CHECK(m.volume(Bus::Effects) == 0.5f);
    out = mix(m, 0.25);
    CHECK(rms(out, 0) == doctest::Approx(full * 0.5).epsilon(0.02));
    m.set_volume(Bus::Master, 0.5f);
    out = mix(m, 0.25);
    CHECK(rms(out, 0) == doctest::Approx(full * 0.25).epsilon(0.02));
    // Another bus's volume changes nothing here; the master at 0 silences everything.
    m.set_volume(Bus::Music, 0.0f);
    out = mix(m, 0.25);
    CHECK(rms(out, 0) == doctest::Approx(full * 0.25).epsilon(0.02));
    m.set_volume(Bus::Master, 0.0f);
    CHECK(peak(mix(m, 0.25)) == 0.0f);
}

TEST_CASE("each voice has its own volume, pan and pitch") {
    Mixer m = offline();
    const auto sound = m.load("tone", tone(500.0, 1.0, 0.4f));
    PlayOptions left;
    left.pan = -1.0f;
    const auto voice = m.play(sound, left);
    auto out = mix(m, 0.2);
    CHECK(rms(out, 0) > 0.2);
    CHECK(rms(out, 1) < 0.001);  // nothing on the right
    m.stop(voice);
    CHECK_FALSE(m.playing(voice));
    CHECK(peak(mix(m, 0.1)) == 0.0f);  // stopped: silence

    PlayOptions up;
    up.pitch = 2.0f;  // an octave up, and over in half the time
    up.volume = 0.5f;
    const auto fast = m.play(sound, up);
    out = mix(m, 0.2);
    CHECK(frequency(out, kRate) == doctest::Approx(1000.0).epsilon(0.01));
    CHECK(rms(out, 0) == doctest::Approx(0.2 / std::sqrt(2.0)).epsilon(0.02));
    mix(m, 0.35);  // 0.55 s in: past its 0.5 s
    CHECK_FALSE(m.playing(fast));
}

TEST_CASE("a sound ends unless it loops, and finished voices are reclaimed") {
    Mixer m = offline();
    const auto beep = m.load("beep", tone(440.0, 0.25, 0.5f));
    const auto once = m.play(beep);
    PlayOptions looped;
    looped.loop = true;
    const auto loop = m.play(beep, looped);
    CHECK(m.voice_count() == 2);
    mix(m, 0.2);
    CHECK(m.playing(once));
    mix(m, 0.2);  // 0.4 s: the beep is over, the loop goes round
    CHECK_FALSE(m.playing(once));
    CHECK(m.playing(loop));
    CHECK(rms(mix(m, 0.2), 0) == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.02));
    m.update();
    CHECK(m.voice_count() == 1);
    m.stop_all();
    CHECK(m.voice_count() == 0);
}

TEST_CASE("voices add up without clipping, and sounds at other rates are resampled") {
    Mixer m = offline();
    // Four quiet voices: together no louder than their sum.
    const auto quiet = m.load("quiet", tone(300.0, 1.0, 0.2f));
    for (int i = 0; i < 4; ++i) m.play(quiet);
    const auto out = mix(m, 0.2);
    CHECK(peak(out) == doctest::Approx(0.8f).epsilon(0.01));
    CHECK(peak(out) <= 0.8001f);
    m.stop_all();
    // A tone recorded at 22.05 kHz plays at its own pitch, for its own length.
    const auto old = m.load("old", tone(441.0, 0.5, 0.5f, 22050));
    CHECK(m.seconds(old) == doctest::Approx(0.5).epsilon(0.01));
    m.play(old);
    CHECK(frequency(mix(m, 0.3), kRate) == doctest::Approx(441.0).epsilon(0.01));
}

TEST_CASE("what the mixer refuses") {
    Mixer m = offline();
    const std::vector<std::uint8_t> junk = {'n', 'o', 't', ' ', 'a', ' ', 's', 'o', 'u', 'n', 'd'};
    CHECK_THROWS_WITH_AS(m.load("junk", junk), doctest::Contains("can't be read"),
                         midgard::audio::AudioError);
    CHECK_FALSE(m.find("junk"));
    const auto sound = m.load("tone", tone(440.0, 0.1, 0.5f));
    CHECK_THROWS_AS(m.load("tone", tone(440.0, 0.1, 0.5f)), midgard::audio::AudioError);
    PlayOptions master;
    master.bus = Bus::Master;
    CHECK_THROWS_AS(m.play(sound, master), midgard::audio::AudioError);
    CHECK_THROWS_AS(m.play(12345), midgard::audio::AudioError);
    // Unloaded: its voices stop, and its name is free again.
    const auto voice = m.play(sound);
    m.unload(sound);
    CHECK_FALSE(m.playing(voice));
    CHECK_FALSE(m.find("tone"));
    CHECK_NOTHROW(m.load("tone", tone(440.0, 0.1, 0.5f)));
}

TEST_CASE("buses by name, and a mixer with a device (or, without one, silent)") {
    for (int i = 0; i < midgard::audio::kBusCount; ++i)
        CHECK(midgard::audio::bus_from_name(midgard::audio::bus_name(Bus(i))) == Bus(i));
    CHECK_FALSE(midgard::audio::bus_from_name("bass"));
    Mixer m;  // the system's device, if it has one (CI machines don't: then it mixes on request)
    if (m.has_device()) {
        CHECK(m.backend() != "none");
        std::vector<float> out(64);
        CHECK_THROWS_AS(m.mix(out), midgard::audio::AudioError);
    } else {
        CHECK(m.backend() == "none");
    }
}

namespace {

// A stereo tone (left and right at different pitches) as a WAV at `rate`.
std::vector<std::uint8_t> stereo_tone(double left_hz, double right_hz, double seconds,
                                      std::uint32_t rate, std::uint32_t channels = 2) {
    std::vector<float> s(size_t(seconds * rate) * channels, 0.0f);
    for (size_t i = 0; i < s.size() / channels; ++i) {
        s[i * channels] = 0.4f * float(std::sin(2.0 * kPi * left_hz * double(i) / rate));
        s[i * channels + 1] = 0.4f * float(std::sin(2.0 * kPi * right_hz * double(i) / rate));
    }
    return midgard::audio::wav_bytes(s, channels, rate);
}

}  // namespace

TEST_CASE("the cook's Vorbis and Opus come back as the sound they were") {
    using midgard::audio::Codec;
    const auto wav = stereo_tone(440.0, 660.0, 1.0, 44100);
    for (const Codec codec : {Codec::Vorbis, Codec::Opus}) {
        CAPTURE(midgard::audio::codec_name(codec));
        const auto encoded = midgard::audio::encode_sound(wav, codec);
        CHECK(encoded.size() < wav.size() / 4);  // (a fraction of the WAV)
        CHECK(std::string(encoded.begin(), encoded.begin() + 4) == "OggS");
        // The same sound: the same bytes (a cook gives the same package every time).
        CHECK(midgard::audio::encode_sound(wav, codec) == encoded);
        const auto info = midgard::audio::sound_info(encoded);
        CHECK(info.seconds == doctest::Approx(1.0).epsilon(0.01));
        CHECK(info.channels == 2);
        CHECK(info.sample_rate == (codec == Codec::Opus ? 48000u : 44100u));
        // Played: each side at its own pitch and about its level (both are lossy).
        Mixer m = offline();
        m.play(m.load("encoded", encoded));
        mix(m, 0.1);  // (past the start)
        const auto out = mix(m, 0.5);
        CHECK(frequency(out, kRate) == doctest::Approx(440.0).epsilon(0.02));
        std::vector<float> right(out.begin() + 1, out.end());
        CHECK(frequency(right, kRate) == doctest::Approx(660.0).epsilon(0.02));
        CHECK(rms(out, 0) == doctest::Approx(0.4 / std::sqrt(2.0)).epsilon(0.05));
        CHECK(rms(out, 1) == doctest::Approx(0.4 / std::sqrt(2.0)).epsilon(0.05));
    }
}

TEST_CASE("encoding: a mono effect, surround mixed down, and what can't be encoded") {
    using midgard::audio::Codec;
    // A mono effect as Opus: read back as stereo (Opus is read that way), the same length.
    const auto click = midgard::audio::encode_sound(tone(1000.0, 0.3, 0.5f), Codec::Opus);
    CHECK(midgard::audio::sound_info(click).seconds == doctest::Approx(0.3).epsilon(0.02));
    // Opus in, Vorbis out (the cook takes whatever the library holds).
    const auto again = midgard::audio::encode_sound(click, Codec::Vorbis);
    CHECK(midgard::audio::sound_info(again).seconds == doctest::Approx(0.3).epsilon(0.02));
    // Six channels: stereo.
    const auto surround =
        midgard::audio::encode_sound(stereo_tone(300, 500, 0.5, 48000, 6), Codec::Vorbis);
    CHECK(midgard::audio::sound_info(surround).channels == 2);
    const std::vector<std::uint8_t> junk(100, 7);
    CHECK_THROWS_AS(midgard::audio::encode_sound(junk, Codec::Opus), midgard::audio::AudioError);
    CHECK_THROWS_AS(midgard::audio::sound_info(junk), midgard::audio::AudioError);
    CHECK(midgard::audio::codec_from_name("opus") == Codec::Opus);
    CHECK_FALSE(midgard::audio::codec_from_name("mp3"));
}

#include "midgard/sound_events.h"

namespace {

// A player over an offline mixer whose sounds are tones by name ("snd:440" is a 440 Hz tone,
// half a second; "snd:long440" two seconds).
struct Events {
    Mixer mixer = offline();
    std::vector<std::string> asked;
    midgard::audio::SoundPlayer player{mixer, [this](const std::string& id) {
                                           asked.push_back(id);
                                           if (id.rfind("snd:long", 0) == 0)
                                               return tone(std::stod(id.substr(8)), 2.0, 0.5f);
                                           if (id.rfind("snd:", 0) != 0 || id == "snd:missing")
                                               return std::vector<std::uint8_t>();
                                           return tone(std::stod(id.substr(4)), 0.5, 0.5f);
                                       }};
    void add(const char* id, const char* json) {
        player.add(midgard::audio::sound_event_from_json(id, std::string("Event ") + id, json));
    }
};

}  // namespace

TEST_CASE("a sound event's settings, and what isn't one") {
    using midgard::audio::sound_event_from_json;
    const auto e = sound_event_from_json(
        "sfx_door", "Door",
        R"({"sounds": [{"sound": "snd:a", "weight": 3}, "snd:b"], "bus": "ambience",
            "volume": [0.5, 0.8], "pitch": 1.2, "loop": true, "cooldown": 2, "max_voices": 3,
            "spatial": false, "min_distance": 2, "max_distance": 1})");
    CHECK(e.sounds.size() == 2);
    CHECK(e.sounds[0].weight == 3.0f);
    CHECK(e.sounds[1].sound == "snd:b");
    CHECK(e.bus == Bus::Ambience);
    CHECK(e.volume_min == 0.5f);
    CHECK(e.volume_max == 0.8f);
    CHECK(e.pitch_min == doctest::Approx(1.2f));
    CHECK(e.pitch_max == doctest::Approx(1.2f));
    CHECK(e.loop);
    CHECK(e.max_voices == 3);
    CHECK_FALSE(e.spatial);
    CHECK(e.max_distance == 2.0f);                         // (never nearer than min_distance)
    const auto d = sound_event_from_json("x", "X", "{}");  // defaults
    CHECK(d.bus == Bus::Effects);
    CHECK(d.volume_min == 1.0f);
    CHECK_FALSE(d.loop);
    CHECK_THROWS_AS(sound_event_from_json("x", "X", R"({"bus": "master"})"),
                    midgard::audio::AudioError);
    CHECK_THROWS_AS(sound_event_from_json("x", "X", R"({"volume": "loud"})"),
                    midgard::audio::AudioError);
    CHECK_THROWS_AS(sound_event_from_json("x", "X", "[1]"), midgard::audio::AudioError);
}

TEST_CASE("an event picks its sounds by weight, and its volume and pitch in their ranges") {
    Events w;
    w.add("steps", R"({"sounds": [{"sound": "snd:400", "weight": 3},
                                  {"sound": "snd:800", "weight": 1}],
                     "volume": [0.4, 0.6], "pitch": [0.9, 1.1]})");
    int low = 0;
    std::vector<double> levels;
    for (int i = 0; i < 40; ++i) {
        w.player.stop_all();
        REQUIRE(w.player.play("Event steps") != 0);  // (by name)
        const auto out = mix(w.mixer, 0.1);
        const double hz = frequency(out, kRate);
        CHECK(((hz > 350 && hz < 450) || (hz > 700 && hz < 900)));  // (pitch within 10%)
        if (hz < 600) ++low;
        levels.push_back(rms(out, 0) / (0.5 / std::sqrt(2.0)));
    }
    CHECK(low > 20);  // three in four, near enough
    CHECK(low < 38);
    CHECK(*std::min_element(levels.begin(), levels.end()) >= 0.39);
    CHECK(*std::max_element(levels.begin(), levels.end()) <= 0.61);
    CHECK(*std::max_element(levels.begin(), levels.end()) -
              *std::min_element(levels.begin(), levels.end()) >
          0.05);  // (they vary)
    // Each sound read once, however often it plays.
    CHECK(std::count(w.asked.begin(), w.asked.end(), "snd:400") == 1);
    // The same seed: the same picks.
    Events again;
    again.add("steps", R"({"sounds": [{"sound": "snd:400", "weight": 3},
                                      {"sound": "snd:800", "weight": 1}],
                         "volume": [0.4, 0.6], "pitch": [0.9, 1.1]})");
    again.player.play("steps");
    Events first;
    first.add("steps", R"({"sounds": [{"sound": "snd:400", "weight": 3},
                                      {"sound": "snd:800", "weight": 1}],
                         "volume": [0.4, 0.6], "pitch": [0.9, 1.1]})");
    first.player.play("steps");
    CHECK(mix(again.mixer, 0.1) == mix(first.mixer, 0.1));
}

TEST_CASE("cooldown, at most so many at once, looping, stopping, and problems") {
    Events w;
    w.add("bell", R"({"sounds": ["snd:500"], "cooldown": 1.0})");
    CHECK(w.player.play("bell") != 0);
    CHECK(w.player.play("bell") == 0);  // cooling down
    w.player.update(0.6);
    CHECK(w.player.play("bell") == 0);
    w.player.update(0.5);  // 1.1 s on
    CHECK(w.player.play("bell") != 0);

    w.add("crowd", R"({"sounds": ["snd:long300"], "max_voices": 2})");
    const auto a = w.player.play("crowd");
    const auto b = w.player.play("crowd");
    const auto c = w.player.play("crowd");  // the oldest goes for it
    CHECK_FALSE(w.player.playing(a));
    CHECK(w.player.playing(b));
    CHECK(w.player.playing(c));
    w.player.stop_event("crowd");
    CHECK_FALSE(w.player.playing(c));

    w.add("hum", R"({"sounds": ["snd:200"], "loop": true, "bus": "ambience"})");
    const auto hum = w.player.play("hum");
    mix(w.mixer, 1.0);  // twice its length
    w.player.update(1.0);
    CHECK(w.player.playing(hum));
    const auto now = w.player.playing_events();
    CHECK(std::count(now.begin(), now.end(), "hum") == 1);
    w.player.stop_event("bell");
    w.mixer.set_volume(Bus::Ambience, 0.0f);  // its bus down: silent
    CHECK(peak(mix(w.mixer, 0.1)) == 0.0f);
    w.player.stop_all();
    CHECK(w.player.playing_events().empty());

    // Problems: an unknown event, a sound not in the game (each once), and the event plays
    // its other sounds.
    w.add("half", R"({"sounds": ["snd:missing", "snd:600"]})");
    CHECK(w.player.play("nothing") == 0);
    CHECK(w.player.play("half") != 0);
    w.player.stop_all();
    CHECK(w.player.play("half") != 0);
    const auto problems = w.player.take_problems();
    CHECK(problems.size() == 2);
    CHECK(std::count(w.asked.begin(), w.asked.end(), "snd:missing") == 1);
    CHECK(w.player.take_problems().empty());
}

TEST_CASE("3D: a placed sound's level by distance, its side, and muffled through walls") {
    using midgard::audio::distance_gain;
    // The curve: all of it within near, then near / distance, fading to silence at far.
    CHECK(distance_gain(0.5f, 1.0f, 40.0f) == 1.0f);
    CHECK(distance_gain(2.0f, 1.0f, 40.0f) == doctest::Approx(0.5f));
    CHECK(distance_gain(4.0f, 1.0f, 40.0f) == doctest::Approx(0.25f));
    CHECK(distance_gain(35.0f, 1.0f, 40.0f) ==
          doctest::Approx((1.0f / 35.0f) * (40.0f - 35.0f) / (40.0f - 32.2f)));
    CHECK(distance_gain(40.0f, 1.0f, 40.0f) == 0.0f);
    CHECK(distance_gain(90.0f, 1.0f, 40.0f) == 0.0f);

    // Panned by direction: on the right, louder on the right (the listener faces -Z).
    Mixer m = offline();
    m.set_listener({0, 0, -1}, {0, 1, 0});
    const auto hum = m.load("hum", tone(300.0, 2.0, 0.5f));
    PlayOptions placed;
    placed.spatial = true;
    placed.loop = true;
    placed.position = {5, 0, 0};
    const auto right = m.play(hum, placed);
    auto out = mix(m, 0.2);
    CHECK(rms(out, 1) > 3.0 * rms(out, 0));
    m.set_voice_position(right, {-5, 0, 0});  // moved to the left
    mix(m, 0.05);
    out = mix(m, 0.2);
    CHECK(rms(out, 0) > 3.0 * rms(out, 1));
    m.set_voice_position(right, {0, 0, -5});  // ahead: both sides alike
    mix(m, 0.05);
    out = mix(m, 0.2);
    CHECK(rms(out, 0) == doctest::Approx(rms(out, 1)).epsilon(0.05));
    m.stop_all();

    // Muffled: a high sound mostly gone, a low one hardly touched.
    for (const double hz : {5000.0, 150.0}) {
        CAPTURE(hz);
        Mixer w = offline();
        placed.position = {0, 0, -2};
        const auto v = w.play(w.load("t", tone(hz, 2.0, 0.5f)), placed);
        mix(w, 0.05);
        const double clear = rms(mix(w, 0.2), 0);
        w.set_voice_muffle(v, 1.0f);
        mix(w, 0.05);
        const double muffled = rms(mix(w, 0.2), 0);
        if (hz > 1000)
            CHECK(muffled < 0.1 * clear);
        else
            CHECK(muffled > 0.8 * clear);
    }
}

TEST_CASE("reverb: an echo after a sound on effects, never on music, none when it's dry") {
    const auto click = [] {  // 20 ms of noise-like tone, then nothing
        std::vector<float> s(48000, 0.0f);
        for (size_t i = 0; i < 960; ++i)
            s[i] = 0.5f * float(std::sin(double(i) * 0.7) * std::sin(double(i) * 0.031));
        return midgard::audio::wav_bytes(s, 1, 48000);
    }();
    const auto tail = [&](Bus bus, float wet) {
        Mixer m = offline();
        auto reverb = midgard::audio::reverb_preset("cave").value();
        reverb.wet = wet;
        m.set_reverb(reverb);
        mix(m, 0.1);  // (the echo's level eased in)
        PlayOptions play;
        play.bus = bus;
        m.play(m.load("click", click), play);
        mix(m, 0.2);
        return rms(mix(m, 0.3), 0);  // 0.2 to 0.5 s after: only an echo would be heard
    };
    CHECK(tail(Bus::Effects, 0.45f) > 0.003);
    CHECK(tail(Bus::Effects, 0.0f) < 1e-6);
    CHECK(tail(Bus::Music, 0.45f) < 1e-6);
    CHECK(midgard::audio::reverb_preset("hall"));
    CHECK_FALSE(midgard::audio::reverb_preset("bathroom"));
    CHECK(midgard::audio::reverb_presets().size() == 6);
}

TEST_CASE("placed events: their distance's level, following, and muffled while blocked") {
    Events w;
    w.add("drip", R"({"sounds": ["snd:long500"], "min_distance": 1, "max_distance": 40,
                      "loop": true})");
    w.add("radio", R"({"sounds": ["snd:long500"], "spatial": false})");
    midgard::audio::SoundPlayer::Listener ear;
    ear.position = {10, 0, 10};  // (anywhere: positions are taken from it)
    w.player.set_listener(ear);
    bool wall = false;
    w.player.set_blocked(
        [&](midgard::audio::VoiceId, const glm::dvec3&, const glm::dvec3&) { return wall; });
    midgard::audio::SoundPlayer::Play near;
    near.at = glm::dvec3(10, 0, 9);  // a metre ahead
    const auto close = w.player.play("drip", near);
    const double at_one = rms(mix(w.mixer, 0.2), 0);
    w.player.set_position(close, {10, 0, 8});  // two metres
    w.player.update(1.0 / 60.0);
    CHECK(w.player.distance_gain(close) == doctest::Approx(0.5f));
    mix(w.mixer, 0.05);
    CHECK(rms(mix(w.mixer, 0.2), 0) == doctest::Approx(at_one * 0.5).epsilon(0.03));
    w.player.set_position(close, {10, 0, -45});  // beyond its far: silent
    w.player.update(1.0 / 60.0);
    mix(w.mixer, 0.05);
    CHECK(rms(mix(w.mixer, 0.2), 0) < 1e-4);
    // Behind a wall: muffled (eased in), then clear again.
    w.player.set_position(close, {10, 0, 9});
    wall = true;
    for (int i = 0; i < 30; ++i) w.player.update(1.0 / 60.0);
    CHECK(w.player.occlusion(close) > 0.9f);
    mix(w.mixer, 0.05);
    CHECK(rms(mix(w.mixer, 0.2), 0) < at_one * 0.45);
    wall = false;
    for (int i = 0; i < 40; ++i) w.player.update(1.0 / 60.0);
    CHECK(w.player.occlusion(close) < 0.05f);
    // An event that isn't placed: the same everywhere, whatever `at` says.
    midgard::audio::SoundPlayer::Play far;
    far.at = glm::dvec3(500, 0, 500);
    w.player.stop_all();
    const auto radio = w.player.play("radio", far);
    CHECK(w.player.distance_gain(radio) == 1.0f);
    CHECK(rms(mix(w.mixer, 0.2), 0) > 0.3);
}

#include "midgard/music.h"

namespace {

// How strong one frequency is in the left channel (Goertzel): a sine of amplitude a gives a.
double strength(const std::vector<float>& s, double hz, std::uint32_t rate = kRate) {
    const size_t n = s.size() / 2;
    const double w = 2.0 * kPi * hz / rate, k = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double s0 = s[i * 2] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) / double(n);
}

// Time passing for the director and the mixer together, in hundredths.
void play_for(Events& w, midgard::audio::MusicDirector& d, double seconds) {
    for (int i = 0; i < int(std::lround(seconds * 100)); ++i) {
        d.update(0.01);
        w.player.update(0.01);
        mix(w.mixer, 0.01);
    }
}

}  // namespace

TEST_CASE("music: playlists, times of day, and which fits where") {
    using midgard::audio::playlist_from_json;
    const auto p = playlist_from_json("mus_forest", "Forest", R"({"tracks": ["a", "b"],
        "order": "shuffle", "biomes": ["Forest"], "times": ["day", "dawn"], "priority": 2,
        "crossfade": 4, "gap": [5, 2]})");
    CHECK(p.tracks.size() == 2);
    CHECK(p.shuffle);
    CHECK(p.biomes == std::vector<std::string>{"Forest"});
    CHECK(p.priority == 2);
    CHECK(p.crossfade == 4.0f);
    CHECK(p.gap_min == 2.0f);
    CHECK(p.gap_max == 5.0f);
    CHECK_THROWS_AS(playlist_from_json("x", "X", R"({"times": ["noon"]})"),
                    midgard::audio::AudioError);
    CHECK_THROWS_AS(playlist_from_json("x", "X", R"({"order": "backwards"})"),
                    midgard::audio::AudioError);
    using midgard::audio::time_of_day;
    CHECK(time_of_day(0.0) == "dawn");
    CHECK(time_of_day(0.99) == "dawn");
    CHECK(time_of_day(0.25) == "day");
    CHECK(time_of_day(0.5) == "dusk");
    CHECK(time_of_day(0.75) == "night");
    CHECK(time_of_day(1.25) == "day");

    Events w;
    midgard::audio::MusicDirector d(w.player);
    d.add(playlist_from_json("any", "Anywhere", R"({"tracks": ["t"]})"));
    d.add(playlist_from_json("forest", "Forest", R"({"tracks": ["t"], "biomes": ["Forest"]})"));
    d.add(playlist_from_json("forest_night", "Forest at night",
                             R"({"tracks": ["t"], "biomes": ["Forest"], "times": ["night"]})"));
    d.add(playlist_from_json("loud", "Loud", R"({"tracks": ["t"], "biomes": ["Forest"],
                                                "priority": 5})"));
    d.add(playlist_from_json("empty", "Empty", R"({"scenes": ["s"], "times": ["day"]})"));
    midgard::audio::MusicContext c{"s", "wt", "Desert", "day"};
    CHECK(d.choose(c)->id == "any");  // (the empty one has no tracks)
    c.biome = "Forest";
    CHECK(d.choose(c)->id == "loud");  // (as specific as "forest", higher priority)
    c.time = "night";
    CHECK(d.choose(c)->id == "forest_night");  // (the most specific)
}

TEST_CASE("music: a change of playlist settles, then crossfades in the time it says") {
    Events w;
    w.add("low", R"({"sounds": ["snd:long400"], "bus": "music", "loop": true,
                     "spatial": false})");
    w.add("high", R"({"sounds": ["snd:long600"], "bus": "music", "loop": true,
                      "spatial": false})");
    midgard::audio::MusicDirector d(w.player);
    using midgard::audio::playlist_from_json;
    d.add(playlist_from_json("woods", "Woods", R"({"tracks": ["low"], "biomes": ["Forest"],
                                                  "crossfade": 1})"));
    d.add(playlist_from_json("sands", "Sands", R"({"tracks": ["high"], "biomes": ["Desert"],
                                                  "crossfade": 1})"));
    d.set_context({"s", "", "Forest", "day"});
    play_for(w, d, 1.5);  // (the first choice at once, faded in over a second)
    CHECK(d.playing() == "woods");
    CHECK(d.track() == "low");
    auto out = mix(w.mixer, 0.1);
    CHECK(strength(out, 400.0) == doctest::Approx(0.5).epsilon(0.05));
    // Desert: it must hold two seconds first (a moment back in the forest starts it again).
    d.set_context({"s", "", "Desert", "day"});
    play_for(w, d, 1.0);
    d.set_context({"s", "", "Forest", "day"});
    play_for(w, d, 0.1);
    d.set_context({"s", "", "Desert", "day"});
    play_for(w, d, 1.9);
    CHECK(d.playing() == "woods");
    for (int i = 0; i < 20 && d.playing() == "woods"; ++i) play_for(w, d, 0.01);
    REQUIRE(d.playing() == "sands");
    // Halfway through the crossfade: half of each; at its end, all of the new.
    play_for(w, d, 0.45);
    out = mix(w.mixer, 0.1);  // (0.45 to 0.55 s)
    CHECK(strength(out, 400.0) == doctest::Approx(0.25).epsilon(0.12));
    CHECK(strength(out, 600.0) == doctest::Approx(0.25).epsilon(0.12));
    play_for(w, d, 0.6);
    out = mix(w.mixer, 0.1);
    CHECK(strength(out, 400.0) < 0.01);
    CHECK(strength(out, 600.0) == doctest::Approx(0.5).epsilon(0.05));
    // A script's choice wins at once; released, the music goes back.
    CHECK(d.force("Woods"));
    CHECK_FALSE(d.force("Jazz"));
    play_for(w, d, 0.05);
    CHECK(d.playing() == "woods");
    d.release();
    play_for(w, d, 2.2);
    CHECK(d.playing() == "sands");
    d.force("");  // silence
    play_for(w, d, 1.2);
    CHECK(d.playing().empty());
    CHECK(peak(mix(w.mixer, 0.1)) < 1e-4f);
}

TEST_CASE("music: tracks follow each other after their gap, and stingers duck the music") {
    Events w;
    w.add("a", R"({"sounds": ["snd:500"], "bus": "music", "spatial": false})");
    w.add("b", R"({"sounds": ["snd:700"], "bus": "music", "spatial": false})");
    midgard::audio::MusicDirector d(w.player);
    using midgard::audio::playlist_from_json;
    d.add(playlist_from_json("two", "Two", R"({"tracks": ["a", "b"], "crossfade": 0,
                                              "gap": 0.3})"));
    d.set_context({});
    play_for(w, d, 0.1);
    CHECK(d.track() == "a");
    play_for(w, d, 0.5);  // a (0.5 s) is over: a gap of 0.3 s
    CHECK(d.track() == "a");
    CHECK(peak(mix(w.mixer, 0.05)) < 1e-4f);
    play_for(w, d, 0.3);
    CHECK(d.track() == "b");
    play_for(w, d, 0.9);
    CHECK(d.track() == "a");  // round again

    Events v;
    v.add("hum", R"({"sounds": ["snd:long300"], "bus": "music", "loop": true,
                     "spatial": false})");
    v.add("sting", R"({"sounds": ["snd:900"], "bus": "music", "spatial": false})");
    midgard::audio::MusicDirector m(v.player);
    m.add(playlist_from_json("hum", "Hum", R"({"tracks": ["hum"], "crossfade": 0})"));
    m.set_context({});
    play_for(v, m, 0.2);
    CHECK(strength(mix(v.mixer, 0.1), 300.0) == doctest::Approx(0.5).epsilon(0.05));
    REQUIRE(m.stinger("sting", 0.4f) != 0);
    play_for(v, m, 0.3);  // (dipped within a quarter second)
    auto out = mix(v.mixer, 0.1);
    CHECK(m.duck_level() == doctest::Approx(0.4f));
    CHECK(strength(out, 300.0) == doctest::Approx(0.2).epsilon(0.1));
    CHECK(strength(out, 900.0) > 0.3);
    play_for(v, m, 1.0);  // the stinger over, the music back
    CHECK(m.duck_level() == doctest::Approx(1.0f));
    CHECK(strength(mix(v.mixer, 0.1), 300.0) == doctest::Approx(0.5).epsilon(0.05));
}
