// SPDX-License-Identifier: Apache-2.0
// miniaudio's implementation, built once (and without the engine's warnings: it's theirs), with
// stb_vorbis for Ogg Vorbis files.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_GENERATION  // (the engine makes its own test tones)
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
