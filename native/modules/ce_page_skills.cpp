// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_page_skills.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ce_page_skills {
namespace {

using ce_draw::Canvas;
using ce_draw::Image;
using ce_draw::Snap;
using Box = Canvas::Box;
namespace col = ce_draw::col;
using namespace geo;

constexpr std::uint32_t BoostTop = 0xffe08a, BoostOff = 0x4a4536;

// ---- render-manage.py helpers --------------------------------------------------------------------

/// shade(): a translucent flat colour over a box
void Shade(Canvas& c, Box b, std::uint32_t rgb, float a) {
    c.Composite(ce_draw::Blank(Snap(b.w), Snap(b.h), (static_cast<std::uint32_t>(255 * a) << 24) | rgb),
                Snap(b.x), Snap(b.y));
}

/// outline(): a t-px frame with cut corners
void Outline(Canvas& c, Box b, std::uint32_t rgb, int t, float a) {
    const int x = Snap(b.x), y = Snap(b.y), w = Snap(b.w), h = Snap(b.h);
    Image ov = ce_draw::Blank(w, h);
    const std::uint32_t px = (static_cast<std::uint32_t>(255 * a) << 24) | rgb;
    const auto fill = [&](int x0, int y0, int x1, int y1) { // inclusive, PIL rectangle
        for (int yy = std::max(0, y0); yy <= std::min(h - 1, y1); ++yy)
            for (int xx = std::max(0, x0); xx <= std::min(w - 1, x1); ++xx) {
                std::uint8_t* p = &ov.rgba[(static_cast<std::size_t>(yy) * w + xx) * 4];
                p[0] = static_cast<std::uint8_t>(px >> 16), p[1] = static_cast<std::uint8_t>(px >> 8);
                p[2] = static_cast<std::uint8_t>(px), p[3] = static_cast<std::uint8_t>(px >> 24);
            }
    };
    fill(t, 0, w - 1 - t, t - 1);
    fill(t, h - t, w - 1 - t, h - 1);
    fill(0, t, t - 1, h - 1 - t);
    fill(w - t, t, w - 1, h - 1 - t);
    c.Composite(ov, x, y);
}

/// strip(): partyInfoFieldBGG stretched to the box (the native list row / "(None)" bar)
void Strip(Canvas& c, Box b, float alpha = 1.0f) {
    const auto a = c.art.Sprite("systemgfx", "partyInfoFieldBGG");
    if (!a)
        return;
    Image s = ce_draw::ResizeNearest(*a, 0, 0, static_cast<int>(a->width), static_cast<int>(a->height),
                                     Snap(b.w), Snap(b.h));
    if (alpha < 1.0f)
        s = ce_draw::Faded(s, alpha);
    c.Composite(s, Snap(b.x), Snap(b.y));
}

/// kit._gold_edge(): the dBox_choice edge band only, over an existing fill
void GoldEdge(Canvas& c, Box b) {
    const int w = Snap(b.w), h = Snap(b.h);
    Canvas tmp(c.art, w, h, 0);
    tmp.Nine("systemgfx", "dBox_choice", {0, 0, w, h}, 4);
    Image& im = tmp.Pixels();
    for (int y = 12; y <= h - 13; ++y)
        for (int x = 12; x <= w - 13; ++x)
            im.rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] = 0;
    c.Composite(im, Snap(b.x), Snap(b.y));
}

/// tag(): the native TableHeader, a dark rounded tag straddling a window's top edge
int Tag(Canvas& c, int x, int y, std::string_view label, std::uint32_t rgb = col::ink) {
    const int w = c.TextWidth(label) + 30;
    x = Snap(x), y = Snap(y);
    Image ov = ce_draw::Blank(w, 36);
    for (int yy = 0; yy < 36; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const bool in = (xx >= 3 && xx <= w - 4) || (yy >= 3 && yy <= 32);
            if (!in)
                continue;
            std::uint8_t* p = &ov.rgba[(static_cast<std::size_t>(yy) * w + xx) * 4];
            p[0] = 8, p[1] = 8, p[2] = 12, p[3] = 255;
        }
    c.Composite(ov, x, y);
    c.Text(label, x + 15, y + 6, 1, rgb);
    return x + w;
}

void RedWindow(Canvas& c, Box b) {
    c.Nine("systemgfx", "dBox_choice_red", b, 12, 2, 40);
}

void EmptyIcon(Canvas& c, int x, int y) {
    Shade(c, {x, y, 42, 36}, 0x06070b, 0.9f);
}

void SpBar(Canvas& c, int x, int y, int w, float frac) {
    c.Bar(x, y, w, frac, 'e', 4);
}

/// lv_sp(): "Lv.1 [bar] 295" right-aligned at xr; Lv 3 reads "MAX" with a full bar
void LvSp(Canvas& c, int xr, int y, const Skill& s) {
    const bool max = s.lvl >= 3;
    const std::string left = max ? "MAX" : std::to_string(s.sp_left);
    c.Text(left, xr, y, 1, max || s.afford ? col::gold : col::ink, true, 'r');
    const int bx = xr - std::max(54, Snap(c.TextWidth(left) + 9)) - 84;
    SpBar(c, bx, y + 3, 84, max ? 1.0f : s.frac);
    const int x = c.Text("Lv.", bx - 66, y, 1, col::gold);
    c.Text(std::to_string(s.lvl), x + 3, y, 1, col::ink);
}

void Icon(Canvas& c, int icon, int x, int y, float alpha = 1.0f) {
    c.Put(c.art.Sprite("systemgfx", "skill_type_" + std::to_string(icon)), x, y, 3, "tl", alpha);
}

void ActionRow(Canvas& c, Box b, const Skill& a) {
    Strip(c, b);
    if (!a.present) {
        EmptyIcon(c, b.x + 15, b.y + 27);
        c.Text("(None)", b.x + 69, b.y + 33, 1, col::muted);
        return;
    }
    Icon(c, a.icon, b.x + 15, b.y + 15);
    c.Text(a.name, b.x + 69, b.y + 18, 1, col::ink, true, 'l', 270);
    c.Text("TP", b.x + 69, b.y + 54, 1, col::muted);
    if (a.tp >= 0)
        c.Text(std::to_string(a.tp), b.x + 69 + 39, b.y + 54, 1, col::ink);
    c.Text(a.type, b.x + 69 + 108, b.y + 54, 1, col::muted);
    LvSp(c, b.x + b.w - 18, b.y + 18, a);
}

/// An equipped passive in a character slot (one line: icon, name, Lv/SP).
/// A plan entry that ticked itself: the tick and "Equipped" replace the Lv/SP block for a while.
void Ticked(Canvas& c, int xr, int y) {
    const int x = c.Text("Equipped", xr, y, 1, col::gold, true, 'r') - c.TextWidth("Equipped");
    c.Put(c.art.Sprite("systemgfx", "boxDarkCheckedGreen", true), x - 9, y + 10, 3, "cr");
}

void PassiveSlot(Canvas& c, Box b, const Skill& p, bool done) {
    Strip(c, b);
    Icon(c, p.icon, b.x + 15, b.y + 27);
    c.Text(p.name, b.x + 69, b.y + 33, 1, col::ink, true, 'l', done ? 210 : 250);
    if (done) {
        Ticked(c, b.x + b.w - 18, b.y + 33);
        GoldEdge(c, b);
    } else {
        LvSp(c, b.x + b.w - 18, b.y + 33, p);
    }
}

/// An empty passive slot: "(None)" with the gold edge; a plan shows its ghost and "Planned".
void EmptyPassiveSlot(Canvas& c, Box b, const Plan* plan, bool flag) {
    Strip(c, b);
    if (plan) {
        Icon(c, plan->icon, b.x + 15, b.y + 27, 0.5f);
        c.Text(plan->name, b.x + 69, b.y + 24, 1, col::ink, true, 'l', 300, 0.5f);
        c.SelectedFill({b.x + 63, b.y + 54, std::min(c.TextWidth(plan->name), 300) + 12, 9});
        c.Text("Planned", b.x + b.w - 18, b.y + 33, 1, col::gold, true, 'r');
    } else {
        EmptyIcon(c, b.x + 15, b.y + 27);
        c.Text("(None)", b.x + 69, b.y + 33, 1, col::muted);
    }
    if (flag || plan)
        GoldEdge(c, b);
}

/// A spare (learned, not equipped) passive, full row (layout A).
void SpareRow(Canvas& c, Box b, const Skill& p, const Plan* plan, bool flag) {
    Strip(c, b);
    Icon(c, p.icon, b.x + 15, b.y + 15);
    c.Text(p.name, b.x + 69, b.y + 18, 1, col::ink, true, 'l', 380);
    if (plan) {
        const int n = plan->slot;
        const std::string where = n < 3 ? "slot " + std::to_string(n + 1)
                                        : "class slot " + std::to_string(n - 2);
        c.Text("Planned for " + where, b.x + 69, b.y + 54, 1, col::muted);
        GoldEdge(c, b);
    } else if (flag) {
        c.Text("Not equipped", b.x + 69, b.y + 54, 1, col::gold);
        c.Put(c.art.Sprite("packedassets", "popUpExclaim"), b.x + b.w - 15, b.y + b.h / 2, 3, "cr");
        GoldEdge(c, b);
    } else {
        c.Text("Not equipped", b.x + 69, b.y + 54, 1, col::muted);
    }
}

/// A spare passive, compact cell (layout B, more than two spare passives). The cell holds the
/// font's full S1 line (30 px with descenders), centred; the icon is dropped when the name would
/// otherwise be cut. The row is too short for the gold edge band: the alarm is the gold name.
void SpareCell(Canvas& c, Box b, const Skill& p, const Plan* plan, bool flag) {
    Strip(c, b);
    const int line = 30, ty = b.y + Snap((b.h - line) / 2);
    const int with_icon = b.w - 42 - 6;
    int tx = b.x + 42;
    if (c.TextWidth(p.name) <= with_icon)
        Icon(c, p.icon, b.x + 3, b.y + Snap((b.h - 36) / 2));
    else
        tx = b.x + 9;
    c.Text(p.name, tx, ty, 1, plan ? col::muted : flag ? col::gold : col::ink, true, 'l', b.x + b.w - 6 - tx);
}

/// class_tile(): a class-slot skill on the red window
void ClassTile(Canvas& c, Box b, const Skill& s, const Plan* plan, bool flag, bool done = false) {
    Shade(c, b, 0x000000, 0.35f);
    if (!s.present) {
        if (plan) {
            Icon(c, plan->icon, b.x + 12, b.y + 12, 0.5f);
            c.Text(plan->name, b.x + 63, b.y + 15, 1, col::ink, true, 'l', b.w - 75, 0.5f);
            c.Text("Planned", b.x + b.w - 15, b.y + 54, 1, col::gold, true, 'r');
        } else {
            EmptyIcon(c, b.x + 12, b.y + 27);
            c.Text("(None)", b.x + 63, b.y + 33, 1, col::muted);
        }
        if (flag || plan)
            GoldEdge(c, b);
        return;
    }
    Icon(c, s.icon, b.x + 12, b.y + 12);
    c.Text(s.name, b.x + (c.TextWidth(s.name) <= b.w - 66 ? 63 : 51), b.y + 15, 1, col::ink, true, 'l',
           b.w - 57);
    const int xr = b.x + b.w - 15;
    if (done) {
        Ticked(c, xr, b.y + 54);
        return;
    }
    const bool max = s.lvl >= 3;
    const std::string left = max ? "MAX" : std::to_string(s.sp_left);
    c.Text(left, xr, b.y + 54, 1, max || s.afford ? col::gold : col::ink, true, 'r');
    const int bx = xr - std::max(54, Snap(c.TextWidth(left) + 9)) - 84;
    SpBar(c, bx, b.y + 57, 84, max ? 1.0f : s.frac);
    const int lx = c.Text("Lv.", bx - 63, b.y + 54, 1, col::gold);
    c.Text(std::to_string(s.lvl), lx + 3, b.y + 54, 1, col::ink);
}

void EmblemTag(Canvas& c, int xr, int y, const std::string& name) {
    const int w = 42 + 12 + c.TextWidth(name) + 30;
    const int x = Snap(xr - w);
    c.Composite(ce_draw::Blank(w, 42, 0xFF08080Cu), x, Snap(y));
    c.Put(c.art.Sprite("systemgfx", "icon_equipment_4"), x + 12, y + 3, 3);
    c.Text(name, x + 66, y + 9, 1, col::gold);
}

void Picker(Canvas& c, const SkillsView& v) {
    const int n = static_cast<int>(v.members.size());
    for (int i = 0; i < n; ++i) {
        const auto& m = v.members[static_cast<std::size_t>(i)];
        const Cell cell = PickerCell(n, i);
        const int x = cell.x, w = cell.w;
        c.Button({x, SkPickY, w, SkPickH});
        if (i == v.sel)
            c.SelectedFill({x + 9, SkPickY + 9, w - 18, SkPickH - 18});
        if (w >= 288) {
            c.Put(c.art.Sprite("systemgfx", m.sprite), x + 12, 117, 3, "bl");
            c.Text(m.name, x + 129, 63, 1, i == v.sel ? col::gold : col::ink, true, 'l',
                   w - 129 - (m.alarm ? 51 : 15));
        } else {
            // more than four members: portraits only, like the native Skills strip
            c.Put(c.art.Sprite("systemgfx", m.sprite), x + w / 2, 117, 3, "bc");
        }
        if (m.alarm)
            c.Put(c.art.Sprite("packedassets", "popUpExclaim"), x + w - 18, 51, 3, "tr");
    }
}

void GrowthStrip(Canvas& c, const SkillsView& v) {
    const Box b{312, 948, 904, 96};
    c.Button(b);
    c.Put(c.art.Sprite("systemgfx", "gs"), b.x + 24, b.y + b.h / 2, 3, "cl");
    c.Text(std::to_string(v.gs), b.x + 84, b.y + 27, 2, v.gs > 0 ? col::gold : col::muted);
    c.Text("SP", b.x + 144, b.y + 36, 1, col::gold);
    c.Digits(v.sp, 3, b.x + 186, b.y + 27, 2);
    const int bx = b.x + 330;
    const int total = static_cast<int>(v.boosters.size());
    const int xe = c.Text("Stat Booster", bx, b.y + 15, 1, col::ink);
    c.Text(std::to_string(v.boost_taken) + "/" + std::to_string(total), xe + 15, b.y + 15, 1, col::gold);
    if (v.learn_more > 0 && v.boost_next.empty())
        c.Text("Learn " + std::to_string(v.learn_more) + " more skills", b.x + b.w - 24, b.y + 15, 1,
               col::gold, true, 'r');
    else if (!v.boost_next.empty())
        c.Text("Next " + v.boost_next, b.x + b.w - 24, b.y + 15, 1, col::gold, true, 'r');
    else if (total > 0)
        c.Text("Complete", b.x + b.w - 24, b.y + 15, 1, col::muted, true, 'r');
    constexpr int pitch = 33, sw = 27, sh = 21;
    const int sy = b.y + 54;
    const int next = v.boost_next.empty() ? -1 : [&] {
        for (int i = 0; i < total; ++i)
            if (!v.boosters[static_cast<std::size_t>(i)].taken && !v.boosters[static_cast<std::size_t>(i)].locked)
                return i;
        return -1;
    }();
    for (int i = 0; i < std::min(total, 16); ++i) {
        const auto& bo = v.boosters[static_cast<std::size_t>(i)];
        const int sx = bx + i * pitch;
        if (bo.taken) {
            c.Fill(sx, sy, sx + sw, sy + sh, col::bar_edge);
            c.Fill(sx + 3, sy + 3, sx + sw - 3, sy + 6, BoostTop);
        } else if (i == next) {
            c.Fill(sx, sy, sx + sw, sy + sh, col::bar_edge);
            c.Fill(sx + 3, sy + 3, sx + sw - 3, sy + sh - 3, col::navy_empty);
        } else {
            c.Fill(sx, sy, sx + sw, sy + sh, BoostOff);
            c.Fill(sx + 3, sy + 3, sx + sw - 3, sy + sh - 3, col::navy_empty);
            if (bo.locked)
                Shade(c, {sx, sy, sw, sh}, 0x08070a, 0.55f);
        }
    }
}

const Plan* PlanFor(const SkillsView& v, int slot) {
    for (const auto& p : v.plans)
        if (p.slot == slot && !p.done)
            return &p;
    return nullptr;
}

const Plan* PlanOf(const SkillsView& v, int id) {
    for (const auto& p : v.plans)
        if (p.id == id && !p.done)
            return &p;
    return nullptr;
}

const Plan* DoneFor(const SkillsView& v, int id) {
    for (const auto& p : v.plans)
        if (p.id == id && p.done)
            return &p;
    return nullptr;
}

} // namespace

Cell PickerCell(int n, int i) {
    if (n <= 4)
        return {24 + i * 300, 288};
    // n > 4: equal cells over 24..1216 with a 12-px gap, on the 3-px grid
    const int pitch = Snap((1192 + 12) / n - 1);
    const int w = pitch - 12;
    const int x0 = Snap(24 + (1192 - (n * pitch - 12)) / 2);
    return {x0 + i * pitch, w};
}

std::string Signature(const SkillsView& v) {
    std::string s;
    char b[96];
    const auto sk = [&](const Skill& k) {
        std::snprintf(b, sizeof b, "%d:%d:%d:%d:%d:%.3f:%d%d%d|", k.present, k.id, k.lvl, k.sp_left, k.tp,
                      k.frac, k.afford, k.cls, k.flag);
        s += b + k.name;
    };
    for (const auto& m : v.members)
        s += m.name + (m.alarm ? "!" : "") + m.sprite + ",";
    std::snprintf(b, sizeof b, "|%d %d %d %d %d %d %d %d %d|", v.sel, v.flag_spare, v.flag_class, v.gs, v.sp,
                  v.boost_taken, v.learn_more, v.mirror, static_cast<int>(v.boosters.size()));
    s += b + v.emblem + "|" + v.boost_next;
    for (const auto& k : v.actions)
        sk(k);
    for (const auto& k : v.class_actions)
        sk(k);
    for (const auto& k : v.passives)
        sk(k);
    s += "/";
    for (const auto& k : v.spare)
        sk(k);
    for (const auto& bo : v.boosters)
        s += (bo.taken ? 'T' : 't') + std::string(bo.locked ? "L" : "") + bo.label;
    for (const auto& l : v.learn_next)
        s += "n" + l;
    for (const auto& p : v.plans) {
        std::snprintf(b, sizeof b, "p%d:%d:%d", p.slot, p.id, p.done);
        s += b;
    }
    return s;
}

ce_draw::Image ComposeSkills(ce_draw::Art& art, const SkillsView& v) {
    Canvas c(art);
    c.Backdrop();
    Picker(c, v);
    // left: character actions (slots 1-6) and the class slots (7-8)
    c.MenuWindow({24, 150, 588, 621});
    Tag(c, 48, 132, "Character ACTION Skills");
    for (int i = 0; i < 6; ++i)
        ActionRow(c, {54, 186 + i * 96, 528, 90}, v.actions[static_cast<std::size_t>(i)]);
    RedWindow(c, {24, 801, 588, 135});
    Tag(c, 48, 783, "Class ACTION Skills");
    if (!v.emblem.empty())
        EmblemTag(c, 570, 780, v.emblem);
    for (int i = 0; i < 2; ++i)
        ClassTile(c, {60 + i * 261, 828, 255, 90}, v.class_actions[static_cast<std::size_t>(i)], nullptr,
                  false);
    // right: character passives (slots 1-3), spare passives, class passive slots (7-8)
    c.MenuWindow({628, 150, 588, 621});
    Tag(c, 652, 132, "Character PASSIVE Skills");
    for (int i = 0; i < 3; ++i) {
        const Box b{SkSlotX, SkSlotY0 + i * SkSlotDy, SkSlotW, SkSlotH};
        const auto& p = v.passives[static_cast<std::size_t>(i)];
        if (p.present) {
            PassiveSlot(c, b, p, DoneFor(v, p.id) != nullptr);
        } else {
            EmptyPassiveSlot(c, b, PlanFor(v, i), v.flag_spare);
        }
    }
    RedWindow(c, {628, 801, 588, 135});
    Tag(c, 652, 783, "Class PASSIVE Skills");
    for (int i = 0; i < 2; ++i) {
        const auto& p = v.passives[static_cast<std::size_t>(3 + i)];
        const Box b{SkClassX0 + i * SkClassDx, SkClassY, SkClassW, SkClassH};
        ClassTile(c, b, p, PlanFor(v, 3 + i), v.flag_class && !p.present, p.present && DoneFor(v, p.id));
    }
    // spare passives
    if (!v.spare.empty()) {
        c.Text("Learned", 664, 474, 1, col::muted);
        if (v.spare.size() <= static_cast<std::size_t>(SpareFullMax)) {
            for (std::size_t i = 0; i < v.spare.size(); ++i)
                SpareRow(c, {SkSlotX, SkFullY0 + static_cast<int>(i) * SkFullDy, SkSlotW, SkSlotH}, v.spare[i],
                         PlanOf(v, v.spare[i].id), v.spare[i].flag);
        } else {
            const std::size_t shown = std::min<std::size_t>(v.spare.size(), SpareGridMax);
            const bool more = v.spare.size() > static_cast<std::size_t>(SpareGridMax);
            for (std::size_t i = 0; i < shown; ++i) {
                const Box b{SkGridX0 + static_cast<int>(i % 2) * SkGridDx,
                            SkGridY0 + static_cast<int>(i / 2) * SkGridDy, SkGridW, SkGridH};
                if (more && i + 1 == shown) {
                    Strip(c, b, 0.6f);
                    c.Text("+" + std::to_string(v.spare.size() - shown + 1) + " more", b.x + 42,
                           b.y + Snap((b.h - 30) / 2), 1, col::muted);
                    continue;
                }
                SpareCell(c, b, v.spare[i], PlanOf(v, v.spare[i].id), v.spare[i].flag);
            }
        }
    }
    // the window's bottom line: guidance while a plan is open, else the next skills to learn
    bool pending = false;
    for (const auto& p : v.plans)
        pending = pending || !p.done;
    if (v.mirror && pending) {
        c.Text("Equip the plan in this menu.", 676, 711, 1, col::gold);
    } else if (pending) {
        c.Text("Equip the plan in the Skills menu.", 676, 711, 1, col::muted);
    } else if (!v.learn_next.empty()) {
        const int x = c.Text("Learn next ", 676, 711, 1, col::muted);
        c.Put(art.Sprite("systemgfx", "gs"), x, 705, 3);
        std::string names;
        for (std::size_t i = 0; i < v.learn_next.size() && i < 2; ++i)
            names += (i ? "   " : "") + v.learn_next[i];
        c.Text(names, x + 57, 711, 1, col::ink, true, 'l', 1190 - x - 57);
    } else if ((v.flag_spare || v.flag_class) && !v.mirror) {
        c.Text("Drag a learned skill to an empty slot.", 676, 711, 1, col::muted);
    }
    // Back and the growth strip
    c.Button({SkBackX, SkBackY, SkBackW, SkBackH});
    c.Text("Back", SkBackX + SkBackW / 2, SkBackY + (SkBackH - 21) / 2, 1, v.mirror ? col::muted : col::ink,
           true, 'c', -1, v.mirror ? 0.45f : 1.0f);
    GrowthStrip(c, v);
    return std::move(c.Pixels());
}

ce_draw::Image ComposeSpareRow(ce_draw::Art& art, const Skill& s, bool compact) {
    Canvas win(art, 588, 621, 0);
    win.MenuWindow({0, 0, 588, 621});
    const Box b = compact ? Box{SkGridX0 - 628, SkGridY0 - 150, SkGridW, SkGridH}
                          : Box{SkSlotX - 628, SkFullY0 - 150, SkSlotW, SkSlotH};
    Canvas c(art, b.w, b.h, 0);
    c.Composite(ce_draw::Crop(win.Pixels(), b.x, b.y, b.w, b.h), 0, 0);
    if (compact)
        SpareCell(c, {0, 0, b.w, b.h}, s, nullptr, true);
    else
        SpareRow(c, {0, 0, b.w, b.h}, s, nullptr, true);
    return std::move(c.Pixels());
}

ce_draw::Image ComposeRowShadow(bool compact) {
    return ce_draw::Blank(compact ? SkGridW : SkSlotW, compact ? SkGridH : SkSlotH, 0x99050609u);
}

ce_draw::Image ComposeRowSelected(ce_draw::Art& art, bool compact) {
    const int w = compact ? SkGridW : SkSlotW, h = compact ? SkGridH : SkSlotH;
    Canvas c(art, w, h, 0);
    c.SelectedFill({0, 0, w, h}, 0.55f);
    if (compact)
        Outline(c, {0, 0, w, h}, col::gold, 3, 1.0f); // the edge band would cut the descenders
    else
        GoldEdge(c, {0, 0, w, h});
    return std::move(c.Pixels());
}

ce_draw::Image ComposeGlow(ce_draw::Art& art, int kind) {
    const int w = kind == 0 ? SkSlotW : SkClassW, h = kind == 0 ? SkSlotH : SkClassH;
    const int p = SkGlowPad;
    Canvas c(art, w + 2 * p, h + 2 * p, 0);
    c.SelectedFill({p + 9, p + 9, w - 18, h - 18}, 0.5f);
    GoldEdge(c, {p, p, w, h});
    Outline(c, {0, 0, w + 2 * p, h + 2 * p}, col::gold, 3, 0.6f);
    return std::move(c.Pixels());
}

} // namespace ce_page_skills
