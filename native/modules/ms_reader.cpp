// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ms_reader.h"

#include <map>
#include "ms_publish.h"
#include "ms_mapdraw.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace ms {

namespace {

// GameController (statics +0 = Instance)
constexpr u64 GcCombat = 0xC0;    // CombatController
constexpr u64 GcGameState = 0xC8; // GameStateManager
// GameStateManager / CombatController
constexpr u64 GsmState = 0x18; // StateMachine<GameStates>
constexpr u64 CcState = 0xB8;  // StateMachine<CombatStates>
// StateMachine<T> (T a value type): states 0x10, CurrentState 0x18; State<T>.ID 0x40
constexpr u64 SmCurrent = 0x18;
constexpr u64 StateId = 0x40;
// PlayerController (statics +0 = Instance)
constexpr u64 PcMinimap = 0x28, PcMonsters = 0x98, PcFollower = 0x108, PcGold = 0x110,
              PcName = 0x118, PcMenuStack = 0x1C0;
// MonsterManager
constexpr u64 MmActive = 0x20, MmInactive = 0x28;
// Monster
constexpr u64 MonName = 0x20, MonHp = 0x120, MonMana = 0x128, MonMount = 0x130,
              MonExplore = 0x138, MonSkills = 0x158, MonEquip = 0x160,
              MonLevel = 0x1B0, MonShift = 0x218, MonStats = 0x25C, MonDifficultyRating = 0x6C,
              MonBuffManager = 0x1D0;
constexpr u64 StatHealth = 0xC, StatMana = 0x10, StatDifficultyBonus = 0x88; // MonsterStats
// ExploreAbility
constexpr u64 AbName = 0x28, AbDesc = 0x30;
// SkillManager
constexpr u64 SkPassives = 0x78, SkPoints = 0x90;
// BaseSkill / BaseAction
constexpr u64 ActName = 0x20, ActIcon = 0x28, ActUltimate = 0x34, ActMana = 0x38,
              ActTarget = 0x3C;
// ActionDamage.Element
constexpr u64 DmgElement = 0x38;
// EquipmentManager.Equipment[] / BaseItem.Name
constexpr u64 EqArray = 0x18, ItemName = 0x20;
// CombatController
constexpr u64 CcEnemies = 0x48, CcBackupEnemies = 0x50, CcTargetSelector = 0x60,
              CcCurrent = 0x68, CcPlayerMonsters = 0x70, CcTurnCount = 0xD8,
              CcEncounter = 0xE0, CcPlayerTurn = 0x154;
// TargetSelector
constexpr u64 TsTarget = 0x18, TsSelecting = 0x50;
// MonsterEncounter.EncounterType
constexpr u64 EncType = 0x68;
// UIController (statics +0 = Instance) -> CombatUI -> Menu -> MenuList.CurrentIndex
constexpr u64 UiCombat = 0x20, UiDialogue = 0x50, CuiMenu = 0x18, CmMenuList = 0x40,
              MlCurrentIndex = 0x6C, MlLists = 0x58, MlCurrentListIndex = 0x70;
constexpr u64 MliText = 0x18, MliIcon = 0x20, MliInfo = 0x30, MliIconInfo = 0x38, MliDisplayable = 0x98,
              MliExpanded = 0xA1, MliSelectable = 0xD0, MliDisabled = 0xD1; // MenuListItem
// CombatMenu.LevelSelectButton (the "[Y] Select Level" hint GameObject)
constexpr u64 CmLevelButton = 0x48;
// CombatUIController.LevelSelect -> LevelSelectMenu .MenuList / .IsOpen
constexpr u64 CuiLevelSelect = 0x78, LsmMenuList = 0x28, LsmIsOpen = 0x38;
// SkillTooltip .Name / .Description / .SubTitle (tk2dTextMesh), isVisible
constexpr u64 StName = 0x28, StDescription = 0x30, StSubTitle = 0x38, StVisible = 0x84;
// Monster.CombatPos (Vector3): its place on the battlefield
constexpr u64 MonCombatPos = 0x140;
// TargetSelector.targetType (ETargetType)
constexpr u64 TsTargetType = 0x28;
// CombatMenuItem (same GameObject as the MenuListItem): ElementIcon, TypeIcon (tk2dSprite)
constexpr u64 CmiElementIcon = 0x20, CmiTypeIcon = 0x28;
constexpr u64 MlActiveListCount = 0x84; // MenuList
constexpr u64 DvIsOpen = 0x88; // DialogueView
// MinimapManager / MinimapEntry / MapData
constexpr u64 MmCurrentEntry = 0x18, MmEntries = 0x28, MeMapData = 0x10, MeExplored = 0x18,
              MdPosition = 0x10, MdSize = 0x34, MdScene = 0x40, MdTeleporter = 0x60, MdMapArea = 0x18;

constexpr std::size_t MaxRooms = 2048;

} // namespace

bool Reader::Read(u64 at, void* out, std::size_t n) const {
    ++reads;
    return host && host->read_memory && at >= 0x1000000 && at < (u64{1} << 48) &&
           host->read_memory(host->userdata, at, out, n) != EDEN_DSMOD_FALSE;
}
u8 Reader::U8(u64 at) const {
    u8 v{};
    return Read(at, &v, 1) ? v : 0;
}
s32 Reader::I32(u64 at, bool* ok) const {
    s32 v{};
    const bool r = Read(at, &v, 4);
    if (ok)
        *ok = r;
    return r ? v : 0;
}
u64 Reader::Ptr(u64 at) const {
    u64 v{};
    return Read(at, &v, 8) ? v : 0;
}
bool Reader::Ok(u64 p) {
    // an uninitialised TypeInfo slot holds a small metadata token, not a pointer
    return p >= 0x1000000 && p < (u64{1} << 48) && (p & 7) == 0;
}
std::string Reader::CStr(u64 at, std::size_t max) const {
    char buf[128];
    if (at < 0x1000000 || at >= (u64{1} << 48) || max > sizeof buf)
        return {};
    for (std::size_t off = 0; off < max; off += 16) {
        if (!Read(at + off, buf + off, 16))
            return {};
        if (const void* z = std::memchr(buf + off, 0, 16))
            return std::string(buf, static_cast<const char*>(z) - buf);
    }
    return {};
}
std::string Reader::String(u64 p) const {
    if (!Ok(p))
        return {};
    const s32 n = I32(p + 0x10);
    if (n <= 0 || n > 512)
        return {};
    std::vector<std::uint16_t> u(static_cast<std::size_t>(n));
    if (!Read(p + 0x14, u.data(), u.size() * 2))
        return {};
    std::string out;
    for (const auto c : u) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}
std::string Reader::ClassName(u64 obj) {
    if (!Ok(obj))
        return {};
    const u64 k = Ptr(obj);
    if (!Ok(k))
        return {};
    if (const auto it = class_names.find(k); it != class_names.end())
        return it->second;
    std::string n = CStr(Ptr(k + 0x10));
    if (!n.empty() && class_names.size() < 4096)
        class_names.emplace(k, n);
    return n;
}
std::vector<u64> Reader::ListItems(u64 list, std::size_t limit) const {
    std::vector<u64> out;
    if (!Ok(list))
        return out;
    const u64 items = Ptr(list + 0x10);
    const s32 n = I32(list + 0x18);
    if (n <= 0 || static_cast<std::size_t>(n) > limit || !Ok(items))
        return out;
    const u64 cap = Ptr(items + 0x18);
    if (cap < static_cast<u64>(n) || cap > 65536)
        return out;
    out.resize(static_cast<std::size_t>(n));
    if (!Read(items + 0x20, out.data(), out.size() * 8))
        out.clear();
    return out;
}
std::vector<u64> Reader::ArrayPtrs(u64 arr, std::size_t limit) const {
    std::vector<u64> out;
    if (!Ok(arr))
        return out;
    const u64 n = Ptr(arr + 0x18);
    if (n == 0 || n > limit)
        return out;
    out.resize(n);
    if (!Read(arr + 0x20, out.data(), n * 8))
        out.clear();
    return out;
}
bool Reader::ListInts(u64 list, std::size_t limit, std::vector<int>& out) const {
    if (!Ok(list))
        return false;
    bool ok{};
    const int count = I32(list + 0x18, &ok);
    const u64 items = Ptr(list + 0x10);
    if (!ok || count < 0 || static_cast<std::size_t>(count) > limit || !Ok(items) ||
        Ptr(items + 0x18) < static_cast<u64>(count))
        return false;
    out.resize(static_cast<std::size_t>(count));
    return count == 0 || Read(items + 0x20, out.data(), out.size() * sizeof(int));
}

u64 Reader::Statics(u64 slot, const char* name) {
    // the static-fields block of a resolved TypeInfo never moves for the life of the guest
    // process (same root-singleton guarantee the guest helper's own load_instance relies on):
    // skip the name/namespace re-verification on every later refresh once it has passed once.
    if (const auto it = statics_cache.find(slot); it != statics_cache.end())
        return it->second;
    const u64 k = Ptr(host->main_base + slot);
    if (!Ok(k) || CStr(Ptr(k + 0x10)) != name)
        return 0;
    const u64 ns = Ptr(k + 0x18);
    if (Ok(ns) && !CStr(ns).empty()) // the game's classes live in the global namespace
        return 0;
    const u64 sf = Ptr(k + 0xB8);
    if (!Ok(sf))
        return 0;
    if (statics_cache.size() < 16)
        statics_cache.emplace(slot, sf);
    return sf;
}
int Reader::MachineState(u64 machine, int max_id) {
    if (!Ok(machine))
        return -1;
    const u64 cur = Ptr(machine + SmCurrent);
    if (!Ok(cur))
        return -1;
    bool ok{};
    const s32 id = I32(cur + StateId, &ok);
    return ok && id >= 0 && id <= max_id ? id : -1;
}
std::vector<std::pair<std::string, u64>> Reader::Siblings(u64 component) {
    // managed component -> native component (+0x10) -> native GameObject (+0x30) ->
    // component array (+0x30, {type, component} pairs, count at +0x40) -> managed (+0x28)
    std::vector<std::pair<std::string, u64>> out;
    const u64 native = Ptr(component + 0x10);
    if (!Ok(native) || Ptr(native + 0x28) != component)
        return out;
    const u64 go = Ptr(native + 0x30);
    if (!Ok(go))
        return out;
    const u64 arr = Ptr(go + 0x30), n = Ptr(go + 0x40);
    if (!Ok(arr) || n == 0 || n > 64)
        return out;
    std::vector<u64> raw(n * 2);
    if (!Read(arr, raw.data(), raw.size() * 8))
        return out;
    for (u64 i = 0; i < n; ++i) {
        const u64 c = raw[i * 2 + 1];
        if (!Ok(c))
            continue;
        const u64 managed = Ptr(c + 0x28);
        if (Ok(managed) && Ptr(managed + 0x10) == c)
            out.emplace_back(ClassName(managed), managed);
    }
    return out;
}

bool Reader::ReadMonster(u64 mon, PartyMonster& out, bool party_fields) {
    out.ptr = mon;
    if (!Ok(mon))
        return false;
    // name and prefab: cached per monster object, revalidated by the name string pointer (a
    // rename or another monster at a reused address gives another string)
    const u64 name_ptr = Ptr(mon + MonName);
    MonCache scratch;
    MonCache* c = nullptr;
    if (use_cache)
        if (const auto it = mon_cache.find(mon); it != mon_cache.end() && it->second.name_ptr == name_ptr)
            c = &it->second;
    if (!c) {
        scratch.name_ptr = name_ptr;
        scratch.name = String(name_ptr);
        // the monster prefab's GameObject name ("Thanatos(Clone)") names its 2D Toolkit
        // collection; shifted monsters use the _light / _dark sprite set
        scratch.base = GameObjectName(mon);
        if (const auto p = scratch.base.find('('); p != std::string::npos)
            scratch.base.resize(p);
        while (!scratch.base.empty() && scratch.base.back() == ' ')
            scratch.base.pop_back();
        c = &scratch;
        if (use_cache && !scratch.name.empty() && mon_cache.size() < 512)
            c = &(mon_cache[mon] = std::move(scratch));
    }
    out.name = c->name;
    bool ok{};
    out.level = I32(mon + MonLevel, &ok);
    if (!ok || out.name.empty() || out.level < 1 || out.level > 200)
        return false;
    out.hp = I32(mon + MonHp);
    // the pair the native MonsterInfo card prints (UpdateMonsterInfo: CurrentMana "/" MaxMana;
    // get_MaxMana 0x385410 = currentStats.mana). The game does not clamp CurrentMana, so a
    // current above max (367/351 seen live) is what the native card shows too.
    out.mana = I32(mon + MonMana);
    out.hp_max = I32(mon + MonStats + StatHealth);
    out.mana_max = I32(mon + MonStats + StatMana);
    out.shift = I32(mon + MonShift);
    if (out.shift < 0 || out.shift > 2)
        out.shift = 0;
    if (!c->base.empty())
        out.sprite = c->base + (out.shift == 1 ? "_light" : out.shift == 2 ? "_dark" : "") + "/0";
    if (!party_fields)
        return true;
    const u64 sk = Ptr(mon + MonSkills);
    if (Ok(sk))
        out.skill_points = std::max(0, I32(sk + SkPoints));
    u64 ab = Ptr(mon + MonExplore);
    if (!Ok(ab))
        ab = Ptr(mon + MonMount);
    if (Ok(ab)) {
        if (!(use_cache && c->ability == ab)) {
            c->ability = ab;
            c->ability_name = String(Ptr(ab + AbName));
            c->ability_desc.clear();
            c->ability_types.clear();
            // the description as the game writes it: one point per line ("- ..."); drop tk2d
            // colour codes (^C<rrggbbaa> / ^c)
            const std::string desc = String(Ptr(ab + AbDesc));
            for (std::size_t i = 0; i < desc.size(); ++i) {
                if (desc[i] == '^' && i + 1 < desc.size() && (desc[i + 1] == 'C' || desc[i + 1] == 'c')) {
                    i += desc[i + 1] == 'C' ? 9 : 1;
                    continue;
                }
                if (desc[i] != '\r')
                    c->ability_desc.push_back(desc[i]);
            }
            const auto cls = ClassName(ab);
            const u64 types = cls == "AttackAbility" ? 0x70 : cls == "MountAbility" ? 0x88 : 0;
            if (types && !ListInts(Ptr(ab + types), 16, c->ability_types))
                c->ability_types.clear();
        }
        out.ability = c->ability_name;
        out.ability_desc = c->ability_desc;
        out.ability_types = c->ability_types;
    }
    const u64 eq = Ptr(mon + MonEquip);
    if (Ok(eq)) {
        const auto items = ArrayPtrs(Ptr(eq + EqArray), 6);
        for (std::size_t k = 0; k < items.size() && k < 6; ++k) {
            if (!Ok(items[k]))
                continue;
            if (use_cache)
                if (const auto it = gear_names.find(items[k]); it != gear_names.end()) {
                    out.gear[k].name = it->second;
                    continue;
                }
            // the GameObject name carries the upgrade level the menus show ("Katar+5");
            // BaseItem.Name is the base name and sometimes empty
            out.gear[k].name = GameObjectName(items[k]);
            if (out.gear[k].name.empty())
                out.gear[k].name = String(Ptr(items[k] + ItemName));
            if (use_cache && !out.gear[k].name.empty() && gear_names.size() < 1024)
                gear_names.emplace(items[k], out.gear[k].name);
        }
    }
    return true;
}

void Reader::Invalidate() {
    if (mon_cache.empty() && gear_names.empty() && !map_cache.valid)
        return;
    if (map_cache.valid) {
        prev_entries = map_cache.entries;
        prev_list = map_cache.list;
    }
    if constexpr (Diagnostics)
        cache_event = "cache dropped: " + std::to_string(mon_cache.size()) + " monsters, " +
                      std::to_string(gear_names.size()) + " items, " + std::to_string(map_cache.rooms.size()) + " rooms";
    mon_cache.clear();
    gear_names.clear();
    party_seen.clear();
    map_cache = {};
}

std::string Reader::TakeCacheEvent() {
    return std::exchange(cache_event, {});
}

void Reader::ReadBattle(u64 gc, u64 ui, Model& m) {
    Battle& b = m.battle;
    const u64 cc = Ptr(gc + GcCombat);
    if (ClassName(cc) != "CombatController")
        return;
    b.combat_state = MachineState(Ptr(cc + CcState), 10);
    b.player_turn = U8(cc + CcPlayerTurn) != 0;
    b.turn = I32(cc + CcTurnCount);
    const u64 enc = Ptr(cc + CcEncounter);
    if (Ok(enc))
        b.encounter_type = I32(enc + EncType); // (Retreat is read from the native rail itself)
    const u64 cur = Ptr(cc + CcCurrent);

    const auto allies = ListItems(Ptr(cc + CcPlayerMonsters), 8);
    for (const u64 a : allies) {
        PartyMonster pm;
        if (!ReadMonster(a, pm, false))
            continue;
        if (a == cur)
            b.caster = static_cast<int>(b.allies.size());
        Read(a + MonCombatPos, &pm.combat_x, 4);
        ElementMods(a, pm.weak, pm.resist);
        b.allies.push_back(std::move(pm));
    }
    const auto enemies = ListItems(Ptr(cc + CcEnemies), 8);
    const u64 ts = Ptr(cc + CcTargetSelector);
    const bool ts_ok = ClassName(ts) == "TargetSelector";
    b.targeting = ts_ok && U8(ts + TsSelecting) != 0;
    const u64 tgt = ts_ok ? Ptr(ts + TsTarget) : 0;
    for (const u64 e : enemies) {
        PartyMonster pm;
        if (!ReadMonster(e, pm, false))
            continue;
        Enemy en;
        en.name = pm.name;
        en.level = pm.level;
        en.hp = pm.hp;
        en.hp_max = pm.hp_max;
        en.sprite = pm.sprite;
        // innate weaknesses / resistances: PassiveElementModifier passives (Element, Modifier),
        // exactly what the native target card lists (MonsterInfo.UpdateMonsterInfo 0x3C94D0:
        // Modifier > 0 -> Weak, anything else -> Resist; no reveal condition; WeaknessBuff /
        // ResistanceBuff special buffs are not on the card)
        ElementMods(e, en.weak, en.resist);
        en.ptr = e;
        Read(e + MonCombatPos, &en.combat_x, 4);
        if (e == tgt)
            b.target = static_cast<int>(b.enemies.size());
        b.enemies.push_back(std::move(en));
    }
    b.backup_enemies = static_cast<int>(ListItems(Ptr(cc + CcBackupEnemies), 16).size());
    // Keeper battles: the encounter has a Trainer NPC (MonsterEncounter.Trainer +0x60)
    b.keeper = Ok(enc) && Ok(Ptr(enc + 0x60));

    // the acting monster's command list as the native combat menu shows it:
    // UIController.CombatUI.Menu.MenuList.CurrentList -> MenuListItem.Displayable (a BaseAction).
    // SkillManager.Actions holds only part of it (live 2026-11-03: 2 of 5 rows).
    int caster_mana = -1;
    if (b.caster >= 0)
        caster_mana = b.allies[static_cast<std::size_t>(b.caster)].mana;
    u64 ml = 0;
    if (Ok(ui)) {
        const u64 cui = Ptr(ui + UiCombat);
        const u64 menu = Ok(cui) ? Ptr(cui + CuiMenu) : 0;
        ml = Ok(menu) ? Ptr(menu + CmMenuList) : 0;
        if (ClassName(ml) != "MenuList")
            ml = 0;
    }
    if (ml) {
        // the native combat menu is three columns of up to five rows (CombatMenu.MENU_COLUMNS,
        // CombatMenu.ShowActions): column 0 the command rail (Attack, Items, Monster Info, Switch
        // Monster, Retreat / Forfeit), then the acting monster's skills in column 1, overflowing
        // into column 2 (the window's third column: a monster with more than five skills).
        // CurrentListIndex / CurrentIndex are the cursor's column / row.
        const int cursor_row = I32(ml + MlCurrentIndex);
        const auto lists = ListItems(Ptr(ml + MlLists), 16);
        const int column = I32(ml + MlCurrentListIndex);
        if (column == 0) {
            b.rail_cursor = cursor_row;
            const u64 cui = Ptr(ui + UiCombat);
            const u64 tooltip = Ok(cui) ? Ptr(cui + 0x40) : 0; // CombatUIController.Tooltip
            if (ClassName(tooltip) == "SkillTooltip" && U8(tooltip + StVisible))
                b.rail_description = TextMesh(Ptr(tooltip + StDescription));
        }
        b.menu_open = MenuOpen(ml);
        if (!lists.empty()) // column 0: Attack, Items, Monster Info, Switch Monster, Retreat/Forfeit
            for (auto& r : MenuRows(ml, 0, 16))
                b.defaults.push_back(std::move(r));
        if constexpr (Diagnostics) {
            menu_debug = " lists=" + std::to_string(lists.size()) + "/" + std::to_string(I32(ml + MlActiveListCount)) +
                         " col=" + std::to_string(column) + " rail=";
            for (const auto& r : b.defaults)
                menu_debug += std::to_string(r.row) + ":" + r.name + "[" + (r.active ? "a" : "-") +
                              (r.selectable ? "s" : "-") + (r.disabled ? "d" : "-") + "]" + r.icon + ",";
            menu_debug += " ";
        }
        for (std::size_t col = 1; col < std::min<std::size_t>(lists.size(), 3); ++col) {
            int row = -1;
            for (const u64 item : ListItems(lists[col], 32)) {
                ++row;
                Skill s;
                if (!SkillRow(item, static_cast<int>(col), row, caster_mana, s))
                    continue;
                if (b.skill_column < 0 && U8(item + MliExpanded) != 0)
                    b.skill_column = static_cast<int>(col);
                if (static_cast<int>(col) == column && row == cursor_row)
                    b.cursor = static_cast<int>(b.skills.size());
                b.skills.push_back(std::move(s));
            }
        }
        // the "[Y] Select Level" hint: shown for the hovered skill when it has several levels
        const u64 cui = Ok(ui) ? Ptr(ui + UiCombat) : 0;
        if (const u64 menu = Ok(cui) ? Ptr(cui + CuiMenu) : 0; Ok(menu))
            b.level_hint = b.cursor >= 0 && GameObjectActive(Ptr(menu + CmLevelButton));
    }
    // the SkillTooltip under the command window: the hovered skill (or level) as the game
    // describes it (CombatMenu.OnItemHovered / LevelSelectMenu.OnItemHovered -> SkillTooltip.Open)
    if (Ok(ui)) {
        const u64 cui = Ptr(ui + UiCombat);
        if (const u64 tt = Ok(cui) ? Ptr(cui + 0x40) : 0; ClassName(tt) == "SkillTooltip" && U8(tt + StVisible)) {
            auto& t = b.skill_tip;
            t.visible = true;
            t.name = TextMesh(Ptr(tt + StName));
            t.description = TextMesh(Ptr(tt + StDescription));
            t.subtitle = TextMesh(Ptr(tt + StSubTitle));
        }
        // the level popup (Y on a skill with several levels): its rows are CombatMenuItems like
        // the skill rows, one per level (LevelSelectMenu.Open: highest available first)
        const u64 ls = Ok(cui) ? Ptr(cui + CuiLevelSelect) : 0;
        const u64 lml = ClassName(ls) == "LevelSelectMenu" ? Ptr(ls + LsmMenuList) : 0;
        if (lml && U8(ls + LsmIsOpen) != 0 && ClassName(lml) == "MenuList") {
            b.level_open = true;
            const int lrow = I32(lml + MlCurrentIndex);
            const auto llists = ListItems(Ptr(lml + MlLists), 4);
            int row = -1;
            for (const u64 item : llists.empty() ? std::vector<u64>{} : ListItems(llists[0], 16)) {
                ++row;
                Skill s;
                if (!Active(item) || !SkillRow(item, 0, row, caster_mana, s))
                    continue;
                if (row == lrow)
                    b.level_cursor = static_cast<int>(b.levels.size());
                b.levels.push_back(std::move(s));
            }
        }
    }
    // the item menu and the switch-monster list, when the native menu opened them
    if (Ok(ui)) {
        const u64 cui = Ptr(ui + UiCombat);
        const u64 im = Ok(cui) ? Ptr(cui + 0x70) : 0;          // CombatItemMenu
        const u64 iml = Ok(im) ? Ptr(im + 0x28) : 0;
        if (ClassName(iml) == "MenuList" && MenuOpen(iml)) {
            b.items_open = true;
            // the native window lists every column (the cursor's one expanded, the others as
            // icons); the item pool keeps parked rows (inactive GameObjects, e.g. a stale
            // "Equipment" row after Small Potion) that the game never draws: ShownRows drops them
            const auto cols = ListItems(Ptr(iml + MlLists), 8);
            for (std::size_t c = 0; c < cols.size(); ++c)
                for (auto& r : ShownRows(MenuRows(iml, static_cast<int>(c), 16)))
                    b.items.push_back(std::move(r));
            b.item_column = I32(iml + MlCurrentListIndex);
            b.item_row = I32(iml + MlCurrentIndex);
            // the ItemTooltip under the menu: the highlighted item as the game describes it
            if (const u64 itt = Ptr(cui + 0x80); ClassName(itt) == "ItemTooltip") {
                auto& t = b.item_tip;
                t.visible = U8(itt + 0x8C) != 0;
                t.name = TextMesh(Ptr(itt + 0x38));
                t.description = TextMesh(Ptr(itt + 0x40));
                t.type = TextMesh(Ptr(itt + 0x60));
                t.icon = RowIcon(Ptr(itt + 0x20));
            }
        }
        const u64 sm = Ok(cui) ? Ptr(cui + 0x90) : 0;          // StartCombatMenu
        if (ClassName(sm) == "StartCombatMenu") {
            // IsBackupSelect +0x70, IsSwitchSelect +0x71, IsOpen +0x72 (auto-property fields)
            b.start_backup = U8(sm + 0x70) != 0;
            b.start_switch = U8(sm + 0x71) != 0;
            b.start_open = U8(sm + 0x72) != 0;
        }
        const u64 sml = Ok(sm) ? Ptr(sm + 0x20) : 0;           // .MonsterMenu
        if (ClassName(sml) == "MenuList" && MenuOpen(sml)) {
            b.switch_open = true;
            b.switch_column = I32(sml + MlCurrentListIndex);
            b.switch_row = I32(sml + MlCurrentIndex);
            b.switches = ShownRows(MenuRows(sml, b.switch_column, 16));
            // the rows only carry the level; the name is on the MonsterStartCombatSummary whose
            // MenuItem is that row (StartCombatMenu.Monsters, Name tk2dTextMesh +0x38)
            for (const u64 sum : ListItems(Ptr(sm + 0x40), 16)) {
                const u64 item = Ptr(sum + 0x58);
                for (auto& r : b.switches)
                    if (r.item == item) {
                        if (const u64 mon = Ptr(sum + 0xA0); ClassName(mon) == "Monster")
                            r.monster = mon; // the summary's Monster (the row's Displayable is not one)
                        const auto nm = TextMesh(Ptr(sum + 0x38));
                        if (!nm.empty()) {
                            r.info = r.name;
                            r.name = nm;
                        }
                    }
            }
            for (auto& r : b.switches) {
                PartyMonster pm;
                if (Ok(r.monster) && ReadMonster(r.monster, pm, false)) {
                    r.sprite = pm.sprite;
                    ElementMods(r.monster, r.weak, r.resist);
                }
            }
        }
    }
    if (ts_ok) {
        b.targets = ListItems(Ptr(ts + 0x40), 8);
        b.target_kind = I32(ts + TsTargetType);
    }
    // the native MonsterInfo card: shown while targeting and for the Monster Info command
    // (TargetSelector in IsEnemyInfo mode); its texts as rendered, the monster's sprite and its
    // innate weaknesses / resistances (the same rule as the enemy cards)
    b.target_monster = ts_ok ? Ptr(ts + TsTarget) : 0;
    if (Ok(ui)) {
        const u64 cui = Ptr(ui + UiCombat);
        if (const u64 mi = Ok(cui) ? Ptr(cui + 0x50) : 0; ClassName(mi) == "MonsterInfo") {
            auto& c = b.info;
            c.open = b.targeting && U8(ts + 0x51) != 0;
            c.visible = Active(mi);
            c.headline = TextMesh(Ptr(mi + 0x30));
            c.health = TextMesh(Ptr(mi + 0x38));
            c.mana = TextMesh(Ptr(mi + 0x40));
            c.crit = TextMesh(Ptr(mi + 0x48));
            c.crit_damage = TextMesh(Ptr(mi + 0x50));
            if (const u64 mon = Ok(b.target_monster) ? b.target_monster : Ptr(mi + 0x80); Ok(mon)) {
                PartyMonster pm;
                if (ReadMonster(mon, pm, false))
                    c.sprite = pm.sprite;
                ElementMods(mon, c.weak, c.resist);
            }
        }
    }
    // the action being targeted (TargetSelector.Action +0x48): ActionDamage Element +0x38,
    // Type +0x3C (0 physical, else magical), as the native row's icons show them
    if (const u64 act = b.targeting && ts_ok ? Ptr(ts + 0x48) : 0; Ok(act)) {
        for (const auto& [cls, comp] : Siblings(act)) {
            if (cls != "ActionDamage")
                continue;
            const s32 el = I32(comp + DmgElement);
            b.target_element = el >= 0 && el <= 9 ? el : -1;
            b.target_type = I32(comp + 0x3C) == 0 ? 5 : 6;
            break;
        }
    }
    // CombatResult (CombatController+0x98), the inputs of the result screen's score
    if (const u64 cr = Ptr(cc + 0x98); ClassName(cr) == "CombatResult") {
        auto& sc = b.score;
        sc.turns = I32(cr + 0x14);
        sc.acted = I32(cr + 0x18);
        Read(cr + 0x24, &sc.health, 4);
        Read(cr + 0x34, &sc.pre, 4);
        Read(cr + 0x38, &sc.exec, 4);
        Read(cr + 0x40, &sc.weakness, 4);
        sc.combo = I32(cr + 0x44);
        sc.levels = I32(cr + 0x64);
        sc.actions = I32(cr + 0x68);
        Read(cr + 0x50, &sc.dot, 4);
        // StarsGained (+0x2C) is 0 from StartCombat until CalculateResult runs for the result
        // screen, which also writes Pre+Exec back into +0x38: the live figures end there, and the
        // result screen's own figures are in the object from then on
        const int stars = I32(cr + 0x2C);
        sc.valid = stars == 0 && sc.turns >= 0 && sc.turns < 1000 && sc.health >= 0 && sc.health <= 1.01f && sc.pre >= 0 &&
                   sc.exec >= 0 && sc.weakness >= 0 && sc.combo >= 0 && sc.actions >= 0 && sc.dot >= 0 && sc.dot < 1e6f;
        if (stars >= 1 && stars <= 6 && sc.turns >= 0 && sc.turns < 1000) {
            sc.final = true;
            sc.final_difficulty = I32(cr + 0x10);
            sc.final_buffs = I32(cr + 0x48);
            Read(cr + 0x3C, &sc.final_exec, 4);
            if (!(sc.health >= 0 && sc.health <= 1.01f && sc.final_exec >= 0 && sc.final_exec < 1000))
                sc.final = false;
        }
        // the other CalculateResult inputs (Enemy Difficulty, Buffs / Debuffs): the encounter's
        // champion flag, each enemy's rating / shift / bonus / debuffs, each ally's buffs
        sc.champion = Ok(enc) && (I32(enc + EncType) | 2) == 3; // MonsterEncounter.get_IsChampion
        const u64 bms = Statics(SlotBuffManager, "BuffManager");
        const int buff_types = bms ? I32(bms) : 0, debuff_types = bms ? I32(bms + 4) : 0;
        // BuffManager.GetTotal{Buff,Debuff}Strength: sum over the type counts of
        // GetBuffStrength(i) = stacks >= 1 ? (stacks - 1) * 0.5 + 1 : 0 (CurrentBuffs +0x60 /
        // CurrentDebuffs +0x68, int[])
        const auto strength = [&](u64 mon, u64 field, int types) {
            const u64 bm = Ok(mon) ? Ptr(mon + MonBuffManager) : 0;
            const u64 arr = Ok(bm) ? Ptr(bm + field) : 0;
            if (!Ok(arr) || types <= 0 || types > 64)
                return 0.0f;
            const u64 n = std::min<u64>(Ptr(arr + 0x18), static_cast<u64>(types));
            int stacks[64]{};
            if (n == 0 || !Read(arr + 0x20, stacks, n * 4))
                return 0.0f;
            float total = 0;
            for (u64 i = 0; i < n; ++i)
                total += stacks[i] >= 1 ? static_cast<float>(stacks[i] - 1) * 0.5f + 1.0f + 0.0f : 0.0f;
            return total;
        };
        for (const auto& e : b.enemies) {
            Battle::Score::Foe f;
            f.level = e.level;
            f.rating = I32(e.ptr + MonDifficultyRating);
            f.shifted = I32(e.ptr + MonShift) != 0;
            Read(e.ptr + MonStats + StatDifficultyBonus, &f.bonus, 4);
            f.debuffs = strength(e.ptr, 0x68, debuff_types);
            sc.foes.push_back(f);
        }
        for (const auto& a : b.allies)
            sc.ally_buffs.push_back(strength(a.ptr, 0x60, buff_types));
    }
    // the shown selection's SelectionView: MenuList+0x78 (command menu) or TargetSelector+0x20;
    // dTimeAcc +0x60 resets on every selection change, isLocked +0x64
    const u64 sv = b.targeting ? (ts_ok ? Ptr(ts + 0x20) : 0) : (ml ? Ptr(ml + 0x78) : 0);
    if (ClassName(sv) == "SelectionView") {
        float t{};
        if (Read(sv + 0x60, &t, 4) && t >= 0 && t < 1e6f)
            b.sel_time = t;
        b.sel_locked = U8(sv + 0x64) != 0;
    }
    // the combo strip as the game renders it (CombatUIController.ComboView.Hits / .Damage)
    if (Ok(ui)) {
        const u64 cui = Ptr(ui + UiCombat);
        const u64 cv = Ok(cui) ? Ptr(cui + 0x48) : 0;
        if (ClassName(cv) == "ComboView") {
            // Native labels can omit the colon (Keeper preview: "Damage 150%").
            b.combo = CombatFigure(TextMesh(Ptr(cv + 0x18)));
            b.damage = CombatFigure(TextMesh(Ptr(cv + 0x20)));
        }
    }
    b.valid = !b.enemies.empty() || !b.allies.empty();
}

void Reader::ScanRooms(u64 mm, u64 list) {
    auto& mc = map_cache;
    mc = {};
    mc.mm = mm;
    mc.list = list;
    std::unordered_map<u64, int> area_index;
    for (const u64 e : ListItems(list, MaxRooms)) {
        const u64 md = Ptr(e + MeMapData);
        if (!Ok(md))
            continue;
        Room r;
        float v[2]{};
        if (Read(md + MdPosition, v, 8)) {
            r.x = v[0];
            r.y = v[1];
        }
        if (Read(md + MdSize, v, 8)) {
            r.w = v[0];
            r.h = v[1];
        }
        r.scene = String(Ptr(md + MdScene));
        // MapData.MapArea (GameObject) -> its MapArea component: Name, MinimapColor
        const u64 area_go = Ptr(md + MdMapArea);
        if (Ok(area_go)) {
            auto it = area_index.find(area_go);
            if (it == area_index.end()) {
                int idx = -1;
                for (const auto& [cls, comp] : GameObjectComponents(area_go)) {
                    if (cls != "MapArea")
                        continue;
                    float col[4]{};
                    Read(comp + 0x28, col, sizeof col);
                    const auto c8 = [](float f) {
                        return static_cast<std::uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
                    };
                    float off[2]{};
                    Read(comp + 0x18, off, sizeof off); // MapArea.MinimapOffset
                    mc.area_offsets.emplace_back(off[0], off[1]);
                    idx = static_cast<int>(mc.area_names.size());
                    mc.area_names.push_back(String(Ptr(comp + 0x20)));
                    mc.area_colors.push_back(c8(col[0]) << 16 | c8(col[1]) << 8 | c8(col[2]));
                    break;
                }
                it = area_index.emplace(area_go, idx).first;
            }
            r.area = it->second;
            if (r.area >= 0) {
                r.x += mc.area_offsets[static_cast<std::size_t>(r.area)].first;
                r.y += mc.area_offsets[static_cast<std::size_t>(r.area)].second;
            }
        }
        r.teleporter = U8(md + MdTeleporter) != 0;
        if (r.teleporter && Read(md + 0x64, v, 8)) {
            r.teleporter_x = v[0];
            r.teleporter_y = v[1];
        }
        // Authored icons are immutable room metadata: read only in the existing cached pass.
        for (const u64 icon : ListItems(Ptr(md + 0x20), 128)) {
            MapIcon pin;
            pin.sprite = String(Ptr(icon + 0x10));
            if (pin.sprite.starts_with("MapIcon") && Read(icon + 0x18, v, 8) &&
                std::isfinite(v[0]) && std::isfinite(v[1])) {
                pin.x = v[0];
                pin.y = v[1];
                pin.label = String(Ptr(icon + 0x20)); // DisplayName (the native map's tile text)
                r.icons.push_back(std::move(pin));
            }
        }
        mc.entries.push_back(e);
        mc.rooms.push_back(std::move(r));
    }
    for (std::size_t i = 0; i < mc.rooms.size(); ++i)
        ReadRoomState(mc.entries[i], mc.rooms[i]);
    mc.count = I32(list + 0x18);
    mc.valid = !mc.rooms.empty();
}

void Reader::ReadRoomState(u64 entry, Room& r) {
    // One bounded read per array; no scan of other rooms on a refresh.
    r.revealed.clear();
    r.markers.clear();
    r.cells = r.explored = 0;
    const u64 ex = Ptr(entry + MeExplored);
    const u64 n = Ok(ex) ? Ptr(ex + 0x18) : 0;
    if (!n || n > 4096)
        return;
    r.revealed.resize(n);
    if (!Read(ex + 0x20, r.revealed.data(), n)) {
        r.revealed.clear();
        return;
    }
    r.cells = static_cast<int>(n);
    r.explored = static_cast<int>(std::count_if(r.revealed.begin(), r.revealed.end(), [](u8 c) { return c != 0; }));
    const u64 markers = Ptr(entry + 0x30);
    if (Ok(markers) && Ptr(markers + 0x18) == n) {
        r.markers.resize(n);
        if (!Read(markers + 0x20, r.markers.data(), n * sizeof(int)))
            r.markers.clear();
    }
}

void Reader::ReadMap(u64 pc, u64 ui, Model& m) {
    struct ObstacleRef {
        std::string_view scene;
        int id, type;
    };
    static constexpr ObstacleRef obstacles[] = {
#include "ms_obstacle_catalog.inc"
    };
    MapInfo& mi = m.map;
    const u64 mm = Ptr(pc + PcMinimap);
    if (ClassName(mm) != "MinimapManager")
        return;
    const u64 current = Ptr(mm + MmCurrentEntry);
    const u64 list = Ptr(mm + MmEntries);
    auto& mc = map_cache;
    // a full scan when nothing is cached, the cache is off, or the entry list itself changed
    // (another MinimapManager / list object, or entries added or removed)
    const s32 count = Ok(list) ? I32(list + 0x18) : -1;
    if (!use_cache || !mc.valid || mc.mm != mm || mc.list != list || count != mc.count) {
        const bool had = mc.valid;
        const std::size_t old_rooms = mc.rooms.size();
        ScanRooms(mm, list);
        if (Diagnostics && use_cache) {
            // room identity across the invalidation (scene load): same list object, same entries
            std::size_t same = 0;
            for (std::size_t i = 0; i < mc.entries.size() && i < prev_entries.size(); ++i)
                same += mc.entries[i] == prev_entries[i];
            cache_event = std::string{"map rooms "} + (had ? "re" : "") + "scanned: " + std::to_string(mc.rooms.size()) +
                          (had ? " (was " + std::to_string(old_rooms) + ")" : "") + " list=" +
                          (prev_list ? (prev_list == list ? "same" : "new") : "first") + " entries same as before=" +
                          std::to_string(same) + "/" + std::to_string(prev_entries.size());
            prev_entries = mc.entries;
            prev_list = list;
        }
    }
    // only the room the player is in changes between scans: re-read its explored cells
    std::size_t cur = mc.entries.size();
    for (std::size_t i = 0; i < mc.entries.size(); ++i)
        if (mc.entries[i] == current) {
            cur = i;
            break;
        }
    if (cur < mc.entries.size())
        ReadRoomState(current, mc.rooms[cur]);
    // Native markers can be edited in a remote explored room. Follow the map selection,
    // refreshing only that entry (also during name entry/deletion), never rescanning the map.
    const u64 menu = Ok(ui) ? Ptr(ui + 0x28) : 0;
    const u64 map = Ok(menu) ? Ptr(menu + 0x48) : 0;
    if (Ok(map) && U8(map + 0xD8)) {
        const u64 selection = Ptr(map + 0x100);
        const u64 tile = Ok(selection) ? Ptr(selection + 0x48) : 0;
        if (ClassName(tile) == "MinimapTileView") {
            const u64 selected = Ptr(tile + 0x40);
            for (std::size_t i = 0; i < mc.entries.size(); ++i)
                if (mc.entries[i] == selected && i != cur) {
                    ReadRoomState(selected, mc.rooms[i]);
                    break;
                }
        }
    }
    mi.area_names = mc.area_names;
    mi.area_colors = mc.area_colors;
    mi.rooms = mc.rooms;
    int icons = 0, hidden_icons = 0, custom = 0, crystals = 0;
    // every native map symbol kind: shown (an explored cell) / hidden, for the inventory log
    std::map<std::string, std::pair<int, int>> kinds;
    for (std::size_t i = 0; i < mi.rooms.size(); ++i) {
        auto& r = mi.rooms[i];
        r.current = i == cur;
        mi.total_cells += r.cells;
        mi.explored_cells += r.explored;
        if constexpr (Diagnostics) { // the map-symbol inventory (reader log)
            const auto revealed = [&](float x, float y) {
                if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= r.w || y >= r.h)
                    return false;
                const auto index = static_cast<std::size_t>(x) + static_cast<std::size_t>(y) * static_cast<int>(r.w);
                return index < r.revealed.size() && r.revealed[index] != 0;
            };
            for (const auto& icon : r.icons) {
                const bool shown = revealed(icon.x, icon.y);
                shown ? ++icons : ++hidden_icons;
                ++(shown ? kinds[icon.sprite].first : kinds[icon.sprite].second);
            }
            for (std::size_t k = 0; k < r.markers.size() && k < r.revealed.size(); ++k)
                if (r.markers[k] > 0 && r.markers[k] <= 16) {
                    custom += r.revealed[k] != 0;
                    auto& kind = kinds["MapMarker" + std::to_string(r.markers[k])];
                    ++(r.revealed[k] ? kind.first : kind.second);
                }
            if (r.teleporter) {
                const bool shown = revealed(r.teleporter_x, r.teleporter_y);
                crystals += shown;
                ++(shown ? kinds["Teleporter"].first : kinds["Teleporter"].second);
            }
        }
        if (!r.current)
            continue;
        mi.scene = r.scene;
        // Scene metadata is gated by a room the player has actually entered, then by
        // the native persistent activation IDs (destroyed obstacles must not linger).
        obstacle_scratch.clear(); // reused across refreshes: no per-tick heap allocation
        if (r.explored > 0 && ListInts(Ptr(current + 0x20), 4096, obstacle_scratch)) {
            for (const auto& obstacle : obstacles) {
                if (obstacle.scene == r.scene &&
                    std::find(obstacle_scratch.begin(), obstacle_scratch.end(), obstacle.id) ==
                        obstacle_scratch.end() &&
                    std::find(mi.obstacles.begin(), mi.obstacles.end(), obstacle.type) == mi.obstacles.end())
                    mi.obstacles.push_back(obstacle.type);
            }
        }
    }
    const auto& area_offsets = mc.area_offsets;
    // the player's map cell (MinimapManager.PlayerPosition), checked against the current room:
    // used only when it lies inside that room, raw or with the area's minimap offset
    float pp[2]{};
    if (Read(mm + 0x20, pp, sizeof pp)) {
        for (const auto& r : mi.rooms) {
            if (!r.current)
                continue;
            float ox = 0, oy = 0;
            if (r.area >= 0 && static_cast<std::size_t>(r.area) < area_offsets.size()) {
                ox = area_offsets[static_cast<std::size_t>(r.area)].first;
                oy = area_offsets[static_cast<std::size_t>(r.area)].second;
            }
            // absolute (with / without the area offset), else a cell inside the room (live
            // 2026-11-09: pp=1,0 in a 2x1 room at -12,4)
            for (const int mode : {0, 1, 2}) {
                const float x = mode == 2 ? r.x + pp[0] + 0.5f : pp[0] + (mode == 0 ? ox : 0);
                const float y = mode == 2 ? r.y + pp[1] + 0.5f : pp[1] + (mode == 0 ? oy : 0);
                if (x >= r.x - 0.01f && x <= r.x + r.w + 0.01f && y >= r.y - 0.01f && y <= r.y + r.h + 0.01f) {
                    mi.player = true;
                    mi.player_x = x;
                    mi.player_y = y;
                    break;
                }
            }
        }
        if constexpr (Diagnostics) {
            map_debug = " pp=" + std::to_string(pp[0]) + "," + std::to_string(pp[1]) + (mi.player ? " in" : " out");
            for (const auto& r : mi.rooms)
                if (r.current)
                    map_debug += " room=" + std::to_string(r.x) + "," + std::to_string(r.y) + "+" + std::to_string(r.w) +
                                 "x" + std::to_string(r.h);
        }
    }
    if constexpr (Diagnostics) {
        map_debug += " icons=" + std::to_string(icons) + " hidden=" + std::to_string(hidden_icons) +
                     " custom=" + std::to_string(custom) + " crystals=" + std::to_string(crystals) + " kinds=";
        for (const auto& [kind, n] : kinds)
            map_debug += kind + ":" + std::to_string(n.first) + "/" + std::to_string(n.second) + ",";
    }
    mi.valid = !mi.rooms.empty();
}

void Reader::ReadMapProgress(u64 ui, MapInfo& mi) {
    // UIController.IngameMenu.Map.areaInfos: List<AreaExplorationProgress> (16-byte structs)
    const u64 menu = Ok(ui) ? Ptr(ui + 0x28) : 0;
    const u64 map = Ok(menu) ? Ptr(menu + 0x48) : 0;
    if (ClassName(map) != "MapMenu")
        return;
    // MapMenu.AreaPercentText (tk2dTextMesh._text): "Total Completion - 76.9%" as the game
    // rendered it; the last number in it is the game's own figure
    const u64 tm = Ptr(map + 0x98);
    if (ClassName(tm) == "tk2dTextMesh") {
        std::string t = String(Ptr(tm + 0x30));
        if (t.find('%') == std::string::npos)
            t = String(Ptr(tm + 0x20));
        mi.completion_text = t;
        const auto pct = t.rfind('%');
        if (pct != std::string::npos) {
            std::size_t b = pct;
            while (b > 0 && (std::isdigit(static_cast<unsigned char>(t[b - 1])) || t[b - 1] == '.' || t[b - 1] == ','))
                --b;
            if (b < pct) {
                std::string num = t.substr(b, pct - b);
                std::replace(num.begin(), num.end(), ',', '.');
                mi.completion10 = static_cast<int>(std::lround(std::atof(num.c_str()) * 10.0));
            }
        }
    }
    const u64 list = Ptr(map + 0xC8);
    if (!Ok(list))
        return;
    const u64 items = Ptr(list + 0x10);
    const s32 n = I32(list + 0x18);
    if (!Ok(items) || n <= 0 || n > 64)
        return;
    struct Raw {
        u64 name;
        float explored, unexplored;
    };
    std::vector<Raw> raw(static_cast<std::size_t>(n));
    if (!Read(items + 0x20, raw.data(), raw.size() * sizeof(Raw)))
        return;
    for (const auto& r : raw) {
        if (!(r.explored >= 0 && r.unexplored >= 0 && r.explored + r.unexplored < 1e6))
            return;
        mi.progress.push_back({String(r.name), r.explored, r.unexplored});
    }
}

bool Reader::MenuOpen(u64 ml) {
    return Ok(ml) && U8(ml + 0x69) != 0 && U8(ml + 0x68) != 0;
}

void Reader::ElementMods(u64 mon, std::vector<int>& weak, std::vector<int>& resist) {
    const u64 sk = Ptr(mon + MonSkills);
    for (const u64 ps : ListItems(Ok(sk) ? Ptr(sk + SkPassives) : 0, 64)) {
        if (ClassName(ps) != "PassiveElementModifier")
            continue;
        const s32 el = I32(ps + 0x3C);
        float mod{};
        Read(ps + 0x40, &mod, 4);
        if (el < 0 || el > 9)
            continue;
        (mod > 0 ? weak : resist).push_back(el);
    }
}

bool Reader::SkillRow(u64 item, int column, int row, int caster_mana, Skill& s) {
    const u64 a = Ptr(item + MliDisplayable);
    if (!Ok(a))
        return false; // a padding row (CombatMenu.ShowActions pads each column with null rows)
    s.column = column;
    s.row = row;
    s.item = item;
    // the row's own rendered label (MenuListItem.text, a tk2dTextMesh) is what the player reads;
    // BaseSkill.Name is empty on most rows and the GameObject name is internal ("GloryMight1"
    // for Riot, live 2026-11-03)
    s.name = TextMesh(Ptr(item + MliText));
    if (s.name.empty())
        s.name = String(Ptr(a + ActName));
    if (s.name.empty())
        return false;
    // the icon the row itself shows: MenuListItem.icon (tk2dSprite) -> its collection's
    // spriteDefinitions[_spriteId].name, prefixed with the collection name as in pictures.idx
    // ("icons/icon_slash"); BaseAction.Icon is set on few actions
    s.icon = RowIcon(Ptr(item + MliIcon));
    if (s.icon.empty())
        if (const auto ic = String(Ptr(a + ActIcon)); !ic.empty())
            s.icon = "icons/" + ic;
    // the small number on the icon (MenuListItem.iconInfo: the action's level)
    if (Active(Ptr(item + MliIconInfo)))
        s.badge = TextMesh(Ptr(item + MliIconInfo));
    // the cost the native row shows (MenuListItem.infoText): the game's effective cost with
    // Channel and other cost modifiers applied; BaseAction.Mana is the base cost
    s.base_mana = I32(a + ActMana);
    s.cost_text = TextMesh(Ptr(item + MliInfo));
    const int shown = LastNumber(s.cost_text);
    s.mana = shown >= 0 ? shown : s.base_mana;
    s.target = I32(a + ActTarget);
    s.ultimate = U8(a + ActUltimate) != 0;
    s.affordable = caster_mana >= 0 && s.mana <= caster_mana;
    s.disabled = U8(item + MliDisabled) != 0;
    if (s.disabled)
        s.affordable = false;
    // the icons the native row draws after the name (type, then element)
    for (const auto& [cls, comp] : Siblings(item)) {
        if (cls != "CombatMenuItem")
            continue;
        for (const u64 at : {CmiTypeIcon, CmiElementIcon})
            if (auto ic = ShownIcon(Ptr(comp + at)); !ic.empty())
                s.inline_icons.push_back(std::move(ic));
        break;
    }
    if (Diagnostics && menu_debug.size() < 900) {
        menu_debug += std::to_string(column) + "." + std::to_string(row) + ":" + s.name + "{";
        for (const auto& ic : s.inline_icons)
            menu_debug += ic + ";";
        menu_debug += "}" + std::to_string(s.base_mana) + ">" + s.cost_text + "#" + s.badge + " ";
    }
    for (const auto& [cls, comp] : Siblings(a)) {
        if (cls == "ActionDamage") {
            const s32 el = I32(comp + DmgElement);
            if (el >= 0 && el <= 9)
                s.element = el;
            break;
        }
    }
    return true;
}

std::vector<MenuRow> Reader::MenuRows(u64 ml, int column, std::size_t limit) {
    std::vector<MenuRow> out;
    const auto lists = ListItems(Ptr(ml + MlLists), 16);
    if (column < 0 || static_cast<std::size_t>(column) >= lists.size())
        return out;
    int row = -1;
    for (const u64 item : ListItems(lists[static_cast<std::size_t>(column)], limit)) {
        ++row;
        MenuRow r;
        r.name = TextMesh(Ptr(item + MliText));
        // monster rows (switch list) label the level; the Displayable is the Monster itself
        const u64 disp = Ptr(item + MliDisplayable);
        if (ClassName(disp) == "Monster") {
            r.monster = disp;
            const std::string mn = String(Ptr(disp + MonName));
            if (!mn.empty()) {
                r.info = r.name;
                r.name = mn;
            }
        }
        if (r.name.empty())
            continue;
        r.icon = RowIcon(Ptr(item + MliIcon));
        if (r.info.empty())
            r.info = TextMesh(Ptr(item + 0x30)); // MenuListItem.infoText
        r.disabled = U8(item + MliDisabled) != 0;
        r.selectable = U8(item + MliSelectable) != 0;
        r.active = Active(item);
        r.column = column;
        r.row = row;
        r.item = item;
        out.push_back(std::move(r));
    }
    return out;
}

void Reader::ReadTeleports(u64 ui, u64 pc, MapInfo& mi) {
    const u64 menu = Ok(ui) ? Ptr(ui + 0x28) : 0;
    const u64 map = Ok(menu) ? Ptr(menu + 0x48) : 0;
    if (ClassName(map) != "MapMenu")
        return;
    mi.teleport_menu = U8(map + 0xD8) != 0 && I32(map + 0xF8) == 2;
    // "here" = the teleporter in the player's current room (StartTeleport is only set while the
    // native menu is open). `pc` is the PlayerController Refresh() already resolved.
    const u64 mm = Ok(pc) ? Ptr(pc + PcMinimap) : 0;
    const u64 here = Ok(mm) ? Ptr(mm + MmCurrentEntry) : 0;
    for (const u64 t : ListItems(Ptr(map + 0xE0), 32)) {
        Teleporter tp;
        tp.ptr = t;
        tp.start = Ok(here) && Ptr(t + 0x20) == here;
        // TeleportSelectable.MinimapEntry -> MapData.SceneName -> the room's area name
        const u64 me = Ptr(t + 0x20);
        const u64 md = Ok(me) ? Ptr(me + MeMapData) : 0;
        const std::string scene = Ok(md) ? String(Ptr(md + MdScene)) : std::string{};
        tp.scene = scene;
        for (const auto& r : mi.rooms)
            if (r.scene == scene && r.area >= 0 && static_cast<std::size_t>(r.area) < mi.area_names.size())
                tp.name = mi.area_names[static_cast<std::size_t>(r.area)];
        if (tp.name.empty())
            tp.name = AreaFromScene(scene);
        // the native map's own name of a place in that room (MapIconData.DisplayName, e.g. the
        // House icon "Monster Farm"): tells two crystals of one area apart without inventing one
        for (const auto& r : mi.rooms)
            if (r.scene == scene)
                for (const auto& icon : r.icons)
                    if (tp.label.empty() && !icon.label.empty())
                        tp.label = icon.label;
        if constexpr (Diagnostics)
            tp_debug += " " + scene + (tp.start ? "*" : "");
        mi.teleports.push_back(std::move(tp));
    }
}

std::string Reader::RowIcon(u64 sprite) {
    if (!Ok(sprite))
        return {};
    u64 col = Ptr(sprite + 0x28); // collectionInst
    if (ClassName(col) != "tk2dSpriteCollectionData")
        col = Ptr(sprite + 0x20);
    if (ClassName(col) != "tk2dSpriteCollectionData")
        return {};
    const s32 id = I32(sprite + 0x4C);
    const auto defs = ArrayPtrs(Ptr(col + 0x20), 8192);
    if (id < 0 || static_cast<std::size_t>(id) >= defs.size() || !Ok(defs[static_cast<std::size_t>(id)]))
        return {};
    const std::string name = String(Ptr(defs[static_cast<std::size_t>(id)] + 0x20));
    std::string cname = String(Ptr(col + 0x80));
    for (auto& ch : cname)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (name.empty() || cname.empty())
        return {};
    return cname + "/" + name;
}

bool Reader::Active(u64 component) {
    const u64 nat = Ptr(component + 0x10);
    if (!Ok(nat) || Ptr(nat + 0x28) != component)
        return false;
    const u64 go = Ptr(nat + 0x30);
    if (!Ok(go))
        return false;
    const u8 cached = U8(go + 0x57);
    return cached <= 1 ? cached != 0 : U8(go + 0x56) != 0;
}

bool Reader::GameObjectActive(u64 go) {
    // a managed GameObject: its native object (+0x10) points back at it (+0x28) and carries the
    // same flags as a component's GameObject (m_IsActiveCached +0x57, else m_IsActive +0x56)
    const u64 nat = Ok(go) ? Ptr(go + 0x10) : 0;
    if (!Ok(nat) || Ptr(nat + 0x28) != go)
        return false;
    const u8 cached = U8(nat + 0x57);
    return cached <= 1 ? cached != 0 : U8(nat + 0x56) != 0;
}

std::string Reader::ShownIcon(u64 sprite) {
    if (!Ok(sprite) || !Active(sprite))
        return {};
    float alpha{};
    if (!Read(sprite + 0x3C, &alpha, 4) || !(alpha > 0.01f)) // tk2dBaseSprite._color.a
        return {};
    return RowIcon(sprite);
}

std::string Reader::TextMesh(u64 tm) {
    if (ClassName(tm) != "tk2dTextMesh")
        return {};
    std::string t = String(Ptr(tm + 0x30)); // _text
    if (t.empty())
        t = String(Ptr(tm + 0x20)); // _formattedText
    // drop tk2d inline colour codes (^C<rrggbbaa> / ^c) and newlines
    std::string out;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '^' && i + 1 < t.size() && (t[i + 1] == 'C' || t[i + 1] == 'c')) {
            i += t[i + 1] == 'C' ? 9 : 1;
            continue;
        }
        out.push_back(t[i] == '\n' ? ' ' : t[i]);
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

std::string Reader::GameObjectName(u64 component) {
    const u64 nat = Ptr(component + 0x10);
    if (!Ok(nat) || Ptr(nat + 0x28) != component)
        return {};
    const u64 go = Ptr(nat + 0x30);
    std::string n = Ok(go) ? CStr(Ptr(go + 0x60), 64) : std::string{};
    for (const char c : n)
        if (static_cast<unsigned char>(c) < 0x20)
            return {};
    return n;
}

std::vector<std::pair<std::string, u64>> Reader::GameObjectComponents(u64 go_managed) {
    std::vector<std::pair<std::string, u64>> out;
    if (ClassName(go_managed) != "GameObject")
        return out;
    const u64 go = Ptr(go_managed + 0x10);
    if (!Ok(go))
        return out;
    const u64 arr = Ptr(go + 0x30), n = Ptr(go + 0x40);
    if (!Ok(arr) || n == 0 || n > 64)
        return out;
    std::vector<u64> raw(n * 2);
    if (!Read(arr, raw.data(), raw.size() * 8))
        return out;
    for (u64 i = 0; i < n; ++i) {
        const u64 c = raw[i * 2 + 1];
        if (!Ok(c))
            continue;
        const u64 managed = Ptr(c + 0x28);
        if (Ok(managed) && Ptr(managed + 0x10) == c)
            out.emplace_back(ClassName(managed), managed);
    }
    return out;
}

std::string Reader::AnimDebug(u64 mon) {
    // Monster.Animator (+0x1C0, tk2dSpriteAnimator): currentClip +0x28 (name +0x10, fps +0x20),
    // clipFps +0x34: the clip the game plays now and its rate (diagnostic, native timing check)
    const u64 an = Ok(mon) ? Ptr(mon + 0x1C0) : 0;
    if (ClassName(an) != "tk2dSpriteAnimator")
        return "-";
    const u64 clip = Ptr(an + 0x28);
    float fps{}, clip_fps{};
    Read(clip + 0x20, &fps, 4);
    Read(an + 0x34, &clip_fps, 4);
    return String(Ptr(clip + 0x10)) + "@" + std::to_string(fps) + "/" + std::to_string(clip_fps);
}

std::string Reader::FollowerWatch(const EdenDsmodHostApi& h) {
    host = &h;
    const u64 pcs = Statics(SlotPlayerController, "PlayerController");
    const u64 pc = pcs ? Ptr(pcs) : 0;
    const u64 pf = Ok(pc) ? Ptr(pc + PcFollower) : 0;
    if (!Ok(pf))
        return {};
    const auto name = [&](u64 mon) { return Ok(mon) ? String(Ptr(mon + MonName)) : std::string{"-"}; };
    return name(Ptr(pf + 0x40)) + ">" + name(Ptr(pf + 0x58)) + "#" + std::to_string(I32(pf + 0x50));
}

Model Reader::Refresh(const EdenDsmodHostApi& h) {
    host = &h;
    const std::uint64_t reads_at = reads;
    Model m = RefreshModel();
    last_reads = reads - reads_at;
    return m;
}

Model Reader::RefreshModel() {
    Model m;
    debug.clear();
    menu_debug.clear();
    enemy_debug.clear();
    tp_debug.clear();
    map_debug.clear();
    const u64 gcs = Statics(SlotGameController, "GameController");
    const u64 gc = gcs ? Ptr(gcs) : 0;
    if (ClassName(gc) != "GameController") {
        if constexpr (!Diagnostics)
            return m;
        // diagnostics: what the slot holds (a metadata token before the class is initialised)
        const u64 k = Ptr(host->main_base + SlotGameController);
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "gc=0 base=%llx slot=%llx q0=%llx q1=%llx q2=%llx q3=%llx name=%s gcs=%llx inst=%s",
                      static_cast<unsigned long long>(host->main_base),
                      static_cast<unsigned long long>(k), static_cast<unsigned long long>(Ptr(k)),
                      static_cast<unsigned long long>(Ptr(k + 8)),
                      static_cast<unsigned long long>(Ptr(k + 16)),
                      static_cast<unsigned long long>(Ptr(k + 24)), CStr(Ptr(k + 0x10)).c_str(),
                      static_cast<unsigned long long>(gcs), ClassName(gc).c_str());
        debug = buf;
        return m;
    }
    const u64 gsm = Ptr(gc + GcGameState);
    m.game_state = ClassName(gsm) == "GameStateManager" ? MachineState(Ptr(gsm + GsmState), 12) : -1;
    m.state = StateFromGame(m.game_state);

    const u64 uis = Statics(SlotUIController, "UIController");
    const u64 ui = uis ? Ptr(uis) : 0;
    if (ClassName(ui) == "UIController") {
        const u64 dv = Ptr(ui + UiDialogue);
        m.dialogue = Ok(dv) && U8(dv + DvIsOpen) != 0;
    }
    // the title screen: the version text it shows and the MainMenu stage. The MainMenu has no
    // static instance; the OptionsMenu and the NewGameMenu (both UIController children) are
    // handed it by the game (OptionsMenu.MainMenu +0xA8, NewGameMenu.MainMenu +0xC0)
    if (m.state == State::Title && ClassName(ui) == "UIController") {
        m.version = TextMesh(Ptr(ui + 0x18));
        u64 mm = 0;
        for (const auto [field, at] : {std::pair<u64, u64>{0xD0, 0xA8}, {0x108, 0xC0}}) {
            const u64 owner = Ptr(ui + field);
            if (Ok(owner) && ClassName(Ptr(owner + at)) == "MainMenu") {
                mm = Ptr(owner + at);
                break;
            }
        }
        // the game's open-menu stack (MenuList.MenuStack, static +0): empty while the title waits
        // for the press, the Main Menu / save slots / options once it was pressed, empty again
        // once a save was picked (the fade to black before the scene load, still GameStates 0)
        s32 open_menus = -1;
        if (const u64 mls = Statics(SlotMenuList, "MenuList")) {
            const u64 stack = Ptr(mls);
            const s32 n = Ok(stack) ? I32(stack + 0x18) : -1;
            open_menus = n >= 0 && n < 64 ? n : -1;
        }
        if (mm) {
            const s32 st = I32(mm + 0xC8);
            m.title_stage = st >= 0 && st <= 5 ? st : -1;
            const u64 sgm = Ptr(mm + 0x58);
            m.save_menu = ClassName(sgm) == "SaveGameMenu" && U8(sgm + 0x4A) != 0;
        } else if (open_menus >= 0) {
            // at the "Press [A] to start" stage neither holder has the MainMenu yet (live
            // 2026-11-12), so the stack tells the stages apart
            m.title_stage = open_menus == 0 ? 0 : 5;
        }
        // the game's own prompt object: shown only once its intro has put it up (the helper finds
        // it with GameObject.Find, which returns active objects only; re-checked live here)
        if (title_prompt_known) {
            m.title_prompt = 0;
            if (ClassName(title_prompt_go) == "GameObject") {
                const u64 go = Ptr(title_prompt_go + 0x10);
                const u8 cached = Ok(go) ? U8(go + 0x57) : 0;
                const bool active = Ok(go) && (cached <= 1 ? cached != 0 : U8(go + 0x56) != 0);
                m.title_prompt = active ? 1 : 0;
                if constexpr (Diagnostics)
                    debug += " prompt=" + std::to_string(active) + ":";
                for (const auto& [cls, comp] : GameObjectComponents(title_prompt_go)) {
                    if constexpr (Diagnostics) {
                        debug += cls + ",";
                        if (cls == "UIOpeningAnim")
                            debug += "open" + std::to_string(U8(comp + 0x44)) + ",";
                    }
                    if (cls == "FlashRenderer") {
                        const auto mask = I32(comp + 0x24);
                        if (mask > 0 && mask <= 1024 && (mask & (mask - 1)) == 0) {
                            m.title_flash_counter = I32(comp + 0x20);
                            m.title_flash_mask = mask;
                        }
                    }
                }
            }
        }
        if (open_menus > 0)
            title_menus_seen = true;
        else if (open_menus == 0 && title_menus_seen) {
            // the menus were open and are all closed again: a save (or a new game) is loading
            m.state = State::Loading;
            m.title_stage = -1;
        }
    } else if (m.state != State::Title) {
        title_menus_seen = false;
    }
    // NPC talk runs as GameStates 11 (Cinematic) with the DialogueView open (live 2026-11-08);
    // a cutscene without the dialogue view stays Cinematic
    if ((m.state == State::Exploring || m.state == State::Cinematic) && m.dialogue)
        m.state = State::Dialogue;

    const u64 pcs = Statics(SlotPlayerController, "PlayerController");
    const u64 pc = pcs ? Ptr(pcs) : 0;
    const bool have_pc = ClassName(pc) == "PlayerController";
    // the per-save caches end with a scene load, the title screen or another PlayerController
    // (a save switch)
    if (m.state == State::Title || m.state == State::Unknown || m.state == State::Loading || !have_pc ||
        pc != cache_pc) {
        Invalidate();
        cache_pc = have_pc ? pc : 0;
    }
    if (have_pc && m.state != State::Title && m.state != State::Unknown &&
        m.state != State::Loading) {
        m.gold = I32(pc + PcGold);
        m.player = String(Ptr(pc + PcName));
        m.menu_open = !ListItems(Ptr(pc + PcMenuStack), 64).empty();
        const u64 mons = Ptr(pc + PcMonsters);
        if (ClassName(mons) == "MonsterManager") {
            const u64 follower = Ptr(pc + PcFollower);
            const u64 fmon = Ok(follower) ? Ptr(follower + 0x40) : 0;
            const auto active = ListItems(Ptr(mons + MmActive), 12);
            m.battle.score.active = static_cast<int>(active.size());
            if (active != party_seen) { // a party change: monsters added, removed or reordered
                if (Diagnostics && !party_seen.empty() && !mon_cache.empty())
                    cache_event = "party changed: monster cache dropped (" + std::to_string(mon_cache.size()) + ")";
                mon_cache.clear();
                party_seen = active;
            }
            for (const u64 mon : active) {
                PartyMonster pm;
                if (!ReadMonster(mon, pm, true))
                    continue;
                pm.active = true;
                if (mon == fmon)
                    m.follower = static_cast<int>(m.party.size());
                m.party.push_back(std::move(pm));
            }
            const u64 inactive = Ptr(mons + MmInactive);
            if (Ok(inactive)) {
                const s32 nb = I32(inactive + 0x18);
                if (nb >= 0 && nb < 4096)
                    m.bench = nb;
            }
            if (Ok(fmon) && !ReadMonster(fmon, m.follower_mon, true))
                m.follower_mon = {};
        }
        // InventoryManager.Weapons / .Accessories (InventoryItem: Item +0x10, Quantity +0x18): only
        // the Team page's equipment picker shows them, so only while it is open
        const u64 inv = want_owner_gear ? Ptr(pc + 0x20) : 0;
        if (inv && ClassName(inv) == "InventoryManager") {
            for (int k = 0; k < 2; ++k) {
                auto& out = k == 0 ? m.weapons : m.accessories;
                for (const u64 ii : ListItems(Ptr(inv + (k == 0 ? 0x38 : 0x40)), 96)) {
                    const u64 item = Ptr(ii + 0x10);
                    if (!Ok(item))
                        continue;
                    InvEquip e;
                    e.ptr = ii;
                    e.item = item;
                    e.quantity = I32(ii + 0x18);
                    if (const auto it = use_cache ? gear_names.find(item) : gear_names.end(); it != gear_names.end()) {
                        e.name = it->second;
                    } else {
                        e.name = GameObjectName(item);
                        if (e.name.empty())
                            e.name = String(Ptr(item + ItemName));
                        if (use_cache && !e.name.empty() && gear_names.size() < 1024)
                            gear_names.emplace(item, e.name);
                    }
                    if (!e.name.empty() && e.quantity > 0)
                        out.push_back(std::move(e));
                }
            }
        }
        // which owned monsters hold each spare item: equipped slots point at the same shared item
        // objects as InventoryItem.Item (EquipmentManager.Equip 0x4780E0), over the team, the bench
        // and the farm (MonsterManager +0x20 / +0x28 / +0x30)
        if (want_owner_gear && (!m.weapons.empty() || !m.accessories.empty()) && ClassName(mons) == "MonsterManager") {
            std::unordered_map<u64, std::vector<InvEquip::Owner>> owners;
            for (const u64 list : {MmActive, MmInactive, u64{0x30}}) {
                for (const u64 mon : ListItems(Ptr(mons + list), 512)) {
                    const u64 eq = Ok(mon) ? Ptr(mon + MonEquip) : 0;
                    if (!Ok(eq))
                        continue;
                    PartyMonster pm;
                    for (const u64 it : ArrayPtrs(Ptr(eq + EqArray), 6)) {
                        if (!Ok(it))
                            continue;
                        if (pm.name.empty() && !ReadMonster(mon, pm, false))
                            break;
                        auto& v = owners[it];
                        if (std::none_of(v.begin(), v.end(), [&](const auto& o) { return o.monster == mon; }))
                            v.push_back({mon, pm.name, pm.sprite});
                    }
                }
            }
            for (auto* list : {&m.weapons, &m.accessories})
                for (auto& e : *list)
                    if (const auto it = owners.find(e.item); it != owners.end())
                        e.equipped_on = it->second;
        }
        if (m.state != State::Loading) {
            ReadMap(pc, ui, m);
            ReadMapProgress(ui, m.map);
            ReadTeleports(ui, pc, m.map);
        }
    }
    if (m.state == State::Combat) {
        const int active = m.battle.score.active;
        ReadBattle(gc, ui, m);
        m.battle.score.active = active;
        if (LeadPick(m.battle))
            m.state = State::LeadPick;
    }

    if constexpr (!Diagnostics)
        return m;
    const std::string prompt_debug = debug;
    debug = "gs=" + std::to_string(m.game_state) + " st=" + std::to_string(static_cast<int>(m.state)) +
            (m.state == State::Title ? " title=" + std::to_string(m.title_stage) + "/" + std::to_string(m.save_menu) +
                                           " tp=" + std::to_string(m.title_prompt) + prompt_debug +
                                           " ver=" + m.version : std::string{}) +
            " pc=" + std::to_string(have_pc) + " party=" + std::to_string(m.party.size()) +
            " fol=" + std::to_string(m.follower) + " gold=" + std::to_string(m.gold) + " dlg=" + std::to_string(m.dialogue) +
            " menu=" + std::to_string(m.menu_open) + " rooms=" + std::to_string(m.map.rooms.size()) +
            " scene=" + m.map.scene +  " areas=" + std::to_string(m.map.area_names.size()) + "/" + std::to_string(m.map.progress.size()) + (m.map.area_names.empty() ? std::string{} : " a0=" + m.map.area_names[0] + "#" + std::to_string(m.map.area_colors[0])) + " total10=" + std::to_string(TotalCompletion10(m.map)) + " text=" + m.map.completion_text + " cells=" + std::to_string(m.map.explored_cells) + "/" + std::to_string(m.map.total_cells) + " follower=" + m.follower_mon.name + "[" + AnimDebug(m.follower_mon.ptr) + "]" + " bench=" + std::to_string(m.bench) +
            " obstacles=" + std::to_string(m.map.obstacles.size()) + " tpmenu=" + std::to_string(m.map.teleport_menu) + " tps=" + std::to_string(m.map.teleports.size()) + tp_debug + map_debug;
    if (m.state == State::Combat || m.state == State::LeadPick) {
        const auto& b = m.battle;
        debug += " smenu=" + std::to_string(b.start_open) + std::to_string(b.start_backup) + std::to_string(b.start_switch);
        debug += " cs=" + std::to_string(b.combat_state) + " turn=" + std::to_string(b.player_turn) +
                 " tgt=" + std::to_string(b.targeting) + " cur=" + std::to_string(b.cursor) +
                 " caster=" + std::to_string(b.caster) + " sk=" + std::to_string(b.skills.size()) +
                 " en=" + std::to_string(b.enemies.size()) + " keeper=" + std::to_string(b.keeper) + " enc=" + std::to_string(b.encounter_type);
        debug += " mopen=" + std::to_string(b.menu_open) + " dflt=" + std::to_string(b.defaults.size()) +
                 " items=" + std::to_string(b.items_open) + "/" + std::to_string(b.items.size()) +
                 " sw=" + std::to_string(b.switch_open) + "/" + std::to_string(b.switches.size()) +
                 " tgts=" + std::to_string(b.targets.size()) + " selt=" + std::to_string(b.sel_time) +
                 (b.sel_locked ? "L" : "");
        for (const auto& r : b.defaults)
            debug += " d" + std::to_string(r.column) + "." + std::to_string(r.row) + "=" + r.name;
        for (const auto& r : b.items)
            debug += " i" + std::to_string(r.column) + "." + std::to_string(r.row) + "=" + r.name + "(" + r.info + ")" +
                     (r.disabled ? "!" : "");
        if (b.items_open)
            debug += " icur=" + std::to_string(b.item_column) + "." + std::to_string(b.item_row) + " tip=" +
                     std::to_string(b.item_tip.visible) + ":" + b.item_tip.name + "|" + b.item_tip.type + "|" + b.item_tip.icon;
        if (b.info.visible)
            debug += " info=" + std::to_string(b.info.open) + ":" + b.info.headline + "|" + b.info.health + "|" +
                     b.info.mana + "|" + b.info.crit + "|" + b.info.crit_damage + "|" + b.info.sprite;
        for (const auto& r : b.switches)
            debug += " w" + std::to_string(r.row) + "=" + r.name + (r.disabled ? "!" : "");
        debug += menu_debug + enemy_debug + " combo=" + b.combo + "/" + b.damage;
        // W5: the Combat Result factors as projected now (final=1: the result screen's own)
        const auto& sc = b.score;
        debug += " w5=" + std::to_string(sc.valid) + std::to_string(sc.final) + " L=" + std::to_string(ScoreLevel(sc)) +
                 " ed=" + std::to_string(EnemyDifficultyPoints(sc)) + " bp=" + std::to_string(BuffPoints(sc)) +
                 " ex=" + std::to_string(ExecutionPercent(sc)) + " hp=" + std::to_string(HealthinessPercent(sc)) +
                 " dot=" + std::to_string(sc.dot) + " act=" + std::to_string(sc.active) + " ch=" + std::to_string(sc.champion);
        for (const auto& f : sc.foes)
            debug += " f" + std::to_string(f.level) + "/" + std::to_string(f.rating) + (f.shifted ? "s" : "") + "/" +
                     std::to_string(f.bonus) + "/" + std::to_string(f.debuffs);
        for (const float a : sc.ally_buffs)
            debug += " ab" + std::to_string(a);
        if (!b.enemies.empty())
            debug += " e0spr=" + b.enemies[0].sprite + "[" + AnimDebug(b.enemies[0].ptr) + "]";
        if (!b.skills.empty())
            debug += " s0icon=" + b.skills[0].icon + " s0=" + b.skills[0].name + "/" + std::to_string(b.skills[0].mana) + "/el" +
                     std::to_string(b.skills[0].element);
    }
    return m;
}

} // namespace ms
