// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "xcx_assets.h"
#include <array>
namespace xcx {
// Offline native geometry preparation only. Not a current equipped appearance:
// attachments, animation, skinning and live paint/material binding are unresolved.
struct FrameModel {
    std::uint16_t frame{}, name_id{};
    std::string resource;
    // Skeleton/animation set (table 130 column 10, e.g. "dl069100" for the dl069110 model); empty
    // when that column does not name a supported resource.
    std::string motion;
};
struct ModelVertex {
    std::array<float, 3> position{};
    std::array<float, 4> normal{}, tangent{};
    std::array<float, 2> uv{};
    // WeightIndex column 0 (row of the weight buffer). Zero when the buffer has no such column.
    std::uint16_t weight_index{};
};
// Skeleton + weight rows, as stored in the model. Positions in the file are already in the bind
// pose, so nothing here is needed to draw the bind pose; it exists to move named bone subtrees.
struct ModelBone {
    std::string name;
    int parent{-1};
    std::array<float, 3> bind_position{}; // world position derived from the inverse bind matrix
    // World rotation (row-major, column-vector convention: world = R * local + position).
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
};
struct WeightRow {
    std::array<std::uint8_t, 4> bone{}; // index into FrameSkin::skin_names
    std::array<float, 4> weight{};
};
struct FrameSkin {
    std::vector<ModelBone> bones;
    std::vector<std::string> skin_names;
    std::vector<std::vector<WeightRow>> weight_buffers; // one entry per weight vertex buffer
    std::vector<unsigned> weight_buffer_vertex_buffer;  // vertex-buffer index of each entry
};
struct ModelMesh {
    unsigned vertex_buffer{}, index_buffer{}, material{}, flags{};
};
struct ModelTextureBinding {
    std::uint16_t texture{}, sampler_flags{};
};
struct ModelMaterial {
    std::string name;
    std::array<float, 4> color{};
    std::vector<ModelTextureBinding> textures;
};
struct ModelTextureSource {
    std::string name, mip_path, base_mip_path;
    std::uint32_t usage{}, low_offset{}, low_size{};
};
struct FrameGeometry {
    std::vector<std::vector<ModelVertex>> vertices;
    std::vector<std::vector<std::uint16_t>> indices;
    std::vector<ModelMesh> meshes;
    // Native bindings only. No shader-channel or current-paint semantics inferred.
    std::vector<ModelMaterial> materials;
    std::vector<ModelTextureSource> texture_sources;
    Bytes low_texture_data;
    FrameSkin skin;
};
// A bounded, per-frame preparation batch. No pixel cache and no production linkage.
class FrameTextureReader {
    struct Entry {
        std::uint64_t offset;
        std::uint32_t size;
    };
    std::map<std::uint64_t, Entry> entries;
    bool initialized{};
    std::uint64_t decoded_texels{};
    Archive *shared{}; // when set, its index is used instead of a private copy
    Bytes ReadSource(const EdenDsmodHostApi &, std::string_view);

  public:
    FrameTextureReader() = default;
    explicit FrameTextureReader(Archive &archive) : shared(&archive) {}
    Image Read(const EdenDsmodHostApi &, const FrameGeometry &, unsigned texture_index);
};
Image DecodeModelTexture(std::span<const std::uint8_t> mibl,
                         std::span<const std::uint8_t> base_mip = {},
                         std::uint64_t remaining_texels = 4 * 1024 * 1024);
Bytes DecodeModelStream(std::span<const std::uint8_t>, unsigned expected_size);
FrameModel OwnedFrameModel(Archive &, const EdenDsmodHostApi &,
                           std::span<const std::uint8_t> owned_record);
// The same table-130 lookup for a frame row id read from the saved record (Member::frame).
FrameModel FrameModelForRow(Archive &, const EdenDsmodHostApi &, unsigned frame_row);
FrameGeometry ReadFrameGeometry(Archive &, const EdenDsmodHostApi &, const FrameModel &);
// A Skell weapon model by resource name ("wd0345012r"): /chr/wd/<name>.wimdo/.wismt.
FrameGeometry ReadWeaponGeometry(Archive &, const EdenDsmodHostApi &, std::string_view name);
// Public for bounded malformed-format checks without game archive modification.
FrameGeometry DecodeFrameGeometry(std::span<const std::uint8_t> wimdo,
                                  std::span<const std::uint8_t> wismt);
} // namespace xcx
