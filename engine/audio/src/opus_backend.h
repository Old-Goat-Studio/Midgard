// SPDX-License-Identifier: Apache-2.0
// Ogg Opus files for miniaudio's decoders, through opusfile (always 48 kHz stereo, float).
#pragma once

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace midgard::audio {

ma_decoding_backend_vtable* opus_backend();

}  // namespace midgard::audio
