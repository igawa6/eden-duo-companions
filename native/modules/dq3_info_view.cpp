// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Party, Bag and Journal tab values (sources and rules: dq3_info_view.h, dq3_info.h).

#include "dq3_info_view.h"

#include <algorithm>
#include <array>
#include <initializer_list>

#include "dq3_battle_art.h"
#include "dq3_page_layout.h"

namespace dq3 {

namespace {

using s64 = std::int64_t;
constexpr u32 Gold = 0xFFECD292, GreyName = 0xFFA89A80, Blue = 0xFF78D4F4, GreyMp = 0xFFA8B0BC;

/// `text` in the first style that fits `width` (measured with the game font), else the last style.
std::string Fitted(const FontAtlas* font, std::u32string_view text, std::initializer_list<FontStyle> styles,
                   int width) {
    std::string out;
    for (const FontStyle st : styles) {
        out = Styled(text, st);
        if (!font || font->Measure(out) <= width)
            break;
    }
    return out;
}

const std::u32string* MenuText(const InfoData& info, std::string_view id) {
    return info.Text(id);
}

/// GOP_Item ParamType -> the native stat name (GOP_Text_FieldMenu STATUS_PARAMNAME_xx): OFFENSE Attack,
/// DEFENSE Defence, PHYSICAL Stamina, INTELLIGENCE Wisdom, AGILITY Agility, LUC Luck, MAXHP / MAXMP.
const char* ParamNameId(u8 type) {
    static constexpr const char* ids[9] = {nullptr,
                                           "Txt_FieldMenu_STATUS_PARAMNAME_03",
                                           "Txt_FieldMenu_STATUS_PARAMNAME_04",
                                           "Txt_FieldMenu_STATUS_PARAMNAME_06",
                                           "Txt_FieldMenu_STATUS_PARAMNAME_07",
                                           "Txt_FieldMenu_STATUS_PARAMNAME_05",
                                           "Txt_FieldMenu_STATUS_PARAMNAME_08",
                                           "Txt_FieldMenu_STATUS_PARAMNAME_17",
                                           "Txt_FieldMenu_STATUS_PARAMNAME_18"};
    return type < 9 ? ids[type] : nullptr;
}

/// The item's stat effect line ("Attack +206", "Agility +10%"): the row's ParamType with its ADD / RATIO
/// value as the stat getters sum it (ItemParam; a Fighter's Attack uses ParamFighterDown, 0x87f24c).
/// Empty for NONE / SPECIAL or a zero effect.
std::u32string ParamLine(const InfoData& info, const OwnedItem& it, std::optional<u8> vocation) {
    const auto row = info.items.find(it.id);
    if (row == info.items.end())
        return {};
    const ItemRow& r = row->second;
    const char* id = ParamNameId(r.param_type);
    const auto* name = id ? MenuText(info, id) : nullptr;
    if (!name || (r.param_up != 1 && r.param_up != 2))
        return {};
    s32 v = ItemParam(info, it, r.param_type, r.param_up);
    if (r.param_type == 1 && vocation && *vocation == 3 && r.fighter_down != 0)
        v = r.fighter_down;
    if (v == 0)
        return {};
    std::u32string out = *name + (v > 0 ? U" +" : U" ") + AsciiToU32(std::to_string(v));
    if (r.param_up == 2)
        out += U"%";
    return out;
}

/// The selection's detail (dq3.dt.*): icon, name, right-aligned sub, two fact lines, description.
struct Detail {
    std::optional<int> item_icon, spell_icon;
    bool spell_grey{};
    std::u32string name, sub, f0, text;
    void Publish(InfoView& v) const {
        auto& I = v.ints;
        auto& T = v.texts;
        if (name.empty() && text.empty())
            return;
        I["dq3.dt.on"] = 1;
        if (item_icon) {
            I["dq3.dt.ion"] = 1;
            I["dq3.dt.ii"] = *item_icon + 1;
        }
        if (spell_icon) {
            I[spell_grey ? "dq3.dt.zon" : "dq3.dt.son"] = 1;
            I["dq3.dt.si"] = *spell_icon + 1;
        }
        T["dq3.dt.name"] = Styled(name, FontStyle::Bold32);
        // optional lines carry their own gate (a bound label without text would draw a placeholder)
        const auto line = [&](const char* key, const std::u32string& t, FontStyle st, bool wrap) {
            if (t.empty())
                return;
            T[key] = wrap ? StyledWrap(t, st) : Styled(t, st);
            I[std::string{key} + ".on"] = 1;
        };
        line("dq3.dt.sub", sub, FontStyle::Semi24, false);
        line("dq3.dt.f0", f0, FontStyle::Value, false);
        line("dq3.dt.text", text, FontStyle::Value, true);
    }
};

/// The Party pane's "where cast" line for a skill entry's use timing (INVENTORY.md section 5): companion words, the
/// game shows only the battle-only refusal message (Txt_FieldMenu_Message_BattleMagic).
std::u32string SpellTimingLabel(u8 timing) {
    switch (timing) {
    case 1:
        return U"Field and battle";
    case 2:
        return U"Battle only";
    case 3:
        return U"Field only";
    default:
        return {};
    }
}

std::u32string ItemName(const InfoData& info, const std::string& id) {
    const auto row = info.items.find(id);
    const std::u32string* n = row == info.items.end() ? nullptr : info.Noun(row->second.name_text);
    return n ? *n : AsciiToU32(id);
}
int ItemIcon(const InfoData& info, const std::string& id) {
    const auto row = info.items.find(id);
    return row == info.items.end() ? 7 : row->second.icon; // the star for an unknown row
}
std::u32string ItemFlavour(const InfoData& info, const std::string& id) {
    const auto row = info.items.find(id);
    if (row != info.items.end())
        if (const auto* f = info.Text(row->second.flavor_text))
            return CleanGameText(*f);
    return {};
}

/// Greedy word wrap at ASCII spaces like the host's label (advances summed with the atlas metrics): the
/// number of lines `text` takes in `width`, or 0 when one word alone is wider.
int WrapLines(const FontAtlas& font, std::u32string_view text, FontStyle st, int width) {
    int lines = 0;
    std::u32string cur;
    std::size_t i = 0;
    while (i <= text.size()) {
        const std::size_t sp = std::min(text.find(U' ', i), text.size());
        const std::u32string word{text.substr(i, sp - i)};
        if (font.Measure(Styled(word, st)) > width)
            return 0;
        const std::u32string joined = cur.empty() ? word : cur + U" " + word;
        if (!cur.empty() && font.Measure(StyledWrap(joined, st)) > width) {
            ++lines;
            cur = word;
        } else {
            cur = joined;
        }
        i = sp + 1;
    }
    return lines + (cur.empty() ? 0 : 1);
}

/// A grid cell's name (render.py party_page): one line 13 px lower (qgy), or wrapped onto two lines, in the
/// first of `styles` that fits the cell's text room.
void CellName(InfoView& v, const FontAtlas* font, const std::string& idx, std::u32string_view name,
              std::initializer_list<FontStyle> styles, u32 colour) {
    constexpr int room = PartyCellW - CellTextPad;
    FontStyle st = *styles.begin();
    int lines = 1;
    if (font) {
        for (const FontStyle s : styles) {
            st = s;
            lines = WrapLines(*font, name, s, room);
            if (lines == 1 || lines == 2)
                break;
        }
        if (lines < 1 || lines > 2)
            lines = 2;
    }
    const std::string text = lines == 1 ? Styled(name, st) : StyledWrap(name, st);
    v.texts["qgn" + idx] = colour ? Colour(colour, text) : text;
    if (lines == 1)
        v.ints["qgy" + idx] = 13;
}

/// Party grid rows qg<field><row> (n, i, x) for `list`. Icon ints are published + 1 (the manifest's
/// src_thresholds: 0 / missing = no image).
int ItemRows(InfoView& v, const InfoData& info, const FontAtlas* font, const std::vector<const OwnedItem*>& list,
             int sel) {
    auto& I = v.ints;
    int n = 0;
    for (const OwnedItem* it : list) {
        if (n >= MaxRows)
            break;
        const std::string idx = std::to_string(n);
        if (n == sel)
            CellName(v, font, idx, ItemName(info, it->id), {FontStyle::Bold24, FontStyle::Semi22, FontStyle::Semi20}, Gold);
        else
            CellName(v, font, idx, ItemName(info, it->id), {FontStyle::Semi24, FontStyle::Semi22, FontStyle::Semi20}, 0);
        I["qgi" + idx] = ItemIcon(info, it->id) + 1;
        if (n == sel)
            I["qgx" + idx] = 1;
        ++n;
    }
    return n;
}

} // namespace

InfoView BuildPartyView(const PartyViewInput& in) {
    InfoView v;
    v.sel = in.sel;
    v.slot = in.slot;
    auto& I = v.ints;
    auto& T = v.texts;
    if (!in.info || !in.battle || !in.member || !in.detail)
        return v;
    const InfoData* info = in.info;
    const BattleData* bdata = in.battle;
    const FontAtlas* font = in.font;
    const auto& m = *in.member;
    const auto& d = *in.detail;
    I["dq3.pp.on"] = 1;
    I["dq3.pp.key"] = in.key;
    const std::string& sprite = in.sprite;
    T["dq3.pp.portrait"] = "module:dq3:portrait/" + sprite;
    T["dq3.pp.name"] = Styled(m.name, FontStyle::Bold36);
    std::u32string job = in.job.empty() ? U"" : in.job + U" · ";
    job += AsciiToU32("Lv " + std::to_string(d.level));
    T["dq3.pp.job"] = Styled(job, FontStyle::Semi28);
    if (const auto p = info->personality.find(d.personality); p != info->personality.end())
        if (const auto* n = info->Noun(p->second)) {
            T["dq3.pp.pers"] = Styled(*n, FontStyle::Bold28);
            I["dq3.pp.pers.on"] = 1;
        }
    if (const auto next = ExpToNext(*info, d.level, d.stats.vocation, d.exp))
        T["dq3.pp.next"] = Styled("Next lv  " + Grouped(*next) + " EXP", FontStyle::Value);
    const auto st = ComputeStats(*info, d.stats);
    const std::string values[10] = {
        std::to_string(st.strength), std::to_string(st.agility), std::to_string(st.resilience),
        std::to_string(st.wisdom), st.luck_ok ? std::to_string(st.luck) : std::string{"-"},
        std::to_string(st.attack), std::to_string(st.defence), std::to_string(st.max_hp), std::to_string(st.max_mp),
        in.gold ? Grouped(s64{*in.gold}) : std::string{"-"}};
    for (int k = 0; k < 10; ++k)
        T["dq3.pp.s" + std::to_string(k)] = Styled(values[k], FontStyle::Bold30);
    // equipped slots (Slot+0x04 = 1 weapon .. 6 sub-accessory); names = the native Equipment screen's
    // empty-slot labels (GOP_Text_FieldMenu Equip_BLANK_01..06)
    if (const auto* h = MenuText(*info, "Txt_FieldMenu_Top_EQUIP"))
        T["dq3.pp.eqh"] = Styled(*h, FontStyle::Bold28);
    std::array<const OwnedItem*, 6> equipped{};
    std::vector<const OwnedItem*> carried;
    for (const auto& it : d.stats.items) {
        if (it.equip >= 1 && it.equip <= 6 && !equipped[it.equip - 1u])
            equipped[it.equip - 1u] = &it;
        else if (!it.equip)
            carried.push_back(&it);
    }
    if (v.slot < -1 || v.slot > 5 || (v.slot >= 0 && !equipped[static_cast<std::size_t>(v.slot)]))
        v.slot = -1;
    for (int k = 0; k < 6; ++k) {
        const std::string e = "dq3.pp.e" + std::to_string(k);
        const std::string id = "Txt_FieldMenu_Equip_BLANK_0" + std::to_string(k + 1);
        if (const auto* l = MenuText(*info, id))
            T[e + ".l"] = Fitted(font, *l, {FontStyle::Semi24, FontStyle::Semi22, FontStyle::Semi20}, SlotTextW);
        I[e + (k == v.slot ? ".sel" : ".off")] = 1;
        if (const OwnedItem* it = equipped[static_cast<std::size_t>(k)]) {
            I[e + ".on"] = 1;
            I[e + ".i"] = ItemIcon(*info, it->id) + 1;
            T[e + ".n"] = Fitted(font, ItemName(*info, it->id), {FontStyle::Semi24, FontStyle::Semi22, FontStyle::Semi20},
                                 SlotTextW);
        }
    }
    // the member's dark window: Carried (equipped gear excluded) or Spells
    const int sub = in.sub == 1 ? 1 : 0;
    I["dq3.pr.on"] = 1;
    I["dq3.pr.key"] = in.key;
    if (sprite != "-")
        T["dq3.pr.head"] = "module:dq3:head/" + sprite;
    for (int k = 0; k < 2; ++k)
        I["dq3.pr.t" + std::to_string(k) + (k == sub ? ".sel" : ".off")] = 1;
    const auto* top = MenuText(*info, sub == 0 ? "Txt_FieldMenu_Top_ITEM" : "Txt_FieldMenu_Top_MAGIC");
    T["dq3.pr.title"] = Styled(m.name + U"'s " + (top ? *top : U""), FontStyle::Bold32);
    Detail det;
    int n = 0;
    if (sub == 0) {
        v.sel = std::clamp(v.sel, 0, std::max(0, static_cast<int>(carried.size()) - 1));
        n = ItemRows(v, *info, font, carried, v.slot >= 0 ? -1 : v.sel);
        std::u32string count = AsciiToU32(std::to_string(carried.size())) + U" carried";
        if (d.limited)
            count += U" · " + AsciiToU32(std::to_string(d.stats.items.size()) + " / " + std::to_string(CarryLimit) + " held");
        T["dq3.pr.count"] = Styled(count, FontStyle::Semi24);
        const OwnedItem* shown = v.slot >= 0 ? equipped[static_cast<std::size_t>(v.slot)]
                                 : carried.empty() ? nullptr : carried[static_cast<std::size_t>(v.sel)];
        if (shown) {
            det.item_icon = ItemIcon(*info, shown->id);
            det.name = ItemName(*info, shown->id);
            if (v.slot >= 0) {
                if (const auto* l = MenuText(*info, "Txt_FieldMenu_Equip_BLANK_0" + std::to_string(v.slot + 1)))
                    det.sub = *l;
            } else {
                det.sub = U"Carried by " + m.name;
            }
            det.f0 = ParamLine(*info, *shown, d.stats.vocation);
            det.text = ItemFlavour(*info, shown->id);
        }
        if (n == 0)
            if (const auto* none = MenuText(*info, "Txt_FieldMenu_Item_NONE")) {
                T["dq3.pr.none"] = Styled(*none, FontStyle::Value);
                I["dq3.pr.empty"] = 1;
            }
    } else {
        // the native field Spells menu (b09 / INVENTORY.md): learnt field-usable spells (timing 1 / 3) in
        // list order, then the learnt battle-only ones (timing 2), then the not yet learnt ("???", learn 1)
        std::vector<const MemberDetail::Skill*> order;
        for (int pass = 0; pass < 3; ++pass)
            for (const auto& s : d.skills) {
                const auto mg = bdata->magic.find(s.id);
                if (mg == bdata->magic.end() || !mg->second.magic || s.learn < 1 || s.learn > 3 || s.timing == 0)
                    continue;
                const int group = s.learn == 1 ? 2 : s.timing == 2 ? 1 : 0;
                if (group == pass)
                    order.push_back(&s);
            }
        v.sel = std::clamp(v.sel, 0, std::max(0, static_cast<int>(order.size()) - 1));
        // owner 2026-10-10 (0.10.2): a Bag-style list in the grid's place (rows qs<field><row>: n name, s / z icon + 1,
        // m "N MP", x selected, l the separator above an unselected row); the detail below as before
        I["dq3.ps.on"] = 1;
        int learnt = 0;
        for (const auto* s : order) {
            const auto& mg = bdata->magic.at(s->id);
            const bool known = s->learn != 1;
            learnt += known ? 1 : 0;
            if (n >= MaxRows)
                continue;
            const bool grey = s->timing == 2 || !known;
            const bool sel = n == v.sel;
            const auto* noun = known ? bdata->Noun(mg.name_text) : nullptr;
            const std::u32string name = known ? (noun ? *noun : AsciiToU32(s->id)) : U"???";
            const std::string idx = std::to_string(n);
            if (sel)
                T["qsn" + idx] = Colour(Gold, Fitted(font, name, {FontStyle::Bold30, FontStyle::Bold28, FontStyle::Bold26},
                                                     SpellNameW));
            else if (grey)
                T["qsn" + idx] = Colour(GreyName, Fitted(font, name, {FontStyle::Semi30, FontStyle::Semi28, FontStyle::Value},
                                                         SpellNameW));
            else
                T["qsn" + idx] = Fitted(font, name, {FontStyle::Semi30, FontStyle::Semi28, FontStyle::Value}, SpellNameW);
            if (known) {
                I[(grey ? "qsz" : "qss") + idx] = mg.icon + 1;
                T["qsm" + idx] = Colour(grey ? GreyMp : Blue, Styled(std::to_string(mg.consume_mp) + " MP", FontStyle::Semi28));
                I["qsm" + idx + ".on"] = 1;
            }
            if (sel)
                I["qsx" + idx] = 1;
            else if (n > 0)
                I["qsl" + idx] = 1;
            if (sel) {
                det.name = name;
                if (known) {
                    // the native Spells box: "MP <cost> / <the member's MP>" (n10)
                    det.sub = AsciiToU32("MP " + std::to_string(mg.consume_mp) + " / " + std::to_string(m.mp));
                    det.spell_icon = mg.icon;
                    det.spell_grey = grey;
                    // where it can be cast: the skill entry's use timing (+0x0D, = GOP_Magic_Spec EnableTymingType:
                    // 1 ALL, 2 BATTLE, 3 SEARCH / FIELD / TOWNDUNGEON); the game has no short label for it
                    det.f0 = SpellTimingLabel(s->timing);
                    if (const auto fl = info->magic_flavor.find(s->id); fl != info->magic_flavor.end())
                        if (const auto* f = info->Text(fl->second))
                            det.text = CleanGameText(*f);
                }
            }
            ++n;
        }
        I["dq3.ps.n"] = n;
        T["dq3.pr.count"] = Styled(std::to_string(learnt) + (learnt == 1 ? " spell" : " spells"), FontStyle::Semi24);
        if (n == 0)
            if (const auto* none = MenuText(*info, "Txt_FieldMenu_Magic_NONE")) {
                T["dq3.pr.none"] = Styled(*none, FontStyle::Value);
                I["dq3.pr.empty"] = 1;
            }
    }
    I["dq3.pr.n"] = sub == 0 ? n : 0; // the Carried grid; the Spells list counts dq3.ps.n
    det.Publish(v);
    return v;
}

InfoView BuildBagView(const BagViewInput& in) {
    // render.py bag_page(): the bag's list (rows qb<field><row>: n name, i icon + 1, c "xN", x selected, l the
    // separator above an unselected row) and the selection's description pane dq3.bd.*
    InfoView v;
    v.sel = in.sel;
    auto& I = v.ints;
    auto& T = v.texts;
    if (!in.info)
        return v;
    const InfoData& info = *in.info;
    const FontAtlas* font = in.font;
    const int page = std::clamp(in.page, 0, 2);
    static constexpr const char* bag_ids[3] = {"Txt_FieldMenu_Item_ITEMBAG", "Txt_FieldMenu_Item_EQUIPBAG",
                                               "Txt_FieldMenu_Item_IMPOTANT"};
    I["dq3.bg.on"] = 1;
    I["dq3.bg.key"] = in.key;
    for (int k = 0; k < 3; ++k)
        I["dq3.bg.b" + std::to_string(k) + (k == page ? ".sel" : ".off")] = 1;
    const auto* bag_name = MenuText(info, bag_ids[page]);
    if (bag_name) {
        T["dq3.bg.title"] = Styled(*bag_name, FontStyle::Bold32);
        // the sub-tabs follow the title (render.py: title width + 30)
        I["dq3.bg.subx"] = (font ? font->Measure(T["dq3.bg.title"]) : 0) + BagSubGap;
    }
    if (!in.rows) {
        I["dq3.bg.wait"] = 1;
        return v;
    }
    std::vector<const OwnedItem*> list;
    for (const auto& it : *in.rows)
        list.push_back(&it);
    v.sel = std::clamp(v.sel, 0, std::max(0, static_cast<int>(list.size()) - 1));
    int n = 0;
    for (const OwnedItem* it : list) {
        if (n >= MaxRows)
            break;
        const std::string idx = std::to_string(n);
        const bool sel = n == v.sel;
        const std::u32string name = ItemName(info, it->id);
        if (sel)
            T["qbn" + idx] = Colour(Gold, Fitted(font, name, {FontStyle::Bold30, FontStyle::Bold28, FontStyle::Bold26},
                                                 BagNameW));
        else
            T["qbn" + idx] = Fitted(font, name, {FontStyle::Semi30, FontStyle::Semi28, FontStyle::Value}, BagNameW);
        I["qbi" + idx] = ItemIcon(info, it->id) + 1;
        T["qbc" + idx] = Styled(U"×" + AsciiToU32(std::to_string(it->count)), FontStyle::Semi28);
        if (sel)
            I["qbx" + idx] = 1;
        else if (n > 0)
            I["qbl" + idx] = 1;
        ++n;
    }
    I["dq3.bg.n"] = n;
    T["dq3.bg.count"] = Styled(std::to_string(list.size()) + (list.size() == 1 ? " kind" : " kinds"), FontStyle::Value);
    if (list.empty()) {
        if (const auto* none = MenuText(info, "Txt_FieldMenu_Item_NONE")) {
            T["dq3.bg.none"] = Styled(*none, FontStyle::Semi28);
            I["dq3.bg.empty"] = 1;
        }
        return v;
    }
    // the description pane: icon, name, "Number Held N" (the native label), "Carried by <bag>", the stat
    // effect (equipment) and the description
    const OwnedItem& it = *list[static_cast<std::size_t>(v.sel)];
    I["dq3.bd.on"] = 1;
    I["dq3.bd.ii"] = ItemIcon(info, it.id) + 1;
    T["dq3.bd.name"] = Fitted(font, ItemName(info, it.id), {FontStyle::Bold34, FontStyle::Bold30, FontStyle::Bold28},
                              BagPaneNameW);
    if (const auto* held = MenuText(info, "Txt_FieldMenu_Item_COUNTA_01")) {
        T["dq3.bd.held"] = Styled(*held + U"  " + AsciiToU32(Grouped(s64{it.count})), FontStyle::Value);
        I["dq3.bd.held.on"] = 1;
    }
    T["dq3.bd.by"] = Styled((bag_name ? *bag_name : U"") + U" (whole party)", FontStyle::Value);
    const std::u32string fact = ParamLine(info, it, std::nullopt);
    if (!fact.empty()) {
        T["dq3.bd.fact"] = Styled(fact, FontStyle::Semi28);
        I["dq3.bd.fact.on"] = 1;
        I["dq3.bd.ty"] = 36; // the description starts one line lower
    }
    const std::u32string text = ItemFlavour(info, it.id);
    if (!text.empty()) {
        T["dq3.bd.text"] = StyledWrap(text, FontStyle::Semi28);
        I["dq3.bd.text.on"] = 1;
    }
    return v;
}

InfoView BuildJournalView(const JournalViewInput& in) {
    InfoView v;
    v.mon_sel = in.mon_sel;
    auto& I = v.ints;
    auto& T = v.texts;
    if (!in.info || !in.state || !in.map)
        return v;
    const InfoData* info = in.info;
    const MapData& map = *in.map;
    const auto& s = *in.state;
    const auto* record = in.record;
    I["dq3.jn.on"] = 1;
    // Next step: the native objective banner (main) and its first sub line
    if (const auto o = ObjectiveFor(*info, map, s.guide, s.progress)) {
        if (!o->main.empty()) {
            T["dq3.jo.main"] = StyledWrap(o->main, FontStyle::Semi30);
            I["dq3.jo.on"] = 1;
        }
        if (!o->subs.empty()) {
            T["dq3.jo.sub"] = StyledWrap(U"• " + o->subs[0], FontStyle::Value);
            I["dq3.jo.sub.on"] = 1;
            // directly under a one-line objective, else under its second line
            const bool one = in.font && WrapLines(*in.font, o->main, FontStyle::Semi30, JournalWrapW) == 1;
            I["dq3.jo.sub.y"] = one ? 0 : 36; // y_bind offsets the label: under the first or second line
        }
    } else {
        I["dq3.jo.none"] = 1;
    }
    // Traveller's Tips (native order, unread = the game's "!")
    const auto tips = VisibleTips(*info, s.progress, s.tips_read);
    int n = 0;
    for (const auto& t : tips) {
        if (n >= MaxRows)
            break;
        const auto* title = info->Text(info->info[t.row].title);
        const std::u32string text = title ? CleanGameText(*title) : AsciiToU32(info->info[t.row].id);
        const std::string idx = std::to_string(n);
        T["qtn" + idx] = Styled(text, t.unread ? FontStyle::Bold30 : FontStyle::Semi30);
        if (t.unread)
            I["qtw" + idx] = 1;
        ++n;
    }
    I["dq3.jt.count"] = n;
    // Mini medals: P+0x34 and the next reward (0xc09d40)
    T["dq3.jm.count"] = Styled(Grouped(s.medals), FontStyle::Bold32);
    if (const auto* m = NextMedal(*info, s.medals_got)) {
        const auto row = info->items.find(m->product);
        const auto* item = row == info->items.end() ? nullptr : info->Noun(row->second.name_text);
        std::u32string line = AsciiToU32((s.medals >= m->require ? "Reward ready at " : "Next reward at ") +
                                          std::to_string(m->require) + " medals");
        if (item)
            line += U": " + *item;
        T["dq3.jm.next"] = StyledWrap(line, FontStyle::Semi28);
        I["dq3.jm.next.on"] = 1;
    }
    // Records . Monsters: the native Defeated Monster List
    const auto kills_of = [&](const LibraryRow& r) -> std::optional<s32> {
        if (!record)
            return std::nullopt;
        const auto it = record->find(r.unit_master);
        return it == record->end() ? std::nullopt : std::optional<s32>{it->second};
    };
    // the native list: recorded species, plus every non-boss row (unrecorded bosses stay hidden)
    static const std::map<std::string, s32> no_record;
    const auto rows = VisibleMonsters(*info, record ? *record : no_record);
    int first = -1;
    for (std::size_t i = 0; i < rows.size() && first < 0; ++i)
        if (kills_of(info->library[rows[i]]))
            first = static_cast<int>(i);
    const int total = static_cast<int>(rows.size());
    // "Types of Monster Defeated" (0xb7abd8): the record's entry count, Pandora's Box and its boss
    // variant counted once; the list itself marks only rows whose own unit is recorded
    const std::size_t types = record ? record->size() - ((record->contains("UNIT_MASTER_MN_154_PANDORAS_BOX") &&
                                                           record->contains("UNIT_MASTER_MN_154_PANDORAS_BOX_BOSS_2")) ? 1 : 0)
                                     : 0;
    T["dq3.jr.counts"] = Styled(record ? "Defeated " + std::to_string(types) + " / " +
                                             std::to_string(info->library.size())
                                       : std::string{"Records unavailable"}, FontStyle::Value);
    if (v.mon_sel < 0)
        v.mon_sel = std::max(0, first);
    v.mon_sel = std::clamp(v.mon_sel, 0, std::max(0, total - 1));
    I["dq3.jr.count"] = total;
    I["dq3.jr.key"] = record ? 1 : 0;
    if (rows.empty())
        return v;
    // one int per cell (sprite index, -1 = not recorded): the manifest formats the ecell key and
    // shows the "?" slime for -1; the selected cell gets the glow overlay
    for (int i = 0; i < total; ++i) {
        const auto& r = info->library[rows[static_cast<std::size_t>(i)]];
        const auto sprite = kills_of(r) ? FindMonsterSprite(r.unit_master) : std::nullopt;
        const std::string idx = std::to_string(i);
        I["qc" + idx] = sprite ? static_cast<s64>(*sprite) : -1;
        if (i == v.mon_sel)
            I["qcs" + idx] = 1;
    }
    const auto& r = info->library[rows[static_cast<std::size_t>(v.mon_sel)]];
    const auto kills = kills_of(r);
    const auto sprite = kills ? FindMonsterSprite(r.unit_master) : std::nullopt;
    I["dq3.jr.d.on"] = 1;
    if (sprite)
        T["dq3.jr.d.sprite"] = "module:dq3:mon/" + std::to_string(*sprite) + "/110x110/4";
    else
        I["dq3.jr.d.q"] = 1;
    const auto* name = kills ? info->Noun(r.name_text) : nullptr;
    T["dq3.jr.d.name"] = Styled(name ? *name : U"???", FontStyle::Bold32);
    if (kills) {
        const auto place = [&](const std::string& id) -> std::u32string {
            if (const auto it = map.menu.find(id); it != map.menu.end() && !it->second.empty() &&
                                                   it->second != U"<ERROR!>")
                return it->second;
            if (const auto* n2 = map.Noun(id))
                return *n2;
            return {};
        };
        std::u32string hab;
        for (const auto& h : r.habitat) {
            const auto p = place(h);
            if (p.empty())
                continue;
            if (!hab.empty())
                hab += U", ";
            hab += p;
        }
        if (r.more)
            hab += hab.empty() ? U"Elsewhere" : U" and Elsewhere";
        if (!hab.empty()) {
            T["dq3.jr.d.hab"] = StyledWrap(U"Habitat  " + hab, FontStyle::Value);
            I["dq3.jr.d.hab.on"] = 1;
        }
        const auto mon = info->monsters.find(r.monster);
        if (mon != info->monsters.end()) {
            const auto item = info->items.find(mon->second.drop);
            const auto* drop = item == info->items.end() ? nullptr : info->Noun(item->second.name_text);
            T["dq3.jr.d.drop"] = Styled(U"Drops  " + (drop ? *drop : U"N/A"), FontStyle::Value);
            T["dq3.jr.d.stat"] = Styled(AsciiToU32("Defeated " + Grouped(*kills)) + U" · " +
                                            AsciiToU32("EXP " + Grouped(mon->second.exp)) + U" · " +
                                            AsciiToU32("Gold " + Grouped(mon->second.gold)), FontStyle::Value);
            I["dq3.jr.d.more"] = 1;
        }
        if (const auto* f = info->Text(r.trivia)) {
            T["dq3.jr.d.flav"] = StyledWrap(CleanGameText(*f), FontStyle::Value);
            I["dq3.jr.d.flav.on"] = 1;
        }
    }
    return v;
}

} // namespace dq3
