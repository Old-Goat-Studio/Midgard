// SPDX-License-Identifier: Apache-2.0
#include "opus_backend.h"

#include <algorithm>
#include <cstdio>
#include <new>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <opusfile.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace midgard::audio {
namespace {

constexpr ma_uint32 kRate = 48000;  // (Opus decodes at 48 kHz)
constexpr ma_uint32 kChannels = 2;  // (read as stereo: mono is spread to both)

struct OpusSource {
    ma_data_source_base base;  // (first: miniaudio treats this as the data source)
    OggOpusFile* file = nullptr;
    // A stream's callbacks (when decoded from a stream rather than memory).
    ma_read_proc read = nullptr;
    ma_seek_proc seek = nullptr;
    ma_tell_proc tell = nullptr;
    void* stream = nullptr;
};

ma_result source_read(ma_data_source* ds, void* out, ma_uint64 frames, ma_uint64* read) {
    auto* s = reinterpret_cast<OpusSource*>(ds);
    auto* f = static_cast<float*>(out);
    ma_uint64 total = 0;
    while (total < frames) {
        const int want = int(std::min<ma_uint64>(frames - total, 5760)) * int(kChannels);
        const int got = op_read_float_stereo(s->file, f + total * kChannels, want);
        if (got == OP_HOLE) continue;  // (a gap in the stream: carry on after it)
        if (got <= 0) break;
        total += ma_uint64(got);
    }
    if (read) *read = total;
    return total == 0 && frames > 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result source_seek(ma_data_source* ds, ma_uint64 frame) {
    auto* s = reinterpret_cast<OpusSource*>(ds);
    return op_pcm_seek(s->file, ogg_int64_t(frame)) == 0 ? MA_SUCCESS : MA_ERROR;
}

ma_result source_format(ma_data_source*, ma_format* format, ma_uint32* channels, ma_uint32* rate,
                        ma_channel* map, size_t map_capacity) {
    if (format) *format = ma_format_f32;
    if (channels) *channels = kChannels;
    if (rate) *rate = kRate;
    if (map)
        ma_channel_map_init_standard(ma_standard_channel_map_default, map, map_capacity, kChannels);
    return MA_SUCCESS;
}

ma_result source_cursor(ma_data_source* ds, ma_uint64* cursor) {
    const ogg_int64_t at = op_pcm_tell(reinterpret_cast<OpusSource*>(ds)->file);
    *cursor = at < 0 ? 0 : ma_uint64(at);
    return at < 0 ? MA_ERROR : MA_SUCCESS;
}

ma_result source_length(ma_data_source* ds, ma_uint64* length) {
    const ogg_int64_t total = op_pcm_total(reinterpret_cast<OpusSource*>(ds)->file, -1);
    *length = total < 0 ? 0 : ma_uint64(total);
    return total < 0 ? MA_NOT_IMPLEMENTED : MA_SUCCESS;
}

ma_data_source_vtable g_source = {
    source_read, source_seek, source_format, source_cursor, source_length, nullptr, 0};

ma_result wrap(OggOpusFile* file, OpusSource* s, ma_data_source** out) {
    s->file = file;
    ma_data_source_config config = ma_data_source_config_init();
    config.vtable = &g_source;
    const ma_result r = ma_data_source_init(&config, &s->base);
    if (r == MA_SUCCESS) *out = &s->base;
    return r;
}

OpusSource* make(const ma_allocation_callbacks* alloc) {
    void* memory = ma_malloc(sizeof(OpusSource), alloc);
    return memory ? new (memory) OpusSource() : nullptr;
}

void release(OpusSource* s, const ma_allocation_callbacks* alloc) {
    if (!s) return;
    if (s->file) op_free(s->file);
    s->~OpusSource();
    ma_free(s, alloc);
}

// From a stream (miniaudio's own reads and seeks).
int stream_read(void* user, unsigned char* ptr, int bytes) {
    auto* s = static_cast<OpusSource*>(user);
    size_t got = 0;
    const ma_result r = s->read(s->stream, ptr, size_t(bytes), &got);
    return r == MA_SUCCESS || r == MA_AT_END ? int(got) : -1;
}

int stream_seek(void* user, opus_int64 offset, int whence) {
    auto* s = static_cast<OpusSource*>(user);
    const ma_seek_origin origin = whence == SEEK_SET   ? ma_seek_origin_start
                                  : whence == SEEK_CUR ? ma_seek_origin_current
                                                       : ma_seek_origin_end;
    return s->seek(s->stream, ma_int64(offset), origin) == MA_SUCCESS ? 0 : -1;
}

opus_int64 stream_tell(void* user) {
    auto* s = static_cast<OpusSource*>(user);
    ma_int64 at = 0;
    return s->tell(s->stream, &at) == MA_SUCCESS ? opus_int64(at) : -1;
}

ma_result init_stream(void*, ma_read_proc read, ma_seek_proc seek, ma_tell_proc tell, void* stream,
                      const ma_decoding_backend_config*, const ma_allocation_callbacks* alloc,
                      ma_data_source** out) {
    OpusSource* s = make(alloc);
    if (!s) return MA_OUT_OF_MEMORY;
    s->read = read;
    s->seek = seek;
    s->tell = tell;
    s->stream = stream;
    const OpusFileCallbacks callbacks = {stream_read, stream_seek, stream_tell, nullptr};
    int error = 0;
    OggOpusFile* file = op_open_callbacks(s, &callbacks, nullptr, 0, &error);
    if (!file) {
        release(s, alloc);
        return MA_INVALID_FILE;  // (not Opus: miniaudio tries its other decoders)
    }
    const ma_result r = wrap(file, s, out);
    if (r != MA_SUCCESS) release(s, alloc);
    return r;
}

ma_result init_memory(void*, const void* data, size_t size, const ma_decoding_backend_config*,
                      const ma_allocation_callbacks* alloc, ma_data_source** out) {
    int error = 0;
    OggOpusFile* file = op_open_memory(static_cast<const unsigned char*>(data), size, &error);
    if (!file) return MA_INVALID_FILE;
    OpusSource* s = make(alloc);
    if (!s) {
        op_free(file);
        return MA_OUT_OF_MEMORY;
    }
    const ma_result r = wrap(file, s, out);
    if (r != MA_SUCCESS) release(s, alloc);
    return r;
}

void uninit(void*, ma_data_source* ds, const ma_allocation_callbacks* alloc) {
    auto* s = reinterpret_cast<OpusSource*>(ds);
    ma_data_source_uninit(&s->base);
    release(s, alloc);
}

ma_decoding_backend_vtable g_backend = {init_stream, nullptr, nullptr, init_memory, uninit};

}  // namespace

ma_decoding_backend_vtable* opus_backend() {
    return &g_backend;
}

}  // namespace midgard::audio
