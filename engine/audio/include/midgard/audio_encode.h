// SPDX-License-Identifier: Apache-2.0
// Sound files made smaller for games (milestone 5.14): the cook turns the sounds a game uses
// into Ogg Vorbis (music and ambience: long, rich, streamed) or Ogg Opus (effects, voice and the
// interface: short, many, quick to start). The editor does this; games only read them
// (midgard/audio.h), so the encoders live in their own library, linked by the editor's module.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace midgard::audio {

enum class Codec { Vorbis, Opus };
const char* codec_name(Codec codec);  // "vorbis", "opus"
std::optional<Codec> codec_from_name(std::string_view name);

struct EncodeOptions {
    float vorbis_quality = 0.5f;  // -0.1 to 1 (0.5: about 160 kbit/s for stereo music)
    int opus_bitrate = 0;         // bits a second (0: 64 kbit/s mono, 96 kbit/s stereo)
};

// A sound file (WAV, FLAC, MP3, Ogg Vorbis or Ogg Opus) as an Ogg Vorbis or Ogg Opus file, its
// channels kept (more than two mixed down to stereo). The same input always gives the same
// bytes. AudioError if it can't be read, or is empty.
std::vector<std::uint8_t> encode_sound(std::span<const std::uint8_t> input, Codec codec,
                                       const EncodeOptions& options = {});

}  // namespace midgard::audio
