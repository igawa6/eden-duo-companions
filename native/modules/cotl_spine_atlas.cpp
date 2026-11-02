// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_spine_atlas.h"

#include <cstdlib>
#include <tuple>
#include <vector>

namespace cotl {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// "a, b" → (a, b)
std::pair<int, int> Pair(std::string_view v) {
    const auto comma = v.find(',');
    const std::string a(Trim(v.substr(0, comma)));
    const std::string b(comma == std::string_view::npos ? "0" : Trim(v.substr(comma + 1)));
    return {std::atoi(a.c_str()), std::atoi(b.c_str())};
}

} // namespace

SpineAtlas ParseSpineAtlas(std::string_view text) {
    SpineAtlas atlas;
    SpineRegion* cur = nullptr;
    bool header = false; // rows right after the page name (size, format, ...)
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const std::string_view raw = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        const std::string_view line = Trim(raw);
        if (line.empty()) {
            if (!atlas.page.empty()) {
                if (!atlas.regions.empty()) {
                    break; // second page: not needed
                }
            }
            cur = nullptr;
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            if (atlas.page.empty()) {
                atlas.page = std::string(line);
                header = true;
            } else {
                header = false;
                cur = &atlas.regions[std::string(line)];
            }
            continue;
        }
        if (header && Trim(line.substr(0, colon)) == "size") {
            std::tie(atlas.page_w, atlas.page_h) = Pair(Trim(line.substr(colon + 1)));
            continue;
        }
        if (cur == nullptr || header) {
            continue;
        }
        const std::string_view key = Trim(line.substr(0, colon));
        const std::string_view val = Trim(line.substr(colon + 1));
        if (key == "rotate") {
            cur->rotate = val == "true" || val == "90";
        } else if (key == "xy") {
            std::tie(cur->x, cur->y) = Pair(val);
        } else if (key == "size") {
            std::tie(cur->w, cur->h) = Pair(val);
        } else if (key == "orig") {
            std::tie(cur->orig_w, cur->orig_h) = Pair(val);
        } else if (key == "offset") {
            std::tie(cur->off_x, cur->off_y) = Pair(val);
        }
    }
    return atlas;
}

std::optional<ce_unity::Image> CropRegion(const ce_unity::Image& page, const SpineRegion& r,
                                          float scale) {
    // Region in texture pixels
    const auto sc = [scale](int v) { return static_cast<int>(static_cast<float>(v) * scale + 0.5f); };
    const int rx = sc(r.x), ry = sc(r.y), rw = sc(r.w), rh = sc(r.h);
    const int aw = r.rotate ? rh : rw; // area used in the atlas
    const int ah = r.rotate ? rw : rh;
    if (rw <= 0 || rh <= 0 || rx < 0 || ry < 0 || rx + aw > static_cast<int>(page.width) ||
        ry + ah > static_cast<int>(page.height)) {
        return std::nullopt;
    }
    ce_unity::Image out;
    out.width = static_cast<uint32_t>(rw);
    out.height = static_cast<uint32_t>(rh);
    out.rgba.resize(out.width * out.height * 4);
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            // Rotated: original (x, y) is at (y, w - 1 - x) in the atlas
            const int sx = r.rotate ? rx + y : rx + x;
            const int sy = r.rotate ? ry + (rw - 1 - x) : ry + y;
            const uint8_t* s = &page.rgba[(static_cast<size_t>(sy) * page.width + sx) * 4];
            uint8_t* d = &out.rgba[(static_cast<size_t>(y) * out.width + x) * 4];
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }
    return out;
}

void BlitOver(ce_unity::Image& dst, const ce_unity::Image& src, int x, int y) {
    for (uint32_t sy = 0; sy < src.height; ++sy) {
        const int dy = y + static_cast<int>(sy);
        if (dy < 0 || dy >= static_cast<int>(dst.height)) {
            continue;
        }
        for (uint32_t sx = 0; sx < src.width; ++sx) {
            const int dx = x + static_cast<int>(sx);
            if (dx < 0 || dx >= static_cast<int>(dst.width)) {
                continue;
            }
            const uint8_t* s = &src.rgba[(sy * src.width + sx) * 4];
            uint8_t* d = &dst.rgba[(static_cast<size_t>(dy) * dst.width + dx) * 4];
            const int a = s[3];
            const int da = d[3];
            const int oa = a + da * (255 - a) / 255;
            for (int c = 0; c < 3; ++c) {
                d[c] = oa == 0 ? 0
                               : static_cast<uint8_t>((s[c] * a + d[c] * da * (255 - a) / 255) / oa);
            }
            d[3] = static_cast<uint8_t>(oa);
        }
    }
}

namespace {

// Layer: region and the region whose original frame sets the position
struct Layer {
    const char* region;
    const char* anchor;
};

std::vector<Layer> HeartLayers(std::string_view kind, std::string_view state) {
    struct Kind {
        const char* name;
        const char* full;
        const char* half;
        const char* back; // empty container, otherwise nullptr
    };
    static constexpr Kind kKinds[] = {
        {"red", "images/HeartFull", "images/HeartHalfLeft", "images/HeartBack"},
        {"spirit", "images/HeartFull", "images/HeartHalfLeft", "images/HeartSpiritOutline"},
        {"blue", "images/HeartFull_Blue", "images/HeartSoulHalfLeft", nullptr},
        {"black", "images/HeartFull_Black", "images/HeartBlackHalfLeft", nullptr},
        {"fire", "images/HeartFull_Fire", "images/HeartFireHalfLeft", nullptr},
        {"ice", "images/HeartFull_Ice", "images/HeartIceHalfLeft", nullptr},
    };
    for (const auto& k : kKinds) {
        if (kind != k.name) {
            continue;
        }
        const bool spirit = kind == "spirit"; // spirit hearts: outline over the heart
        if (state == "full") {
            return spirit ? std::vector<Layer>{{k.full, k.full}, {k.back, k.back}}
                          : std::vector<Layer>{{k.full, k.full}};
        }
        if (state == "half") {
            if (k.back == nullptr) {
                return {{k.half, k.full}};
            }
            return spirit ? std::vector<Layer>{{k.half, k.full}, {k.back, k.back}}
                          : std::vector<Layer>{{k.back, k.back}, {k.half, k.full}};
        }
        if (state == "empty" && k.back != nullptr) {
            return {{k.back, k.back}};
        }
        return {};
    }
    return {};
}

} // namespace

std::optional<ce_unity::Image> ComposeHeart(const SpineAtlas& atlas, const ce_unity::Image& page,
                                            std::string_view kind, std::string_view state) {
    const auto layers = HeartLayers(kind, state);
    if (layers.empty()) {
        return std::nullopt;
    }
    ce_unity::Image out;
    out.width = out.height = kHeartCanvas;
    out.rgba.assign(static_cast<size_t>(kHeartCanvas) * kHeartCanvas * 4, 0);
    for (const auto& l : layers) {
        const auto r = atlas.regions.find(l.region);
        const auto a = atlas.regions.find(l.anchor);
        if (r == atlas.regions.end() || a == atlas.regions.end()) {
            return std::nullopt;
        }
        const auto img = CropRegion(page, r->second);
        if (!img) {
            return std::nullopt;
        }
        const SpineRegion& rr = r->second;
        const SpineRegion& ar = a->second;
        const int aw = ar.orig_w > 0 ? ar.orig_w : ar.w, ah = ar.orig_h > 0 ? ar.orig_h : ar.h;
        const int ow = rr.orig_w > 0 ? rr.orig_w : rr.w, oh = rr.orig_h > 0 ? rr.orig_h : rr.h;
        (void)ow;
        // Anchor frame centred; own frame flush left and vertically centred inside it
        const int frame_x = (kHeartCanvas - aw) / 2;
        const int frame_y = (kHeartCanvas - ah) / 2 + (ah - oh) / 2;
        BlitOver(out, *img, frame_x + rr.off_x, frame_y + (oh - rr.off_y - rr.h));
    }
    return out;
}

} // namespace cotl
