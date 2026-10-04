// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "isaac_map.h"

#include <algorithm>
#include <climits>

namespace isaac_map {

namespace {
int Hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Large map config (Minimap::Init 0x41C6A4..0x41C798 for the minimap2 Config at Minimap+0x1C8):
// cell size (16,14) at Config+0x170, margin 4.0 at +0x178, spacing 1.0 at +0x17C.
constexpr int CellW = 16, CellH = 14, Margin = 4, Spacing = 1;
static_assert(PitchX == CellW + Spacing && PitchY == CellH + Spacing);

// The icon anchor offset before the slot: (margin + spacing) + per-shape half cells
// (0x41BD70..0x41BDDC: 1x2 (4) +cellH/2 on y, 2x1 (6) +cellW/2 on x, 2x2 (8) both).
void ShapeIconOffset(int shape, int& ox, int& oy) {
    ox = Margin + Spacing;
    oy = Margin + Spacing;
    if (shape == 8) {
        ox += CellW / 2;
        oy += CellH / 2;
    } else if (shape == 6) {
        ox += CellW / 2;
    } else if (shape == 4) {
        oy += CellH / 2;
    }
}

struct Op {
    const Sprite* sprite;
    int x, y; // top-left in surface coordinates
    bool red;
};

// Red rooms: ColorMod::SetTint(255, 75, 75, 255) on the map ANM2 (0x41B3CC), a multiply.
isaac_pcx::Image Tinted(const isaac_pcx::Image& src) {
    isaac_pcx::Image o = src;
    for (std::size_t i = 0; i + 3 < o.px.size(); i += 4) {
        o.px[i + 1] = static_cast<std::uint8_t>((o.px[i + 1] * 75u + 127u) / 255u);
        o.px[i + 2] = static_cast<std::uint8_t>((o.px[i + 2] * 75u + 127u) / 255u);
    }
    return o;
}

int DrawCell(const Room& r) {
    return r.shape == 9 ? r.grid + 1 : r.grid;
}
} // namespace

bool DecodeHex(std::string_view hex, Floor& out) {
    out = Floor{};
    if (hex.size() < 2 || hex.size() % 2)
        return false;
    std::vector<std::uint8_t> b;
    b.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = Hex(hex[i]), lo = Hex(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        b.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
    }
    out.lost = (b[0] & 1) != 0;
    if ((b.size() - 1) % 6 || b.size() - 1 > 6 * 169)
        return false;
    for (std::size_t i = 1; i < b.size(); i += 6) {
        Room r{b[i], b[i + 1], b[i + 2], b[i + 3], b[i + 4], b[i + 5]};
        if (r.grid > 168 || r.shape < 1 || r.shape > 12)
            return false;
        out.rooms.push_back(r);
    }
    return true;
}

std::vector<std::pair<int, int>> ShapeCells(int shape) {
    switch (shape) {
    case 1: // 1x1
    case 2: // IH
    case 3: // IV
        return {{0, 0}};
    case 4: // 1x2
    case 5: // IIV
        return {{0, 0}, {0, 1}};
    case 6: // 2x1
    case 7: // IIH
        return {{0, 0}, {1, 0}};
    case 8: // 2x2
        return {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    case 9: // LTL
        return {{1, 0}, {0, 1}, {1, 1}};
    case 10: // LTR
        return {{0, 0}, {0, 1}, {1, 1}};
    case 11: // LBL
        return {{0, 0}, {1, 0}, {1, 1}};
    case 12: // LBR
        return {{0, 0}, {1, 0}, {0, 1}};
    default:
        return {};
    }
}

std::string_view RoomIcon(int type, bool red_treasure, bool boss_challenge) {
    // Minimap::Config::render 0x41BBE4: switch on Data+0x8 (RoomType) - 1, jump table 0x8BFBCC.
    switch (type) {
    case 2:
        return "IconShop";
    case 4:
        return red_treasure ? "IconTreasureRoomRed" : "IconTreasureRoom"; // Flags 0x800 (0x41C32C)
    case 5:
        return "IconBoss";
    case 6:
        return "IconMiniboss";
    case 7:
        return "IconSecretRoom";
    case 8:
        return "IconSuperSecretRoom";
    case 9:
        return "IconArcade";
    case 10:
        return "IconCurseRoom";
    case 11:
        return boss_challenge ? "IconBossAmbushRoom" : "IconAmbushRoom"; // Data subtype == 1
    case 12:
        return "IconLibrary";
    case 13:
        return "IconSacrificeRoom";
    case 14:
        return "IconDevilRoom";
    case 15:
        return "IconAngelRoom";
    case 18:
        return "IconIsaacsRoom";
    case 19:
        return "IconBarrenRoom";
    case 20:
        return "IconChestRoom";
    case 21:
        return "IconDiceRoom";
    case 24:
        return "IconPlanetarium";
    case 25:
    case 26:
        return "IconTeleporterRoom";
    case 29:
        return "IconUltraSecretRoom";
    default:
        return {};
    }
}

namespace {
std::vector<Op> Plan(const Floor& floor, const Sprites& sp) {
    std::vector<Op> ops;
    if (floor.lost)
        return ops;
    std::vector<const Room*> rooms;
    for (const auto& r : floor.rooms)
        if ((r.dflags & 7) || r.current)
            rooms.push_back(&r);
    // The game walks the grid row-major; a room is drawn when the walk reaches its draw cell.
    std::stable_sort(rooms.begin(), rooms.end(),
                     [](const Room* a, const Room* b) { return DrawCell(*a) < DrawCell(*b); });
    const auto cellpos = [](int cell, int& x, int& y) {
        x = (cell % 13) * PitchX;
        y = (cell / 13) * PitchY;
    };
    // 1. outline pass: all cells, row-major (black quads; order does not matter)
    if (sp.outline.ok)
        for (const Room* r : rooms)
            for (const auto& [dc, dr] : ShapeCells(r->shape)) {
                const int c = r->grid % 13 + dc, rr = r->grid / 13 + dr;
                if (c > 12 || rr > 12)
                    continue;
                ops.push_back({&sp.outline, c * PitchX - sp.outline.pivot_x,
                               rr * PitchY - sp.outline.pivot_y, false});
            }
    // 2. fill pass
    for (const Room* r : rooms) {
        int state;
        if (r->current)
            state = 2;
        else if ((r->flags & 1) && (r->dflags & 1))
            state = 0;
        else if ((r->dflags & 1) && r->type != 7 && r->type != 8 && r->type != 29)
            state = 1;
        else
            continue;
        const Sprite& s = sp.room[state][r->shape - 1];
        if (!s.ok)
            continue;
        int x, y;
        cellpos(r->grid, x, y);
        ops.push_back({&s, x - s.pivot_x, y - s.pivot_y, (r->flags & 2) != 0});
    }
    // 3. icon pass (room icon only: the saved-pickup icons are not in the encoding)
    for (const Room* r : rooms) {
        std::string_view name;
        if (r->dflags & 4)
            name = RoomIcon(r->type, (r->flags & 4) != 0, (r->flags & 8) != 0);
        if (name.empty() && r->type != 1 && !(r->dflags & 0x80) && (r->dflags & 2) &&
            r->type <= 24 && ((1u << r->type) & 0x013C1204u))
            name = "IconLockedRoom";
        if (name.empty())
            continue;
        const auto it = sp.icons.find(name);
        if (it == sp.icons.end() || !it->second.ok)
            continue;
        int x, y, ox, oy;
        cellpos(DrawCell(*r), x, y);
        ShapeIconOffset(r->shape, ox, oy);
        // one icon: slot = anchor + cell * 0.5, drawn at slot - (6,6), floored (0x41C000..0x41C010)
        const int ix = x + ox + CellW / 2 - 6, iy = y + oy + CellH / 2 - 6;
        ops.push_back({&it->second, ix - it->second.pivot_x, iy - it->second.pivot_y, false});
    }
    return ops;
}
} // namespace

bool RenderNative(const Floor& floor, const Sprites& sp, isaac_pcx::Image& out, int& ox, int& oy) {
    out = {};
    ox = oy = 0;
    const std::vector<Op> ops = Plan(floor, sp);
    if (ops.empty())
        return false;
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (const auto& o : ops) {
        x0 = std::min(x0, o.x);
        y0 = std::min(y0, o.y);
        x1 = std::max(x1, o.x + static_cast<int>(o.sprite->img.w));
        y1 = std::max(y1, o.y + static_cast<int>(o.sprite->img.h));
    }
    out = isaac_pcx::Blank(static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0));
    for (const auto& o : ops) {
        if (o.red)
            isaac_pcx::Over(out, Tinted(o.sprite->img), o.x - x0, o.y - y0);
        else
            isaac_pcx::Over(out, o.sprite->img, o.x - x0, o.y - y0);
    }
    // crop to what is actually visible (transparent frame borders do not count)
    std::uint32_t bx0, by0, bx1, by1;
    if (!isaac_pcx::AlphaBox(out, bx0, by0, bx1, by1)) {
        out = {};
        return false;
    }
    out = isaac_pcx::Crop(out, static_cast<int>(bx0), static_cast<int>(by0), bx1 - bx0, by1 - by0);
    ox = x0 + static_cast<int>(bx0);
    oy = y0 + static_cast<int>(by0);
    return true;
}

namespace {
struct Box {
    int x0{INT_MAX}, y0{INT_MAX}, x1{INT_MIN}, y1{INT_MIN};
    bool Empty() const {
        return x0 > x1;
    }
    void AddCell(int c, int r) {
        x0 = std::min(x0, c * PitchX);
        y0 = std::min(y0, r * PitchY);
        x1 = std::max(x1, c * PitchX + OutlineW);
        y1 = std::max(y1, r * PitchY + OutlineH);
    }
};
bool Drawn(const Room& r) {
    return (r.dflags & 7) || r.current;
}
void AddRoom(Box& b, const Room& r) {
    for (const auto& [dc, dr] : ShapeCells(r.shape)) {
        const int c = r.grid % 13 + dc, rr = r.grid / 13 + dr;
        if (c <= 12 && rr <= 12)
            b.AddCell(c, rr);
    }
}
} // namespace

Layout PlanLayout(const Floor& floor) {
    Layout l;
    if (floor.lost)
        return l;
    Box all, cur;
    for (const Room& r : floor.rooms) {
        if (!Drawn(r))
            continue;
        AddRoom(all, r);
        if (r.current)
            AddRoom(cur, r);
    }
    if (all.Empty())
        return l;
    const int fw = all.x1 - all.x0, fh = all.y1 - all.y0;
    std::uint32_t k = MaxScale;
    while (k > 1 && (fw * static_cast<int>(k) > FitW || fh * static_cast<int>(k) > FitH))
        --k;
    const int K = static_cast<int>(k);
    l.ok = true;
    l.k = k;
    l.ox = FitX + (FitW - fw * K) / 2 - all.x0 * K;
    l.oy = FitY + (FitH - fh * K) / 2 - all.y0 * K;
    const Box& c = cur.Empty() ? all : cur;
    l.cx = (static_cast<float>(l.ox) + static_cast<float>(c.x0 + c.x1) * 0.5f * static_cast<float>(K)) /
           static_cast<float>(CanvasW);
    l.cy = (static_cast<float>(l.oy) + static_cast<float>(c.y0 + c.y1) * 0.5f * static_cast<float>(K)) /
           static_cast<float>(CanvasH);
    l.sig = (k << 24) ^ (static_cast<std::uint32_t>(l.ox & 0xFFF) << 12) ^
            static_cast<std::uint32_t>(l.oy & 0xFFF);
    return l;
}

isaac_pcx::Image RenderCanvas(const Floor& floor, const Sprites& sp, std::uint32_t* scale_out) {
    isaac_pcx::Image canvas = isaac_pcx::Blank(CanvasW, CanvasH);
    if (scale_out)
        *scale_out = 0;
    const Layout l = PlanLayout(floor);
    const std::vector<Op> ops = Plan(floor, sp);
    if (!l.ok || ops.empty())
        return canvas;
    // native render on the layout's floor box (not the alpha crop: the reader's view math uses
    // the same box), then x k at the layout's place
    Box all;
    for (const Room& r : floor.rooms)
        if (Drawn(r))
            AddRoom(all, r);
    isaac_pcx::Image native = isaac_pcx::Blank(static_cast<std::uint32_t>(all.x1 - all.x0),
                                               static_cast<std::uint32_t>(all.y1 - all.y0));
    for (const auto& o : ops) {
        if (o.red)
            isaac_pcx::Over(native, Tinted(o.sprite->img), o.x - all.x0, o.y - all.y0);
        else
            isaac_pcx::Over(native, o.sprite->img, o.x - all.x0, o.y - all.y0);
    }
    const int K = static_cast<int>(l.k);
    isaac_pcx::Over(canvas, isaac_pcx::Upscale(native, l.k), l.ox + all.x0 * K, l.oy + all.y0 * K);
    if (scale_out)
        *scale_out = l.k;
    return canvas;
}

} // namespace isaac_map
