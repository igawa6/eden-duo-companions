// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "acnh_photo.h"

#include <cstring>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 4096
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace acnh {

bool DecodeJpeg(std::span<const uint8_t> jpeg, Image& out) {
    if (jpeg.size() < 4 || jpeg.size() > (16u << 20) || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
        return false;
    int w = 0, h = 0, n = 0;
    unsigned char* px =
        stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &w, &h, &n, 4);
    if (!px)
        return false;
    out = Image{w, h};
    std::memcpy(out.rgba.data(), px, out.rgba.size());
    stbi_image_free(px);
    return true;
}

} // namespace acnh
