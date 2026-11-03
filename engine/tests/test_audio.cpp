// SPDX-License-Identifier: Apache-2.0
// The mixer (milestone 5.14 step 1), mixed offline without a device and measured.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "midgard/audio.h"

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
