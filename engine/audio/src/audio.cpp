// SPDX-License-Identifier: Apache-2.0
#include "midgard/audio.h"

#include "opus_backend.h"

#include <algorithm>
#include <array>
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

}  // namespace

const char* bus_name(Bus bus) {
    return kBusNames[size_t(bus)];
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
    };
    std::unordered_map<VoiceId, std::unique_ptr<Voice>> voices;
    VoiceId next_voice = 1;

    ~Impl() {
        for (auto& [id, v] : voices) ma_sound_uninit(&v->played);
        voices.clear();
        for (auto& [id, s] : sounds) {
            ma_sound_uninit(&s->decoded);
            ma_resource_manager_unregister_data(&resources, s->name.c_str());
        }
        for (int i = 1; i < kBusCount; ++i) ma_sound_group_uninit(&groups[size_t(i)]);
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
            ma_sound_uninit(&v->second->played);
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
    check(ma_sound_init_copy(&m.engine, &it->second->decoded, MA_SOUND_FLAG_NO_SPATIALIZATION,
                             &m.groups[size_t(options.bus)], &v->played),
          "playing '" + it->second->name + "'");
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
    ma_sound_uninit(&it->second->played);
    impl_->voices.erase(it);
}

void Mixer::stop_all() {
    for (auto& [id, v] : impl_->voices) ma_sound_uninit(&v->played);
    impl_->voices.clear();
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
            ma_sound_uninit(&it->second->played);
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
