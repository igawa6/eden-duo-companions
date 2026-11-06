// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_follower_portrait.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>

namespace cotl {

namespace {

struct SkinEntry {
    std::string_view name;
    std::vector<int> parts;
};

#define COTL_PORTRAIT_PART(slot, region, a, b, c, d, x, y) PortraitPart{slot, region, {a, b, c, d, x, y}},
const PortraitPart kParts[] = {
#include "cotl_follower_parts_1_5_27.inc"
};
#undef COTL_PORTRAIT_PART

const std::map<std::string_view, std::vector<int>>& Skins() {
    static const std::map<std::string_view, std::vector<int>> skins = [] {
        std::map<std::string_view, std::vector<int>> m;
#define COTL_PORTRAIT_SKIN(name, ...) m.emplace(name, std::vector<int>{__VA_ARGS__});
#include "cotl_follower_parts_1_5_27.inc"
#undef COTL_PORTRAIT_SKIN
        return m;
    }();
    return skins;
}

std::optional<uint32_t> Hex8(std::string_view s) {
    if (s.size() != 8) {
        return std::nullopt;
    }
    uint32_t v = 0;
    for (const char ch : s) {
        int d = -1;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        if (d < 0) {
            return std::nullopt;
        }
        v = v << 4 | static_cast<uint32_t>(d);
    }
    return v;
}

// Bilinear, premultiplied; transparent outside the image
void Sample(const ce_unity::Image& img, float x, float y, float out[4]) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const float tx = x - fx, ty = y - fy;
    for (int c = 0; c < 4; ++c) {
        out[c] = 0;
    }
    const int w = static_cast<int>(img.width), h = static_cast<int>(img.height);
    for (int k = 0; k < 4; ++k) {
        const int sx = x0 + (k & 1), sy = y0 + (k >> 1);
        if (sx < 0 || sy < 0 || sx >= w || sy >= h) {
            continue;
        }
        const float wgt = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
        const uint8_t* p = &img.rgba[(static_cast<size_t>(sy) * img.width + sx) * 4];
        const float a = p[3] / 255.0f * wgt;
        out[0] += p[0] / 255.0f * a;
        out[1] += p[1] / 255.0f * a;
        out[2] += p[2] / 255.0f * a;
        out[3] += a;
    }
}

} // namespace

std::vector<const PortraitPart*> PortraitParts(std::string_view skin) {
    std::vector<const PortraitPart*> out;
    const auto it = Skins().find(skin);
    if (it == Skins().end()) {
        return out;
    }
    for (const int i : it->second) {
        if (i >= 0 && static_cast<size_t>(i) < std::size(kParts)) {
            out.push_back(&kParts[i]);
        }
    }
    return out;
}

std::string PortraitArg(std::string_view skin, std::span<const SlotTint> tints) {
    std::string out(skin);
    for (const auto& t : tints) {
        char hex[9];
        std::snprintf(hex, sizeof hex, "%08X", static_cast<unsigned>(t.rgba));
        out += ";" + t.slot + "=" + hex;
    }
    return out;
}

std::string PortraitKey(std::string_view skin, std::span<const SlotTint> tints) {
    const auto parts = PortraitParts(skin);
    if (parts.empty()) {
        return {};
    }
    std::vector<SlotTint> used;
    for (const auto& t : tints) {
        const bool drawn = std::any_of(parts.begin(), parts.end(),
                                       [&](const PortraitPart* p) { return p->slot == t.slot; });
        if (drawn && t.rgba != 0xFFFFFFFF) {
            used.push_back(t);
        }
    }
    return "module:cotl:follower/" + PortraitArg(skin, used);
}

std::optional<PortraitSpec> ParsePortraitArg(std::string_view arg) {
    PortraitSpec spec;
    auto semi = arg.find(';');
    spec.skin = std::string(arg.substr(0, semi));
    if (spec.skin.empty()) {
        return std::nullopt;
    }
    while (semi != std::string_view::npos) {
        arg.remove_prefix(semi + 1);
        semi = arg.find(';');
        const std::string_view item = arg.substr(0, semi);
        const auto eq = item.find('=');
        const auto rgba = eq == std::string_view::npos ? std::nullopt : Hex8(item.substr(eq + 1));
        if (!rgba || eq == 0) {
            return std::nullopt;
        }
        spec.tints.push_back({std::string(item.substr(0, eq)), *rgba});
    }
    return spec;
}

std::optional<ce_unity::Image> ComposePortrait(std::span<const PortraitPart* const> parts,
                                               const PortraitRegionFn& region, float scale,
                                               std::span<const SlotTint> tints, int size) {
    if (size <= 0 || scale <= 0) {
        return std::nullopt;
    }
    struct Placed {
        const PortraitPart* part;
        const ce_unity::Image* img;
        float tint[4];
    };
    std::vector<Placed> placed;
    float x0 = std::numeric_limits<float>::max(), y0 = x0;
    float x1 = std::numeric_limits<float>::lowest(), y1 = x1;
    for (const PortraitPart* p : parts) {
        const ce_unity::Image* img = p ? region(p->region) : nullptr;
        if (img == nullptr || img->width == 0 || img->height == 0) {
            continue;
        }
        Placed pl{p, img, {1, 1, 1, 1}};
        for (const auto& t : tints) {
            if (t.slot == p->slot) {
                for (int c = 0; c < 4; ++c) {
                    pl.tint[c] = static_cast<float>((t.rgba >> (24 - 8 * c)) & 0xFF) / 255.0f;
                }
            }
        }
        const float w = static_cast<float>(img->width) / scale, h = static_cast<float>(img->height) / scale;
        const auto& m = p->m;
        for (const auto [px, py] : {std::pair{0.0f, 0.0f}, {w, 0.0f}, {0.0f, h}, {w, h}}) {
            const float wx = m[0] * px + m[1] * py + m[4], wy = m[2] * px + m[3] * py + m[5];
            x0 = std::min(x0, wx);
            x1 = std::max(x1, wx);
            y0 = std::min(y0, wy);
            y1 = std::max(y1, wy);
        }
        placed.push_back(pl);
    }
    if (placed.empty() || x1 <= x0 || y1 <= y0) {
        return std::nullopt;
    }
    // Fit as a square, centred, small margin
    constexpr float kMargin = 0.03f;
    const float side = std::max(x1 - x0, y1 - y0);
    const float k = static_cast<float>(size) * (1 - 2 * kMargin) / side; // canvas pixels per world unit
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    std::vector<float> acc(static_cast<size_t>(size) * size * 4, 0.0f); // premultiplied
    for (const auto& pl : placed) {
        const auto& m = pl.part->m;
        const float det = m[0] * m[3] - m[1] * m[2];
        if (std::fabs(det) < 1e-6f) {
            continue;
        }
        // World → original pixels
        const float ia = m[3] / det, ib = -m[1] / det, ic = -m[2] / det, id = m[0] / det;
        for (int y = 0; y < size; ++y) {
            const float wy = cy - (static_cast<float>(y) + 0.5f - size / 2.0f) / k;
            for (int x = 0; x < size; ++x) {
                const float wx = cx + (static_cast<float>(x) + 0.5f - size / 2.0f) / k;
                const float dx = wx - m[4], dy = wy - m[5];
                const float px = ia * dx + ib * dy, py = ic * dx + id * dy;
                float s[4];
                Sample(*pl.img, px * scale - 0.5f, py * scale - 0.5f, s);
                const float a = s[3] * pl.tint[3];
                if (a <= 0) {
                    continue;
                }
                float* d = &acc[(static_cast<size_t>(y) * size + x) * 4];
                for (int c = 0; c < 3; ++c) {
                    d[c] = s[c] * pl.tint[c] * pl.tint[3] + d[c] * (1 - a);
                }
                d[3] = a + d[3] * (1 - a);
            }
        }
    }
    ce_unity::Image out;
    out.width = out.height = static_cast<uint32_t>(size);
    out.rgba.resize(acc.size());
    for (size_t i = 0; i < acc.size(); i += 4) {
        const float a = acc[i + 3];
        for (int c = 0; c < 3; ++c) {
            const float v = a > 0 ? acc[i + c] / a : 0;
            out.rgba[i + c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        out.rgba[i + 3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return out;
}

} // namespace cotl
