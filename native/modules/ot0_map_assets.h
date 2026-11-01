// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ot0_pak.h"
namespace ot0 {
// Accepts the exact current native texture UObject path (/Game/.../Name.Name).
// No archive directory or keys are needed at runtime; only current revealed
// data may be composed by the caller. Every texture uses header/stored/decoded
// pins.
std::optional<Image> LoadMapTexture(const RangeReader &read,
                                    std::string_view object_path,
                                    std::string *why = nullptr);
std::size_t MapTextureCount();
} // namespace ot0
