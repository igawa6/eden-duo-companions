// SPDX-FileCopyrightText: Copyright 2026 Fab
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace KirbyLayout {

struct Material {
    std::string name;
    std::vector<int> textures; // texture maps, indices into Layout::textures
    std::uint8_t black[4]{};
    std::uint8_t white[4]{};
};

struct Pane {
    std::string name;
    std::string kind; // pan1, pic1, txt1, wnd1, prt1, bnd1
    int parent{-1};
    float tx{}, ty{}, sx{1}, sy{1}, w{}, h{};
    std::uint8_t alpha{255};
    bool visible{true};
    int material{-1}; // pic1 only
};

struct Layout {
    std::vector<std::string> textures;
    std::vector<Material> materials;
    std::vector<Pane> panes;
};

/// The parts of a BFLYT a static picture needs: texture names, material colours and texture maps,
/// and the pane tree with translate, scale, size, alpha and visibility (rotation is ignored).
std::optional<Layout> Parse(std::span<const std::uint8_t> bflyt);

} // namespace KirbyLayout
