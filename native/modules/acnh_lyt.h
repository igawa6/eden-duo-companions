// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH UI layouts (Layout/<Name>.Nin_NX_NVN.zs: blyt/*.bflyt v9, anim/*.bflan,
// timg/__Combined.bntx) for the companion's art recipes. Port of research/acnh/tools/acnh_bflyt.py,
// acnh_bflan.py and acnh_compose.py (section layouts from Switch-Toolbox, a LEAD, verified there by
// every section size summing to the file size for all 960 layouts):
//   - pane tree (pan1/pic1/txt1/wnd1/prt1/bnd1...), transforms, flags, alpha, pic1 vertex colours
//   - materials: name, black/white colour, texture names of the texmaps
//   - BFLAN pai1: FLVI (visibility), FLMC (material colour: targets 0-3 black RGBA, 4-7 white),
//     FLTP (texture pattern index into the anim's texture list); value = last key <= frame
//   - Compose(): draws a pane subtree like acnh_compose: translate/rotate/scale/size/origin,
//     visibility (optionally from an anim frame), pane alpha, vertex colour and the material tint
//     of the FIRST texmap. Not reproduced (as in the research tool): extra texmaps / TEV stages /
//     combiner shaders, texture SRT, window panes, text, masks.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "acnh_romfs.h"
#include "acnh_types.h"

namespace acnh {

struct LytMaterial {
    std::string name;
    Rgba black{0, 0, 0, 0}, white{255, 255, 255, 255};
    std::vector<std::string> tex; ///< texmap texture names
};

struct LytPane {
    std::string tag, name;
    float w = 0, h = 0, x = 0, y = 0, sx = 1, sy = 1, rz = 0;
    uint8_t origin = 0, flags = 0, alpha = 255;
    bool visible = true;
    std::array<Rgba, 4> vtx{};
    int material = -1;
    std::vector<int> children;
};

struct Bflyt {
    std::vector<std::string> textures;
    std::vector<LytMaterial> materials;
    std::vector<LytPane> panes;
    bool Parse(const std::vector<uint8_t>& bytes);
    int Find(std::string_view pane) const;
    int Material(std::string_view name) const;
};

struct BflanKey {
    float frame;
    float value;
};
struct BflanTag {
    std::string magic; ///< FLVI, FLMC, FLTP, FLPA, ...
    uint8_t index = 0, target = 0, curve = 0;
    std::vector<BflanKey> keys;
};
struct BflanEntry {
    std::string name;
    std::vector<BflanTag> tags;
};
struct Bflan {
    std::vector<std::string> textures;
    std::vector<BflanEntry> entries;
    bool Parse(const std::vector<uint8_t>& bytes);
    /// Value of (entry, magic, index, target) at `frame` (last key <= frame; else the first key).
    std::optional<float> At(std::string_view entry, std::string_view magic, int index, int target,
                            float frame) const;
};

/// A decoded layout: tree, anims, and its texture archive.
class Layout {
public:
    static std::shared_ptr<Layout> Load(Romfs& romfs, std::string_view name);
    const Bflyt& Tree() const {
        return tree;
    }
    const Bflan* Anim(std::string_view name) const; ///< "LMenuDeviceBtn_Type"
    /// A texture of this layout (BRTI selectors applied); cached.
    std::shared_ptr<const Image> Texture(std::string_view name) const;

    /// Material colours at an anim frame (FLMC overrides; untouched targets keep `black`/`white`).
    void AnimMaterial(std::string_view anim, float frame, std::string_view material, Rgba& black,
                      Rgba& white) const;
    bool AnimVisible(std::string_view anim, float frame, std::string_view pane,
                     bool fallback) const;
    /// FLTP texture name of a material at a frame ("" when not animated).
    std::string AnimTexture(std::string_view anim, float frame, std::string_view material) const;

    struct ComposeOptions {
        std::string anim;
        float frame = 0;
        int canvas_w = 400, canvas_h = 400;
        double scale = 1.0;
        double center_x = 0, center_y = 0;
        std::vector<std::string> hide;
    };
    /// acnh_compose.compose(): the children of `root` drawn on a transparent canvas, layout y up.
    Image Compose(std::string_view root, const ComposeOptions& opt) const;

private:
    Bflyt tree;
    std::map<std::string, Bflan, std::less<>> anims;
    std::shared_ptr<const Archive> archive; ///< keeps `bntx` alive (no copy)
    const std::vector<uint8_t>* bntx = nullptr;
    mutable std::mutex tex_mutex;
    mutable std::map<std::string, std::shared_ptr<const Image>, std::less<>> tex_cache;
};

/// Romfs-instance layout cache shared by art and map code (thread-safe).
std::shared_ptr<Layout> LoadLayout(Romfs& romfs, std::string_view name);

} // namespace acnh
