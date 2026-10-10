// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_page_crystals.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ce_page_crystals {
namespace {

using ce_draw::Canvas;
using ce_draw::Image;
using Box = Canvas::Box;
namespace col = ce_draw::col;
using ce_draw::Snap;
using namespace geo;

constexpr const char* CatIcon[4] = {"icon_equipment_30", "icon_equipment_8", "icon_equipment_31",
                                    "icon_equipment_29"};
constexpr const char* CatChip[4] = {"icon_category_20", "icon_category_21", "icon_category_22",
                                    "icon_category_23"};
constexpr const char* CatName[4] = {"Status", "Offensive", "Defensive", "Utility"};
constexpr const char* SortName[4] = {"Rank", "Purity", "Size", "Property"};
constexpr const char* ReadOnlyLine = "Close the menu to make changes";
constexpr int Orn = 36, Clear = 12; // commandbox_bg corner ornament (render-manage.py ORN / CLEAR)

std::string Roman(int r) {
    static const char* R[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
    return r >= 1 && r <= 10 ? R[r] : std::to_string(r);
}

const char* CatIconOf(const Crystal& c) {
    return CatIcon[std::clamp(c.cat, 0, 3)];
}

// ---- render-manage.py small helpers ---------------------------------------------------------------

void Shade(Canvas& c, Box b, std::uint32_t rgb, float a) {
    const int x = Snap(b.x), y = Snap(b.y), w = Snap(b.w), h = Snap(b.h);
    const std::uint32_t argb = (static_cast<std::uint32_t>(std::lround(255 * a)) << 24) | rgb;
    c.Composite(ce_draw::Blank(w, h, argb), x, y);
}

void Outline(Canvas& c, Box b, std::uint32_t rgb, int t, float a) {
    const int x = Snap(b.x), y = Snap(b.y), w = Snap(b.w), h = Snap(b.h);
    Image o = ce_draw::Blank(w, h);
    const std::uint32_t argb = (static_cast<std::uint32_t>(std::lround(255 * a)) << 24) | rgb;
    const auto fill = [&](int x0, int y0, int x1, int y1) {
        for (int yy = std::max(0, y0); yy <= y1 && yy < h; ++yy)
            for (int xx = std::max(0, x0); xx <= x1 && xx < w; ++xx) {
                std::uint8_t* p = &o.rgba[(static_cast<std::size_t>(yy) * w + xx) * 4];
                p[0] = (argb >> 16) & 0xFF, p[1] = (argb >> 8) & 0xFF, p[2] = argb & 0xFF,
                p[3] = argb >> 24;
            }
    };
    fill(t, 0, w - 1 - t, t - 1);
    fill(t, h - t, w - 1 - t, h - 1);
    fill(0, t, t - 1, h - 1 - t);
    fill(w - t, t, w - 1, h - 1 - t);
    c.Composite(o, x, y);
}

/// partyInfoFieldBGG stretched (render-manage.py strip)
void Strip(Canvas& c, Box b, float alpha = 1.0f) {
    const auto a = c.art.Sprite("systemgfx", "partyInfoFieldBGG");
    if (!a)
        return;
    Image im = ce_draw::ResizeNearest(*a, 0, 0, static_cast<int>(a->width), static_cast<int>(a->height),
                                      Snap(b.w), Snap(b.h));
    if (alpha < 1.0f)
        im = ce_draw::Faded(im, alpha);
    c.Composite(im, Snap(b.x), Snap(b.y));
}

/// kit._gold_edge: the dBox_choice edge band only, over an existing fill
void GoldEdge(Canvas& c, Box b) {
    const int x = Snap(b.x), y = Snap(b.y), w = Snap(b.w), h = Snap(b.h);
    Canvas t(c.art, w, h, 0);
    t.Button({0, 0, w, h});
    Image& im = t.Pixels();
    for (int yy = 12; yy <= h - 13; ++yy)
        for (int xx = 12; xx <= w - 13; ++xx)
            im.rgba[(static_cast<std::size_t>(yy) * w + xx) * 4 + 3] = 0;
    c.Composite(im, x, y);
}

void MuteEdge(Canvas& c, Box b) {
    Image& im = c.Pixels();
    const int x = Snap(b.x), y = Snap(b.y), w = Snap(b.w), h = Snap(b.h);
    for (int yy = y; yy < y + h && yy < ce_draw::H; ++yy)
        for (int xx = x; xx < x + w && xx < ce_draw::W; ++xx) {
            std::uint8_t* p = &im.rgba[(static_cast<std::size_t>(yy) * im.width + xx) * 4];
            if (p[0] > 180 && p[1] > 100 && p[2] < 90)
                p[0] = 110, p[1] = 103, p[2] = 84;
        }
}

/// kit.button
void Button(Canvas& c, Box b, std::string_view label, bool disabled = false) {
    c.Button(b);
    if (disabled)
        MuteEdge(c, b);
    if (!label.empty())
        c.Text(label, b.x + b.w / 2, b.y + (b.h - 21) / 2, 1, disabled ? col::muted : col::ink, true,
               'c', -1, disabled ? 0.45f : 1.0f);
}

void Glow(Canvas& c, Box b) {
    c.SelectedFill({b.x + 9, b.y + 9, b.w - 18, b.h - 18}, 0.45f);
    GoldEdge(c, b);
    Outline(c, {b.x - 6, b.y - 6, b.w + 12, b.h + 12}, col::gold, 3, 0.55f);
}

int SizeDots(Canvas& c, int x, int y, int n, float alpha = 1.0f) {
    const auto dot = c.art.Sprite("packedassets", "crSlotIcon");
    for (int i = 0; i < n; ++i)
        c.Put(dot, x + i * 21, y, 3, "tl", alpha);
    return x + (n - 1) * 21 + 33;
}

int CrystalName(Canvas& c, const Crystal& k, int x, int y, int maxw = -1, std::uint32_t rgb = col::ink,
                float alpha = 1.0f) {
    std::string name = (k.art ? "*" : "") + k.name;
    const std::string num = k.rank > 0 ? " " + Roman(k.rank) : std::string{};
    if (maxw > 0 && c.TextWidth(name + num, 1) > maxw)
        name = c.Fit(name, maxw - c.TextWidth(num, 1), 1);
    const int x2 = c.Text(name, x, y, 1, rgb, true, 'l', -1, alpha);
    return c.Text(num, x2, y, 1, col::gold, true, 'l', -1, alpha);
}

void Purity(Canvas& c, int p, int xr, int y, float alpha = 1.0f) {
    const std::string n = std::to_string(p);
    const int w = c.TextWidth(n, 1);
    c.Text(n, xr, y, 1, p > 0 ? col::gold : col::muted, true, 'r', -1, alpha);
    c.Text("Purity ", xr - w, y, 1, col::ink, true, 'r', -1, alpha);
}

void Tag(Canvas& c, int x, int y, std::string_view label) {
    const int w = c.TextWidth(label, 1) + 30;
    x = Snap(x), y = Snap(y);
    c.Fill(x + 3, y, x + w - 3, y + 36, 0x08080c);
    c.Fill(x, y + 3, x + w, y + 33, 0x08080c);
    c.Text(label, x + 15, y + 6, 1, col::ink);
}

void Picker(Canvas& c, const CrystalsView& v) {
    const int n = static_cast<int>(v.members.size());
    for (int i = 0; i < n; ++i) {
        const auto& m = v.members[static_cast<std::size_t>(i)];
        const Cell cell = PickerCell(n, i);
        const int x = cell.x, w = cell.w;
        c.Button({x, CrPickY, w, CrPickH});
        if (i == v.sel)
            c.SelectedFill({x + 9, CrPickY + 9, w - 18, CrPickH - 18});
        if (w >= 288) {
            c.Put(c.art.Sprite("systemgfx", m.sprite), x + 12, 117, 3, "bl");
            c.Text(m.name, x + 129, 63, 1, i == v.sel ? col::gold : col::ink, true, 'l', w - 144);
        } else {
            // more than four members: portraits only (the native Equipment strip)
            c.Put(c.art.Sprite("systemgfx", m.sprite), x + w / 2, 117, 3, "bc");
        }
    }
}

struct Btn {
    std::string label;
    bool disabled{};
};

void Bottom(Canvas& c, const std::vector<Btn>& buttons, bool read_only) {
    Button(c, {CrBackX, CrBtnY, CrBtnW, CrBtnH}, "Back");
    if (read_only) {
        c.Text(ReadOnlyLine, (312 + 1216) / 2, CrBtnY + (CrBtnH - 21) / 2, 1, col::muted, true, 'c');
        return;
    }
    constexpr int xs[3] = {CrBtn2, CrBtn1, CrBtn0};
    for (std::size_t i = 0; i < buttons.size() && i < 3; ++i) {
        const auto& b = buttons[buttons.size() - 1 - i];
        Button(c, {xs[i], CrBtnY, CrBtnW, CrBtnH}, b.label, b.disabled);
    }
}

// ---- equipment window ---------------------------------------------------------------------------

void SlotRowDraw(Canvas& c, Box b, const SlotRow& r, bool read_only, bool dim = false) {
    const int x = b.x, y = b.y, w = b.w, h = b.h;
    if (r.state == 2) {
        Strip(c, b, 0.8f);
        c.Put(c.art.Sprite("systemgfx", "crSlotIconEmpty"), x + 15, y + h / 2, 3, "cl", 0.35f);
        c.Text("(Locked)", x + 66, y + (h - 21) / 2, 1, col::muted, true, 'l', -1, 0.45f);
        if (dim)
            Shade(c, b, 0, 0.55f);
        return;
    }
    Strip(c, b);
    const int cy = y + h / 2;
    if (r.state == 0 && !r.planned && r.valid && !read_only) {
        c.SelectedFill({x + 9, y + 9, w - 18, h - 18}, 0.5f);
        c.Put(c.art.Sprite("packedassets", "crSlotIconEmptyGold"), x + 15, cy, 3, "cl");
        c.Text("(None)", x + 66, cy - 10, 1, col::gold);
    } else if (r.state == 0 && !r.planned) {
        c.Put(c.art.Sprite("systemgfx", "crSlotIconEmpty"), x + 15, cy, 3, "cl");
        c.Text("(None)", x + 66, cy - 10, 1, col::muted);
    } else if (r.state == 0 && r.planned) {
        c.Put(c.art.Sprite("packedassets", "crSlotIconEmptyGold"), x + 15, cy, 3, "cl");
        c.Put(c.art.Sprite("systemgfx", CatIconOf(r.plan)), x + 63, cy, 3, "cl", 0.5f);
        CrystalName(c, r.plan, x + 117, cy - 10, w - 140, col::ink, 0.5f);
        c.SelectedFill({x + 111, y + h - 18, std::min(w - 135, c.TextWidth(r.plan.name, 1) + 60), 9});
    } else {
        const Crystal& k = r.crystal;
        c.Put(c.art.Sprite("systemgfx", "crSlotIconFull"), x + 15, cy, 3, "cl");
        c.Put(c.art.Sprite("systemgfx", CatIconOf(k)), x + 63, cy, 3, "cl");
        if (k.known) {
            CrystalName(c, k, x + 117, cy - 25, w - 140);
            SizeDots(c, x + 111, cy + 6, k.size);
            Purity(c, k.purity, x + w - 18, cy + 9);
        } else {
            CrystalName(c, k, x + 117, cy - 10, w - 140);
        }
        if (r.planned) {
            // a different crystal is planned over an inserted one: its ghost on the right
            c.Put(c.art.Sprite("systemgfx", CatIconOf(r.plan)), x + w - 60, y + 12, 3, "tl", 0.5f);
            c.SelectedFill({x + 111, y + h - 18, std::min(w - 135, 240), 9});
        }
    }
    if (!read_only && r.state == 0)
        GoldEdge(c, b);
    if (dim)
        Shade(c, b, 0, 0.55f);
}

void ItemLine(Canvas& c, const Piece& p, std::string_view kind, int ix, int iw, int cy) {
    c.Put(c.art.Sprite("systemgfx", p.icon), ix, cy, 3);
    c.Text(p.present ? p.name : "(None)", ix + 57, cy + 6, 1, p.present ? col::ink : col::muted, true, 'l',
           iw - 57 - c.TextWidth(kind, 1) - 24);
    c.Text(kind, ix + iw, cy + 6, 1, col::muted, true, 'r');
}

} // namespace

std::vector<Box> SlotBoxes(const CrystalsView& v, int& acc_y, int& emblem_y, std::array<int, 2>& item_y) {
    // render-manage.py equip_window: rows of 90 (42 locked), gap 6; compact (66) when crowded
    int rows = 0;
    for (int k = 0; k < 2; ++k)
        rows += static_cast<int>(v.pieces[k].rows.size());
    const int row_h = rows > 6 ? 66 : CrRowH;
    std::vector<Box> out;
    const int ix = CrEquipX + 30, iw = CrEquipW - 60;
    int cy = CrEquipY + 36;
    for (int k = 0; k < 2; ++k) {
        item_y[static_cast<std::size_t>(k)] = cy;
        cy += CrItemH;
        for (const auto& r : v.pieces[static_cast<std::size_t>(k)].rows) {
            const int rh = r.state == 2 ? CrLockH : row_h;
            out.push_back({ix, cy, iw, rh});
            cy += rh + 6;
        }
        cy += 9;
    }
    acc_y = cy;
    emblem_y = cy + 84;
    return out;
}

namespace {

void EquipWindow(Canvas& c, const CrystalsView& v) {
    const Box box{CrEquipX, CrEquipY, CrEquipW, CrEquipH};
    c.MenuWindow(box);
    const int ix = box.x + 30, iw = box.w - 60;
    int acc_y = 0, emblem_y = 0;
    std::array<int, 2> item_y{};
    const CrystalsView& vv = v;
    const auto boxes = SlotBoxes(vv, acc_y, emblem_y, item_y);
    static constexpr const char* Kind[4] = {"Weapon", "Armor", "Accessory", "Class Emblem"};
    std::size_t bi = 0;
    for (int k = 0; k < 2; ++k) {
        const Piece& p = vv.pieces[static_cast<std::size_t>(k)];
        if (p.selected && !v.read_only)
            c.SelectedFill({ix - 6, item_y[static_cast<std::size_t>(k)] - 6, iw + 12, 54}, 0.6f);
        ItemLine(c, p, Kind[k], ix, iw, item_y[static_cast<std::size_t>(k)]);
        for (std::size_t j = 0; j < p.rows.size(); ++j, ++bi) {
            const auto& r = p.rows[j];
            Box b = boxes[bi];
            if (r.state == 3)
                continue; // covered by the size-2/3 crystal drawn above
            if (r.state == 1 && r.crystal.size > 1) {
                // a size-2/3 crystal spans its slots: one merged row
                int span = 1;
                for (std::size_t q = j + 1; q < p.rows.size() && p.rows[q].state == 3; ++q)
                    ++span;
                if (span > 1)
                    b.h = boxes[bi + static_cast<std::size_t>(span) - 1].y + boxes[bi + static_cast<std::size_t>(span) - 1].h - b.y;
            }
            SlotRowDraw(c, b, r, v.read_only);
            if (r.valid && !v.read_only && r.state == 0 && !r.planned)
                Glow(c, b);
        }
    }
    const Piece& acc = vv.pieces[2];
    ItemLine(c, acc, Kind[2], ix, iw, acc_y);
    if (acc.present && !acc.fixed.empty())
        c.Text(acc.fixed, ix + 57, acc_y + 42, 1, col::muted, true, 'l', iw - 57 - 270);
    if (acc.present)
        c.Text("No crystal slots", ix + iw, acc_y + 42, 1, col::muted, true, 'r');
    ItemLine(c, vv.pieces[3], Kind[3], ix, iw, emblem_y);
}

// ---- inventory -----------------------------------------------------------------------------------

void CellDraw(Canvas& c, const Crystal& k, int x, int y, bool sel, bool ghost = false) {
    c.Nine(c.art.Sprite("packedassets", sel ? "borderOn" : "borderOff", true), {x, y, CrCell, CrCell}, 4, 4,
           4, 4, 3, 2);
    const float a = ghost ? 0.3f : 1.0f;
    c.Put(c.art.Sprite("systemgfx", CatIconOf(k)), x + CrCell / 2, y + 18, 3, "tc", a);
    const auto dot = c.art.Sprite("packedassets", "crSlotIcon");
    for (int i = 0; i < k.size; ++i)
        c.Put(dot, x + 3 + i * 15, y + 3, 3, "tl", a);
    c.Text(Roman(k.rank), x + 15, y + CrCell - 36, 1, col::gold, true, 'l', -1, a);
    c.Text(std::to_string(k.purity), x + CrCell - 15, y + CrCell - 36, 1, k.purity > 0 ? col::gold : col::muted,
           true, 'r', -1, a);
    if (!k.owner.empty() && !ghost)
        c.Put(c.art.Sprite("systemgfx", k.owner), x + CrCell - 3, y + 3, 1, "tr");
}

void Chips(Canvas& c, const CrystalsView& v) {
    for (int i = 0; i < 5; ++i) {
        const int cat = i - 1;
        const Box b{CrChipX + i * CrChipDx, CrChipY, CrChipW, CrChipW};
        c.Button(b);
        if (cat == v.chip)
            c.SelectedFill({b.x + 9, b.y + 9, 72, 72});
        if (cat < 0)
            c.Text("All", b.x + 45, b.y + 34, 1, v.chip < 0 ? col::gold : col::ink, true, 'c');
        else
            c.Put(c.art.Sprite("systemgfx", CatChip[cat]), b.x + 45, b.y + 45, 3, "c");
    }
    constexpr int xr = 1204;
    c.Digits(v.shown, 2, xr, CrChipY + 9, 2, col::ink, 0.45f, 'r');
    c.Text(v.filter || v.chip >= 0 ? "shown" : "total", xr, CrChipY + 60, 1, col::muted, true, 'r');
}

void Inventory(Canvas& c, const CrystalsView& v) {
    c.Nine("systemgfx", "skillInfo_BG", {624, 132, 592, 564}, 4, 4, -1, true, 1.0f);
    Chips(c, v);
    for (std::size_t i = 0; i < v.grid.size() && i < static_cast<std::size_t>(CrGridCols * CrGridRows); ++i) {
        const int r = static_cast<int>(i) / CrGridCols, k = static_cast<int>(i) % CrGridCols;
        CellDraw(c, v.grid[i], CrGridX + k * CrPitch, CrGridY + r * CrPitch, static_cast<int>(i) == v.selected);
    }
    if (v.grid.empty())
        c.Text(v.total ? "No crystal matches" : "No crystals yet", 624 + 296 - 18, CrGridY + 180, 1, col::muted,
               true, 'c');
    // scrollbar: native 10x64 track with the thumb at the scroll position (3x)
    if (v.rows > CrGridRows) {
        const auto sb = c.art.Sprite("systemgfx", "scrollbar");
        if (sb) {
            const int track = CrGridRows * CrPitch - 6;
            const int th = static_cast<int>(sb->height) * 3;
            const int range = std::max(1, v.rows - CrGridRows);
            const int ty = CrGridY + (track - th) * std::clamp(v.first_row, 0, range) / range;
            c.Put(sb, 1179, ty, 3);
        }
    }
}

void Detail(Canvas& c, const Crystal& k, Box b, const std::string& extra) {
    c.MenuWindow(b);
    const int x = b.x, y = b.y, w = b.w, h = b.h;
    const int xr = x + w - Orn - Clear - 3;
    c.Put(c.art.Sprite("systemgfx", CatIconOf(k)), x + 30, y + 24, 3);
    CrystalName(c, k, x + 87, y + 30, xr - 180 - x - 87);
    const std::string where = k.owner_name.empty() ? "Not inserted" : "In " + k.owner_name + "'s gear";
    c.Text(where, xr, y + 30, 1, col::muted, true, 'r');
    const int xe = SizeDots(c, x + 81, y + 60, k.size);
    const std::string pw = "Purity " + std::to_string(k.purity);
    Purity(c, k.purity, xe + 12 + c.TextWidth(pw, 1), y + 66);
    const std::string kind = std::string(CatName[std::clamp(k.cat, 0, 3)]) + "   " + (k.art ? "Artificial" : "Natural");
    c.Text(kind, xr, y + 66, 1, col::muted, true, 'r');
    std::vector<std::pair<std::string, std::uint32_t>> lines;
    if (!extra.empty())
        lines.push_back({extra, col::ink});
    else {
        lines.push_back({k.effect, k.known && k.rank > 0 && k.rank < 3 ? col::muted : col::ink});
        if (k.known && k.rank > 0 && k.rank < 3) // the game greys the effect below rank III
            lines.push_back({"Active from rank III. Fuse it to set it.", col::muted});
        else if (k.purity == 0 && !k.art)
            lines.push_back({"Not a fuse base (purity 0).", col::muted});
    }
    int yy = y + 111;
    const int bottom_limit = y + h - Orn - Clear;
    for (const auto& [ln, cc] : lines) {
        const int room = (bottom_limit - yy - 21) / 30 + 1;
        if (room <= 0)
            break;
        yy = c.Paragraph(ln, x + 30, yy, w - 63, 1, cc, std::min(2, room), 30);
    }
}

void CheckRow(Canvas& c, Box b, const Step& s) {
    const int x = b.x, y = b.y, h = b.h;
    Strip(c, b);
    c.Put(c.art.Sprite("systemgfx", s.done ? "boxDarkCheckedGreen" : "boxDarkUnchecked"), x + 18, y + h / 2, 3, "cl",
          s.stale ? 0.45f : 1.0f);
    const int tx = x + 75;
    const std::uint32_t cc = s.done || s.stale ? col::muted : s.next ? col::gold : col::ink;
    const int ty = y + (h - 21) / 2 - (s.sub.empty() ? 0 : 15);
    if (s.insert) {
        c.Put(c.art.Sprite("systemgfx", CatIconOf(s.crystal)), tx, ty - 6, 3, "tl", s.done ? 0.6f : 1.0f);
        const int tx2 = c.Text("Insert ", tx + 51, ty, 1, cc);
        CrystalName(c, s.crystal, tx2, ty, b.w - (tx2 - x) - 18, cc);
    } else {
        c.Text(s.label, tx, ty, 1, cc, true, 'l', b.w - 93);
    }
    if (!s.sub.empty())
        c.Text(s.sub, x + 75, ty + 33, 1, col::muted, true, 'l', b.w - 93);
}

/// The transfer checklist (crystals-transfer.png right window)
void TransferPane(Canvas& c, const CrystalsView& v) {
    const Box box{624, 132, 592, 804};
    c.MenuWindow(box);
    const int ix = box.x + 30, iw = box.w - 60;
    c.Put(c.art.Sprite("systemgfx", v.transfer_old.icon), ix, 171, 3);
    const std::string old_name = c.Fit(v.transfer_old.name, 180, 1);
    c.Text(old_name, ix + 57, 177, 1, col::muted);
    const int ax = ix + 57 + c.TextWidth(old_name, 1) + 18;
    c.Put(c.art.Sprite("systemgfx", "pointerRight"), ax, 168, 3);
    c.Put(c.art.Sprite("systemgfx", "pointerRight"), ax + 27, 168, 3);
    const int gx = ax + 27 + 48 + 12;
    c.Put(c.art.Sprite("systemgfx", v.transfer_new_icon), gx, 171, 3);
    c.Text(v.transfer_new, gx + 57, 177, 1, col::gold, true, 'l', ix + iw - gx - 57);
    int done = 0;
    for (const auto& s : v.steps)
        done += s.done ? 1 : 0;
    const int n = static_cast<int>(v.steps.size());
    c.Digits(done, 1, ix, 234, 2, col::ink);
    c.Text("/ " + std::to_string(n), ix + 54, 255, 1, col::muted);
    c.Text(v.plan_stale ? "Plan out of date" : "steps done", ix + iw, 255, 1, v.plan_stale ? col::low : col::muted,
           true, 'r');
    c.Bar(ix, 306, iw, n ? static_cast<double>(done) / n : 0.0, 'h', 5);
    const int pitch = n > 4 ? 90 : 114, rh = n > 4 ? 84 : 105;
    int ry = 345;
    for (const auto& s : v.steps) {
        if (ry + rh > box.y + box.h - 90)
            break;
        CheckRow(c, {ix, ry, iw, rh}, s);
        ry += pitch;
    }
    c.Paragraph("Steps tick themselves as the gear changes. Crystals are set at a smith.", ix, ry + 15, iw, 1,
                col::muted, 2, 30);
}

/// The slot-plan steps in the detail pane's place (UX-SPEC 4.5 E3 "Steps"). Every listed step
/// shows all its lines (name + location); steps that do not fit above the ornament clearance are
/// counted in a "+N more" beside the header instead of being cut.
void StepsPane(Canvas& c, const CrystalsView& v) {
    const Box b{624, 708, 592, 228};
    c.MenuWindow(b);
    const int ix = b.x + 30, iw = b.w - 60;
    constexpr int Line = 27, Gap = 3, Glyph = 24; // 3-px grid: line pitch 27, steps 30 apart
    int done = 0;
    for (const auto& s : v.steps)
        done += s.done ? 1 : 0;
    const int xr = b.x + b.w - Orn - Clear - 3;
    const int limit = b.y + b.h - Orn - Clear; // text bottom (corner ornament + clearance)
    const auto lines = [](const Step& s) { return s.insert && !s.sub.empty() ? 2 : 1; };
    int shown = 0;
    for (int y = b.y + 66; shown < static_cast<int>(v.steps.size()); ++shown) {
        const int bottom = y + (lines(v.steps[static_cast<std::size_t>(shown)]) - 1) * Line + Glyph;
        if (bottom > limit)
            break;
        y = bottom - Glyph + Line + Gap;
    }
    const int hidden = static_cast<int>(v.steps.size()) - shown;
    c.Text(v.plan_stale ? "Plan out of date" : "Steps", ix + 21, b.y + 30, 1, v.plan_stale ? col::low : col::muted);
    const int dx = c.Text(std::to_string(done) + " / " + std::to_string(v.steps.size()) + " done", xr, b.y + 30, 1,
                          col::muted, true, 'r');
    (void)dx;
    if (hidden > 0) {
        const std::string more = "+" + std::to_string(hidden) + " more";
        const std::string tail = std::to_string(done) + " / " + std::to_string(v.steps.size()) + " done";
        c.Text(more, xr - c.TextWidth(tail, 1) - 30, b.y + 30, 1, col::gold, true, 'r');
    }
    int y = b.y + 66;
    for (int i = 0; i < shown; ++i) {
        const Step& s = v.steps[static_cast<std::size_t>(i)];
        c.Put(c.art.Sprite("systemgfx", s.done ? "boxDarkCheckedGreen" : "boxDarkUnchecked"), ix, y + 10, 3, "cl");
        const std::uint32_t cc = s.done || s.stale ? col::muted : s.next ? col::gold : col::ink;
        if (s.insert) {
            const int tx = c.Text("Insert ", ix + 48, y, 1, cc);
            CrystalName(c, s.crystal, tx, y, iw - (tx - ix) - 12, cc);
            if (!s.sub.empty()) {
                c.Text(s.sub, ix + 48, y + Line, 1, col::muted, true, 'l', iw - 60);
                y += Line;
            }
        } else {
            c.Text(s.label, ix + 48, y, 1, cc, true, 'l', iw - 60);
        }
        y += Line + Gap;
    }
}

void FusibleList(Canvas& c, const std::vector<Pair>& pairs, Box box, int max_rows, bool title_tag) {
    c.MenuWindow(box);
    const int wx = box.x, wy = box.y, ww = box.w;
    if (title_tag)
        Tag(c, wx + 30, wy + 18, "Fusible Crystals");
    else
        c.Text("Combine at a smith", wx + 36, wy + 30, 1, col::muted);
    int py = wy + 60;
    int shown = 0;
    for (const auto& p : pairs) {
        if (shown == max_rows)
            break;
        const Box b{wx + 30, py, ww - 60, 66};
        Strip(c, b);
        if (p.current)
            c.SelectedFill({b.x + 3, b.y + 3, b.w - 6, b.h - 6});
        const int half = (b.w - 120) / 2;
        const Crystal* ks[2] = {&p.base, &p.fuse};
        for (int k = 0; k < 2; ++k) {
            const int cx = k == 0 ? b.x + 60 : b.x + 60 + half + 60;
            c.Put(c.art.Sprite("systemgfx", CatIconOf(*ks[k])), cx, b.y + b.h / 2, 3, "cl");
            if (half >= 420) { // crystals-smith.png: name, size dots, "Purity n"
                CrystalName(c, *ks[k], cx + 54, b.y + (b.h - 21) / 2, half - 54 - 98 - 75 - 24 - 12);
                SizeDots(c, cx + half - 24 - 98 - 75, b.y + b.h / 2 - 18, ks[k]->size);
                Purity(c, ks[k]->purity, cx + half - 24, b.y + (b.h - 21) / 2);
            } else { // half-width pane: name, then the purity number
                const std::string pn = std::to_string(ks[k]->purity);
                CrystalName(c, *ks[k], cx + 51, b.y + (b.h - 21) / 2, half - 51 - c.TextWidth(pn, 1) - 24);
                c.Text(pn, cx + half - 9, b.y + (b.h - 21) / 2, 1, ks[k]->purity > 0 ? col::gold : col::muted, true, 'r');
            }
        }
        c.Text("+", b.x + 60 + half + (half >= 420 ? 18 : 24), b.y + (b.h - 21) / 2, 1, col::ink);
        py += 72;
        ++shown;
    }
    if (pairs.empty())
        c.Text("No fusible pair in the bag", wx + ww / 2, wy + 120, 1, col::muted, true, 'c');
    else if (static_cast<int>(pairs.size()) > shown) {
        const std::string more = "+" + std::to_string(pairs.size() - static_cast<std::size_t>(shown)) + " more pairs";
        // beside the title (the rows fill the window down to its bottom ornament)
        c.Text(more, wx + ww - Orn - Clear - 3, title_tag ? wy + 24 : wy + 30, 1, col::muted, true, 'r');
    }
}

void SmithBox(Canvas& c, Box b, std::string_view label, const Crystal* k, bool result) {
    if (result)
        c.Nine(c.art.Sprite("systemgfx", "dBox_choice_red", true), b, 12, 12, 12, 12, 3, 2, 1.0f, -1, 40);
    else
        c.PartyWindow(b);
    const int x = b.x, y = b.y, w = b.w;
    c.Text(label, x + 42, y + 27, 1, col::muted);
    const Box rb{x + 21, y + 63, w - 42, 54};
    Strip(c, rb);
    if (!k) {
        c.Text(result ? "" : "(None)", rb.x + 24, rb.y + 16, 1, col::muted);
        return;
    }
    if (result)
        c.SelectedFill({rb.x + 3, rb.y + 3, rb.w - 6, rb.h - 6});
    c.Put(c.art.Sprite("systemgfx", CatIconOf(*k)), rb.x + 12, rb.y + 27, 3, "cl");
    CrystalName(c, *k, rb.x + 66, rb.y + 16, rb.w - 90);
    SizeDots(c, x + 36, y + 126, k->size);
    Purity(c, k->purity, x + w - 60, y + 132);
    if (result) {
        c.Text(std::string(CatName[std::clamp(k->cat, 0, 3)]) + "   " + (k->art ? "Artificial" : "Natural"), x + 42,
               y + 186, 1, col::muted);
        c.Paragraph(k->effect, x + 42, y + 225, w - 84, 1, col::ink, 2, 30);
    }
}

Image ComposeSmith(ce_draw::Art& art, const CrystalsView& v) {
    Canvas c(art);
    c.Backdrop();
    Picker(c, v);
    SmithBox(c, {24, 141, 552, 186}, "Base Crystal", v.has_base ? &v.base : nullptr, false);
    c.Text("+", 300, 342, 2, col::ink, true, 'c');
    SmithBox(c, {24, 411, 552, 186}, "Fuse Crystal", v.has_fuse ? &v.fuse : nullptr, false);
    c.Put(art.Sprite("systemgfx", "pointerRight"), 591, 369, 3, "cl");
    c.Put(art.Sprite("systemgfx", "pointerRight"), 618, 369, 3, "cl");
    SmithBox(c, {672, 219, 544, 300}, "Resulting Crystal", v.has_result ? &v.result : nullptr, true);
    c.Text("Confirm the fusion in the Craft menu.", 944, 540, 1, col::muted, true, 'c');
    FusibleList(c, v.pairs, {24, 621, 1192, 315}, 3, true);
    Bottom(c, {}, false);
    return std::move(c.Pixels());
}

void ComposeFilterPicker(Canvas& c, const CrystalsView& v) {
    // near-full-screen one-tap picker over the body (UX-SPEC 4.5 Filter), 168x168 cells
    c.Nine("systemgfx", "skillInfo_BG", {24, 132, 1192, 804}, 4, 4, -1, true, 0.95f);
    c.Text("Filter crystals", 48, 150 - 6, 1, col::muted);
    static const char* labels[] = {"All", "Size 1", "Size 2", "Size 3", "Rank I", "Rank II", "Rank III+",
                                   "Fuse bases", "In the bag", "Inserted", "Artificial", "Natural"};
    for (int i = 0; i < 12; ++i) {
        const int r = i / CrPickCols, k = i % CrPickCols;
        const Box b{CrPickCellX + k * CrPickCellDx, CrPickCellY + 36 + r * CrPickCellDy, CrPickCell, CrPickCell};
        c.Nine(c.art.Sprite("packedassets", i == v.filter ? "borderOn" : "borderOff", true), b, 4, 4, 4, 4, 3, 2);
        if (i >= 1 && i <= 3) {
            SizeDots(c, b.x + b.w / 2 - (i * 21 + 12) / 2, b.y + 45, i);
        } else if (i >= 4 && i <= 6) {
            c.Text(Roman(i - 3) + (i == 6 ? "+" : ""), b.x + b.w / 2, b.y + 36, 2, col::gold, true, 'c');
        } else if (i == 7) {
            c.Put(c.art.Sprite("packedassets", "crSlotIconEmptyGold"), b.x + b.w / 2, b.y + 54, 3, "c");
        } else if (i == 8 || i == 9) {
            c.Put(c.art.Sprite("systemgfx", i == 8 ? "crSlotIconEmpty" : "crSlotIconFull"), b.x + b.w / 2, b.y + 54,
                  3, "c");
        } else if (i == 10 || i == 11) {
            c.Text(i == 10 ? "*" : "-", b.x + b.w / 2, b.y + 36, 2, col::gold, true, 'c');
        } else {
            c.Text("All", b.x + b.w / 2, b.y + (b.h - 42) / 2, 2, i == v.filter ? col::gold : col::ink, true, 'c');
            continue;
        }
        c.Text(labels[i], b.x + b.w / 2, b.y + b.h - 48, 1, i == v.filter ? col::gold : col::ink, true, 'c', b.w - 12);
    }
}

void TargetPicker(Canvas& c, const CrystalsView& v) {
    c.Nine("systemgfx", "skillInfo_BG", {24, 132, 1192, 804}, 4, 4, -1, true, 0.95f);
    const Piece& p = v.pieces[0].selected ? v.pieces[0] : v.pieces[1];
    c.Text("Transfer the crystals of " + p.name + " to:", 48, 144, 1, col::muted, true, 'l', 1140);
    const int n = std::min<int>(static_cast<int>(v.targets.size()), CrPickCols * CrPickRows);
    for (int i = 0; i < n; ++i) {
        const auto& t = v.targets[static_cast<std::size_t>(i)];
        const int r = i / CrPickCols, k = i % CrPickCols;
        const Box b{CrPickCellX + k * CrPickCellDx, CrPickCellY + 36 + r * CrPickCellDy, CrPickCell, CrPickCell};
        c.Nine(c.art.Sprite("packedassets", "borderOff", true), b, 4, 4, 4, 4, 3, 2);
        c.Put(c.art.Sprite("systemgfx", t.icon), b.x + b.w / 2, b.y + 30, 3, "tc");
        if (!t.owner.empty())
            c.Put(c.art.Sprite("systemgfx", t.owner), b.x + b.w - 6, b.y + 6, 1, "tr");
        const auto lines = c.Wrap(t.name, b.w - 18, 1);
        for (std::size_t l = 0; l < lines.size() && l < 2; ++l)
            c.Text(lines[l], b.x + b.w / 2, b.y + 87 + static_cast<int>(l) * 30, 1, col::ink, true, 'c', b.w - 12);
        if (t.count > 1)
            c.Text("x" + std::to_string(t.count), b.x + 12, b.y + 9, 1, col::muted);
    }
    if (v.targets.empty())
        c.Text("No other equipment of this kind", 620, 480, 1, col::muted, true, 'c');
}

} // namespace

bool SameKind(const Crystal& a, const Crystal& b) {
    return a.prop == b.prop && a.rank == b.rank && (!a.known || !b.known ||
           (a.purity == b.purity && a.size == b.size && a.art == b.art));
}

Cell PickerCell(int n, int i) {
    if (n <= 4)
        return {24 + i * 300, 288};
    const int pitch = Snap((1192 + 12) / n - 1);
    const int w = pitch - 12;
    const int x0 = Snap(24 + (1192 - (n * pitch - 12)) / 2);
    return {x0 + i * pitch, w};
}

std::string Signature(const CrystalsView& v) {
    std::string s;
    char b[160];
    const auto cr = [&](const Crystal& k) {
        std::snprintf(b, sizeof b, "[%d,%d,%d,%d,%d,%d,%d,%d]", k.prop, k.cat, k.rank, k.purity, k.size, k.art,
                      k.known, k.key);
        s += b;
        s += k.name + "|" + k.owner + "|" + k.owner_name + "|" + k.effect;
    };
    std::snprintf(b, sizeof b, "s%d t%d/%d r%d/%d c%d o%d f%d sel%d m%d ro%d tr%d st%d sm%d%d%d%d", v.sel, v.shown,
                  v.total, v.first_row, v.rows, v.chip, v.sort, v.filter, v.selected, v.mode, v.read_only, v.transfer,
                  v.plan_stale, v.smith, v.has_base, v.has_fuse, v.has_result);
    s += b;
    for (const auto& m : v.members)
        s += m.name + "," + m.sprite + ";";
    for (const auto& p : v.pieces) {
        std::snprintf(b, sizeof b, "{%d%d%d,%d,%d}", p.present, p.kind, p.selected, p.item, p.equip);
        s += b;
        s += p.name + "|" + p.icon + "|" + p.fixed;
        for (const auto& r : p.rows) {
            std::snprintf(b, sizeof b, "<%d%d%d>", r.state, r.planned, r.valid);
            s += b;
            cr(r.crystal);
            if (r.planned)
                cr(r.plan);
        }
    }
    for (const auto& k : v.grid)
        cr(k);
    if (v.has_detail)
        cr(v.detail);
    s += "|" + v.transfer_old.name + ">" + v.transfer_new + v.transfer_new_icon + v.transfer_old.icon;
    for (const auto& st : v.steps) {
        std::snprintf(b, sizeof b, "(%d%d%d%d)", st.done, st.stale, st.next, st.insert);
        s += b + st.label + "|" + st.sub;
        if (st.insert)
            cr(st.crystal);
    }
    for (const auto& t : v.targets)
        s += "T" + std::to_string(t.equip) + t.name + t.icon + std::to_string(t.count) + t.owner;
    for (const auto& p : v.pairs) {
        s += p.current ? "P*" : "P";
        cr(p.base);
        cr(p.fuse);
    }
    if (v.has_base)
        cr(v.base);
    if (v.has_fuse)
        cr(v.fuse);
    if (v.has_result)
        cr(v.result);
    return s;
}

ce_draw::Image ComposeCrystals(ce_draw::Art& art, const CrystalsView& v) {
    if (v.smith)
        return ComposeSmith(art, v);
    Canvas c(art);
    c.Backdrop();
    Picker(c, v);
    if (v.mode == Filter) {
        ComposeFilterPicker(c, v);
        Bottom(c, {}, false);
        return std::move(c.Pixels());
    }
    if (v.mode == TransferPick) {
        TargetPicker(c, v);
        Bottom(c, {}, false);
        return std::move(c.Pixels());
    }
    if (v.transfer && !v.read_only) {
        // crystals-transfer.png: the equipment window shows the target with the planned crystals
        EquipWindow(c, v);
        TransferPane(c, v);
        Bottom(c, {{"Clear plan"}}, false);
        return std::move(c.Pixels());
    }
    EquipWindow(c, v);
    if (v.mode == Fusible) {
        FusibleList(c, v.pairs, {624, 132, 592, 804}, 9, false);
        Bottom(c, {{"Filter", true}, {"Sort", true}, {"Fusible"}}, v.read_only);
        return std::move(c.Pixels());
    }
    Inventory(c, v);
    if (!v.steps.empty())
        StepsPane(c, v);
    else if (v.has_detail)
        Detail(c, v.detail, {624, 708, 592, 228}, "");
    else
        c.MenuWindow({624, 708, 592, 228});
    const bool piece_sel = v.pieces[0].selected || v.pieces[1].selected;
    const std::string sort_label = v.sort == 0 ? "Sort" : std::string("Sort: ") + SortName[std::clamp(v.sort, 0, 3)];
    std::vector<Btn> btns{{"Filter"}, {v.steps.empty() ? sort_label : "Clear plan"}, {piece_sel ? "Transfer" : "Fusible"}};
    Bottom(c, btns, v.read_only);
    return std::move(c.Pixels());
}

ce_draw::Image ComposeCell(ce_draw::Art& art, const Crystal& k, bool selected) {
    Canvas c(art, CrCell, CrCell, 0);
    CellDraw(c, k, 0, 0, selected);
    return std::move(c.Pixels());
}

ce_draw::Image ComposeCellShadow() {
    return ce_draw::Blank(CrCell, CrCell, 0x99050609u);
}

} // namespace ce_page_crystals
