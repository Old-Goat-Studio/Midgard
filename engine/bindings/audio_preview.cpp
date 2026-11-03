// SPDX-License-Identifier: Apache-2.0
// The editor's sound preview (milestone 5.14): the game's own mixer, sound events, music and
// ambience, played through the editor's audio device (or, without one or when asked, mixed on
// request, for the editor's tests).
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "midgard/ambience.h"
#include "midgard/audio.h"
#include "midgard/music.h"
#include "midgard/sound_events.h"

namespace py = pybind11;

namespace {

using namespace midgard::audio;

class AudioPreview {
public:
    explicit AudioPreview(bool device)
        : mixer_(options(device)),
          player_(mixer_,
                  [this](const std::string& id) {
                      const auto it = files_.find(id);
                      return it == files_.end() ? std::vector<std::uint8_t>() : it->second;
                  }),
          music_(player_),
          ambience_(player_) {}

    bool has_device() const { return mixer_.has_device(); }
    std::string backend() const { return mixer_.backend(); }

    // A sound's file (any of the formats miniaudio reads), by its content id.
    void set_sound(const std::string& id, const py::bytes& data) {
        const std::string_view v(data);
        files_[id].assign(v.begin(), v.end());
        player_.forget(id);
    }
    void set_event(const std::string& id, const std::string& name, const std::string& json) {
        player_.add(sound_event_from_json(id, name, json));
    }
    void set_playlist(const std::string& id, const std::string& name, const std::string& json) {
        music_.add(playlist_from_json(id, name, json));
    }
    void set_ambience(const std::string& id, const std::string& name, const std::string& json) {
        ambience_.add(ambience_from_json(id, name, json));
    }

    VoiceId play(const std::string& event, float volume) {
        SoundPlayer::Play p;
        p.volume = volume;
        return player_.play(event, p);
    }
    void play_music(const std::string& playlist) {
        if (!music_.force(playlist)) throw py::value_error("no playlist '" + playlist + "'");
    }
    void play_ambience(const std::string& set, const std::string& time) {
        if (!ambience_.force(set)) throw py::value_error("no ambience '" + set + "'");
        MusicContext c;
        c.time = time;
        ambience_.set_context(c);
    }
    void stop_all() {
        music_.force("");
        ambience_.force("");
        player_.stop_all();
    }
    bool playing(VoiceId voice) const { return player_.playing(voice); }
    std::vector<std::string> playing_events() const { return player_.playing_events(); }
    std::string playlist() const { return music_.playing(); }
    std::vector<std::string> layers() const { return ambience_.layers(); }
    std::vector<std::string> problems() { return player_.take_problems(); }
    void set_volume(const std::string& bus, float volume) {
        const auto b = bus_from_name(bus);
        if (!b) throw py::value_error("no bus '" + bus + "'");
        mixer_.set_volume(*b, volume);
    }

    // Time passing (the editor calls it from a timer).
    void update(double dt) {
        music_.update(dt);
        ambience_.update(dt);
        player_.update(dt);
    }

    // Without a device: `seconds` of sound mixed (updating as it goes, a hundredth at a time),
    // as 32-bit float stereo bytes.
    py::bytes mix(double seconds) {
        std::vector<float> out;
        std::vector<float> block(size_t(mixer_.sample_rate() / 100) * mixer_.channels());
        for (int i = 0; i < int(seconds * 100.0 + 0.5); ++i) {
            update(0.01);
            mixer_.mix(block);
            out.insert(out.end(), block.begin(), block.end());
        }
        return py::bytes(reinterpret_cast<const char*>(out.data()), out.size() * sizeof(float));
    }

private:
    static MixerOptions options(bool device) {
        MixerOptions o;
        o.device = device;
        return o;
    }

    Mixer mixer_;
    std::map<std::string, std::vector<std::uint8_t>> files_;
    SoundPlayer player_;
    MusicDirector music_;
    AmbienceDirector ambience_;
};

}  // namespace

void bind_audio_preview(py::module_& m) {
    py::class_<AudioPreview>(m, "AudioPreview",
                             "The editor's sound preview: the game's mixer, sound events, music "
                             "and ambience, through the editor's audio device (device=False: "
                             "mixed on request, for tests).")
        .def(py::init<bool>(), py::arg("device") = true)
        .def_property_readonly("has_device", &AudioPreview::has_device)
        .def_property_readonly("backend", &AudioPreview::backend)
        .def("set_sound", &AudioPreview::set_sound, py::arg("id"), py::arg("data"),
             "A sound file's bytes (WAV, FLAC, MP3, Ogg Vorbis or Opus) by its content id.")
        .def("set_event", &AudioPreview::set_event, py::arg("id"), py::arg("name"), py::arg("json"),
             "A sound event (a sound_event row). RuntimeError if malformed.")
        .def("set_playlist", &AudioPreview::set_playlist, py::arg("id"), py::arg("name"),
             py::arg("json"))
        .def("set_ambience", &AudioPreview::set_ambience, py::arg("id"), py::arg("name"),
             py::arg("json"))
        .def("play", &AudioPreview::play, py::arg("event"), py::arg("volume") = 1.0f,
             "Plays a sound event: its voice (0: it didn't play; problems() says why).")
        .def("play_music", &AudioPreview::play_music, py::arg("playlist"))
        .def("play_ambience", &AudioPreview::play_ambience, py::arg("set"), py::arg("time") = "day",
             "An ambience set, its layers as at a time of day.")
        .def("stop_all", &AudioPreview::stop_all)
        .def("playing", &AudioPreview::playing, py::arg("voice"))
        .def_property_readonly("playing_events", &AudioPreview::playing_events)
        .def_property_readonly("playlist", &AudioPreview::playlist)
        .def_property_readonly("layers", &AudioPreview::layers)
        .def("problems", &AudioPreview::problems)
        .def("set_volume", &AudioPreview::set_volume, py::arg("bus"), py::arg("volume"))
        .def("update", &AudioPreview::update, py::arg("dt"))
        .def("mix", &AudioPreview::mix, py::arg("seconds"),
             "Without a device: seconds of sound mixed, as 32-bit float stereo bytes.");
}
