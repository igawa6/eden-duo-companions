// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Battle page values for Dragon Quest III HD-2D Remake 1.1.0.0. Layout and rules: dq3_battle_view.h.

#include "dq3_battle_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "dq3_battle_art.h"

namespace dq3 {
namespace {

using namespace bl;

// revision 2 colours (render.py): SOFT = the lifted 0.9.1 DIM, Weak / Resist darkened for the light scrap
constexpr u32 Cream = 0xFFF2EBD3, Gold = 0xFFECD292, Dim = 0xFFD6CEB8;
constexpr u32 Ink = 0xFF281C0E, Ink2 = 0xFF4A361E, Brown = 0xFF682E00;
constexpr u32 Weak = 0xFF6E1C00, Resist = 0xFF10328C, Grey = 0xFF8A7C64;

std::u32string U(std::string_view ascii) {
    return AsciiToU32(ascii);
}

// Effect classes by controller vtable (main-relative), research runs/slice2-battle/re/status-icons.
bool IsBuffClass(std::uint64_t vt) {
    switch (vt) {
    case 0x4dfffe8: // OFFENSE_UP / DOWN
    case 0x4e00010: // DEFENSE_UP / DOWN
    case 0x4e00038: // SPEED_UP / DOWN
    case 0x4e00060: // AVOID_UP / DOWN
    case 0x4e00088: // POWER_DOWN
    case 0x4e000b0: // SPELL_ATTACK_UP
    case 0x4e000d8: // SPELL_DAMAGE_DOWN
    case 0x4e00100: // SPELL_WEAKNESS
        return true;
    default:
        return false;
    }
}
bool IsFlagClass(std::uint64_t vt) {
    static constexpr std::uint64_t Flags[] = {
        0x4e01648, 0x4e017c0, 0x4e017e8, 0x4e01810, 0x4e01838, 0x4e01860, 0x4e01888, 0x4e018b0,
        0x4e018d8, 0x4e01900, 0x4e01928, 0x4e01950, 0x4e01978, 0x4e019a0, 0x4e01b18, 0x4e01b40,
        0x4e01b68, 0x4e01b90, 0x4e01bb8, 0x4e01be0, 0x4e01c08, 0x4e01c30, 0x4e01da8, 0x4e01dd0,
        0x4e01e20, 0x4e01e48, 0x4e01e70, 0x4e01df8};
    return std::find(std::begin(Flags), std::end(Flags), vt) != std::end(Flags);
}

struct IconRule {
    u32 cell;
    bool buff; ///< EBattleBuffDebuffFlag (buff classes) or ENicolaUnitStatusEffectFlag (flag classes)
    u32 mask;
};
// Panel 00 then panel 01, each in the rebuild's candidate order (0xae4ed0).
constexpr IconRule IconRules[] = {
    {1, false, 0x1000},   {3, true, 0x4},         {5, true, 0x10},      {19, false, 0x20000},
    {22, false, 0x100000}, {20, false, 0x40000},  {23, false, 0x200000}, {21, false, 0x80000},
    {2, true, 0x2},        {4, true, 0x8},         {6, true, 0x20},      {16, false, 0x20},
    {18, false, 0x10010000}, {8, true, 0x800},
};

const std::u32string* MonsterName(const BattleViewInput& in, const BattleUnit& u, bool& retry) {
    const auto text = in.fname ? in.fname(u.name_text) : std::nullopt;
    if (!text) {
        retry = true;
        return nullptr;
    }
    const auto* n = in.data ? in.data->Noun(*text) : nullptr;
    if (!n)
        retry = true;
    return n;
}

struct Out {
    BattleView& v;
    char key[64];
    void I(const char* k, std::int64_t x) {
        v.ints.emplace_back(k, x);
    }
    void T(const char* k, std::string s) {
        v.texts.emplace_back(k, std::move(s));
    }
    const char* K(const char* fmt, int a, int b = 0) {
        std::snprintf(key, sizeof(key), fmt, a, b);
        return key;
    }
};

std::string Num(std::int64_t v) {
    return std::to_string(v);
}

/// Stat-row icons: up to two 28 px status icons right-aligned before the value (x offsets relative
/// to the value's right edge); `key` is the row prefix ("dq3.pd.s0", "dq3.ed.atk").
void StatIcons(Out& o, const std::string& key, const std::vector<u32>& cells, int value_w) {
    const int n = static_cast<int>(std::min<std::size_t>(cells.size(), 2));
    for (int j = 0; j < n; ++j) {
        const std::string p = key + ".fx" + std::to_string(j);
        o.I((p + ".on").c_str(), 1);
        o.I((p + ".x").c_str(), -(value_w + 6 + 30 * (n - j)));
        o.T((p + ".src").c_str(), "module:dq3:status/" + std::to_string(cells[static_cast<std::size_t>(j)]) + "/28");
    }
}

} // namespace

void MergeIconOrder(std::vector<u32>& order, const std::vector<u32>& active) {
    std::erase_if(order, [&](u32 c) { return std::find(active.begin(), active.end(), c) == active.end(); });
    for (const u32 c : active)
        if (std::find(order.begin(), order.end(), c) == order.end())
            order.push_back(c);
}

std::vector<u32> BattleIconCells(const BattleUnit& unit) {
    std::vector<u32> cells;
    if (!unit.active)
        return cells;
    for (const IconRule& r : IconRules) {
        s32 turns = 0;
        for (const BattleEffect& e : unit.effects) {
            const bool cls = r.buff ? IsBuffClass(e.vtable) : IsFlagClass(e.vtable);
            const bool hit = r.buff ? e.flag == r.mask : (e.flag & r.mask) != 0;
            if (cls && hit)
                turns = std::max(turns, e.turns);
        }
        if (turns > 0)
            cells.push_back(r.cell);
    }
    return cells;
}

std::vector<u32> StatIconCells(const std::vector<u32>& cells, int stat) {
    static constexpr u32 Cells[3][2] = {{1, 2}, {3, 4}, {5, 6}};
    std::vector<u32> out;
    if (stat < 0 || stat > 2)
        return out;
    for (const u32 c : cells)
        if (c == Cells[stat][0] || c == Cells[stat][1])
            out.push_back(c);
    return out;
}

std::string EnemySpellLabel(std::u32string_view name, const FontAtlas* font, int width) {
    std::string label;
    for (const auto style : {FontStyle::Semi28, FontStyle::Semi24, FontStyle::Semi22, FontStyle::Semi20}) {
        label = Styled(name, style);
        if (!font || font->Measure(label) <= width) break;
    }
    return label;
}

std::vector<std::pair<int, int>> EnemySpellCells(const std::vector<std::u32string>& names, const FontAtlas* font) {
    std::vector<std::pair<int, int>> out;
    int col = 0, row = 0;
    for (const auto& n : names) {
        const bool wide = font && font->Measure(EnemySpellLabel(n, font)) > bl::EnemySpellW;
        if (wide && col != 0) {
            col = 0;
            ++row;
        }
        out.emplace_back(col, row);
        if (wide || col == 1) {
            col = 0;
            ++row;
        } else {
            col = 1;
        }
    }
    return out;
}

bool ActorCurrent(const BattleSnapshot& b, s32 turn_actor) {
    if (b.state < 8 || b.state > 14 || b.actor < 0)
        return false;
    // first action of the turn: ACTION_START entered from TURN_START, the id not yet rewritten
    return !(b.state == 8 && b.prev == 6 && b.actor == turn_actor);
}

FollowSelection FollowSelect(const BattleSnapshot& b, const std::vector<BattleMember>& members,
                             const std::optional<BattleMenu>& menu, bool actor_valid) {
    FollowSelection f;
    const int n_enemies = static_cast<int>(std::min<std::size_t>(b.enemies.size(), 10));
    const int n_party = static_cast<int>(std::min<std::size_t>(members.size(), 4));
    const auto party_card = [&](s32 id) {
        for (const auto& p : b.party)
            if (p.id == id)
                for (int k = 0; k < n_party; ++k)
                    if (p.field && p.field == members[static_cast<std::size_t>(k)].field)
                        return k;
        return -1;
    };
    if (b.state >= 8 && b.state <= 14 && b.actor >= 0 && actor_valid) {
        for (int k = 0; k < n_enemies; ++k)
            if (b.enemies[static_cast<std::size_t>(k)].id == b.actor) {
                // an actor already out of the battle (HP 0: defeated, or a flee, which sets HP 0 when it
                // starts) is no longer on the field: nothing is selected, the normal layout stays
                if (b.enemies[static_cast<std::size_t>(k)].Defeated())
                    return f;
                f.sel = BattleSel::Enemy;
                f.index = k;
                f.swap = true;
                f.glow = {k};
                return f;
            }
        if (const int k = party_card(b.actor); k >= 0) {
            f.sel = BattleSel::Party;
            f.index = k;
        }
        return f;
    }
    if (!b.CommandInput() || !menu)
        return f;
    switch (menu->focus) {
    case MenuFocus::Member:
        if (const int k = party_card(menu->member); k >= 0) {
            f.sel = BattleSel::Party;
            f.index = k;
        }
        break;
    case MenuFocus::TargetAlly:
        if (const int k = party_card(menu->target_unit); k >= 0) {
            f.sel = BattleSel::Party;
            f.index = k;
        }
        break;
    case MenuFocus::TargetEnemy:
        for (int k = 0; k < n_enemies; ++k) {
            const BattleUnit& e = b.enemies[static_cast<std::size_t>(k)];
            if (e.group != menu->target_group || e.Defeated())
                continue;
            if (f.index < 0) {
                f.sel = BattleSel::Enemy;
                f.index = k;
            }
            f.glow.push_back(k);
        }
        break;
    default:
        break;
    }
    return f;
}

BattleView BuildBattleView(const BattleViewInput& in) {
    BattleView v;
    Out o{v, {}};
    if (!in.battle || !in.battle->in_battle)
        return v;
    const BattleSnapshot& b = *in.battle;
    const FontAtlas* font = in.font;
    const auto measure = [font](const std::string& styled) { return font ? font->Measure(styled) : 0; };

    // --- selection (validated against what is on screen) ---------------------------------------
    const int n_enemies = static_cast<int>(std::min<std::size_t>(b.enemies.size(), 10));
    const int n_party = static_cast<int>(std::min<std::size_t>(in.members.size(), 4));
    v.sel = in.sel;
    v.sel_index = in.sel_index;
    std::vector<int> glow;
    if (in.follow) {
        const FollowSelection f = FollowSelect(b, in.members, in.menu, in.actor_valid);
        v.sel = f.sel;
        v.sel_index = f.index;
        v.swap = f.swap;
        glow = f.glow;
    }
    if ((v.sel == BattleSel::Enemy && (v.sel_index < 0 || v.sel_index >= n_enemies)) ||
        (v.sel == BattleSel::Party && (v.sel_index < 0 || v.sel_index >= n_party)) ||
        v.sel == BattleSel::None) {
        v.sel = BattleSel::None;
        v.sel_index = -1;
        v.swap = false;
        glow.clear();
    }
    if (!in.follow && v.sel == BattleSel::Enemy)
        glow = {v.sel_index};
    const bool names = v.sel != BattleSel::Party;
    const int enemy_y = v.swap ? SwapEnemyY : EnemyY;
    const int party_bottom = v.swap ? SwapPartyBottom : PartyBottom;
    o.I("dq3.b.swap", v.swap);
    // the party row's layout gates (owner 2026-10-10, gen_manifest battle_party): N cards share the width;
    // pl adds 4 in the Follow enemy-turn swap
    o.I("dq3.b.pn", n_party);
    o.I("dq3.b.pl", n_party + (v.swap ? 4 : 0));
    o.I("dq3.b.m_enemy", v.sel == BattleSel::Enemy);
    o.I("dq3.b.m_party", v.sel == BattleSel::Party);
    o.I("dq3.b.names", names);
    o.I("dq3.b.bare", !names);

    // --- command tabs: manual selection or the native acting unit --------------------------------
    {
        // render.py command_tabs(icons=False, px=34): the chevron (34 px) and 8 px before the selected label,
        // the group centred in its half
        const std::string t0 = Styled(std::string_view{"Tactical"}, FontStyle::Bold34);
        const std::string t1 = Styled(std::string_view{"Follow"}, FontStyle::Bold34);
        const double slot = TabsW / 2.0;
        constexpr int cs = 34;
        const double gx0 = (slot - (measure(t0) + (!in.follow ? cs + 8 : 0))) / 2.0 + (!in.follow ? cs + 8 : 0);
        const double gx1 = slot + (slot - (measure(t1) + (in.follow ? cs + 8 : 0))) / 2.0 + (in.follow ? cs + 8 : 0);
        o.T("dq3.b.tabs", "module:dq3:tabs/" + Num(TabsW) + "x" + Num(HeadH) + "/" + Num(in.follow ? 1 : 0) + "/" +
                              Num(static_cast<int>((in.follow ? gx1 : gx0) - cs - 8)));
        o.I("dq3.b.tab0x", TabsX + static_cast<int>(gx0));
        o.I("dq3.b.tab1x", TabsX + static_cast<int>(gx1));
        o.T("dq3.b.tab0", in.follow ? t0 : Colour(Gold, t0));
        o.T("dq3.b.tab1", in.follow ? Colour(Gold, t1) : t1);
    }

    // --- enemies card (render.py enemy_box): 2 x 5 cells, rows centred in the box and in themselves ---------
    const int cell_h = names ? CellNames : CellBare;
    const int box_h = names ? BoxNames : BoxBare;
    const double inner = CW - 2 * Gi;
    const double cw = (inner - 4 * Gap) / 5.0;
    const int nrows = n_enemies > 5 ? 2 : 1;
    const double top0 = enemy_y + (box_h - (nrows * cell_h + (nrows - 1) * Gap)) / 2.0;
    for (int i = 0; i < n_enemies; ++i) {
        const BattleUnit& e = b.enemies[static_cast<std::size_t>(i)];
        const int r = i < 5 ? 0 : 1, c = i < 5 ? i : i - 5;
        const int in_row = r == 0 ? std::min(n_enemies, 5) : n_enemies - 5;
        const double x0 = X0 + Gi + (inner - (in_row * cw + (in_row - 1) * Gap)) / 2.0;
        const double cx = x0 + c * (cw + Gap), cy = top0 + r * (cell_h + Gap);
        const int bx = static_cast<int>(cx), by = static_cast<int>(cy);
        const bool sel = v.sel == BattleSel::Enemy && std::find(glow.begin(), glow.end(), i) != glow.end();
        const bool dead = e.Defeated();
        // Follow, enemy turn: every enemy but the acting one is drawn grey (render.py battle_follow)
        const bool dim = dead || (v.swap && i != v.sel_index);
        const auto mtext = in.fname ? in.fname(e.master) : std::nullopt;
        const auto sprite = mtext ? FindMonsterSprite(*mtext) : std::nullopt;
        if (!mtext)
            v.retry = true;
        o.I(o.K(names ? "dq3.e%d.on_n" : "dq3.e%d.on_b", i), 1);
        o.I(o.K("dq3.e%d.x", i), bx);
        o.I(o.K("dq3.e%d.y", i), by);
        // ecell/<sprite>/<w>x<h>/<sprite box h>/<1 = bottom-aligned above the name, 0 = centred>/<flags>
        std::string flags = std::string{sel ? "s" : ""} + (dim ? "g" : "");
        o.T(o.K("dq3.e%d.img", i), "module:dq3:ecell/" + Num(sprite ? static_cast<std::int64_t>(*sprite) : -1) +
                                       "/" + Num(EnemyCellW) + "x" + Num(cell_h) + "/" + Num(names ? 56 : cell_h - 12) +
                                       "/" + Num(names ? 1 : 0) + "/" + flags);
        if (!names)
            continue;
        const auto* nm = MonsterName(in, e, v.retry);
        std::u32string label = nm ? *nm : U"";
        const auto grp = static_cast<std::size_t>(e.group);
        if (grp < b.groups.size() && b.groups[grp].initial > 1 && e.index >= 0 && e.index < 26)
            label += U" " + std::u32string(1, static_cast<char32_t>(U'A' + e.index));
        const int room = static_cast<int>(cw) - 10;
        FontStyle st = FontStyle::Value;
        if (sel && measure(Styled(label, FontStyle::Bold26)) <= room) {
            st = FontStyle::Bold26;
        } else {
            for (const FontStyle s2 : {FontStyle::Value, FontStyle::Semi24, FontStyle::Semi22, FontStyle::Semi20}) {
                st = s2;
                if (measure(Styled(label, s2)) <= room)
                    break;
            }
        }
        const std::string styled = Styled(label, st);
        o.T(o.K("dq3.e%d.name", i), sel ? Colour(Gold, styled) : dim ? Colour(Dim, styled) : styled);
        o.I(o.K("dq3.e%d.lx", i), static_cast<int>(cx + cw / 2));
        o.I(o.K("dq3.e%d.ly", i), by + 90 - 15); // baseline cy + 90
    }

    // --- party: battle units matched to the field members by F --------------------------------
    std::vector<const BattleUnit*> units(static_cast<std::size_t>(n_party), nullptr);
    for (int k = 0; k < n_party; ++k)
        for (const BattleUnit& u : b.party)
            if (u.field && u.field == in.members[static_cast<std::size_t>(k)].field)
                units[static_cast<std::size_t>(k)] = &u;
    // Queued move of member k: the command record while commands are entered (cleared at
    // TURN_START), then the unit's action slot 0 during the turn (an AI member's slot is filled at
    // its own ACTION_START; after acting, the target is the resolved unit).
    struct Move {
        std::u32string cmd, tgt;
        bool self{};
    };
    // The name and target of an action FName for unit u (the command name rules below).
    const auto describe = [&](const BattleUnit* u, const std::uint32_t action[2], s32 target) -> std::optional<Move> {
        const auto id = in.fname(action);
        if (!id) {
            v.retry = true;
            return std::nullopt;
        }
        if (*id == "ACTION_NO_ACTION" || id->empty() || *id == "None")
            return std::nullopt;
        Move mv;
        // an enemy's slot holds a GOP_Battle_Monster_Action row: its GOP_Magic row names it
        std::string_view row = *id;
        if (const auto ma = in.data->monster_actions.find(row); ma != in.data->monster_actions.end())
            row = ma->second;
        if (const auto it = in.data->magic.find(row); it != in.data->magic.end()) {
            if (row == "MAGIC_GUARD")
                mv.cmd = U"Defend"; // Txt_BattleMenu_Fight_COMMAND_02
            else if (const auto* n = in.data->Noun(it->second.name_text))
                mv.cmd = *n;
            else if (it->second.attack)
                mv.cmd = U"Attack"; // SkillType ATTACK rows without a noun: Txt_BattleMenu_Fight_COMMAND_00
            // other rows without a noun (MAGIC_RUN_AWAY, MAGIC_CALL_*, confusion actions) have no game
            // name: no move line
        } else if (const auto itm = in.data->items.find(*id); itm != in.data->items.end()) {
            if (const auto* n = in.data->Noun(itm->second))
                mv.cmd = *n;
        }
        if (mv.cmd.empty())
            return std::nullopt;
        if (target >= 0 && target < 20) {
            for (const BattleUnit& e : b.enemies)
                if (e.id == target)
                    if (const auto* n = MonsterName(in, e, v.retry)) {
                        mv.tgt = *n;
                        const auto g = static_cast<std::size_t>(e.group);
                        if (g < b.groups.size() && b.groups[g].initial > 1 && e.index >= 0 && e.index < 26)
                            mv.tgt += U" " + std::u32string(1, static_cast<char32_t>(U'A' + e.index));
                    }
        } else if (target >= 20 && target < 24) {
            if (target == u->id) {
                mv.tgt = U"Self";
                mv.self = true;
            } else {
                for (int j = 0; j < n_party; ++j)
                    if (units[static_cast<std::size_t>(j)] && units[static_cast<std::size_t>(j)]->id == target)
                        mv.tgt = in.members[static_cast<std::size_t>(j)].name;
            }
        } else if ((target & 0x60) == 0x20) { // EBattleTargetMaskType GROUP
            const int g = target & 0x1f;
            if (g == 6) {
                mv.tgt = U"Party";
            } else if (static_cast<std::size_t>(g) < b.groups.size()) {
                const BattleGroup& grp = b.groups[static_cast<std::size_t>(g)];
                for (const BattleUnit& e : b.enemies)
                    if (e.group == g)
                        if (const auto* n = MonsterName(in, e, v.retry)) {
                            mv.tgt = *n;
                            break;
                        }
                (void)grp;
            }
        } else if (target == 0x41 || target == 0x44) {
            mv.tgt = U"All enemies";
        } else if (target == 0x42 || target == 0x48) {
            mv.tgt = U"Party";
        } else if (target == 0x43) {
            mv.tgt = U"Everyone";
        }
        return mv;
    };
    const auto move_of = [&](int k) -> std::optional<Move> {
        const BattleUnit* u = units[static_cast<std::size_t>(k)];
        if (!u || !in.fname || !in.data)
            return std::nullopt;
        std::uint32_t action[2]{};
        s32 target = 0x40;
        const std::size_t rec = static_cast<std::size_t>(u->id - 20);
        if (b.CommandInput()) {
            // Follow: the member choosing a target shows its pending command at the cursor
            // (C+0xE0 record; enemy target = the cursor's group | 0x20, ally target = the unit id)
            if (in.follow && in.menu && in.menu->member == u->id &&
                (in.menu->focus == MenuFocus::TargetEnemy || in.menu->focus == MenuFocus::TargetAlly)) {
                const BattleCommand& c = in.menu->pending;
                std::memcpy(action, c.command == 2 ? c.item : c.action, 8);
                target = in.menu->focus == MenuFocus::TargetEnemy ? (in.menu->target_group | 0x20) : in.menu->target_unit;
            } else {
                if (u->id < 20 || rec >= b.commands.size() || !b.commands[rec].valid)
                    return std::nullopt;
                const BattleCommand& c = b.commands[rec];
                std::memcpy(action, c.command == 2 ? c.item : c.action, 8);
                target = c.target;
            }
        } else if (b.state >= 6 && b.state <= 14 && b.state != 7) {
            std::memcpy(action, u->action, 8);
            target = u->target;
        } else {
            return std::nullopt;
        }
        return describe(u, action, target);
    };
    std::vector<std::optional<Move>> moves(static_cast<std::size_t>(n_party));
    for (int k = 0; k < n_party; ++k)
        moves[static_cast<std::size_t>(k)] = move_of(k);

    for (int k = 0; k < n_party; ++k) {
        const BattleMember& m = in.members[static_cast<std::size_t>(k)];
        const BattleUnit* u = units[static_cast<std::size_t>(k)];
        o.I(o.K("dq3.bp%d.on", k), 1);
        if (const auto& mv = moves[static_cast<std::size_t>(k)];
            mv && !(v.sel == BattleSel::Party && v.sel_index == k)) {
            o.I(o.K("dq3.bp%d.mv", k), 1);
            o.T(o.K("dq3.bp%d.cmd", k), Styled(mv->cmd, FontStyle::Bold32));
            if (!mv->tgt.empty()) {
                const std::string t = Styled(mv->tgt, FontStyle::Semi28);
                o.T(o.K("dq3.bp%d.tgt", k), mv->self ? Colour(Dim, t) : t);
            }
        }
        if (!u)
            continue;
        const int hps = HpState(u->hp, u->hp_max);
        const u32 colour = !in.colours || hps == 0 ? 0
                           : hps == 2              ? in.colours->dead
                           : hps == 3              ? in.colours->low
                                                   : in.colours->half;
        const auto paint = [colour](const std::string& s) { return colour ? Colour(colour, s) : s; };
        const std::string name = Styled(m.name, FontStyle::Bold34);
        o.T(o.K("dq3.bp%d.name", k), paint(name));
        o.T(o.K("dq3.bp%d.hp_t", k), paint(Styled(U(Num(u->hp)), FontStyle::Bold32)));
        o.T(o.K("dq3.bp%d.mp_t", k), u->mp_max > 0 ? Styled(U(Num(u->mp)), FontStyle::Bold32) : Styled(U"-", FontStyle::Bold32));
        o.I(o.K("dq3.bp%d.hp", k), u->hp);
        o.I(o.K("dq3.bp%d.hp_max", k), u->hp_max);
        o.I(o.K("dq3.bp%d.mp", k), u->mp);
        o.I(o.K("dq3.bp%d.mp_max", k), u->mp_max);
        // sprite: coffin when dead (GetLooksId's coffin rule on the battle HP)
        const auto spr = u->hp < 1 ? m.coffin : m.sprite;
        if (spr) {
            o.T(o.K("dq3.bp%d.sp_full", k), "module:dq3:pcsprite/" + Num(*spr) + "/116/b");
            o.T(o.K("dq3.bp%d.sp_below", k), "module:dq3:pcsprite/" + Num(*spr) + "/84/c");
        }
        // status icons after the name, three at a time (paging like the game's panels)
        const auto cells = m.icons ? *m.icons : BattleIconCells(*u);
        const int nw = measure(name);
        const int shown = static_cast<int>(std::min<std::size_t>(cells.size(), 3));
        const int pages = cells.empty() ? 1 : static_cast<int>((cells.size() + 2) / 3);
        const int first = (in.icon_page % pages) * 3;
        const int count = std::min<int>(3, static_cast<int>(cells.size()) - first);
        const int iw = count * 34;
        const int left = -(nw + (count ? 10 + iw : 0)) / 2; // relative to the card centre
        o.I(o.K("dq3.bp%d.nx", k), left + nw / 2);          // name label centre offset
        for (int j = 0; j < count; ++j) {
            o.I(o.K("dq3.bp%d.st%d.on", k, j), 1);
            o.I(o.K("dq3.bp%d.st%d.x", k, j), left + nw + 10 + j * 34);
            o.T(o.K("dq3.bp%d.st%d.src", k, j),
                "module:dq3:status/" + Num(cells[static_cast<std::size_t>(first + j)]) + "/30");
        }
        (void)shown;
        const bool grown = v.sel == BattleSel::Party && v.sel_index == k;
        const bool full = v.sel == BattleSel::None;
        o.I(o.K("dq3.bp%d.full", k), full);
        o.I(o.K("dq3.bp%d.short", k), v.sel == BattleSel::Enemy || (v.sel == BattleSel::Party && !grown));
        o.I(o.K("dq3.bp%d.grow", k), grown);
        o.I(o.K("dq3.bp%d.tap_s", k), !full);
        // the name's baseline: full cards put the sprite above it (y + 168), short / grown cards y + 40
        o.I(o.K("dq3.bp%d.ny", k), full ? party_bottom - FullH + 168 : party_bottom - ShortH + 40);
    }

    // Always clear list gates when selection/species changes, including empty/unavailable data.
    if (v.sel != BattleSel::Enemy) {
        o.I("dq3.ed.group.on", 0);
        o.I("dq3.ed.act.on", 0);
        o.I("dq3.ed.act.tgt.on", 0);
        o.I("dq3.ed.sp.none", 0);
        o.I("dq3.ed.sp.unavailable", 0);
        for (int k = 0; k < MaxEnemySpells; ++k)
            o.I(o.K("dq3.ed.sp%d.on", k), 0);
    }
    // --- enemy detail ----------------------------------------------------------------------------
    if (v.sel == BattleSel::Enemy) {
        const BattleUnit& e = b.enemies[static_cast<std::size_t>(v.sel_index)];
        const auto mtext = in.fname ? in.fname(e.master) : std::nullopt;
        const auto sprite = mtext ? FindMonsterSprite(*mtext) : std::nullopt;
        if (sprite)
            // the sprite box left of the name, above the three stats (render.py enemy_detail: 240 wide, scale <= 6)
            o.T("dq3.ed.sprite", "module:dq3:mon/" + Num(*sprite) + "/240x" +
                                     Num((v.swap ? SwapDetailH : NormalDetailH) - 42 - 3 * 38 - 4) + "/6");
        const auto* nm = MonsterName(in, e, v.retry);
        std::u32string label = nm ? *nm : U"";
        const auto grp = static_cast<std::size_t>(e.group);
        const s32 group_n = grp < b.groups.size() ? b.groups[grp].initial : 1;
        if (group_n > 1 && e.index >= 0 && e.index < 26)
            label += U" " + std::u32string(1, static_cast<char32_t>(U'A' + e.index));
        const std::string name = Styled(label, FontStyle::Bold40);
        o.T("dq3.ed.name", name);
        o.T("dq3.ed.lv", Styled(U("Lv " + Num(e.level)), FontStyle::Semi28));
        o.I("dq3.ed.lvx", measure(name) + 12);
        o.T("dq3.ed.group", Styled(group_n > 1 ? U(Num(group_n) + " in group") : U"Alone", FontStyle::Semi28));
        o.T("dq3.ed.hp_t", Styled(U(Num(e.hp) + " / " + Num(e.hp_max)), FontStyle::Bold34));
        o.I("dq3.ed.hp", e.hp);
        o.I("dq3.ed.hp_max", e.hp_max);
        const auto row = in.fname ? in.fname(e.monster) : std::nullopt;
        const MonsterInfo* mi = nullptr;
        if (row && in.data)
            if (const auto it = in.data->monsters.find(*row); it != in.data->monsters.end())
                mi = &it->second;
        if (!mi)
            v.retry = true;
        if (mi) {
            const s32 vals[3] = {mi->attack, mi->defense, mi->speed};
            static constexpr const char* Keys[3] = {"dq3.ed.atk", "dq3.ed.def", "dq3.ed.agi"};
            const auto cells = BattleIconCells(e);
            for (int k = 0; k < 3; ++k) {
                const std::string t = Styled(U(Num(vals[k])), FontStyle::Bold30);
                o.T(Keys[k], t);
                StatIcons(o, Keys[k], StatIconCells(cells, k), measure(t));
            }
        }
        std::vector<std::u32string> spell_names;
        bool spells_ok = mi && in.data;
        if (spells_ok && mi->action_group != "None") {
            const auto it = in.data->enemy_spells.find(mi->action_group);
            spells_ok = it != in.data->enemy_spells.end() && it->second.size() <= MaxEnemySpells;
            if (spells_ok)
                for (const auto& text_id : it->second) {
                    const auto* name = in.data->Noun(text_id);
                    if (!name || name->empty()) { spells_ok = false; break; }
                    spell_names.push_back(*name);
                }
        }
        if (spells_ok) {
            std::sort(spell_names.begin(), spell_names.end());
            spell_names.erase(std::unique(spell_names.begin(), spell_names.end()), spell_names.end());
            const auto cells = EnemySpellCells(spell_names, font);
            for (std::size_t k = 0; k < spell_names.size(); ++k) {
                const auto [col, row] = cells[k];
                const bool wide = font && font->Measure(EnemySpellLabel(spell_names[k], font)) > EnemySpellW;
                o.T(o.K("dq3.ed.sp%d.name", static_cast<int>(k)),
                    EnemySpellLabel(spell_names[k], font, wide ? EnemySpellRowW : EnemySpellW));
                o.I(o.K("dq3.ed.sp%d.x", static_cast<int>(k)), col * EnemySpellCol);
                o.I(o.K("dq3.ed.sp%d.y", static_cast<int>(k)), row * EnemySpellRowH);
            }
        } else {
            v.retry = true;
        }
        o.I("dq3.ed.sp.none", spells_ok && spell_names.empty());
        o.I("dq3.ed.sp.unavailable", !spells_ok);
        for (int k = 0; k < MaxEnemySpells; ++k)
            o.I(o.K("dq3.ed.sp%d.on", k), spells_ok && static_cast<std::size_t>(k) < spell_names.size());
        // affinities: known (the monster record holds the species) -> multipliers, else "?"
        const bool known = in.known && mtext && in.known->contains(RecordKey(*mtext));
        const ResistInfo* ri = nullptr;
        if (mi && in.data)
            if (const auto it = in.data->resists.find(mi->resist); it != in.data->resists.end())
                ri = &it->second;
        for (int k = 0; k < ResistElements; ++k) {
            const auto* sn = in.data ? in.data->Noun(ResistSpellText[static_cast<std::size_t>(k)]) : nullptr;
            if (sn)
                o.T(o.K("dq3.ed.a%d.name", k), Styled(*sn, FontStyle::Semi30));
            // the element icon (dq3_element_art.h): the game's UI has none per family (GOP_Magic UIIconNo is
            // 0 for all six), so it is built from the family's own battle-effect textures and tints
            o.T(o.K("dq3.ed.a%d.icon", k), "module:dq3:elem/" + Num(k) + "/" + Num(bl::AffIconPx));
            if (known && ri) {
                const float m = ri->mult[static_cast<std::size_t>(k)];
                const Affinity a = AffinityOf(m);
                const std::string t = Styled(U(AffinityWord(a)), FontStyle::Bold32);
                o.T(o.K("dq3.ed.a%d.val", k), a == Affinity::Weak                                ? Colour(Weak, t)
                                                 : a == Affinity::Resist || a == Affinity::Immune ? Colour(Resist, t)
                                                                                                    : t);
                o.I(o.K("dq3.ed.a%d.q", k), 0);
            } else {
                o.T(o.K("dq3.ed.a%d.val", k), "");
                o.I(o.K("dq3.ed.a%d.q", k), 1);
            }
        }
        // Follow, enemy turn: the acting enemy's action (its action slot 0) and target
        bool acting = false, acting_tgt = false;
        if (v.swap && e.id == b.actor)
            if (const auto mv = describe(&e, e.action, e.target)) {
                acting = true;
                acting_tgt = !mv->tgt.empty();
                const std::string cmd = Styled(mv->cmd, FontStyle::Bold30);
                const std::string tgt = mv->tgt.empty() ? std::string{} : Styled(mv->tgt, FontStyle::Bold30);
                // right-aligned at the card's right edge: chevron, action, (chevron, target)
                int w = 32 + measure(cmd);
                if (!tgt.empty())
                    w += 10 + 32 + measure(tgt);
                int ax = -w;
                o.I("dq3.ed.act.ar0", ax);
                o.T("dq3.ed.act.cmd", cmd);
                o.I("dq3.ed.act.cmdx", ax + 32);
                if (!tgt.empty()) {
                    ax += 32 + measure(cmd) + 10;
                    o.I("dq3.ed.act.ar1", ax);
                    o.T("dq3.ed.act.tgt", mv->self ? Colour(Grey, tgt) : tgt);
                    o.I("dq3.ed.act.tgtx", ax + 32);
                }
            }
        o.I("dq3.ed.act.on", acting);
        o.I("dq3.ed.act.tgt.on", acting_tgt);
        o.I("dq3.ed.group.on", !acting);
    }

    // --- party detail ----------------------------------------------------------------------------
    if (v.sel == BattleSel::Party) {
        const BattleMember& m = in.members[static_cast<std::size_t>(v.sel_index)];
        const BattleUnit* u = units[static_cast<std::size_t>(v.sel_index)];
        const std::string pname = Styled(m.name, FontStyle::Bold36);
        o.T("dq3.pd.name", pname);
        // "name > move > target" (two_col_detail): chevrons 26 px, 32 px steps, 10 px after a word
        if (const auto& mv = moves[static_cast<std::size_t>(v.sel_index)]) {
            const std::string cmd = Styled(mv->cmd, FontStyle::Bold30);
            int ax = measure(pname) + 12;
            o.I("dq3.pd.mv", 1);
            o.I("dq3.pd.ar0", ax);
            ax += 32;
            o.T("dq3.pd.cmd", cmd);
            o.I("dq3.pd.cmdx", ax);
            ax += measure(cmd) + 10;
            if (!mv->tgt.empty()) {
                o.I("dq3.pd.mvt", 1);
                o.I("dq3.pd.ar1", ax);
                ax += 32;
                o.T("dq3.pd.tgt", Styled(mv->tgt, FontStyle::Bold30));
                o.I("dq3.pd.tgtx", ax);
            }
        }
        if (u) {
            o.T("dq3.pd.hp_t", Styled(U(Num(u->hp) + " / " + Num(u->hp_max)), FontStyle::Bold32));
            o.T("dq3.pd.mp_t", Styled(U(Num(u->mp) + " / " + Num(u->mp_max)), FontStyle::Bold32));
            o.I("dq3.pd.hp", u->hp);
            o.I("dq3.pd.hp_max", u->hp_max);
            o.I("dq3.pd.mp", u->mp);
            o.I("dq3.pd.mp_max", u->mp_max);
        }
        // Attack, Defence, Agility, Wisdom, Strength, Luck (revision 7 order)
        if (u) {
            const s32 vals[6] = {u->attack, u->defense, u->agility, u->wisdom, m.strength, u->luck};
            const auto cells = m.icons ? *m.icons : BattleIconCells(*u);
            for (int k = 0; k < 6; ++k) {
                if (k == 4 && !m.strength_ok)
                    continue;
                const std::string t = Styled(U(Num(vals[k])), FontStyle::Bold30);
                o.T(o.K("dq3.pd.s%d", k), t);
                if (k < 3)
                    StatIcons(o, o.K("dq3.pd.s%d", k), StatIconCells(cells, k), measure(t));
            }
        }
        o.T("dq3.pd.job", Styled(m.job, FontStyle::Bold30));
        o.T("dq3.pd.lv", Styled(U("Lv " + Num(u ? u->level : m.level)), FontStyle::Semi28));
        const int n = static_cast<int>(m.spells.size());
        o.I("dq3.pd.sp.count", n);
        o.I("dq3.pd.sp.none", n == 0);
        for (int i = 0; i < n; ++i) {
            const auto& sp = m.spells[static_cast<std::size_t>(i)];
            const std::string nm = Styled(sp.name, FontStyle::Semi28);
            o.T(o.K("dq3.pd.sp%d.name", i), sp.state == 2 ? nm : Colour(Grey, nm));
            if (sp.state != 0) {
                const std::string mp = Styled(U(Num(sp.mp) + " MP"), FontStyle::Bold26);
                o.T(o.K("dq3.pd.sp%d.mp", i), sp.state == 2 ? mp : Colour(Grey, mp));
            }
        }
    }
    (void)Ink;
    (void)Ink2;
    (void)Brown;
    (void)Cream;
    return v;
}

} // namespace dq3
