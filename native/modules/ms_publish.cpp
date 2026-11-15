// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ms_publish.h"
#include "ms_mapdraw.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace ms {

std::string CombatFigure(std::string_view text) {
    const auto first = text.find_first_of("0123456789");
    return first == std::string_view::npos ? std::string{} : std::string{text.substr(first)};
}

State StateFromGame(int s) {
    switch (s) {
    case 0:
        return State::Title;
    case 1:
        return State::Exploring;
    case 2:
        return State::Combat;
    case 3:
        return State::Menu;
    case 11:
    case 12:
        return State::Cinematic;
    default:
        return s >= 4 && s <= 10 ? State::Loading : State::Unknown;
    }
}

bool LeadPick(const Battle& b) {
    // StartCombatMenu opens at the Intro (combat state 1) of every battle: a wild one shows only
    // Start Combat / Retreat (kept on the Combat page), a Keeper battle (the encounter has a
    // Trainer) the "choose 3" lead pick. The same menu reopens mid-battle as the backup / switch
    // select, with its flag set (live 2026-11-09: Knight Challenge and MountainPath_East1).
    // A Keeper battle's whole Intro (CombatStates Inactive 0 / Intro 1) is the native VS / pick
    // screen: the menu itself opens ~1 s into it and closes ~0.4 s before Upkeep, so the gate is
    // the combat state, not the menu flag (live 2026-11-09: cs=1 smenu=000 at 111.4 s, 100 at
    // 112.4 s, 000 at 126.6 s, cs=2 at 127.1 s).
    // A wild battle's start screen is the same native VS screen: the Start Combat / Choose
    // Monsters / Retreat menu (and its Choose Monsters pick, the menu's MonsterMenu) is open over
    // the whole top at the Intro; the bottom dims for it too (small gaps 2026-11-12). Without the
    // menu open (a scripted battle that skips it) the Combat page stays.
    if (b.start_backup || b.start_switch || (b.combat_state != 0 && b.combat_state != 1))
        return false;
    return b.keeper || b.start_open;
}

std::string_view ElementName(int e) {
    switch (e) {
    case 0:
        return "Neutral";
    case 1:
        return "Fire";
    case 2:
        return "Water";
    case 3:
        return "Wind";
    case 4:
        return "Earth";
    case 5:
        return "Physical";
    case 6:
        return "Magical";
    default:
        return "";
    }
}

std::string ElementSprite(int e) {
    switch (e) {
    case 7:
        return "ui/ElementDebuff";
    case 9:
        return "ui/ElementQuad";
    default:
        return ElementName(e).empty() ? std::string{} : "ui/Element" + std::string{ElementName(e)};
    }
}

std::string WithIcons(std::string text, const std::vector<std::string>& sprites) {
    for (const auto& sp : sprites)
        if (!sp.empty())
            text += "{i:" + sp + "}";
    return text;
}

float ScoreLevel(const Battle::Score& sc) {
    return sc.actions > 0 ? static_cast<float>(sc.levels) / static_cast<float>(sc.actions) + 1.0f : 0.0f;
}

namespace {
/// Mathf.RoundToInt (round half to even, as System.Math.Round).
int RoundToInt(float f) {
    return static_cast<int>(std::nearbyint(f));
}
} // namespace

int ExecutionPercent(const Battle::Score& sc) {
    if (sc.final)
        return RoundToInt(sc.final_exec * 100.0f);
    if (!sc.valid)
        return -1;
    // CalculateResult 0x2ffd78.., single-precision as the game computes it
    const float level = ScoreLevel(sc);
    const float damage = sc.pre + sc.exec;
    const float combo = static_cast<float>(sc.combo);
    const float weakness = sc.weakness * 3.0f / 100.0f;
    float pct;
    if (level > 30.0f)
        pct = damage * 0.25f + (weakness + combo * 0.2f / 100.0f);
    else if (level > 10.0f)
        pct = damage / 3.0f + (weakness + combo * 0.25f / 100.0f);
    else
        pct = damage / 2.5f + (weakness + combo * 0.3f / 100.0f);
    return RoundToInt(pct * 100.0f);
}

int EnemyDifficultyPoints(const Battle::Score& sc) {
    if (sc.final)
        return sc.final_difficulty;
    if (!sc.valid)
        return -1;
    if (sc.foes.empty())
        return 0;
    // CalculateResult 0x2ff6a8..0x2ffa0c
    float sum = 0, levels = 0;
    for (const auto& f : sc.foes) {
        sum += f.bonus + static_cast<float>((f.rating + (f.shifted ? 1 : 0)) * 225 + 50);
        levels += static_cast<float>(f.level);
    }
    if (sc.foes.size() <= 2) {
        if (sc.active >= 0 && sc.active < 3)
            sum *= 1.5f;
        else if (!sc.champion)
            sum *= 0.5f;
    }
    const float avg = levels / static_cast<float>(sc.foes.size());
    sum *= 1.2f + (0.8f - 1.2f) * std::clamp(avg / 40.0f, 0.0f, 1.0f); // Mathf.Lerp
    const float level = ScoreLevel(sc);
    const float factor = level > avg ? std::max(avg + 10.0f - level, 1.0f) / 10.0f
                                     : std::min(avg + 15.0f - level, 25.0f) / 15.0f;
    return static_cast<int>(sum * factor);
}

int BuffPoints(const Battle::Score& sc) {
    if (sc.final)
        return sc.final_buffs;
    if (!sc.valid)
        return -1;
    // CalculateResult 0x2ffae0..0x2ffd74: weights by the player level, then each ally's total buff
    // strength and each enemy's total debuff strength, truncated one by one, capped at 4000
    const float level = ScoreLevel(sc);
    const float dot_w = level > 30.0f ? 60.0f : level > 20.0f ? 70.0f : 80.0f;
    const float buff_w = level > 30.0f ? 40.0f : level > 20.0f ? 45.0f : 50.0f;
    int points = static_cast<int>(dot_w * sc.dot);
    for (const float b : sc.ally_buffs)
        points += static_cast<int>(buff_w * b);
    for (const auto& f : sc.foes)
        points += static_cast<int>(buff_w * f.debuffs);
    return std::min(points, 4000);
}

int HealthinessPercent(const Battle::Score& sc) {
    // after CalculateResult the field already holds HP%^2
    return RoundToInt((sc.final ? sc.health : sc.health * sc.health) * 100.0f);
}

std::string TurnsText(const Battle::Score& sc, int monsters) {
    std::string out = std::to_string(sc.turns);
    if (sc.acted >= 1 && monsters == 2)
        out += ".5";
    else if (sc.acted >= 1 && monsters == 3)
        out += sc.acted == 1 ? ".33" : ".66";
    return out;
}

std::vector<int> VisibleRail(const Battle& b, int limit) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(b.defaults.size()) && static_cast<int>(out.size()) < limit; ++i) {
        const auto& r = b.defaults[static_cast<std::size_t>(i)];
        if (r.active && r.selectable && !r.disabled && !r.name.empty())
            out.push_back(i);
    }
    return out;
}

std::vector<MenuRow> ShownRows(const std::vector<MenuRow>& rows) {
    std::vector<MenuRow> out;
    for (const auto& r : rows)
        if (r.active && !r.name.empty())
            out.push_back(r);
    return out;
}

int ActiveCommand(const Battle& b) {
    // the rail rows by their native icon (the captions are localized, the sprites are not)
    const auto find = [&](std::string_view icon) {
        for (int i = 0; i < static_cast<int>(b.defaults.size()); ++i)
            if (b.defaults[static_cast<std::size_t>(i)].icon == icon)
                return i;
        return -1;
    };
    if (b.rail_cursor >= 0 && !b.targeting && !b.items_open && !b.switch_open) {
        for (int i = 0; i < static_cast<int>(b.defaults.size()); ++i)
            if (b.defaults[static_cast<std::size_t>(i)].row == b.rail_cursor)
                return i;
        return -1;
    }
    if (b.items_open)
        return find("icons/icon_default_items");
    if (b.switch_open)
        return find("icons/icon_switch");
    if (b.info.open)
        return find("icons/icon_default_enemy_info");
    return find("icons/icon_default_attack");
}

ItemLayout LayoutItems(const Battle& b, int max_columns) {
    ItemLayout out;
    std::vector<int> cols;
    for (const auto& r : b.items)
        if (std::find(cols.begin(), cols.end(), r.column) == cols.end() && static_cast<int>(cols.size()) < max_columns)
            cols.push_back(r.column);
    std::sort(cols.begin(), cols.end());
    if (cols.empty())
        return out;
    out.expanded = std::find(cols.begin(), cols.end(), b.item_column) != cols.end() ? b.item_column : cols.front();
    for (const int c : cols)
        if (c != out.expanded) {
            out.strips.push_back(c);
            if (c < out.expanded)
                ++out.left;
        }
    return out;
}

const MenuRow* ItemAt(const Battle& b, int slot, int row) {
    const auto lay = LayoutItems(b);
    if (lay.expanded < 0 || slot < 0 || slot > static_cast<int>(lay.strips.size()) || row < 0)
        return nullptr;
    const int column = slot == 0 ? lay.expanded : lay.strips[static_cast<std::size_t>(slot - 1)];
    int n = 0;
    for (const auto& r : b.items)
        if (r.column == column && n++ == row)
            return &r;
    return nullptr;
}

SkillLayout LayoutSkills(const Battle& b) {
    SkillLayout out;
    bool has[3]{};
    for (const auto& s : b.skills)
        if (s.column >= 1 && s.column <= 2)
            has[s.column] = true;
    if (!has[1] && !has[2])
        return out;
    // the column the native window expands (its rows' isExpanded), else the cursor's, else the first
    int ex = b.skill_column;
    if ((ex != 1 && ex != 2) || !has[ex]) {
        const Skill* cur = b.cursor >= 0 && b.cursor < static_cast<int>(b.skills.size())
                               ? &b.skills[static_cast<std::size_t>(b.cursor)] : nullptr;
        ex = cur && (cur->column == 1 || cur->column == 2) && has[cur->column] ? cur->column : (has[1] ? 1 : 2);
    }
    out.expanded = ex;
    const int other = ex == 1 ? 2 : 1;
    if (has[other]) {
        out.strip = other;
        out.strip_left = other < ex;
    }
    return out;
}

int SkillIndex(const Battle& b, int slot, int row) {
    const auto lay = LayoutSkills(b);
    const int column = slot == 0 ? lay.expanded : slot == 1 ? lay.strip : -1;
    if (column < 0 || row < 0)
        return -1;
    int n = 0;
    for (int i = 0; i < static_cast<int>(b.skills.size()); ++i)
        if (b.skills[static_cast<std::size_t>(i)].column == column && n++ == row)
            return i;
    return -1;
}

const Skill* SkillAt(const Battle& b, int slot, int row) {
    const int i = SkillIndex(b, slot, row);
    return i < 0 ? nullptr : &b.skills[static_cast<std::size_t>(i)];
}

std::vector<int> FieldSlots(const std::vector<float>& xs) {
    std::vector<int> order(xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i)
        order[i] = static_cast<int>(i);
    // left to right as the monsters stand (CombatPos.x); equal x keeps the list order
    std::stable_sort(order.begin(), order.end(), [&](int a, int c) {
        return xs[static_cast<std::size_t>(a)] < xs[static_cast<std::size_t>(c)];
    });
    std::vector<int> slot(xs.size());
    for (std::size_t k = 0; k < order.size(); ++k)
        slot[static_cast<std::size_t>(order[k])] = static_cast<int>(k);
    return slot;
}

std::vector<int> CardSlots(const Battle& b, bool ally) {
    std::vector<float> xs;
    if (ally)
        for (std::size_t i = 0; i < b.allies.size() && i < 3; ++i)
            xs.push_back(b.allies[i].combat_x);
    else
        for (std::size_t i = 0; i < b.enemies.size() && i < 3; ++i)
            xs.push_back(b.enemies[i].combat_x);
    return FieldSlots(xs);
}

bool TargetsAllies(const Battle& b) {
    // ETargetType Self (2), SingleAlly (3), AllAllies (4), DeadAlly (5): the own side
    return b.targeting && !b.info.open && b.target_kind >= 2 && b.target_kind <= 5;
}

bool TargetTakes(const Battle& b, std::uint64_t monster, bool ally) {
    if (!b.targeting || b.info.open || !monster)
        return false;
    if (std::find(b.targets.begin(), b.targets.end(), monster) != b.targets.end())
        return true;
    return b.target_kind == (ally ? 4 : 1);
}

std::string ElementList(const std::vector<int>& elements) {
    std::string out;
    for (const int e : elements) {
        const auto n = ElementName(e);
        if (n.empty() || out.find(n) != std::string::npos)
            continue;
        if (!out.empty())
            out += ", ";
        out += n;
    }
    return out;
}

std::string AreaFromScene(std::string_view scene) {
    while (!scene.empty() && std::isdigit(static_cast<unsigned char>(scene.back())))
        scene.remove_suffix(1);
    while (!scene.empty() && (scene.back() == '_' || scene.back() == ' '))
        scene.remove_suffix(1);
    std::string out;
    for (std::size_t i = 0; i < scene.size(); ++i) {
        const char c = scene[i];
        if (c == '_') {
            out += ' ';
            continue;
        }
        if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
            std::islower(static_cast<unsigned char>(scene[i - 1])) && out.back() != ' ')
            out += ' ';
        out += c;
    }
    return out;
}

namespace {
bool DuplicateArea(const MapInfo& map, int index) {
    const auto& t = map.teleports[index];
    return !t.scene.empty() && std::count_if(map.teleports.begin(), map.teleports.end(),
        [&](const auto& other) { return other.name == t.name; }) > 1;
}

}

bool CanBattleInput(const Model& m) {
    return m.state == State::Combat && m.battle.valid &&
           m.battle.combat_state == 3 && m.battle.player_turn;
}

bool CanTeleport(const Model& m) {
    return (m.state == State::Exploring && !m.menu_open && !m.dialogue) ||
           ((m.state == State::Exploring || m.state == State::Menu) && m.map.teleport_menu);
}

std::string TeleportMiniKey(const MapInfo& map, const Teleporter& t) {
    return "module:ms:tpa:" + std::to_string(MapSignature(map)) + ":" + t.scene;
}

void PublishTeleports(const MapInfo& map, const Sink& o, bool enabled) {
    if (map.teleports.empty())
        return;
    int y = 0;
    const int rows = std::min<int>(static_cast<int>(map.teleports.size()), TeleportRows);
    for (int row = 0; row < rows; ++row) {
        const int index = row;
        const auto& t = map.teleports[static_cast<std::size_t>(index)];
        const auto k = "tq" + std::to_string(row) + ".";
        o.i64(k + "v", 1);
        o.i64(k + "dy", y);
        o.i64(k + "index", index);
        if (enabled)
            o.i64(k + (DuplicateArea(map, index) ? "room.tap" : "single.tap"), 1);
        o.text(k + "name", t.name);
        // Two crystals in one area: told apart the native map's way, never by an invented name
        // or the internal scene suffix: where the crystal sits in its area (the area's explored
        // rooms in the map's own colours, this crystal's room lit and its cyan teleporter mark),
        // plus the native map's name of a place in that room when it has one ("Monster Farm").
        if (DuplicateArea(map, index)) {
            o.text(k + "mini", TeleportMiniKey(map, t));
            if (!t.label.empty()) {
                o.text(k + "label", t.label);
                o.i64(k + "label.on", 1);
            }
            o.i64(k + "room.on", 1);
            y += TeleportRoomRowPx + TeleportGapPx;
        } else {
            o.i64(k + "single", 1);
            y += TeleportRowPx + TeleportGapPx;
        }
        if (t.start)
            o.i64(k + (DuplicateArea(map, index) ? "room.here" : "single.here"), 1);
        if (t.start)
            o.i64(k + "here", 1);
    }
    // the scroll region's content: every row and the gaps between them
    o.i64("m.tp.h", std::max(0, y - TeleportGapPx));
}

namespace {

std::string Key(char p, int i, std::string_view field) {
    std::string k(1, p);
    k += std::to_string(i);
    k += '.';
    k += field;
    return k;
}

std::string Frame(const Sink& o, const std::string& sprite, int salt) {
    return o.sprite ? o.sprite(sprite, salt) : sprite;
}

/// Which card a monster is being published for: a party-list row (Field follower candidates /
/// Team list, both need the sprite, the ability name and, Team only, the level and shift), the
/// Team page's Equipment-side detail panel (no sprite or ability shown there; HP/MP/skill points/
/// gear instead), or the Exploration page's follower card (sprite, ability name and its wrapped
/// description). Each view only publishes what some widget actually binds to.
enum class MonsterView { Row, Detail, Follower };

void PublishMonster(const Sink& o, const std::string& pre, const PartyMonster& m, int salt,
                    bool in_combat, MonsterView view) {
    o.i64(pre + "v", 1);
    o.text(pre + "name", m.name);
    if (view == MonsterView::Detail) {
        o.i64(pre + "sp", m.skill_points);
        o.text(pre + "hptxt", std::to_string(m.hp) + "/" + std::to_string(m.hp_max));
        // CurrentMana is initialized for combat; the native pause pane shows the Mana stat.
        o.text(pre + "mptxt", in_combat ? std::to_string(m.mana) + "/" + std::to_string(m.mana_max)
                                       : std::to_string(m.mana_max));
        for (int k = 0; k < 6; ++k) {
            const auto& g = m.gear[static_cast<std::size_t>(k)];
            if (!g.name.empty())
                o.text(pre + "g" + std::to_string(k), g.name);
        }
        return; // the Equipment panel shows no sprite, level or ability
    }
    if (view == MonsterView::Row) {
        o.i64(pre + "lv", m.level);
        o.i64(pre + "shift", m.shift); // -> p<i>.light / p<i>.dark derived in the manifest
    }
    if (!m.sprite.empty()) {
        const auto frame = Frame(o, m.sprite, salt);
        o.text(pre + "spr", "module:ms:box2:" + frame);
        if (view == MonsterView::Row)
            o.text(pre + "rowspr", "module:ms:row2:" + frame);
    }
    if (!m.ability.empty()) {
        o.text(pre + "ab", m.ability);
        if (view == MonsterView::Follower) {
            // one native point per line for the wrapped description ("|" = line break)
            std::string lines;
            for (const char c : m.ability_desc)
                lines.push_back(c == '\n' ? '|' : c);
            o.text(pre + "abdw", lines);
        }
    }
}

/// "[sign] Weak [icon] Earth  [sign] Resist [icon] Wind": the native sign, label, element icon and
/// the element's own localized name, each icon right after the text before it.
std::string WeakResist(const std::vector<int>& weak, const std::vector<int>& resist) {
    const auto part = [](const char* sign, const char* label, const std::vector<int>& els) {
        std::string out;
        std::vector<int> seen;
        for (const int el : els) {
            const auto sp = ElementSprite(el);
            if (sp.empty() || std::find(seen.begin(), seen.end(), el) != seen.end())
                continue;
            seen.push_back(el);
            if (out.empty())
                out = std::string{"{i:"} + sign + "}{s:" + label + "}";
            std::string word{ElementName(el)};
            for (auto& ch : word)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            out += " {i:" + sp + "}" + (word.empty() ? "" : "{S:" + word + "}");
        }
        return out;
    };
    const auto w = part("ui/WeaknessSign", "weak", weak);
    const auto r = part("ui/ResistanceSign", "resist", resist);
    return w + (w.empty() || r.empty() ? "" : "  ") + r;
}

/// One side of a target card: "[sign] Weak [icon][icon]" (the native sign, label and element
/// icons, no element words: the compact horizontal card), "" when the monster has none. The
/// card's 60 px line holds the sign, the word and two icons (measured, ms_art_test); with more
/// icons the word gives way so the icons stay whole (the full words stay in the strip below,
/// b.tweak / b.tres).
std::string WeakResistLine(bool weak, const std::vector<int>& els) {
    constexpr std::size_t IconsWithWord = 2;
    std::string icons;
    std::vector<int> seen;
    for (const int el : els) {
        const auto sp = ElementSprite(el);
        if (sp.empty() || std::find(seen.begin(), seen.end(), el) != seen.end())
            continue;
        seen.push_back(el);
        icons += "{i:" + sp + "}";
    }
    if (seen.empty())
        return {};
    std::string out = weak ? "{i:ui/WeaknessSign}" : "{i:ui/ResistanceSign}";
    if (seen.size() <= IconsWithWord)
        out += weak ? "{s:weak}" : "{s:resist}";
    return out + icons;
}

/// A skill row (the command window's expanded column, the level popup): its key prefix k.
void PublishSkillRow(const Sink& o, const std::string& k, const Skill& s, bool targeting) {
    o.i64(k + "v", 1);
    // the name with the icons the native row draws right after it (type, element); without a
    // CombatMenuItem read, the element mark of the first ActionDamage
    std::vector<std::string> icons = s.inline_icons;
    if (icons.empty() && s.element >= 1 && s.element <= 4)
        icons.push_back(ElementSprite(s.element));
    o.text(k + "label", WithIcons(s.name, icons));
    o.i64(k + "mp", s.mana);
    if (!s.icon.empty())
        o.text(k + "ico", "module:ms:spr4:" + s.icon);
    if (!s.badge.empty())
        o.text(k + "badge", s.badge);
    if (s.affordable)
        o.i64(k + "ok", 1);
    else if (!targeting)
        o.i64(k + "dim", 1);
}

/// The native submenus the right pane mirrors: the item window (every shown row, the cursor's
/// column expanded, the others as icon strips; the ItemTooltip below), the Switch Monster list
/// and the Monster Info card.
void PublishSubmenus(const Battle& b, const Sink& o) {
    if (b.items_open) {
        o.i64("b.items", 1);
        const auto lay = LayoutItems(b);
        const int ncols = lay.expanded < 0 ? 1 : 1 + static_cast<int>(lay.strips.size());
        o.i64("it.w" + std::to_string(ncols), 1);
        o.i64("it.ex", lay.left * ItemStripPx);
        const auto publish_row = [&](const std::string& k, const MenuRow& r, bool expanded) {
            o.i64(k + "v", 1);
            if (!r.icon.empty())
                o.text(k + "ico", "module:ms:spr4:" + r.icon);
            if (expanded) {
                o.text(k + "name", r.name);
                if (!r.info.empty())
                    o.text(k + "info", r.info);
            }
            // 0/1: the cell's dim gate reads !ok (a derived expr reads an unpublished name as
            // missing, which would never dim)
            o.i64(k + "ok", !r.disabled && r.selectable ? 1 : 0);
        };
        for (int slot = 0; slot <= static_cast<int>(lay.strips.size()); ++slot)
            for (int row = 0; row < ItemRows; ++row) {
                const auto* r = ItemAt(b, slot, row);
                if (!r)
                    break;
                const std::string k = slot == 0 ? "ix" + std::to_string(row) + "." :
                                                  "sk" + std::to_string(slot - 1) + "." + std::to_string(row) + ".";
                publish_row(k, *r, slot == 0);
                if (slot == 0 && r->row == b.item_row && r->column == b.item_column)
                    o.i64(k + "sel", 1);
            }
        // strip k's canvas x: left strips first, then the expanded column, then the right ones
        const int rows_w = ItemRowsPx - (ncols - 1) * ItemStripPx;
        for (int k = 0; k < static_cast<int>(lay.strips.size()); ++k)
            o.i64("sk" + std::to_string(k) + ".x",
                  k < lay.left ? k * ItemStripPx : lay.left * ItemStripPx + rows_w + (k - lay.left) * ItemStripPx);
        const auto& t = b.item_tip;
        if (t.visible && !t.name.empty()) {
            o.i64("b.tip", 1);
            o.text("b.tipname", t.name);
            o.text("b.tipdesc", t.description);
            o.text("b.tiptype", t.type);
            if (!t.icon.empty())
                o.text("b.tipico", "module:ms:spr4:" + t.icon);
        }
    } else if (b.switch_open) {
        o.i64("b.switch", 1);
        const int n = static_cast<int>(std::min<std::size_t>(b.switches.size(), 3));
        for (int i = 0; i < n; ++i) {
            const auto& r = b.switches[static_cast<std::size_t>(i)];
            const std::string k = "w" + std::to_string(i) + ".";
            o.i64(k + "v", 1);
            o.text(k + "name", r.name);
            if (!r.info.empty())
                o.text(k + "info", r.info);
            if (!r.sprite.empty())
                o.text(k + "spr", "module:ms:box2:" + Frame(o, r.sprite, 50 + i));
            // the native switch card's element weaknesses / resistances
            if (const auto wr = WeakResist(r.weak, r.resist); !wr.empty())
                o.text(k + "wr", wr);
            if (!r.disabled)
                o.i64(k + "ok", 1);
            // the native cursor (the D-pad on the top moves it too)
            if (r.column == b.switch_column && r.row == b.switch_row)
                o.i64(k + "sel", 1);
        }
    }
    if (b.targeting && b.info.open) {
        const auto& c = b.info;
        o.text("b.ihead", c.headline);
        o.text("b.ihp", c.health);
        o.text("b.imp", c.mana);
        o.text("b.icrit", c.crit);
        o.text("b.icd", c.crit_damage);
        if (!c.sprite.empty())
            o.text("b.ispr", "module:ms:box4:" + Frame(o, c.sprite, 60));
        if (const auto wr = WeakResist(c.weak, c.resist); !wr.empty())
            o.text("b.iwr", wr);
    }
}

} // namespace

void Publish(const Model& m, int team_focus, const Sink& sink) {
    const Sink o{sink.i64,
                 [&sink](std::string_view k, std::string_view v) {
                     sink.text(k, v);
                     if (!v.empty())
                         sink.i64(std::string{k} + ".on", 1);
                 },
                 sink.sprite};
    o.i64("ms.state", static_cast<int>(m.state));
    if (m.state == State::Loading)
        o.i64("ms.loading", 1);
    // always published (0/1): page_binds arm only on a present value
    o.i64("ms.combat", m.state == State::Combat ? 1 : 0);
    // always published (0/1): the battle page's layout gates read it (the Keeper duel's first
    // turn gives the native rule its own band and shrinks the lists, see gen_manifest.py)
    o.i64("b.t1", m.state == State::Combat && m.battle.valid && m.battle.keeper && m.battle.turn == 0 ? 1 : 0);
    if (m.state == State::Exploring || m.state == State::Dialogue || m.state == State::Menu ||
        m.state == State::Cinematic)
        o.i64("ms.field", 1);
    // NPC dialogue and the Keeper Duel's native lead pick: the page stays readable but dimmed,
    // tabs live, game actions off (r3 states)
    if (m.state == State::Dialogue || m.state == State::LeadPick)
        o.i64("ms.dim", 1);
    if (m.state == State::Unknown || m.state == State::Title || m.state == State::Loading) {
        // the title / loading scene: the four Spectral Familiars of the starter pick idling
        for (int i = 0; i < 4; ++i)
            o.text("sc.m" + std::to_string(i), "module:ms:box6:" + Frame(o, std::string{Familiars[i]} + "/0", 70 + i));
    }
    if (m.state == State::Title) {
        // "Press [A] to start" only while the MainMenu waits for that press (EState PressAnyButton,
        // no save menu over it) and the game's own prompt is up (not during the intro before it;
        // without the helper the stage alone decides); the version text as the title renders it
        if (m.title_stage == 0 && !m.save_menu && m.title_prompt != 0)
            o.i64("ti.press", 1);
        if (!m.version.empty())
            o.text("ti.ver", m.version);
    }
    if (m.state == State::Unknown || m.state == State::Title || m.state == State::Loading)
        return; // no game loaded: nothing below is valid

    if (m.gold >= 0)
        o.i64("ms.gold", m.gold);
    if (!m.player.empty())
        o.text("ms.player", m.player);

    const int n = static_cast<int>(std::min<std::size_t>(m.party.size(), 12));
    if (n > 0) {
        o.i64("ms.party.n", n);
        for (int i = 0; i < n; ++i) {
            const std::string pre = "p" + std::to_string(i) + ".";
            PublishMonster(o, pre, m.party[static_cast<std::size_t>(i)], i, m.state == State::Combat,
                           MonsterView::Row);
            if (i == m.follower)
                o.i64(pre + "fol", 1);
        }
        const int f = std::clamp(team_focus, 0, n - 1);
        o.i64("t.i", f);
        PublishMonster(o, "t.", m.party[static_cast<std::size_t>(f)], f, m.state == State::Combat,
                       MonsterView::Detail);
    }
    // the follower card on the Exploration page
    if (!m.follower_mon.name.empty())
        PublishMonster(o, "f.", m.follower_mon, 12, m.state == State::Combat, MonsterView::Follower);

    const Battle& b = m.battle;
    const bool player_turn = CanBattleInput(m);
    if (m.state == State::Combat && b.valid) {
        o.i64("b.v", 1);
        if (player_turn)
            o.i64("b.turn", 1);
        // targeting (a skill's target, or the Monster Info command's browse: the native
        // TargetSelector in IsEnemyInfo mode, which shows the info card instead of a target list)
        if (player_turn && b.targeting) {
            o.i64("b.back", 1);
            o.i64(b.info.open ? "b.info" : "b.tgt", 1);
            // a skill on the own side: the target cards are the allies (ETargetType 2..5)
            if (TargetsAllies(b))
                o.i64("b.tgta", 1);
        }
        // the command list gives way to the native item menu / switch list when those are open
        const bool sub = b.items_open || b.switch_open;
        // the level popup's Back: its native B (LevelSelectMenu.OnSelectionCancelled)
        if (player_turn && !b.targeting && b.level_open)
            o.i64("b.subback", 1);
        // their Back: what B does there natively (the menu's own cancel, back to the commands)
        if (player_turn && !b.targeting && sub)
            o.i64("b.subback", 1);
        if (player_turn && !b.targeting && !sub)
            o.i64("b.cmd", 1);
        if (b.keeper) {
            o.i64("b.keeper", 1);
            std::string unknown;
            for (int i = 0; i < std::clamp(b.backup_enemies, 0, 6); ++i)
                unknown += i ? "  ?" : "?";
            if (!unknown.empty())
                o.text("b.unknown", unknown); // count only; never read backup identities
            // the duel's first-turn rule banner: CombatController.TurnCount is 0 through the preview
            // and both sides' first turn, 1 from the second player turn on (read live 2026-11-09)
            if (b.turn == 0)
                o.i64("b.turn1", 1);
        }
        // the Combat Result screen's live factors (the game's own labels)
        if (const int exec = ExecutionPercent(b.score); exec >= 0) {
            o.text("b.exec", std::to_string(exec) + "%");
            o.text("b.dur", TurnsText(b.score, static_cast<int>(b.allies.size())) + " {s:turns}");
            o.text("b.heal", std::to_string(HealthinessPercent(b.score)) + "%");
            o.text("b.diff", std::to_string(EnemyDifficultyPoints(b.score)));
            o.text("b.buffs", std::to_string(BuffPoints(b.score)));
        }
        o.text("b.combo", b.combo.empty() ? "0" : b.combo);
        if (!b.damage.empty())
            o.text("b.dmg", b.damage);
        const int na = static_cast<int>(std::min<std::size_t>(b.allies.size(), 3));
        // the target cards stand left to right as the monsters do on the battlefield
        const auto aslot = CardSlots(b, true);
        // a group skill (AllEnemies 1 / AllAllies 4): the native selector points at the whole side
        const bool all_allies = b.targeting && !b.info.open && b.target_kind == 4;
        const bool all_enemies = b.targeting && !b.info.open && b.target_kind == 1;
        for (int i = 0; i < na; ++i) {
            const auto& a = b.allies[static_cast<std::size_t>(i)];
            o.i64(Key('a', i, "v"), 1);
            o.text(Key('a', i, "name"), a.name);
            o.i64(Key('a', i, "hp"), a.hp);
            o.i64(Key('a', i, "hpmax"), a.hp_max);
            o.i64(Key('a', i, "mp"), a.mana);
            o.i64(Key('a', i, "mpmax"), a.mana_max);
            if (!a.sprite.empty())
                o.text(Key('a', i, "spr"), "module:ms:box2:" + Frame(o, a.sprite, 20 + i));
            if (i == b.caster)
                o.i64(Key('a', i, "cur"), 1);
            // the native TargetSelector's cursor (a skill's ally target, or the Monster Info
            // browse, which takes allies too)
            if (player_turn && b.targeting && ((a.ptr && a.ptr == b.target_monster) || all_allies))
                o.i64(Key('a', i, "tsel"), 1);
            // the native selector takes this ally: its card / party-bar tap is live (a Self or
            // DeadAlly skill lists only the one valid ally)
            if (player_turn && TargetTakes(b, a.ptr, true))
                o.i64(Key('a', i, "tok"), 1);
            // the ally target card (a skill on the own side): level, weak / resist, field slot;
            // only while one targets the own side (the cards show only then, b.tgta)
            if (!(player_turn && TargetsAllies(b)))
                continue;
            o.i64(Key('a', i, "lv"), a.level);
            // the card's slot, and its selector corners' (the module sways them: xl / xr)
            const int ax = aslot[static_cast<std::size_t>(i)] * TargetCardPx;
            o.i64(Key('a', i, "x"), ax);
            o.i64(Key('a', i, "xl"), ax);
            o.i64(Key('a', i, "xr"), ax);
            if (const auto line = WeakResistLine(true, a.weak); !line.empty())
                o.text(Key('a', i, "wk"), line);
            if (const auto line = WeakResistLine(false, a.resist); !line.empty())
                o.text(Key('a', i, "rs"), line);
        }
        if (b.caster >= 0 && b.caster < na) {
            const auto& c = b.allies[static_cast<std::size_t>(b.caster)];
            o.text("b.cname", c.name);
            o.text("b.cmptxt", std::to_string(c.mana) + "/" + std::to_string(c.mana_max));
        }
        if (player_turn) {
            // the command window's skill columns: the expanded one as rows (s<i>), the other one
            // as an icon strip (ss<i>) on its native side (sk.w2: two columns, s.ex / ss.x their
            // canvas x shifts)
            const auto lay = LayoutSkills(b);
            o.i64("b.skills", static_cast<std::int64_t>(b.skills.size()));
            // the rows and the strip only while they show: target select hides them and the
            // level popup covers them (the page reads nothing of them then; steward re-check R5)
            const bool skill_rows = !b.targeting && !b.level_open;
            // the focused skill has levels: its "[Y] Select Level" control (Y does exactly that);
            // 0/1, the window title's centred form reads !b.lvly
            const bool lvly = skill_rows && b.menu_open && b.level_hint && b.cursor >= 0 &&
                              b.cursor < static_cast<int>(b.skills.size());
            o.i64("b.lvly", lvly ? 1 : 0);
            if (skill_rows && lay.strip >= 0) {
                o.i64("sk.w2", 1);
                o.i64("s.ex", lay.strip_left ? SkillStripPx : 0);
                o.i64("ss.x", lay.strip_left ? 0 : SkillRowsPx - SkillStripPx);
            } else if (skill_rows) {
                o.i64("sk.w1", 1);
            }
            for (int row = 0; row < SkillRows && skill_rows; ++row) {
                const int i = SkillIndex(b, 0, row);
                if (i < 0)
                    break;
                const std::string k = "s" + std::to_string(row) + ".";
                PublishSkillRow(o, k, b.skills[static_cast<std::size_t>(i)], false);
                if (i == b.cursor)
                    o.i64(k + "sel", 1);
            }
            for (int row = 0; row < SkillRows && skill_rows && lay.strip >= 0; ++row) {
                const int i = SkillIndex(b, 1, row);
                if (i < 0)
                    break;
                const auto& sk = b.skills[static_cast<std::size_t>(i)];
                const std::string k = "ss" + std::to_string(row) + ".";
                o.i64(k + "v", 1);
                if (!sk.icon.empty())
                    o.text(k + "ico", "module:ms:spr4:" + sk.icon);
                if (!sk.badge.empty())
                    o.text(k + "badge", sk.badge);
                o.i64(k + "ok", sk.affordable ? 1 : 0); // 0/1: ss<i>.dim reads !ok
            }
            // the description strip: the hovered skill as the native SkillTooltip shows it
            // ("Slash Lvl 5", its description, the cost), else the row's own name and cost
            const Skill* focus = nullptr;
            if (b.level_open && b.level_cursor >= 0 && b.level_cursor < static_cast<int>(b.levels.size()))
                focus = &b.levels[static_cast<std::size_t>(b.level_cursor)];
            else if (b.cursor >= 0 && b.cursor < static_cast<int>(b.skills.size()))
                focus = &b.skills[static_cast<std::size_t>(b.cursor)];
            if (focus) {
                const auto& t = b.skill_tip;
                o.text("b.sname", t.visible && !t.name.empty() ? t.name : focus->name);
                o.i64("b.smp", focus->mana);
                if (t.visible && !t.description.empty() && !b.targeting)
                    o.text("b.sdesc", t.description);
            }
            // the level popup (Y on a skill with levels): its rows, the native cursor
            if (b.level_open) {
                o.i64("b.lvl", 1);
                const int nl = static_cast<int>(std::min<std::size_t>(b.levels.size(), LevelRows));
                for (int i = 0; i < nl; ++i) {
                    const std::string k = "lv" + std::to_string(i) + ".";
                    PublishSkillRow(o, k, b.levels[static_cast<std::size_t>(i)], false);
                    if (i == b.level_cursor)
                        o.i64(k + "sel", 1);
                }
            }
        }
        // the native command rail: only the commands the game offers this turn
        if (player_turn) {
            const auto rail = VisibleRail(b);
            if (b.rail_cursor >= 0 && !b.targeting && !sub) {
                o.i64("b.railfocus", 1);
                // the strip heading: the focused command's own name, as the native tooltip
                // titles it ("Attack" over its description)
                for (const auto& r : b.defaults)
                    if (r.row == b.rail_cursor && !r.name.empty()) {
                        o.text("b.rname", r.name);
                        break;
                    }
                if (!b.rail_description.empty())
                    o.text("b.rdesc", b.rail_description);
            }
            o.i64("b.rail", static_cast<std::int64_t>(rail.size()));
            const bool can_tap = b.menu_open && !b.targeting && !b.items_open && !b.switch_open;
            for (int i = 0; i < static_cast<int>(rail.size()); ++i) {
                const auto& r = b.defaults[static_cast<std::size_t>(rail[static_cast<std::size_t>(i)])];
                o.i64(Key('r', i, "v"), 1);
                o.text(Key('r', i, "name"), r.name);
                if (!r.icon.empty())
                    o.text(Key('r', i, "ico"), "module:ms:spr4:" + r.icon);
                if (rail[static_cast<std::size_t>(i)] == ActiveCommand(b))
                    o.i64(Key('r', i, "sel"), 1);
                if (can_tap)
                    o.i64(Key('r', i, "tap"), 1);
            }
        }
        if (player_turn)
            PublishSubmenus(b, o);
        const int ne = static_cast<int>(std::min<std::size_t>(b.enemies.size(), 3));
        o.i64("b.enemies", ne);
        const auto eslot = CardSlots(b, false);
        for (int i = 0; i < ne; ++i) {
            const auto& e = b.enemies[static_cast<std::size_t>(i)];
            // a defeated enemy stays in CombatController.Enemies (the result screen's score still
            // weighs it, live 2026-11-12: e0.hp=0, 3 listed, 2 on the field) but is gone from the
            // field and from the native selector's targets: its card is left empty, in place
            if (e.hp_max > 0 && e.hp <= 0)
                continue;
            // the slot holds a monster at all (an empty card -- a <3-enemy fight, a defeated
            // enemy -- is left empty)
            o.i64(Key('e', i, "v"), 1);
            // the native selector takes this enemy: its card tap is live (gen_manifest.py's
            // tgt.e<i> = b.tgt && !b.tgta && e<i>.tok)
            if (player_turn && TargetTakes(b, e.ptr, false))
                o.i64(Key('e', i, "tok"), 1);
            if (!(player_turn && !b.targeting) && !TargetsAllies(b))
                o.i64(Key('e', i, "show"), 1); // the enemy list gives way to the command list
            const int ex = eslot[static_cast<std::size_t>(i)] * TargetCardPx;
            o.i64(Key('e', i, "x"), ex);
            o.i64(Key('e', i, "xl"), ex);
            o.i64(Key('e', i, "xr"), ex);
            o.text(Key('e', i, "name"), e.name);
            if (!e.sprite.empty())
                o.text(Key('e', i, "spr"), "module:ms:box2:" + Frame(o, e.sprite, 30 + i));
            if (const auto wr = WeakResist(e.weak, e.resist); !wr.empty())
                o.text(Key('e', i, "wr"), wr);
            if (const auto line = WeakResistLine(true, e.weak); !line.empty())
                o.text(Key('e', i, "wk"), line);
            if (const auto line = WeakResistLine(false, e.resist); !line.empty())
                o.text(Key('e', i, "rs"), line);
            // the skill being targeted against what the card shows: weak / resist highlight
            if (b.targeting && b.target_element >= 0) {
                const auto has = [&](const std::vector<int>& els) {
                    return std::any_of(els.begin(), els.end(), [&](int el) {
                        return (el != 0 && el == b.target_element) || el == b.target_type;
                    });
                };
                if (has(e.weak))
                    o.i64(Key('e', i, "hitw"), 1);
                else if (has(e.resist))
                    o.i64(Key('e', i, "hitr"), 1);
            }
            o.i64(Key('e', i, "lv"), e.level);
            o.i64(Key('e', i, "hp"), e.hp);
            o.i64(Key('e', i, "hpmax"), e.hp_max);
            if (b.targeting && (i == b.target || all_enemies))
                o.i64(Key('e', i, "sel"), 1);
            // the Monster Info browse: the card shows this enemy (its chip carries the selector)
            if (player_turn && b.targeting && b.info.open && e.ptr && e.ptr == b.target_monster)
                o.i64(Key('e', i, "isel"), 1);
        }
        if (b.targeting && !b.info.open && b.target >= 0 && b.target < ne && !TargetsAllies(b)) {
            const auto& e = b.enemies[static_cast<std::size_t>(b.target)];
            o.text("b.tname", e.name);
            const auto weak = ElementList(e.weak), res = ElementList(e.resist);
            if (!weak.empty())
                o.text("b.tweak", weak);
            if (!res.empty())
                o.text("b.tres", res);
        }
        // a skill on the own side: the targeted ally in the strip, as for an enemy
        if (TargetsAllies(b) && b.target_monster)
            for (int i = 0; i < na; ++i) {
                const auto& a = b.allies[static_cast<std::size_t>(i)];
                if (a.ptr != b.target_monster)
                    continue;
                o.text("b.tname", a.name);
                const auto weak = ElementList(a.weak), res = ElementList(a.resist);
                if (!weak.empty())
                    o.text("b.tweak", weak);
                if (!res.empty())
                    o.text("b.tres", res);
            }
    }

    const MapInfo& mp = m.map;
    if (mp.valid && m.state != State::Loading) {
        o.i64("m.v", 1);
        o.text("m.scene", mp.scene);
        std::string area = AreaFromScene(mp.scene);
        for (const auto& r : mp.rooms)
            if (r.current && r.area >= 0 && static_cast<std::size_t>(r.area) < mp.area_names.size())
                area = mp.area_names[static_cast<std::size_t>(r.area)];
        o.text("m.area", area);
        // A sprite and its native ability name, only when an owned follower can clear an
        // obstacle in this visited current room. Prefer the current follower when it matches.
        const auto matches = [&](const PartyMonster& mon) {
            return !mon.sprite.empty() && !mon.ability.empty() &&
                std::any_of(mon.ability_types.begin(), mon.ability_types.end(), [&](int type) {
                    return std::find(mp.obstacles.begin(), mp.obstacles.end(), type) != mp.obstacles.end();
                });
        };
        const bool visited = std::any_of(mp.rooms.begin(), mp.rooms.end(),
            [](const Room& room) { return room.current && room.explored > 0; });
        if (visited && (m.state == State::Exploring || m.state == State::Menu)) {
            const PartyMonster* hint = matches(m.follower_mon) ? &m.follower_mon : nullptr;
            if (!hint)
                for (const auto& mon : m.party)
                    if (matches(mon)) {
                        hint = &mon;
                        break;
                    }
            if (hint) {
                o.i64("m.hint", 1);
                o.text("m.hint.sprite", "module:ms:row2:" + hint->sprite);
                o.text("m.hint.ability", hint->ability);
            }
        }
        // only the game's own figure is called "Total Completion" (MapMenu.areaInfos, filled
        // once the native map menu was opened this session)
        if (const int t = TotalCompletion10(mp); t >= 0) {
            o.i64("m.pct10", t);
            o.text("m.pct", std::to_string(t / 10) + "." + std::to_string(t % 10) + "%");
        }
        const std::string sig = std::to_string(MapSignature(mp));
        o.text("m.pic", "module:ms:map:L:" + sig);
        o.text("m.picw", "module:ms:map:W:" + sig);
        o.text("m.mini", "module:ms:map:S:" + sig);
    }
}

} // namespace ms
