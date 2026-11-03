// SPDX-License-Identifier: Apache-2.0
#include "midgard/audio.h"

#include "opus_backend.h"
#include "reverb.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace midgard::audio {
namespace {

constexpr std::array<const char*, kBusCount> kBusNames = {"master", "music",    "effects",
                                                          "voice",  "ambience", "interface"};

// The decoders beyond miniaudio's own (WAV, FLAC, MP3, and Vorbis through stb_vorbis).
ma_decoding_backend_vtable* g_backends[] = {opus_backend()};

void check(ma_result result, const std::string& what) {
    if (result != MA_SUCCESS) throw AudioError(what + ": " + ma_result_description(result));
}

// The buses whose sound echoes in reverb zones (not music, nor the interface).
constexpr std::array<Bus, 3> kReverbBuses = {Bus::Effects, Bus::Voice, Bus::Ambience};
constexpr float kOpenCutoff = 20000.0f;   // a placed voice's filter, wide open
constexpr float kMuffledCutoff = 700.0f;  // ... and through a wall

}  // namespace

const char* bus_name(Bus bus) {
    return kBusNames[size_t(bus)];
}

float distance_gain(float distance, float nearest, float farthest) {
    nearest = std::max(nearest, 0.001f);
    if (distance <= nearest) return 1.0f;
    if (farthest <= nearest || distance >= farthest) return 0.0f;
    float gain = nearest / distance;
    const float fade =
        nearest + 0.8f * (farthest - nearest);  // the last fifth: to silence at farthest
    if (distance > fade) gain *= (farthest - distance) / (farthest - fade);
    return gain;
}

namespace {

struct Preset {
    const char* name;
    ReverbSettings settings;
};
const std::array<Preset, 6> kPresets = {{{"none", {0.5f, 0.5f, 0.0f, 1.0f}},
                                         {"room", {0.5f, 0.5f, 0.25f, 1.0f}},
                                         {"hall", {0.8f, 0.35f, 0.35f, 1.0f}},
                                         {"cave", {0.9f, 0.2f, 0.45f, 1.0f}},
                                         {"cathedral", {0.96f, 0.15f, 0.5f, 1.0f}},
                                         {"tunnel", {0.85f, 0.5f, 0.4f, 0.6f}}}};

}  // namespace

std::optional<ReverbSettings> reverb_preset(std::string_view name) {
    for (const Preset& p : kPresets)
        if (name == p.name) return p.settings;
    return std::nullopt;
}

const std::vector<std::string>& reverb_presets() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const Preset& p : kPresets) out.emplace_back(p.name);
        return out;
    }();
    return names;
}

std::optional<Bus> bus_from_name(std::string_view name) {
    for (int i = 0; i < kBusCount; ++i)
        if (name == kBusNames[size_t(i)]) return Bus(i);
    return std::nullopt;
}

struct Mixer::Impl {
    ma_resource_manager resources{};
    ma_engine engine{};
    bool device = false;
    // The buses under the master (the engine's own output): Music ... Interface.
    std::array<ma_sound_group, kBusCount> groups{};
    std::array<float, kBusCount> volumes{1, 1, 1, 1, 1, 1};
    // The echo: effects, voice and ambience go through it (in line) to the master.
    ReverbNode reverb;
    bool has_reverb = false;

    struct Sound {
        std::string name;
        std::vector<std::uint8_t> bytes;  // (the resource manager reads them in place)
        ma_sound decoded{};               // keeps the decoded data alive; never started
        double seconds = 0.0;
    };
    std::unordered_map<SoundId, std::unique_ptr<Sound>> sounds;
    std::unordered_map<std::string, SoundId> by_name;
    SoundId next_sound = 1;

    struct Voice {
        SoundId sound = 0;
        ma_sound played{};
        // A placed voice's filter (muffled through walls), between it and its bus.
        bool filtered = false;
        ma_lpf_node lpf{};
        float muffle = 0.0f;
    };
    void free(Voice& v) {
        ma_sound_uninit(&v.played);
        if (v.filtered) ma_lpf_node_uninit(&v.lpf, nullptr);
    }
    std::unordered_map<VoiceId, std::unique_ptr<Voice>> voices;
    VoiceId next_voice = 1;

    ~Impl() {
        for (auto& [id, v] : voices) free(*v);
        voices.clear();
        for (auto& [id, s] : sounds) {
            ma_sound_uninit(&s->decoded);
            ma_resource_manager_unregister_data(&resources, s->name.c_str());
        }
        for (int i = 1; i < kBusCount; ++i) ma_sound_group_uninit(&groups[size_t(i)]);
        if (has_reverb) {
            ma_node_uninit(&reverb.base, nullptr);
            delete reverb.verb;
        }
        ma_engine_uninit(&engine);
        ma_resource_manager_uninit(&resources);
    }
};

Mixer::Mixer(const MixerOptions& options) : impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    ma_resource_manager_config rc = ma_resource_manager_config_init();
    rc.decodedFormat = ma_format_f32;
    rc.decodedChannels = 0;  // (each sound's own: mono stays mono until it's mixed)
    rc.decodedSampleRate = options.sample_rate;
    rc.ppCustomDecodingBackendVTables = g_backends;
    rc.customDecodingBackendCount = 1;
    check(ma_resource_manager_init(&rc, &m.resources), "starting the sound decoder");

    const auto start = [&](bool device) {
        ma_engine_config ec = ma_engine_config_init();
        ec.pResourceManager = &m.resources;
        ec.channels = options.channels;
        ec.sampleRate = options.sample_rate;
        ec.noDevice = device ? MA_FALSE : MA_TRUE;
        ec.listenerCount = 1;
        return ma_engine_init(&ec, &m.engine);
    };
    m.device = options.device && start(true) == MA_SUCCESS;
    if (!m.device) {  // (no device, or none wanted: mixed only when asked)
        const ma_result r = start(false);
        if (r != MA_SUCCESS) {
            ma_resource_manager_uninit(&m.resources);
            check(r, "starting the mixer");
        }
    }
    for (int i = 1; i < kBusCount; ++i)
        check(ma_sound_group_init(&m.engine, 0, nullptr, &m.groups[size_t(i)]),
              std::string("the ") + kBusNames[size_t(i)] + " bus");
    // The echo (stereo): those buses through the reverb, which adds it, to the master.
    if (channels() == 2) {
        ma_node_graph* graph = ma_engine_get_node_graph(&m.engine);
        check(reverb_node_init(graph, sample_rate(), &m.reverb), "the reverb");
        m.has_reverb = true;
        ma_node_attach_output_bus(&m.reverb.base, 0, ma_engine_get_endpoint(&m.engine), 0);
        for (const Bus bus : kReverbBuses)
            ma_node_attach_output_bus(&m.groups[size_t(bus)], 0, &m.reverb.base, 0);
    }
}

Mixer::~Mixer() = default;

bool Mixer::has_device() const {
    return impl_->device;
}

std::string Mixer::backend() const {
    if (!impl_->device) return "none";
    ma_device* d = ma_engine_get_device(&impl_->engine);
    return d && d->pContext ? ma_get_backend_name(d->pContext->backend) : "unknown";
}

std::uint32_t Mixer::sample_rate() const {
    return ma_engine_get_sample_rate(&impl_->engine);
}
std::uint32_t Mixer::channels() const {
    return ma_engine_get_channels(&impl_->engine);
}

SoundId Mixer::load(const std::string& name, std::span<const std::uint8_t> encoded) {
    Impl& m = *impl_;
    if (m.by_name.contains(name)) throw AudioError("sound '" + name + "' is loaded already");
    auto s = std::make_unique<Impl::Sound>();
    s->name = name;
    s->bytes.assign(encoded.begin(), encoded.end());
    check(ma_resource_manager_register_encoded_data(&m.resources, name.c_str(), s->bytes.data(),
                                                    s->bytes.size()),
          "sound '" + name + "'");
    const ma_result r = ma_sound_init_from_file(
        &m.engine, name.c_str(), MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr,
        nullptr, &s->decoded);
    if (r != MA_SUCCESS) {
        ma_resource_manager_unregister_data(&m.resources, name.c_str());
        throw AudioError("sound '" + name + "' can't be read (" + ma_result_description(r) +
                         "): WAV, FLAC, MP3, Ogg Vorbis and Ogg Opus files can");
    }
    float seconds = 0.0f;
    ma_sound_get_length_in_seconds(&s->decoded, &seconds);
    s->seconds = seconds;
    const SoundId id = m.next_sound++;
    m.by_name[name] = id;
    m.sounds[id] = std::move(s);
    return id;
}

void Mixer::unload(SoundId sound) {
    Impl& m = *impl_;
    const auto it = m.sounds.find(sound);
    if (it == m.sounds.end()) return;
    for (auto v = m.voices.begin(); v != m.voices.end();) {
        if (v->second->sound == sound) {
            m.free(*v->second);
            v = m.voices.erase(v);
        } else {
            ++v;
        }
    }
    ma_sound_uninit(&it->second->decoded);
    ma_resource_manager_unregister_data(&m.resources, it->second->name.c_str());
    m.by_name.erase(it->second->name);
    m.sounds.erase(it);
}

double Mixer::seconds(SoundId sound) const {
    const auto it = impl_->sounds.find(sound);
    return it == impl_->sounds.end() ? 0.0 : it->second->seconds;
}

std::optional<SoundId> Mixer::find(const std::string& name) const {
    const auto it = impl_->by_name.find(name);
    return it == impl_->by_name.end() ? std::nullopt : std::optional<SoundId>(it->second);
}

VoiceId Mixer::play(SoundId sound, const PlayOptions& options) {
    Impl& m = *impl_;
    const auto it = m.sounds.find(sound);
    if (it == m.sounds.end()) throw AudioError("no such sound");
    if (options.bus == Bus::Master) throw AudioError("a sound plays on a bus under the master");
    auto v = std::make_unique<Impl::Voice>();
    v->sound = sound;
    // (From the decoded data the sound's own copy keeps: no decoding again.)
    ma_sound_group* group = &m.groups[size_t(options.bus)];
    check(ma_sound_init_copy(&m.engine, &it->second->decoded,
                             options.spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION, group,
                             &v->played),
          "playing '" + it->second->name + "'");
    if (options.spatial) {
        // Panned by its direction; its distance is the caller's (distance_gain), no Doppler.
        // (A model with no roll-off, not "none": that would skip the panning and volume too.)
        ma_sound_set_attenuation_model(&v->played, ma_attenuation_model_linear);
        ma_sound_set_rolloff(&v->played, 0.0f);
        ma_sound_set_doppler_factor(&v->played, 0.0f);
        ma_sound_set_position(&v->played, options.position.x, options.position.y,
                              options.position.z);
        const ma_lpf_node_config lc =
            ma_lpf_node_config_init(channels(), sample_rate(), kOpenCutoff, 2);
        if (ma_lpf_node_init(ma_engine_get_node_graph(&m.engine), &lc, nullptr, &v->lpf) ==
            MA_SUCCESS) {
            v->filtered = true;
            ma_node_attach_output_bus(&v->played, 0, &v->lpf, 0);
            ma_node_attach_output_bus(&v->lpf, 0, group, 0);
        }
    }
    ma_sound_set_volume(&v->played, std::max(options.volume, 0.0f));
    ma_sound_set_pitch(&v->played, std::clamp(options.pitch, 0.01f, 100.0f));
    ma_sound_set_pan(&v->played, std::clamp(options.pan, -1.0f, 1.0f));
    ma_sound_set_looping(&v->played, options.loop ? MA_TRUE : MA_FALSE);
    check(ma_sound_start(&v->played), "starting '" + it->second->name + "'");
    const VoiceId id = m.next_voice++;
    m.voices[id] = std::move(v);
    return id;
}

void Mixer::stop(VoiceId voice) {
    const auto it = impl_->voices.find(voice);
    if (it == impl_->voices.end()) return;
    impl_->free(*it->second);
    impl_->voices.erase(it);
}

void Mixer::stop_all() {
    for (auto& [id, v] : impl_->voices) impl_->free(*v);
    impl_->voices.clear();
}

void Mixer::set_voice_position(VoiceId voice, const glm::vec3& position) {
    const auto it = impl_->voices.find(voice);
    if (it != impl_->voices.end())
        ma_sound_set_position(&it->second->played, position.x, position.y, position.z);
}

void Mixer::set_voice_muffle(VoiceId voice, float amount) {
    const auto it = impl_->voices.find(voice);
    if (it == impl_->voices.end() || !it->second->filtered) return;
    amount = std::clamp(amount, 0.0f, 1.0f);
    if (std::abs(amount - it->second->muffle) < 0.01f && amount != 0.0f && amount != 1.0f)
        return;  // (no audible change: the filter left as it is)
    it->second->muffle = amount;
    // Its cutoff from wide open down to a wall's, evenly in pitch.
    const double cutoff =
        kOpenCutoff * std::pow(double(kMuffledCutoff) / kOpenCutoff, double(amount));
    const ma_lpf_config lc = ma_lpf_config_init(ma_format_f32, channels(), sample_rate(),
                                                std::min(cutoff, 0.45 * sample_rate()), 2);
    ma_lpf_node_reinit(&lc, &it->second->lpf);
}

void Mixer::set_listener(const glm::vec3& forward, const glm::vec3& up) {
    ma_engine_listener_set_position(&impl_->engine, 0, 0.0f, 0.0f, 0.0f);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0, up.x, up.y, up.z);
}

void Mixer::set_reverb(const ReverbSettings& r) {
    ReverbNode& n = impl_->reverb;
    n.room_size = std::clamp(r.room_size, 0.0f, 1.0f);
    n.damping = std::clamp(r.damping, 0.0f, 1.0f);
    n.wet = std::clamp(r.wet, 0.0f, 1.0f);
    n.width = std::clamp(r.width, 0.0f, 1.0f);
}

ReverbSettings Mixer::reverb() const {
    const ReverbNode& n = impl_->reverb;
    return {n.room_size.load(), n.damping.load(), n.wet.load(), n.width.load()};
}

bool Mixer::playing(VoiceId voice) const {
    const auto it = impl_->voices.find(voice);
    return it != impl_->voices.end() && !ma_sound_at_end(&it->second->played);
}

std::size_t Mixer::voice_count() const {
    return std::size_t(std::count_if(impl_->voices.begin(), impl_->voices.end(),
                                     [](auto& v) { return !ma_sound_at_end(&v.second->played); }));
}

void Mixer::set_voice_volume(VoiceId voice, float volume) {
    const auto it = impl_->voices.find(voice);
    if (it != impl_->voices.end()) ma_sound_set_volume(&it->second->played, std::max(volume, 0.f));
}

void Mixer::set_volume(Bus bus, float volume) {
    Impl& m = *impl_;
    volume = std::max(volume, 0.0f);
    m.volumes[size_t(bus)] = volume;
    if (bus == Bus::Master)
        ma_engine_set_volume(&m.engine, volume);
    else
        ma_sound_group_set_volume(&m.groups[size_t(bus)], volume);
}

float Mixer::volume(Bus bus) const {
    return impl_->volumes[size_t(bus)];
}

void Mixer::mix(std::span<float> out) {
    Impl& m = *impl_;
    if (m.device) throw AudioError("the mixer plays through its device: mix() is for none");
    const ma_uint64 frames = out.size() / channels();
    ma_uint64 read = 0;
    check(ma_engine_read_pcm_frames(&m.engine, out.data(), frames, &read), "mixing");
    std::fill(out.begin() + std::ptrdiff_t(read * channels()), out.end(), 0.0f);
}

void Mixer::update() {
    Impl& m = *impl_;
    for (auto it = m.voices.begin(); it != m.voices.end();) {
        if (ma_sound_at_end(&it->second->played)) {
            m.free(*it->second);
            it = m.voices.erase(it);
        } else {
            ++it;
        }
    }
}

SoundInfo sound_info(std::span<const std::uint8_t> encoded) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    config.ppCustomBackendVTables = g_backends;
    config.customBackendCount = 1;
    ma_decoder decoder;
    check(ma_decoder_init_memory(encoded.data(), encoded.size(), &config, &decoder),
          "reading the sound");
    SoundInfo info;
    ma_format format;
    ma_decoder_get_data_format(&decoder, &format, &info.channels, &info.sample_rate, nullptr, 0);
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) != MA_SUCCESS || frames == 0) {
        // (Not known up front: counted by reading it all.)
        std::vector<float> block(4096 * std::max<ma_uint32>(info.channels, 1));
        ma_uint64 got = 0;
        frames = 0;
        while (ma_decoder_read_pcm_frames(&decoder, block.data(), 4096, &got) == MA_SUCCESS &&
               got > 0)
            frames += got;
    }
    ma_decoder_uninit(&decoder);
    info.seconds = info.sample_rate ? double(frames) / info.sample_rate : 0.0;
    return info;
}

std::vector<std::uint8_t> wav_bytes(std::span<const float> interleaved, std::uint32_t channels,
                                    std::uint32_t sample_rate) {
    std::vector<std::uint8_t> out;
    const auto put = [&](const void* p, size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    const auto u32 = [&](std::uint32_t v) { put(&v, 4); };
    const auto u16 = [&](std::uint16_t v) { put(&v, 2); };
    const auto data_bytes = std::uint32_t(interleaved.size() * sizeof(float));
    put("RIFF", 4);
    u32(36 + data_bytes);
    put("WAVEfmt ", 8);
    u32(16);
    u16(3);  // IEEE float
    u16(std::uint16_t(channels));
    u32(sample_rate);
    u32(sample_rate * channels * 4);
    u16(std::uint16_t(channels * 4));
    u16(32);
    put("data", 4);
    u32(data_bytes);
    put(interleaved.data(), data_bytes);
    return out;
}

}  // namespace midgard::audio
