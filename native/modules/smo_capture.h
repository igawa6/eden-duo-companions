// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fe3h_ui2d.h"
#include "core/mods/dsmod_module_extensions.h"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SmoCapture {
// Runtime-loaded ObjectData/HackListTexture.szs, packed Yaz0 or expanded SARC.
// The decoder is the host extension's RGBA8 ASTC service, with its matching userdata.
// No channel swap: BRTI selectors are applied to the host's RGBA output.
std::optional<fe3h_ui2d::Texture> DecodeArchive(std::span<const uint8_t> archive,
                                              std::string_view capture, void* userdata,
                                              EdenDsmodAstcDecoder decode);
// Validated native portrait names; no image decode or host callback is needed.
std::vector<std::string> Names(std::span<const uint8_t> archive);
} // namespace SmoCapture
