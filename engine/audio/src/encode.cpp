// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstring>
#include <string>

#include "midgard/audio.h"
#include "midgard/audio_encode.h"
#include "opus_backend.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <opus/opusenc.h>
#include <vorbis/vorbisenc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace midgard::audio {
namespace {

constexpr int kSerial = 0x4d494447;  // ("MIDG": the same bytes from the same sound, every time)

struct Pcm {
    std::vector<float> samples;  // interleaved
    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
    std::size_t frames() const { return channels ? samples.size() / channels : 0; }
};

Pcm decode(std::span<const std::uint8_t> input) {
    ma_decoding_backend_vtable* backends[] = {opus_backend()};
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    config.ppCustomBackendVTables = backends;
    config.customBackendCount = 1;
    ma_decoder decoder;
    if (ma_decoder_init_memory(input.data(), input.size(), &config, &decoder) != MA_SUCCESS)
        throw AudioError("the sound can't be read: WAV, FLAC, MP3, Ogg Vorbis and Ogg Opus can");
    ma_format format;
    ma_uint32 channels = 0, rate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &rate, nullptr, 0);
    if (channels > 2) {  // (surround: mixed down to stereo)
        ma_decoder_uninit(&decoder);
        config.channels = 2;
        if (ma_decoder_init_memory(input.data(), input.size(), &config, &decoder) != MA_SUCCESS)
            throw AudioError("the sound can't be read");
        channels = 2;
    }
    Pcm pcm;
    pcm.channels = channels;
    pcm.rate = rate;
    std::vector<float> block(4096 * channels);
    ma_uint64 got = 0;
    while (ma_decoder_read_pcm_frames(&decoder, block.data(), 4096, &got) == MA_SUCCESS && got > 0)
        pcm.samples.insert(pcm.samples.end(), block.begin(),
                           block.begin() + std::ptrdiff_t(got * channels));
    ma_decoder_uninit(&decoder);
    if (pcm.frames() == 0 || channels == 0 || rate == 0) throw AudioError("the sound is empty");
    return pcm;
}

std::vector<std::uint8_t> vorbis(const Pcm& pcm, float quality) {
    vorbis_info info;
    vorbis_info_init(&info);
    if (vorbis_encode_init_vbr(&info, long(pcm.channels), long(pcm.rate),
                               std::clamp(quality, -0.1f, 1.0f)) != 0) {
        vorbis_info_clear(&info);
        throw AudioError("Vorbis can't encode " + std::to_string(pcm.channels) + " channels at " +
                         std::to_string(pcm.rate) + " Hz");
    }
    vorbis_comment comment;
    vorbis_comment_init(&comment);
    vorbis_dsp_state dsp;
    vorbis_block block;
    vorbis_analysis_init(&dsp, &info);
    vorbis_block_init(&dsp, &block);
    ogg_stream_state stream;
    ogg_stream_init(&stream, kSerial);
    std::vector<std::uint8_t> out;
    ogg_page page;
    const auto write = [&](const ogg_page& p) {
        out.insert(out.end(), p.header, p.header + p.header_len);
        out.insert(out.end(), p.body, p.body + p.body_len);
    };
    ogg_packet header, header_comment, header_code;
    vorbis_analysis_headerout(&dsp, &comment, &header, &header_comment, &header_code);
    ogg_stream_packetin(&stream, &header);
    ogg_stream_packetin(&stream, &header_comment);
    ogg_stream_packetin(&stream, &header_code);
    while (ogg_stream_flush(&stream, &page)) write(page);
    // The sound a block at a time, then the end; packets out as the encoder makes them.
    const auto drain = [&] {
        ogg_packet packet;
        while (vorbis_analysis_blockout(&dsp, &block) == 1) {
            vorbis_analysis(&block, nullptr);
            vorbis_bitrate_addblock(&block);
            while (vorbis_bitrate_flushpacket(&dsp, &packet)) {
                ogg_stream_packetin(&stream, &packet);
                while (ogg_stream_pageout(&stream, &page)) write(page);
            }
        }
    };
    const std::size_t frames = pcm.frames();
    for (std::size_t at = 0; at < frames; at += 1024) {
        const int n = int(std::min<std::size_t>(1024, frames - at));
        float** buffer = vorbis_analysis_buffer(&dsp, n);
        for (int i = 0; i < n; ++i)
            for (std::uint32_t c = 0; c < pcm.channels; ++c)
                buffer[c][i] = pcm.samples[(at + std::size_t(i)) * pcm.channels + c];
        vorbis_analysis_wrote(&dsp, n);
        drain();
    }
    vorbis_analysis_wrote(&dsp, 0);
    drain();
    while (ogg_stream_flush(&stream, &page)) write(page);
    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    return out;
}

int opus_write(void* user, const unsigned char* data, opus_int32 length) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(user);
    out->insert(out->end(), data, data + length);
    return 0;
}

int opus_close(void*) {
    return 0;
}

std::vector<std::uint8_t> opus(const Pcm& pcm, int bitrate) {
    std::vector<std::uint8_t> out;
    OpusEncCallbacks callbacks = {opus_write, opus_close};
    OggOpusComments* comments = ope_comments_create();
    int error = 0;
    OggOpusEnc* enc = ope_encoder_create_callbacks(&callbacks, &out, comments, opus_int32(pcm.rate),
                                                   int(pcm.channels), 0, &error);
    if (!enc) {
        ope_comments_destroy(comments);
        throw AudioError(std::string("Opus can't encode it: ") + ope_strerror(error));
    }
    ope_encoder_ctl(enc, OPE_SET_SERIALNO(kSerial));
    ope_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate > 0         ? bitrate
                                          : pcm.channels == 1 ? 64000
                                                              : 96000));
    ope_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
    error = ope_encoder_write_float(enc, pcm.samples.data(), int(pcm.frames()));
    if (error == OPE_OK) error = ope_encoder_drain(enc);
    ope_encoder_destroy(enc);
    ope_comments_destroy(comments);
    if (error != OPE_OK) throw AudioError(std::string("Opus encoding: ") + ope_strerror(error));
    return out;
}

}  // namespace

const char* codec_name(Codec codec) {
    return codec == Codec::Vorbis ? "vorbis" : "opus";
}

std::optional<Codec> codec_from_name(std::string_view name) {
    if (name == "vorbis") return Codec::Vorbis;
    if (name == "opus") return Codec::Opus;
    return std::nullopt;
}

std::vector<std::uint8_t> encode_sound(std::span<const std::uint8_t> input, Codec codec,
                                       const EncodeOptions& options) {
    const Pcm pcm = decode(input);
    return codec == Codec::Vorbis ? vorbis(pcm, options.vorbis_quality)
                                  : opus(pcm, options.opus_bitrate);
}

}  // namespace midgard::audio
