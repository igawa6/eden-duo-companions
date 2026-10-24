// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_assets.h"
#include "core/mods/dsmod_module_extensions.h"
#include <array>
#include <span>

namespace echoes {
struct MapBounds { float left{}, top{}, width{}, height{}; };
struct MapPane {
    std::string name, part_layout;
    int parent = -1;
    float x{}, y{}, z{}, width{}, height{}, scale_x{}, scale_y{};
    float rotation_x{}, rotation_y{}, rotation_z{};
    uint8_t origin{}, flags{}, alpha{};
};
struct MapMaterial {
    std::string name;
    uint32_t flags{};
    std::array<uint8_t,4> black{}, white{};
    std::vector<std::string> textures;
    // Preserve original shader/texture-transform fields without assuming their evaluation.
    std::vector<uint8_t> native_record;
};
struct MapPicture {
    MapPane pane;
    MapMaterial material;
    std::array<std::array<uint8_t,4>,4> vertex_colors{};
    std::vector<std::array<float,8>> uv;
};
struct TraverseShape {
    uint16_t index{};
    MapPane pane;
    std::array<MapPicture,2> pictures;
};
struct MapTextureInfo {
    std::string name;
    uint32_t width{}, height{}, format{}, selectors{}, layout{}, data_size{}, tile_mode{};
    uint64_t data_offset{};
};
struct FieldMapGeometry {
    std::vector<MapPane> field_panes, traverse_panes, part_panes, attach_panes;
    std::vector<MapBounds> excluded_quads;
    std::vector<size_t> field_extension_quads, sandstorm_quads;
    std::vector<MapPicture> terrain;
    std::vector<TraverseShape> shapes;
    std::vector<MapTextureInfo> textures;
};
// Original FLYT v9 parsing only. No extracted geometry or native bitmaps are bundled.
bool ParseFieldMapLayouts(std::span<const uint8_t> field, std::span<const uint8_t> traverse,
                          FieldMapGeometry& out);
enum class MapComposeStatus { MissingOrInvalidSource, UnsupportedMaterialAndStoryVisibility,
                              UnsupportedDecoderOrDiscovery, Ready };
// Asset-worker-only. Original terrain layers retain authored bounds and aspect. Only explored
// mask interiors survive; unknown story/sandstorm quads and unvisited overlapping fog stay
// transparent. Decoder calls use bounded ASTC tiles. There is no raw-terrain fallback.
// Bounds use native layout coordinates (y up); fit the image without changing its aspect.
class FieldMapAssets {
public:
    explicit FieldMapAssets(ReadAssetFn read, void* decode_userdata = nullptr,
                            EdenDsmodAstcDecoder decode_astc = nullptr);
    ~FieldMapAssets();
    bool Inspect(FieldMapGeometry& out);
    bool Compose(std::span<const uint8_t,64> discovered, Image& out, MapBounds& bounds,
                 MapComposeStatus* status = nullptr, bool field_extensions_hidden = false, bool sandstorm_hidden = false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace echoes
