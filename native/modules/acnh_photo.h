// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The passport photo: Player.ProfileMain.JPEG (personal.dat 0x13540: +0x10 s32 size, +0x14 data,
// research/acnh/catalog/DATA.md §6) is a baseline JPEG (500x500 / 350x350 on the test saves). The
// live lane copies the bytes out of guest memory; this decodes them with stb_image (JPEG only,
// private static copy; stb_image is public domain / MIT, see its header).
#pragma once

#include <cstdint>
#include <span>

#include "acnh_types.h"

namespace acnh {

/// JPEG -> RGBA8 (alpha 255). False for anything that is not a sane JPEG (<= 4096 px a side).
bool DecodeJpeg(std::span<const uint8_t> jpeg, Image& out);

} // namespace acnh
