// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "kirby_assets.h"

// Merry Magoland's attraction doors (gfx/Common/Gimmick/CastleGate: GateModel with one of GateDecoModel's ten models) and
// Samurai Kirby 100's globe (gfx/Yy/Entrance/Gimmick/SetsunaNetEnter), drawn from the front in software with the sticker
// look the Magoland page gives them: a thin black outline, a white border and a soft shadow.
namespace KirbyGates {
using KirbyAssets::Image;

// a texture of the model's own BNTX by the material's name for it (hash suffix included)
using Textures = std::function<std::shared_ptr<const Image>(const std::string&)>;

// GateDecoModel's models: EggCatcher, Hayauchi, Ochiochi, Otsukai, Punch, Rally, Ride, Setsuna, Shuriken, Tps
constexpr int Doors = 10;
// the models' files (/gfx/<file>.bfres.cmp) and their textures' keys (gfx/<file>/<texture>)
constexpr std::string_view GateFile = "Common/Gimmick/CastleGate/GateModel";
constexpr std::string_view DecoFile = "Common/Gimmick/CastleGate/GateDecoModel";
constexpr std::string_view GlobeFile = "Yy/Entrance/Gimmick/SetsunaNetEnter/Model";
// every door in the same framing (the gate as large in each), centred on all the decorations' bounds
constexpr std::array<std::uint32_t, 2> DoorSize{330, 216}, GlobeSize{380, 480};

std::optional<Image> Door(std::span<const std::uint8_t> gate, const Textures& gate_textures, std::span<const std::uint8_t> decos,
                          const Textures& deco_textures, int deco, int ss);
std::optional<Image> Globe(std::span<const std::uint8_t> model, const Textures& textures, int ss);

} // namespace KirbyGates
