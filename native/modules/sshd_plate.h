// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD map labels (neighbour-area plates on the Map page, island names on the Fly
// page): where a label box may sit on screen. The native map shows its whole area, so its label
// placement always fits; the companion's maps follow Link in a smaller viewport with the area
// title plate, the compass and the page's buttons drawn over them. FitLabel keeps a label fully
// inside the viewport and clear of those zones (sliding it along its native outward direction or
// clamping it onto the viewport edge nearest its anchor) and answers "hide" when it cannot.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>

namespace sshd {

/// An axis-aligned box in canvas px (x0 <= x1, y0 <= y1, y down).
struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float W() const { return x1 - x0; }
    float H() const { return y1 - y0; }
};

/// Strict overlap (boxes that only touch do not overlap).
inline bool Overlaps(const Box& a, const Box& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

inline bool Within(const Box& a, const Box& outer, float eps = 0.01f) {
    return a.x0 >= outer.x0 - eps && a.y0 >= outer.y0 - eps && a.x1 <= outer.x1 + eps && a.y1 <= outer.y1 + eps;
}

/// The neighbouring-area plate's band centre relative to its mark (canvas px, y down), as the
/// native mapPlace_00 placement (main+0xC830A8..): the box (text width x 1.2, band height) moves
/// out along the mark's screen direction `deg` (clockwise from up) until its edge meets the mark.
/// `canvas` is the art's square canvas (centre = the mark); the band is kept on it. Shared by the
/// plate art (Art::Place) and the module's on-screen fit.
inline std::array<float, 2> PlaceOffset(int deg, float tw, float bw, float bh, float canvas = 900.0f) {
    const float a = static_cast<float>(deg) * 3.14159265f / 180.0f, dx = std::sin(a), dy = -std::cos(a);
    const float hx = tw * 1.2f / 2.0f, hy = bh / 2.0f;
    const float t = std::min(std::fabs(dx) > 1e-4f ? hx / std::fabs(dx) : 1e9f,
                             std::fabs(dy) > 1e-4f ? hy / std::fabs(dy) : 1e9f);
    const float s = canvas / 2.0f;
    const float cx = std::clamp(s + t * dx, bw / 2.0f, canvas - bw / 2.0f);
    const float cy = std::clamp(s + t * dy, bh / 2.0f, canvas - bh / 2.0f);
    return {cx - s, cy - s};
}

/// Where a w x h label goes. `anchor` is its world point on screen, `native` the label box's
/// centre where the native draws it (anchor + the art's offset), `view` the map viewport and
/// `avoid` the screen zones drawn over the map (title plate, compass, buttons). Returns the box
/// centre to draw at, or nullopt to hide the label. Candidates:
///  - the native spot;
///  - the box slid along its native outward direction (anchor -> native centre), its centre from
///    `slide_from` px out of the anchor (0: centred on it) outwards, each clamped into the
///    viewport: clamping the native spot puts it on the viewport edge nearest its anchor;
///  - each of those pushed off a zone it meets (left / right / up / down of the zone, clamped
///    into the viewport again), twice, for a box squeezed between two zones.
/// A candidate counts only when the box lies fully inside the viewport, overlaps no zone, still
/// lies within max_gap px of its anchor (the native box touches it: the label stays by its
/// mark) and, with slide_from > 0, still has its centre at least slide_from out along the native
/// direction (an island name never comes back over its island). The one nearest the native spot
/// wins (the native spot itself when it fits). Never clips: a box larger than the viewport, or
/// one that cannot get clear near its anchor, is hidden.
inline std::optional<std::array<float, 2>> FitLabel(std::array<float, 2> anchor, std::array<float, 2> native,
                                                    float w, float h, const Box& view,
                                                    std::span<const Box> avoid, float max_gap,
                                                    float slide_from = 0.0f) {
    if (!(w > 0) || !(h > 0) || w > view.W() || h > view.H() || !std::isfinite(native[0]) ||
        !std::isfinite(native[1]) || !std::isfinite(anchor[0]) || !std::isfinite(anchor[1]))
        return std::nullopt;
    const auto box_at = [&](float cx, float cy) { return Box{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}; };
    const auto clamp_in = [&](std::array<float, 2> c) -> std::array<float, 2> {
        return {std::clamp(c[0], view.x0 + w / 2, view.x1 - w / 2), std::clamp(c[1], view.y0 + h / 2, view.y1 - h / 2)};
    };
    const auto fits = [&](std::array<float, 2> c) {
        const Box b = box_at(c[0], c[1]);
        if (!Within(b, view))
            return false;
        for (const Box& z : avoid)
            if (Overlaps(b, z))
                return false;
        return true;
    };
    std::optional<std::array<float, 2>> best;
    float best_d = 1e30f;
    const float ox = native[0] - anchor[0], oy = native[1] - anchor[1];
    const float len = std::hypot(ox, oy);
    const float ux = len > 0.5f ? ox / len : 0.0f, uy = len > 0.5f ? oy / len : 0.0f;
    const auto consider = [&](std::array<float, 2> c) {
        const float d = std::hypot(c[0] - native[0], c[1] - native[1]);
        // slide_from > 0: the box centre may not come back nearer its anchor than that (along
        // the native direction), whatever moved it (a clamp, a push)
        if (slide_from > 0 && (c[0] - anchor[0]) * ux + (c[1] - anchor[1]) * uy < slide_from - 0.5f)
            return;
        const Box b = box_at(c[0], c[1]);
        const float gx = std::max({b.x0 - anchor[0], 0.0f, anchor[0] - b.x1});
        const float gy = std::max({b.y0 - anchor[1], 0.0f, anchor[1] - b.y1});
        if (std::hypot(gx, gy) > max_gap + 0.01f)
            return;
        if (d < best_d - 1e-3f && fits(c)) {
            best = c;
            best_d = d;
        }
    };
    if (fits(native))
        return native;
    // pushes off every zone a box at c meets, then clamped into the viewport (depth levels)
    const auto pushes = [&](std::array<float, 2> c, int depth, const auto& self) -> void {
        consider(c);
        if (depth == 0)
            return;
        const Box b = box_at(c[0], c[1]);
        for (const Box& z : avoid) {
            if (!Overlaps(b, z))
                continue;
            for (const std::array<float, 2> p : {std::array<float, 2>{z.x0 - w / 2, c[1]}, std::array<float, 2>{z.x1 + w / 2, c[1]},
                                                 std::array<float, 2>{c[0], z.y0 - h / 2}, std::array<float, 2>{c[0], z.y1 + h / 2}})
                self(clamp_in(p), depth - 1, self);
        }
    };
    pushes(clamp_in(native), 2, pushes);
    if (len > 0.5f) {
        for (float s = std::max(0.0f, slide_from); s <= len + std::max(w, h); s += 4.0f)
            pushes(clamp_in({anchor[0] + s * ux, anchor[1] + s * uy}), 2, pushes);
    }
    return best;
}

/// Canvas px of a map widget's view (the runtime's "@map_view_*:<area>" values): a world point
/// (u, v) is drawn at (x + w / 2 + (u - cx) * ppw, y + h / 2 - (v - cy) * ppw), y up.
struct MapView {
    double cx = 0, cy = 0, ppw = 0, x = 0, y = 0, w = 0, h = 0;
    bool Valid() const { return ppw > 0 && w > 0 && h > 0 && std::isfinite(cx) && std::isfinite(cy); }
    std::array<float, 2> ToScreen(double u, double v) const {
        return {static_cast<float>(x + w / 2 + (u - cx) * ppw), static_cast<float>(y + h / 2 - (v - cy) * ppw)};
    }
    std::array<double, 2> ToWorld(float sx, float sy) const {
        return {cx + (sx - (x + w / 2)) / ppw, cy - (sy - (y + h / 2)) / ppw};
    }
    Box Rect() const {
        return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + w), static_cast<float>(y + h)};
    }
};

} // namespace sshd
