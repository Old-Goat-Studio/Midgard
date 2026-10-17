// SPDX-License-Identifier: Apache-2.0
// stb_image, compiled once: decoding the PNG and JPEG images of imported models. Only those two
// formats are built in, and images are limited in size, so a damaged or hostile file can't ask
// for more than a large texture's memory.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS 16384
#define STBI_NO_STDIO
#include <stb_image.h>
