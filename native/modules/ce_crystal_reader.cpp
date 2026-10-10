// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_crystal_reader.h"

#include "ce_draw.h"
#include "ce_parse.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace ce_crystal {
namespace {

using namespace ce_page_crystals;
using namespace ce_page_crystals::geo;

// TypeInfo slots, main-relative (1.41 only; research fixtures/usage-slot-map-boot1.json)
constexpr u64 SlotGameManager = 0x4930BB8; // direct
constexpr u64 SlotSaveDb = 0x4AFB318;      // usage: GetData, statics +0 = SaveDataOwn
constexpr u64 SlotMainMenu = 0x4AFE258;    // usage: MainMenu, statics +0 = instance GameObject
constexpr u64 SlotCraftMenu = 0x4B007F0;   // usage: CraftMenu, statics +0 = instance GameObject

constexpr int MenuEvery = 6;   // 10 Hz: native menu / smith state
constexpr int FastEvery = 12;  // 5 Hz while the page is shown or mirroring
constexpr int SlowEvery = 120; // 0.5 Hz otherwise (plans keep ticking)
constexpr std::size_t ModelKeep = 24;

// GameFunctions.ReturnTypeName (table 120 "type") -> UserInterfaceElements spriteList icon
// (research contracts/crystal-readers.md "Icons")
std::string TypeIcon(int type) {
    static const std::pair<int, int> map[] = {{0, 0},   {1, 17}, {2, 19}, {3, 21},  {4, 27},  {5, 26},
                                              {6, 20},  {7, 22}, {8, 16}, {9, 23},  {10, 24}, {11, 25},
                                              {12, 9},  {13, 2}, {14, 11}, {15, 10}, {16, 3}, {26, 32}};
    for (const auto& [t, i] : map)
        if (t == type)
            return "icon_equipment_" + std::to_string(i);
    return "icon_equipment_0";
}
constexpr const char* KindIcon[4] = {"icon_equipment_0", "icon_equipment_9", "icon_equipment_3",
                                     "icon_equipment_4"};
constexpr const char* KindName[2] = {"Weapon", "Armor"};

std::string SpriteFor(std::string name) {
    name.erase(std::remove(name.begin(), name.end(), '\''), name.end());
    if (name == "BaThraz")
        return "Bathraz";
    return name;
}

std::string Plain(std::string s) {
    for (const char* q : {"\xE2\x80\x99", "\xE2\x80\x98"})
        for (std::size_t p = s.find(q); p != std::string::npos; p = s.find(q, p + 1))
            s.replace(p, 3, "'");
    return s;
}

std::string Hex(u64 v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

/// Single.ToString() of a table value ("10", "0.5")
std::string Num(const std::string& cell) {
    const double d = std::strtod(cell.c_str(), nullptr); // as atof, without its overflow UB
    char b[32];
    if (std::fabs(d) < 1e15 && std::fabs(d - std::round(d)) < 1e-6)
        std::snprintf(b, sizeof b, "%lld", static_cast<long long>(std::llround(d)));
    else
        std::snprintf(b, sizeof b, "%g", d);
    return b;
}

std::string Replace(std::string s, std::string_view from, const std::string& to) {
    for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
        s.replace(p, from.size(), to);
    return s;
}

int RowOf(const std::vector<std::vector<std::string>>* t, int id) {
    if (!t)
        return -1;
    const std::string k = std::to_string(id);
    for (std::size_t i = 1; i < t->size(); ++i)
        if (!(*t)[i].empty() && (*t)[i][0] == k)
            return static_cast<int>(i) - 1;
    return -1;
}

std::string CellOf(const std::vector<std::vector<std::string>>* t, int row, int col) {
    if (!t || row < 0 || static_cast<std::size_t>(row + 1) >= t->size())
        return {};
    const auto& r = (*t)[static_cast<std::size_t>(row + 1)];
    return static_cast<std::size_t>(col) < r.size() ? r[static_cast<std::size_t>(col)] : std::string{};
}

int ColumnIndex(const std::vector<std::vector<std::string>>* t, std::string_view name) {
    if (!t || t->empty())
        return -1;
    for (std::size_t i = 0; i < (*t)[0].size(); ++i)
        if ((*t)[0][i] == name)
            return static_cast<int>(i);
    return -1;
}

} // namespace

// ---- guest memory ------------------------------------------------------------------------------

struct Reader::Mem {
    const EdenDsmodHostApi& h;
    std::unordered_map<u64, std::string>& names;

    bool Read(u64 at, void* out, std::size_t n) const {
        return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(h, at, out, n);
    }
    template <class T>
    T Get(u64 at) const {
        T v{};
        return Read(at, &v, sizeof v) ? v : T{};
    }
    u64 Ptr(u64 at) const {
        return Get<u64>(at);
    }
    static bool Ok(u64 p) {
        return p >= 0x1000000 && p < (u64{1} << 48) && (p & 7) == 0;
    }
    std::string CStr(u64 at) const {
        char buf[64];
        if (!at)
            return {};
        for (std::size_t off = 0; off < 64; off += 16) {
            if (!Read(at + off, buf + off, 16))
                break;
            if (const void* z = std::memchr(buf + off, 0, 16))
                return std::string(buf, static_cast<const char*>(z) - buf);
        }
        return {};
    }
    std::string ClassName(u64 obj) {
        if (!Ok(obj))
            return {};
        const u64 k = Ptr(obj);
        if (!Ok(k))
            return {};
        if (const auto it = names.find(k); it != names.end())
            return it->second;
        std::string n = CStr(Ptr(k + 0x10));
        if (!n.empty() && names.size() < 4096)
            names.emplace(k, n);
        return n;
    }
    std::string String(u64 p) const {
        if (!Ok(p))
            return {};
        const s32 n = Get<s32>(p + 0x10);
        if (n <= 0 || n > 256)
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
    std::vector<u64> List(u64 list, std::size_t limit) const {
        std::vector<u64> out;
        if (!Ok(list))
            return out;
        const u64 arr = Ptr(list + 0x10);
        const s32 n = Get<s32>(list + 0x18);
        if (n <= 0 || static_cast<std::size_t>(n) > limit || !Ok(arr) || Ptr(arr + 0x18) < static_cast<u64>(n))
            return out;
        out.resize(static_cast<std::size_t>(n));
        if (!Read(arr + 0x20, out.data(), out.size() * 8))
            out.clear();
        return out;
    }
    std::vector<int> Ints(u64 list, std::size_t limit) const { ///< List<int>
        std::vector<int> out;
        if (!Ok(list))
            return out;
        const u64 arr = Ptr(list + 0x10);
        const s32 n = Get<s32>(list + 0x18);
        if (n <= 0 || static_cast<std::size_t>(n) > limit || !Ok(arr) || Ptr(arr + 0x18) < static_cast<u64>(n))
            return out;
        out.resize(static_cast<std::size_t>(n));
        if (!Read(arr + 0x20, out.data(), out.size() * 4))
            out.clear();
        return out;
    }
    std::vector<int> IntArray(u64 arr, std::size_t limit) const { ///< int[]
        std::vector<int> out;
        if (!Ok(arr))
            return out;
        const u64 n = Ptr(arr + 0x18);
        if (n == 0 || n > limit)
            return out;
        out.resize(n);
        if (!Read(arr + 0x20, out.data(), n * 4))
            out.clear();
        return out;
    }
    u64 Usage(u64 slot, std::string_view name) {
        const u64 s = Ptr(h.main_base + slot);
        const u64 k = Ok(s) ? Ptr(s) : 0;
        if (!Ok(k) || CStr(Ptr(k + 0x10)) != name)
            return 0;
        const u64 sf = Ptr(k + 0xB8);
        return Ok(sf) ? sf : 0;
    }
    u64 Native(u64 go) const {
        const u64 nat = Ok(go) ? Ptr(go + 0x10) : 0;
        return Ok(nat) && Ptr(nat + 0x28) == go ? nat : 0;
    }
    u64 Component(u64 go, std::string_view cls) {
        const u64 nat = Native(go);
        if (!nat)
            return 0;
        const u64 arr = Ptr(nat + 0x30), n = Ptr(nat + 0x40);
        if (!Ok(arr) || n == 0 || n > 64)
            return 0;
        std::vector<u64> raw(n * 2);
        if (!Read(arr, raw.data(), raw.size() * 8))
            return 0;
        for (u64 i = 0; i < n; ++i) {
            const u64 comp = raw[i * 2 + 1];
            if (!Ok(comp))
                continue;
            const u64 managed = Ptr(comp + 0x28);
            if (Ok(managed) && Ptr(managed + 0x10) == comp && ClassName(managed) == cls)
                return managed;
        }
        return 0;
    }
};

// ---- reader --------------------------------------------------------------------------------------

Reader::Reader(ce_draw::Art& a) : art{a} {}

std::string Reader::Tr(std::string_view meta, std::int64_t table, int id, std::string fallback, int name_col) {
    const auto* t = art.Table(table);
    const int row = RowOf(t, id);
    if (fallback.empty() && name_col >= 0)
        fallback = CellOf(t, row, name_col);
    const auto* column = art.BgText(meta); // the BGDatabase text, read once for every reader
    if (!column || row < 0 || row >= static_cast<int>(column->size()) ||
        (*column)[static_cast<std::size_t>(row)].empty())
        return Plain(fallback);
    return Plain((*column)[static_cast<std::size_t>(row)]);
}

void Reader::Finish(Crystal& c) {
    // name / effect: GameFunctions.ReturnPassiveDescr(id, level 0, quality): quality 1-4 -> Value
    // 1a/1b, 5-9 -> 2a/2b (+ descr2 when not "-"), 10+ -> 3a/3b (+ descr3); "#va" / "#vb" replaced
    const int lvl = c.rank >= 10 ? 3 : c.rank >= 5 ? 2 : 1;
    if (const auto it = finished.find({c.prop, lvl}); it != finished.end()) {
        c.name = it->second.first;
        c.effect = it->second.second;
        return;
    }
    c.name = Tr("tr_PASSIVES_NAME", 117, c.prop, {}, 1);
    std::string d = Tr("tr_PASSIVES_DESCR", 117, c.prop, {}, 2);
    const auto* t = art.Table(117);
    const int row = RowOf(t, c.prop);
    if (row >= 0) {
        const int va = ColumnIndex(t, lvl == 1 ? "Value 1a" : lvl == 2 ? "Value 2a" : "Value 3a");
        if (lvl >= 2) {
            const std::string alt = CellOf(t, row, ColumnIndex(t, lvl == 2 ? "descr2" : "descr3"));
            if (!alt.empty() && alt != "-")
                d = Plain(alt);
        }
        if (va >= 0) {
            d = Replace(d, "#va", Num(CellOf(t, row, va)));
            d = Replace(d, "#vb", Num(CellOf(t, row, va + 1)));
        }
    }
    c.effect = d;
    if (finished.size() < 4096)
        finished.emplace(std::pair{c.prop, lvl}, std::pair{c.name, c.effect});
}

Crystal Reader::ReadCrystal(Mem& m, u64 p) {
    Crystal c;
    s32 f[6]{};
    if (!m.Read(p + 0x10, f, sizeof f)) {
        c.prop = -1;
        return c;
    }
    c.prop = f[1];
    c.cat = std::clamp(f[2], 0, 3);
    c.rank = f[3];
    c.purity = f[4];
    c.size = std::clamp(f[5] + 1, 1, 3);
    c.art = m.Get<u8>(p + 0x28) != 0;
    if (c.prop < 0 || c.prop > 4096 || c.rank < 1 || c.rank > 20 || c.purity < -1 || c.purity > 20)
        c.prop = -1;
    return c;
}

void Reader::ReadMenus(Mem& m, u64 gm) {
    const bool menu = m.Get<u8>(gm + 0xF1) != 0;
    int menu_pos = -1;
    menu_member_id = -1;
    if (const u64 sf = m.Usage(SlotMainMenu, "MainMenu")) {
        const u64 mm = m.Component(m.Ptr(sf), "MainMenu");
        if (mm)
            menu_pos = m.Get<s32>(mm + 0x100);
        // the Equipment tab shows pm[currentActiveSlot +0xE8] (List<PartyMember> +0xF8), the
        // member L/R cycles -- the same pair the Skills screens use (skill-readers.md)
        if (mm && menu && (menu_pos == 1 || menu_pos / 100 == 1)) {
            const s32 at = m.Get<s32>(mm + 0xE8);
            const u64 pm = m.Ptr(mm + 0xF8);
            const u64 arr = Mem::Ok(pm) ? m.Ptr(pm + 0x10) : 0;
            const s32 n = Mem::Ok(pm) ? m.Get<s32>(pm + 0x18) : 0;
            if (at >= 0 && at < 16 && at < n && Mem::Ok(arr) &&
                m.Get<u64>(arr + 0x18) > static_cast<u64>(at)) {
                const u64 p = m.Ptr(arr + 0x20 + 8 * static_cast<u64>(at));
                if (m.ClassName(p) == "PartyMember")
                    menu_member_id = m.Get<s32>(p + 0x10);
            }
        }
    }
    bool smith = false, craft_set = false;
    smith_base = smith_fuse = smith_result = false;
    if (const u64 sf = m.Usage(SlotCraftMenu, "CraftMenu")) {
        const u64 cm = m.Component(m.Ptr(sf), "CraftMenu");
        // crystalMode +0xB0 is also set in the smith's Set / Remove screens (insertWeaponMode
        // +0xB1 / insertArmorMode +0xB2); only the Combine screen is the fusion mirror
        const bool ins = cm && (m.Get<u8>(cm + 0xB1) || m.Get<u8>(cm + 0xB2));
        craft_set = cm && menu && ins;
        if (cm && menu && m.Get<u8>(cm + 0xB0) && !ins) {
            smith = true;
            const u64 ce = m.Ptr(cm + 0x20);
            if (m.ClassName(ce) == "CraftMenuEquipment") {
                const auto rd = [&](u64 at, Crystal& out) {
                    const u64 p = m.Ptr(ce + at);
                    if (m.ClassName(p) != "Crystal")
                        return false;
                    out = ReadCrystal(m, p);
                    if (out.prop < 0)
                        return false;
                    Finish(out);
                    return true;
                };
                smith_base = rd(0x68, base);
                smith_fuse = rd(0x70, fuse);
                smith_result = rd(0x78, result);
            }
        }
    }
    mirror_smith = smith;
    mirror_equip = !smith && menu && (menu_pos == 1 || menu_pos / 100 == 1 || craft_set);
    last_menu_pos = menu ? menu_pos : -1;
}

bool Reader::Refresh(const EdenDsmodHostApi& host) {
    Mem m{host, class_names};
    const u64 slot = m.Ptr(host.main_base + SlotSaveDb);
    const u64 kl = Mem::Ok(slot) ? m.Ptr(slot) : 0;
    if (!Mem::Ok(kl) || m.CStr(m.Ptr(kl + 0x10)) != "GetData")
        return false;
    const u64 sf = m.Ptr(kl + 0xB8);
    const u64 sd = Mem::Ok(sf) ? m.Ptr(sf) : 0;
    if (m.ClassName(sd) != "SaveDataOwn")
        return false;
    // party: available members of the formation (posInParty 0-7), native Equipment order
    std::vector<MemberData> mem;
    member_sprites.clear();
    for (const u64 p : m.List(m.Ptr(sd + 0x40), 64)) {
        if (m.ClassName(p) != "PartyMember")
            continue;
        MemberData d;
        d.id = m.Get<s32>(p + 0x10);
        d.pos = m.Get<s32>(p + 0x94);
        d.name = Plain(m.String(m.Ptr(p + 0x18)));
        d.sprite = SpriteFor(d.name);
        if (d.id >= 0 && d.id < 100)
            member_sprites.push_back({d.id, d.sprite});
        if (!m.Get<u8>(p + 0x110) || d.pos < 0 || d.pos > 7 || d.id < 0 || d.id >= 100 || d.name.empty())
            continue;
        d.weapon = m.Get<s32>(p + 0xD8);
        d.body = m.Get<s32>(p + 0xE0);
        d.acc = m.Get<s32>(p + 0xE4);
        d.emblem = m.Get<s32>(p + 0xE8);
        d.allowed = m.IntArray(m.Ptr(p + 0xF8), 64);
        mem.push_back(std::move(d));
    }
    if (mem.empty())
        return false;
    std::sort(mem.begin(), mem.end(), [](const auto& a, const auto& b) { return a.pos < b.pos; });
    // equipment instances
    std::map<int, Item> it;
    for (const u64 e : m.List(m.Ptr(sd + 0x20), 4096)) {
        if (m.ClassName(e) != "EquipItem")
            continue;
        Item i;
        s32 f[3]{};
        if (!m.Read(e + 0x10, f, sizeof f))
            continue;
        i.id = f[0], i.equip = f[1], i.lvl = f[2];
        i.props = m.Ints(m.Ptr(e + 0x20), 16);
        i.plvl = m.Ints(m.Ptr(e + 0x28), 16);
        i.by = m.Get<s32>(e + 0x30);
        i.filled = m.Get<s32>(e + 0x34);
        it[i.id] = std::move(i);
    }
    // crystals: the bag and the inserted ones (since 1.2 an inserted crystal is kept whole in
    // insertedCrystals with id = the EquipItem it is in)
    std::vector<BagCrystal> b;
    for (const u64 p : m.List(m.Ptr(sd + 0x50), 4096)) {
        if (m.ClassName(p) != "Crystal")
            continue;
        BagCrystal c{p, ReadCrystal(m, p)};
        if (c.c.prop < 0)
            continue;
        b.push_back(std::move(c));
    }
    std::vector<Inserted> ins;
    for (const u64 p : m.List(m.Ptr(sd + 0x108), 4096)) {
        if (m.ClassName(p) != "Crystal")
            continue;
        Inserted c{p, ReadCrystal(m, p), m.Get<s32>(p + 0x10)};
        if (c.c.prop < 0)
            continue;
        ins.push_back(std::move(c));
    }
    std::map<int, std::string> em;
    for (const u64 e : m.List(m.Ptr(sd + 0x60), 256))
        if (m.ClassName(e) == "ClassEmblem")
            em[m.Get<s32>(e + 0x10)] = Plain(m.String(m.Ptr(e + 0x18)));
    // names / effects (Finish caches them by property and effect level)
    for (auto& c : b)
        Finish(c.c);
    for (auto& c : ins) {
        Finish(c.c);
        if (const auto f = it.find(c.item); f != it.end())
            for (const auto& [id, spr] : member_sprites)
                if (id == f->second.by) {
                    c.c.owner = spr;
                    for (const auto& md : mem)
                        if (md.id == id)
                            c.c.owner_name = md.name;
                    if (c.c.owner_name.empty())
                        c.c.owner_name = spr;
                }
    }
    members = std::move(mem);
    items = std::move(it);
    bag = std::move(b);
    inserted = std::move(ins);
    emblem_names = std::move(em);
    have = true;
    return true;
}

const Reader::Item* Reader::FindItem(int id) const {
    const auto it = items.find(id);
    return it == items.end() ? nullptr : &it->second;
}

std::vector<const Reader::Inserted*> Reader::InsertedIn(int item) const {
    std::vector<const Inserted*> out;
    for (const auto& i : inserted)
        if (i.item == item)
            out.push_back(&i);
    return out;
}

std::vector<Crystal> Reader::PieceCrystals(int item) {
    std::vector<Crystal> out;
    for (const auto* i : InsertedIn(item))
        out.push_back(i->c);
    if (!out.empty())
        return out;
    // a pre-1.2 save: only the property / level list of the piece (no insertedCrystals entry)
    const Item* e = FindItem(item);
    if (!e)
        return out;
    for (std::size_t k = 0; k < e->props.size(); ++k) {
        Crystal c;
        c.prop = e->props[k];
        c.rank = 0; // plvl is the effect level bucket (1 for rank I-IV), not the rank
        c.known = false;
        c.size = e->props.size() == 1 ? std::clamp(e->filled, 1, 3) : 1;
        Finish(c);
        out.push_back(c);
    }
    return out;
}

ce_page_crystals::Piece Reader::BuildPiece(const MemberData& md, int kind, const CrystalsView& v) {
    Piece p;
    p.kind = kind;
    p.icon = KindIcon[kind];
    const auto* t120 = art.Table(120);
    if (kind == 3) {
        p.present = md.emblem >= 0;
        if (p.present) {
            const auto f = emblem_names.find(md.emblem);
            p.name = Tr("tr_CLASSEMBLEMS_NAME", 111, md.emblem, f != emblem_names.end() ? f->second : "", 1);
        }
        return p;
    }
    const int id = kind == 0 ? md.weapon : kind == 1 ? md.body : md.acc;
    const Item* e = id >= 0 ? FindItem(id) : nullptr;
    if (!e)
        return p;
    p.present = true;
    p.item = e->id;
    p.equip = e->equip;
    const int row = RowOf(t120, e->equip);
    p.name = Tr("tr_EQUIPS_NAME", 120, e->equip, {}, 1);
    p.icon = TypeIcon(ce_parse::Leading(CellOf(t120, row, ColumnIndex(t120, "type"))).value_or(0));
    p.selected = piece_sel == kind && kind < 2;
    if (kind == 2) {
        if (!e->props.empty()) {
            Crystal c;
            c.prop = e->props[0];
            p.fixed = Tr("tr_PASSIVES_NAME", 117, c.prop, {}, 1);
        }
        return p;
    }
    // slots: crystalslots + the upgrade bonus (EquipFunctions.GetCrystalSlotsAmount: level 1 +1,
    // level 2 +2)
    const int base_slots = ce_parse::Leading(CellOf(t120, row, ColumnIndex(t120, "crystalslots"))).value_or(0);
    const int slots = std::clamp(base_slots + (e->lvl == 1 ? 1 : e->lvl >= 2 ? 2 : 0), 0, CrMaxRows);
    std::vector<SlotRow> rows(static_cast<std::size_t>(slots));
    int j = 0;
    for (const auto& c : PieceCrystals(e->id)) {
        if (j >= slots)
            break;
        rows[static_cast<std::size_t>(j)].state = 1;
        rows[static_cast<std::size_t>(j)].crystal = c;
        for (int q = 1; q < c.size && j + q < slots; ++q)
            rows[static_cast<std::size_t>(j + q)].state = 3;
        j += c.size;
    }
    // slot plans of this member / piece
    for (const auto& pl : plans)
        if (pl.member == md.id && pl.kind == kind && pl.item == e->id && pl.slot < slots) {
            auto& r = rows[static_cast<std::size_t>(pl.slot)];
            if (r.state == 1 && SameKind(r.crystal, pl.c))
                continue; // done: the planned crystal sits in that slot
            r.planned = true;
            r.plan = pl.c;
        }
    // transfer: the target piece shows the crystals still to insert as planned ghosts
    if (const auto tf = transfers.find(md.id); tf != transfers.end() && tf->second.kind == kind &&
                                               tf->second.new_equip == e->equip) {
        std::vector<Crystal> have = PieceCrystals(e->id);
        int r = 0;
        for (const auto& want : tf->second.crystals) {
            bool got = false;
            for (auto h = have.begin(); h != have.end(); ++h)
                if (SameKind(*h, want)) {
                    have.erase(h);
                    got = true;
                    break;
                }
            if (got)
                continue;
            while (r < slots && (rows[static_cast<std::size_t>(r)].state != 0 || rows[static_cast<std::size_t>(r)].planned))
                ++r;
            if (r < slots) {
                rows[static_cast<std::size_t>(r)].planned = true;
                rows[static_cast<std::size_t>(r)].plan = want;
            }
        }
    }
    // tap-tap / drag: where the selected bag crystal fits (empty slots with room for its size)
    if (v.has_detail && v.detail.owner.empty() && v.detail.rank >= 3 && !v.read_only && !transfers.contains(md.id)) {
        for (int s = 0; s < slots; ++s) {
            bool fit = s + v.detail.size <= slots;
            for (int q = 0; q < v.detail.size && fit; ++q)
                fit = rows[static_cast<std::size_t>(s + q)].state == 0 && !rows[static_cast<std::size_t>(s + q)].planned;
            rows[static_cast<std::size_t>(s)].valid = fit;
        }
    }
    if (base_slots > 0 && e->lvl < 2) {
        SlotRow locked{};
        locked.state = 2;
        rows.push_back(std::move(locked));
    }
    p.rows = std::move(rows);
    return p;
}

void Reader::Steps(CrystalsView& v, const MemberData& md) {
    const auto mark_next = [&] {
        for (auto& s : v.steps)
            if (!s.done && !s.stale) {
                s.next = true;
                break;
            }
    };
    // transfer plan
    if (const auto tf = transfers.find(md.id); tf != transfers.end()) {
        auto& t = tf->second;
        v.transfer = true;
        v.transfer_old.name = t.old_name;
        v.transfer_old.icon = t.old_icon;
        v.transfer_new = t.new_name;
        v.transfer_new_icon = t.new_icon;
        const int cur_id = t.kind == 0 ? md.weapon : md.body;
        const Item* cur = FindItem(cur_id);
        const bool on_new = cur && cur->equip == t.new_equip;
        const bool on_old = cur && cur->id == t.old_item;
        v.plan_stale = !on_new && !on_old;
        if (!t.crystals.empty()) {
            if (PieceCrystals(t.old_item).empty())
                t.remove_done = true;
            Step s;
            s.label = "Remove all from " + t.old_name;
            s.sub = t.crystals.size() == 1 ? "The crystal goes back to the bag"
                    : t.crystals.size() == 2 ? "Both crystals go back to the bag"
                                             : "All crystals go back to the bag";
            s.done = t.remove_done;
            v.steps.push_back(s);
        }
        Step e;
        e.label = "Equip " + t.new_name;
        e.sub = std::string(KindName[t.kind]) + " slot";
        e.done = on_new;
        e.stale = v.plan_stale;
        v.steps.push_back(e);
        std::vector<Crystal> have = on_new ? PieceCrystals(cur->id) : std::vector<Crystal>{};
        int slot = 1;
        for (const auto& c : t.crystals) {
            Step s;
            s.insert = true;
            s.crystal = c;
            s.sub = "into " + t.new_name + ", slot " + std::to_string(slot);
            slot += c.size;
            for (auto h = have.begin(); h != have.end(); ++h)
                if (SameKind(*h, c)) {
                    have.erase(h);
                    s.done = true;
                    break;
                }
            s.stale = v.plan_stale;
            v.steps.push_back(s);
        }
        mark_next();
        return;
    }
    // slot plans, per piece
    for (int kind = 0; kind < 2; ++kind) {
        std::vector<const SlotPlan*> ps;
        for (const auto& pl : plans)
            if (pl.member == md.id && pl.kind == kind)
                ps.push_back(&pl);
        if (ps.empty())
            continue;
        std::sort(ps.begin(), ps.end(), [](auto* a, auto* b) { return a->slot < b->slot; });
        const int cur_id = kind == 0 ? md.weapon : md.body;
        const bool stale = cur_id != ps.front()->item;
        v.plan_stale = v.plan_stale || stale;
        const Piece& piece = v.pieces[static_cast<std::size_t>(kind)];
        const std::string pname = piece.present ? piece.name : KindName[kind];
        // crystals in the piece now, with their slot ranges
        struct Range {
            int a, b;
            Crystal c;
        };
        std::vector<Range> cur;
        int j = 0;
        for (const auto& c : PieceCrystals(cur_id)) {
            cur.push_back({j, j + c.size, c});
            j += c.size;
        }
        bool overlap = false;
        std::vector<Crystal> desired;
        std::vector<int> desired_slot;
        for (const auto& r : cur) {
            bool hit = false;
            for (const auto* pl : ps)
                hit = hit || (pl->slot < r.b && pl->slot + pl->c.size > r.a &&
                              !(pl->slot == r.a && SameKind(pl->c, r.c)));
            overlap = overlap || hit;
            if (!hit) {
                desired.push_back(r.c);
                desired_slot.push_back(r.a);
            }
        }
        auto& st = piece_steps[{md.id, kind}];
        if (overlap) {
            if (cur.empty())
                st.remove_done = true;
            Step s;
            s.label = "Remove all from " + pname;
            s.done = st.remove_done;
            s.stale = stale;
            v.steps.push_back(s);
        }
        const std::size_t kept = desired.size();
        for (const auto* pl : ps) {
            desired.push_back(pl->c);
            desired_slot.push_back(pl->slot);
        }
        std::vector<Crystal> have;
        for (const auto& r : cur)
            have.push_back(r.c);
        for (std::size_t i = 0; i < desired.size(); ++i) {
            if (!overlap && i < kept)
                continue; // untouched crystals stay where they are
            Step s;
            s.insert = true;
            s.crystal = desired[i];
            s.sub = "into " + pname + ", slot " + std::to_string(desired_slot[i] + 1);
            if (!overlap || st.remove_done)
                for (auto h = have.begin(); h != have.end(); ++h)
                    if (SameKind(*h, desired[i])) {
                        have.erase(h);
                        s.done = true;
                        break;
                    }
            s.stale = stale;
            v.steps.push_back(s);
        }
    }
    mark_next();
}

ce_page_crystals::CrystalsView Reader::Build() {
    CrystalsView v;
    v.read_only = mirror_equip;
    v.smith = mirror_smith;
    if (mirror_equip && menu_member_id >= 0) // follow the native Equipment member
        for (const auto& md : members)
            if (md.id == menu_member_id && sel_member_id != menu_member_id) {
                sel_member_id = menu_member_id;
                piece_sel = -1;
                if (mode == TransferPick)
                    mode = Browse;
            }
    int sel = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        v.members.push_back({members[i].id, members[i].name, members[i].sprite});
        if (members[i].id == sel_member_id)
            sel = static_cast<int>(i);
    }
    v.sel = sel;
    sel_member_id = members[static_cast<std::size_t>(sel)].id;
    const MemberData& md = members[static_cast<std::size_t>(sel)];
    // inventory list: the bag, then the inserted crystals (with their wearer's sprite)
    struct Entry {
        u64 ptr;
        const Crystal* c;
        bool in_bag;
    };
    std::vector<Entry> all;
    for (const auto& b : bag)
        all.push_back({b.ptr, &b.c, true});
    for (const auto& i : inserted)
        all.push_back({i.ptr, &i.c, false});
    v.total = static_cast<int>(all.size());
    std::vector<Entry> list;
    for (const auto& e : all) {
        const Crystal& c = *e.c;
        if (chip >= 0 && c.cat != chip)
            continue;
        bool keep = true;
        switch (filter) {
        case 1: case 2: case 3: keep = c.size == filter; break;
        case 4: keep = c.rank == 1; break;
        case 5: keep = c.rank == 2; break;
        case 6: keep = c.rank >= 3; break;
        case 7: keep = e.in_bag && (c.art || c.purity > 0); break;
        case 8: keep = e.in_bag; break;
        case 9: keep = !e.in_bag; break;
        case 10: keep = c.art; break;
        case 11: keep = !c.art; break;
        default: break;
        }
        if (keep)
            list.push_back(e);
    }
    std::stable_sort(list.begin(), list.end(), [&](const Entry& a, const Entry& b) {
        const Crystal &x = *a.c, &y = *b.c;
        switch (sort) {
        case 1:
            if (x.purity != y.purity)
                return x.purity > y.purity;
            if (x.rank != y.rank)
                return x.rank > y.rank;
            return x.name < y.name;
        case 2:
            if (x.size != y.size)
                return x.size > y.size;
            if (x.rank != y.rank)
                return x.rank > y.rank;
            return x.name < y.name;
        case 3:
            if (x.name != y.name)
                return x.name < y.name;
            if (x.rank != y.rank)
                return x.rank > y.rank;
            return x.purity > y.purity;
        default:
            if (x.rank != y.rank)
                return x.rank > y.rank;
            if (x.purity != y.purity)
                return x.purity > y.purity;
            return x.name < y.name;
        }
    });
    v.shown = static_cast<int>(list.size());
    v.rows = (v.shown + CrGridCols - 1) / CrGridCols;
    first_row = std::clamp(first_row, 0, std::max(0, v.rows - CrGridRows));
    v.first_row = first_row;
    v.chip = chip, v.sort = sort, v.filter = filter, v.mode = mode;
    const bool listed = std::any_of(list.begin(), list.end(), [&](const Entry& e) { return e.ptr == selected_ptr; });
    if ((!selected_set || !listed) && !list.empty()) {
        selected_ptr = list.front().ptr;
        selected_set = true;
    }
    for (const auto& e : list)
        if (e.ptr == selected_ptr) {
            v.detail = *e.c;
            v.has_detail = true;
        }
    grid_ptrs.clear();
    grid_cells.clear();
    for (int i = first_row * CrGridCols; i < v.shown && static_cast<int>(v.grid.size()) < CrGridCols * CrGridRows; ++i) {
        const auto& e = list[static_cast<std::size_t>(i)];
        Crystal c = *e.c;
        c.key = static_cast<int>(v.grid.size());
        const bool is_sel = e.ptr == selected_ptr;
        if (is_sel)
            v.selected = static_cast<int>(v.grid.size());
        grid_ptrs.push_back(e.ptr);
        grid_cells.push_back(CellId(c) * 2 + (is_sel ? 1 : 0) + 1);
        v.grid.push_back(std::move(c));
    }
    for (int k = 0; k < 4; ++k)
        v.pieces[static_cast<std::size_t>(k)] = BuildPiece(md, k, v);
    if (piece_sel >= 0 && !v.pieces[static_cast<std::size_t>(piece_sel)].present)
        piece_sel = -1;
    Steps(v, md);
    // transfer targets: owned equipment of the same kind the member can wear, one cell per item
    target_equips.clear();
    if (mode == TransferPick && piece_sel >= 0) {
        const auto* t120 = art.Table(120);
        const int slot_col = ColumnIndex(t120, "slot"), type_col = ColumnIndex(t120, "type");
        std::map<int, Target> by_equip;
        std::vector<int> order_v;
        const int cur = v.pieces[static_cast<std::size_t>(piece_sel)].equip;
        for (const auto& [id, e] : items) {
            const int row = RowOf(t120, e.equip);
            if (row < 0 || e.equip == cur)
                continue;
            if (ce_parse::Leading(CellOf(t120, row, slot_col)).value_or(0) != piece_sel)
                continue;
            const int type = ce_parse::Leading(CellOf(t120, row, type_col)).value_or(0);
            if (!md.allowed.empty() && std::find(md.allowed.begin(), md.allowed.end(), type) == md.allowed.end())
                continue;
            auto [itx, fresh] = by_equip.try_emplace(e.equip);
            if (fresh) {
                itx->second.equip = e.equip;
                itx->second.name = Tr("tr_EQUIPS_NAME", 120, e.equip, {}, 1);
                itx->second.icon = TypeIcon(type);
                order_v.push_back(e.equip);
            }
            ++itx->second.count;
            if (e.by != 99 && itx->second.owner.empty())
                for (const auto& [mid, spr] : member_sprites)
                    if (mid == e.by)
                        itx->second.owner = spr;
        }
        std::sort(order_v.begin(), order_v.end(), [&](int a, int b) { return a > b; }); // newest tiers first
        for (const int eq : order_v) {
            v.targets.push_back(by_equip[eq]);
            target_equips.push_back(eq);
        }
    }
    // fusible pairs (bag only): same property, base artificial or purity > 0, fuse natural
    if (mode == Fusible || mirror_smith) {
        std::set<std::string> seen;
        for (const auto& a : bag) {
            if (!a.c.art && a.c.purity <= 0)
                continue;
            for (const auto& b : bag) {
                if (&a == &b || b.c.art || b.c.prop != a.c.prop)
                    continue;
                char k[96];
                std::snprintf(k, sizeof k, "%d/%d/%d/%d/%d|%d/%d/%d", a.c.prop, a.c.rank, a.c.purity, a.c.size, a.c.art,
                              b.c.rank, b.c.purity, b.c.size);
                if (!seen.insert(k).second)
                    continue;
                Pair p{a.c, b.c, false};
                p.current = smith_base && SameKind(base, a.c) && (!smith_fuse || SameKind(fuse, b.c));
                v.pairs.push_back(std::move(p));
            }
        }
        std::stable_sort(v.pairs.begin(), v.pairs.end(), [](const Pair& x, const Pair& y) {
            if (x.current != y.current)
                return x.current;
            if (x.base.name != y.base.name)
                return x.base.name < y.base.name;
            if (x.base.rank + x.fuse.rank != y.base.rank + y.fuse.rank)
                return x.base.rank + x.fuse.rank > y.base.rank + y.fuse.rank;
            return x.base.purity > y.base.purity;
        });
    }
    v.has_base = smith_base, v.has_fuse = smith_fuse, v.has_result = smith_result;
    v.base = base, v.fuse = fuse, v.result = result;
    // tap tables for PublishFor
    int acc_y = 0, emblem_y = 0;
    std::array<int, 2> item_y{};
    const auto boxes = SlotBoxes(v, acc_y, emblem_y, item_y);
    slot_rows.assign(8, {-1, -1});
    slot_y.assign(8, 0);
    std::size_t bi = 0;
    for (int k = 0; k < 2; ++k) {
        const auto& rows = v.pieces[static_cast<std::size_t>(k)].rows;
        for (std::size_t j = 0; j < rows.size(); ++j, ++bi)
            if (rows[j].state != 2 && j < 4) {
                slot_rows[static_cast<std::size_t>(k * 4) + j] = {k, static_cast<int>(j)};
                slot_y[static_cast<std::size_t>(k * 4) + j] = boxes[bi].y;
            }
        piece_y[static_cast<std::size_t>(k)] = item_y[static_cast<std::size_t>(k)];
        piece_ok[static_cast<std::size_t>(k)] = v.pieces[static_cast<std::size_t>(k)].present;
    }
    rows_total = v.rows;
    return v;
}

int Reader::CellId(const Crystal& c) {
    char b[128];
    std::snprintf(b, sizeof b, "%d/%d/%d/%d/%d|", c.cat, c.size, c.rank, c.purity, c.art);
    const std::string k = b + c.owner;
    std::scoped_lock lock{mutex};
    if (const auto it = cell_ids.find(k); it != cell_ids.end())
        return it->second;
    const int id = static_cast<int>(cell_ids.size());
    cell_ids.emplace(k, id);
    cells[id] = c;
    return id;
}

std::string Reader::Store(const CrystalsView& v) {
    const std::string sig = Signature(v) + std::string{ce_draw::PictureStamp};
    const std::string k =
        "module:ce:cr:" + Hex(dsmod_sdk::Fnv1a64(reinterpret_cast<const u8*>(sig.data()), sig.size()));
    std::scoped_lock lock{mutex};
    if (!models.contains(k)) {
        models.emplace(k, v);
        order.push_back(k);
        while (order.size() > ModelKeep) {
            models.erase(order.front());
            order.pop_front();
        }
    }
    return k;
}

void Reader::Sample(const EdenDsmodHostApi& host, bool in_game) {
    try {
        if (in_game && sample % MenuEvery == 0) {
            Mem m{host, class_names};
            const u64 slot = m.Ptr(host.main_base + SlotGameManager);
            const u64 sf = Mem::Ok(slot) && m.CStr(m.Ptr(slot + 0x10)) == "GameManager" ? m.Ptr(slot + 0xB8) : 0;
            if (Mem::Ok(sf) && std::fabs(m.Get<float>(sf + 0x48) - 1.41f) < 0.005f) {
                const bool before = Mirror();
                const int member_before = menu_member_id;
                ReadMenus(m, sf);
                if (Mirror() != before || menu_member_id != member_before)
                    refresh_now = true;
            } else {
                mirror_equip = mirror_smith = false;
            }
        }
        if (!in_game) {
            mirror_equip = mirror_smith = false;
        }
        const int every = active || Mirror() ? FastEvery : SlowEvery;
        if (in_game && (sample % every == 0 || refresh_now || !have)) {
            refresh_now = false;
            if (Refresh(host)) {
                view = Build();
                int done = 0;
                for (const auto& st : view.steps)
                    done += st.done ? 1 : 0;
                if (done > last_done && view.steps.size() == last_steps)
                    ++tick_seq; // a plan step ticked itself
                last_done = done;
                last_steps = view.steps.size();
                key = Store(view);
            }
            // debug line on a change of the counts / menu state only
            const std::array<long long, 13> mark{
                static_cast<long long>(members.size()), static_cast<long long>(bag.size()),
                static_cast<long long>(inserted.size()), static_cast<long long>(items.size()), last_menu_pos,
                menu_member_id, mirror_equip, mirror_smith, smith_base, smith_fuse, smith_result,
                static_cast<long long>(plans.size()), static_cast<long long>(transfers.size())};
            if (host.log && (!logged || mark != log_mark)) {
                log_mark = mark;
                logged = true;
                char b[200];
                std::snprintf(b, sizeof b,
                              "CE crystals: members=%zu bag=%zu inserted=%zu items=%zu menuPos=%d member=%d equip=%d smith=%d "
                              "base=%d fuse=%d result=%d plans=%zu transfers=%zu",
                              members.size(), bag.size(), inserted.size(), items.size(), last_menu_pos,
                              menu_member_id, mirror_equip ? 1 : 0, mirror_smith ? 1 : 0, smith_base ? 1 : 0, smith_fuse ? 1 : 0,
                              smith_result ? 1 : 0, plans.size(), transfers.size());
                host.log(host.userdata, EDEN_DSMOD_LOG_DEBUG, b);
            }
        }
    } catch (...) {
    }
    ++sample;
}

std::string Reader::Key() {
    return have ? key : std::string{};
}

void Reader::PublishFor(const EdenDsmodHostApi& h, const std::string& shown, bool loading) {
    active = shown.starts_with("module:ce:cr:");
    if (!h.publish_i64 || !active || loading || !have)
        return;
    // the gates follow the newest model (the shown picture lags it by one decode at most)
    const auto pi = [&](const std::string& n, s64 v) { h.publish_i64(h.userdata, n.c_str(), v); };
    const CrystalsView& v = view;
    pi("ce.cr.on", 1);
    pi("ce.cr.back", 1);
    const int n = static_cast<int>(v.members.size());
    for (int i = 0; i < n && i < CrMaxMembers; ++i)
        pi("ce.cr.m" + std::to_string(n) + "_" + std::to_string(i), 1);
    if (v.smith)
        return;
    if (v.mode == Filter) {
        for (int i = 0; i < 12; ++i)
            pi("ce.cr.f" + std::to_string(i), 1);
        return;
    }
    if (v.mode == TransferPick) {
        for (std::size_t i = 0; i < v.targets.size() && i < static_cast<std::size_t>(CrPickCols * CrPickRows); ++i)
            pi("ce.cr.t" + std::to_string(i), 1);
        return;
    }
    pi("ce.cr.swipe", 1);
    if (v.transfer && !v.read_only) {
        pi("ce.cr.b2", 1); // Clear plan
        return;
    }
    if (v.mode == Fusible) {
        if (!v.read_only)
            pi("ce.cr.b2", 1);
        return;
    }
    pi("ce.cr.chip", 1);
    for (std::size_t i = 0; i < grid_cells.size(); ++i) {
        pi("ce.cr.g" + std::to_string(i), grid_cells[i]);
        pi("ce.cr.p" + std::to_string(i), static_cast<s64>(i));
        pi("ce.cr.gv" + std::to_string(i), 1);
    }
    if (rows_total > CrGridRows) {
        pi("ce.cr.su", 1);
        pi("ce.cr.sd", 1);
    }
    if (v.read_only)
        return;
    for (std::size_t k = 0; k < slot_rows.size(); ++k)
        if (slot_rows[k].first >= 0) {
            pi("ce.cr.s" + std::to_string(k), 1);
            pi("ce.cr.sy" + std::to_string(k), slot_y[k]);
        }
    for (int p = 0; p < 2; ++p)
        if (piece_ok[static_cast<std::size_t>(p)]) {
            pi("ce.cr.pc" + std::to_string(p), 1);
            pi("ce.cr.py" + std::to_string(p), piece_y[static_cast<std::size_t>(p)]);
        }
    pi("ce.cr.b0", 1);
    pi("ce.cr.b1", 1);
    pi("ce.cr.b2", 1);
}

bool Reader::Action(std::string_view name, s64 argument) {
    if (!name.starts_with("cr_") || !have)
        return false;
    const bool ro = mirror_equip || mirror_smith;
    const auto done = [&] {
        refresh_now = true;
        return true;
    };
    const auto member_index = [&] {
        for (std::size_t i = 0; i < members.size(); ++i)
            if (members[i].id == sel_member_id)
                return static_cast<int>(i);
        return 0;
    };
    const auto set_member = [&](int i) {
        if (members.empty())
            return;
        i = (i % static_cast<int>(members.size()) + static_cast<int>(members.size())) % static_cast<int>(members.size());
        sel_member_id = members[static_cast<std::size_t>(i)].id;
        piece_sel = -1;
        if (mode == TransferPick)
            mode = Browse;
    };
    if (name == "cr_back") {
        if (mode != Browse && !ro) {
            mode = Browse;
            return done();
        }
        piece_sel = -1;
        if (go_home)
            go_home();
        return done();
    }
    if (name == "cr_member") {
        if (argument < 0 || argument >= static_cast<s64>(members.size()))
            return false;
        set_member(static_cast<int>(argument));
        return done();
    }
    if (name == "cr_next" || name == "cr_prev") {
        set_member(member_index() + (name == "cr_next" ? 1 : -1));
        return done();
    }
    if (name == "cr_chip") {
        if (argument < 0 || argument > 4)
            return false;
        chip = static_cast<int>(argument) - 1;
        first_row = 0;
        return done();
    }
    if (name == "cr_sel") {
        if (argument < 0 || static_cast<std::size_t>(argument) >= grid_ptrs.size())
            return false;
        selected_ptr = grid_ptrs[static_cast<std::size_t>(argument)];
        selected_set = true;
        return done();
    }
    if (name == "cr_scroll") {
        first_row += argument > 0 ? 1 : -1;
        return done();
    }
    if (ro)
        return false; // plan editing pauses while the native menu has focus
    if (name == "cr_piece") {
        if (argument < 0 || argument > 1 || !piece_ok[static_cast<std::size_t>(argument)])
            return false;
        piece_sel = piece_sel == argument ? -1 : static_cast<int>(argument);
        return done();
    }
    if (name == "cr_filt") {
        if (argument < 0 || argument > 11)
            return false;
        filter = static_cast<int>(argument);
        first_row = 0;
        mode = Browse;
        return done();
    }
    if (name == "cr_tgt") {
        if (mode != TransferPick || piece_sel < 0 || argument < 0 ||
            static_cast<std::size_t>(argument) >= target_equips.size())
            return false;
        const MemberData& md = members[static_cast<std::size_t>(member_index())];
        const Piece& old = view.pieces[static_cast<std::size_t>(piece_sel)];
        Transfer t;
        t.kind = piece_sel;
        t.old_item = old.item;
        t.old_name = old.name;
        t.old_icon = old.icon;
        t.new_equip = target_equips[static_cast<std::size_t>(argument)];
        for (const auto& tg : view.targets)
            if (tg.equip == t.new_equip) {
                t.new_name = tg.name;
                t.new_icon = tg.icon;
            }
        t.crystals = PieceCrystals(old.item);
        transfers[md.id] = std::move(t);
        // a transfer replaces this member's slot plans for that piece
        std::erase_if(plans, [&](const SlotPlan& p) { return p.member == md.id && p.kind == piece_sel; });
        piece_sel = -1;
        mode = Browse;
        return done();
    }
    if (name == "cr_btn") {
        const MemberData& md = members[static_cast<std::size_t>(member_index())];
        const bool has_plan = transfers.contains(md.id) ||
                              std::any_of(plans.begin(), plans.end(), [&](const SlotPlan& p) { return p.member == md.id; });
        if (transfers.contains(md.id)) {
            if (argument != 2)
                return false;
            transfers.erase(md.id); // Clear plan (local only, no confirm)
            return done();
        }
        if (mode == Fusible) {
            if (argument != 2)
                return false;
            mode = Browse;
            return done();
        }
        switch (argument) {
        case 0:
            mode = Filter;
            return done();
        case 1:
            if (has_plan) {
                std::erase_if(plans, [&](const SlotPlan& p) { return p.member == md.id; });
                piece_steps.erase({md.id, 0});
                piece_steps.erase({md.id, 1});
            } else {
                sort = (sort + 1) % 4;
            }
            return done();
        case 2:
            if (piece_sel >= 0)
                mode = TransferPick;
            else
                mode = Fusible;
            return done();
        default:
            return false;
        }
    }
    // planning: a slot tap (tap-tap) or a drop (payload = grid cell)
    const bool drop = name.starts_with("cr_drop");
    if (name == "cr_slot" || drop) {
        const auto parsed = drop ? ce_parse::Index(name.substr(7)) : std::optional<int>{};
        if ((drop && !parsed) || (!drop && (argument < 0 ||
            static_cast<std::uint64_t>(argument) >= slot_rows.size())))
            return false;
        const int k = drop ? *parsed : static_cast<int>(argument);
        if (k < 0 || static_cast<std::size_t>(k) >= slot_rows.size() || slot_rows[static_cast<std::size_t>(k)].first < 0)
            return false;
        const auto [kind, slot0] = slot_rows[static_cast<std::size_t>(k)];
        const MemberData& md = members[static_cast<std::size_t>(member_index())];
        if (transfers.contains(md.id) && transfers.at(md.id).kind == kind)
            return false;
        const Piece& p = view.pieces[static_cast<std::size_t>(kind)];
        if (!p.present)
            return false;
        int slot = slot0;
        while (slot > 0 && p.rows[static_cast<std::size_t>(slot)].state == 3)
            --slot;
        u64 src = selected_ptr;
        if (drop) {
            if (argument < 0 || static_cast<std::size_t>(argument) >= grid_ptrs.size())
                return false;
            src = grid_ptrs[static_cast<std::size_t>(argument)];
            selected_ptr = src;
            selected_set = true;
        } else {
            // tap on a planned slot (no new crystal for it) clears that plan entry
            const auto it = std::find_if(plans.begin(), plans.end(), [&](const SlotPlan& pl) {
                return pl.member == md.id && pl.kind == kind && pl.slot == slot;
            });
            if (it != plans.end() && (it->src == src || !src)) {
                plans.erase(it);
                return done();
            }
        }
        const BagCrystal* c = nullptr;
        for (const auto& b : bag)
            if (b.ptr == src)
                c = &b;
        if (!c || c->c.rank < 3)
            return false; // only bag crystals of rank III+ (the smith's Set list) are planned
        int unlocked = 0;
        for (const auto& r : p.rows)
            unlocked += r.state != 2 ? 1 : 0;
        if (slot + c->c.size > unlocked)
            return false; // "This item has no free slots left!"
        std::erase_if(plans, [&](const SlotPlan& pl) {
            return pl.src == src || (pl.member == md.id && pl.kind == kind && pl.slot < slot + c->c.size &&
                                     pl.slot + pl.c.size > slot);
        });
        plans.push_back({md.id, kind, slot, p.item, src, c->c});
        piece_steps.erase({md.id, kind});
        return done();
    }
    return false;
}

std::optional<ce_page_crystals::CrystalsView> Reader::Model(const std::string& k) {
    std::scoped_lock lock{mutex};
    if (const auto it = models.find(k); it != models.end())
        return it->second;
    return std::nullopt;
}

std::optional<ce_draw::Image> Reader::CellImage(std::string_view arg) {
    const auto parsed = ce_parse::Index(arg);
    if (!parsed || *parsed == 0)
        return std::nullopt;
    const int v = *parsed - 1;
    Crystal c;
    {
        std::scoped_lock lock{mutex};
        const auto it = cells.find(v >> 1);
        if (it == cells.end())
            return std::nullopt;
        c = it->second;
    }
    return ce_page_crystals::ComposeCell(art, c, (v & 1) != 0);
}

} // namespace ce_crystal
