// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_skill_reader.h"

#include "ce_draw.h"
#include "ce_parse.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ce_skills {
namespace {

// TypeInfo slots, main-relative (1.41 only); see contracts/field-readers.md
constexpr u64 SlotGameManager = 0x4930BB8; // direct
constexpr u64 SlotBattle = 0x492F298;      // direct
constexpr u64 SlotSaveDb = 0x4AFB318;      // usage: GetData, statics +0 = SaveDataOwn
constexpr u64 SlotMainMenu = 0x4AFE258;    // usage: MainMenu, statics +0 = the menu GameObject

constexpr int MenuEvery = 6;  // 10 Hz: native menu tab / member
constexpr int DataEvery = 60;       // 1 Hz: skill list, members, boosters
constexpr int DataEveryMirror = 20; // 3 Hz while the native Skills menu is open (plan ticks)
constexpr std::size_t ModelKeep = 24;
constexpr u64 DoneShow = 480; // a ticked plan entry stays visible 8 s

// MainMenuSkills.FillLearnSkillList (0x1CDFCF0): the learn grid's lower rows unlock at 9 / 18 / 25
// learned skills (character skills + learned Stat Boosters); member 10 is exempt
constexpr std::array<int, 4> TierNeed{0, 9, 18, 25};
constexpr int RowsPerTier = 4;

// MainMenuSkills.ReturnSkillType (0x1CD8900) type names as the Set Skills screen shows them
constexpr const char* TypeName[7] = {"", "Attack", "Heal", "Buff", "Debuff", "Utility", "Magic"};

std::string Hex(u64 v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

/// The CE font has no typographic quotes; the tables use them in a few names.
std::string Plain(std::string s) {
    for (const char* q : {"\xE2\x80\x99", "\xE2\x80\x98"})
        for (std::size_t p = s.find(q); p != std::string::npos; p = s.find(q, p + 1))
            s.replace(p, 3, "'");
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

std::string SpriteFor(std::string name) {
    name.erase(std::remove(name.begin(), name.end(), '\''), name.end());
    if (name == "BaThraz")
        return "Bathraz";
    return name;
}

/// Table 122 stat names -> the mockup's short labels
std::string StatShort(std::string_view stat) {
    constexpr std::pair<std::string_view, std::string_view> map[] = {
        {"Health Points", "HP"}, {"Tech Points", "TP"}, {"Attack", "ATK"}, {"Magic", "MAG"},
        {"Defense", "DEF"},      {"Mind", "MND"},       {"Agility", "AGI"}, {"Critical %", "Crit"}};
    for (const auto& [a, b] : map)
        if (stat == a)
            return std::string{b};
    return std::string{stat};
}

int Int(const std::vector<std::string>& r, std::size_t i, int fallback = 0) {
    if (i >= r.size())
        return fallback;
    return ce_parse::Leading(r[i]).value_or(fallback);
}

} // namespace

// ---- guest memory --------------------------------------------------------------------------------

struct Reader::Mem {
    const EdenDsmodHostApi& h;
    std::unordered_map<u64, std::string>& names;

    bool Read(u64 at, void* out, std::size_t n) const {
        return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(h, at, out, n);
    }
    template <class T>
    T Get(u64 at, bool* ok = nullptr) const {
        T v{};
        const bool r = Read(at, &v, sizeof v);
        if (ok)
            *ok = r;
        return r ? v : T{};
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
        for (std::size_t off = 0; off < sizeof buf; off += 16) {
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
    /// List<T> items (validated count / capacity); empty when the list is longer than `limit`.
    std::vector<u64> List(u64 list, std::size_t limit) const {
        std::vector<u64> out;
        if (!Ok(list))
            return out;
        const u64 items = Ptr(list + 0x10);
        const s32 n = Get<s32>(list + 0x18);
        if (n <= 0 || static_cast<std::size_t>(n) > limit || !Ok(items))
            return out;
        if (Ptr(items + 0x18) < static_cast<u64>(n))
            return out;
        out.resize(static_cast<std::size_t>(n));
        if (!Read(items + 0x20, out.data(), out.size() * 8))
            out.clear();
        return out;
    }
    u64 Statics(u64 slot, std::string_view name, bool usage) {
        u64 k = Ptr(h.main_base + slot);
        if (usage)
            k = Ok(k) ? Ptr(k) : 0;
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

// ---- reader ----------------------------------------------------------------------------------------

Reader::Reader(ce_draw::Art& a) : art{a} {}

void Reader::LoadTables() {
    if (tables)
        return;
    tables = true;
    if (const auto* t = art.Table(123))
        for (std::size_t i = 1; i < t->size(); ++i) {
            const auto& r = (*t)[i];
            if (r.size() < 21 || r[0].empty() || !std::isdigit(static_cast<unsigned char>(r[0][0])))
                continue;
            SkillRow s;
            s.name = Plain(r[1]);
            s.user = Int(r, 3, -1);
            s.cost[0] = Int(r, 5), s.cost[1] = Int(r, 6), s.cost[2] = Int(r, 7);
            s.type = Int(r, 8);
            s.sp2 = Int(r, 10), s.sp3 = Int(r, 11);
            s.order = Int(r, 20);
            t123[Int(r, 0)] = std::move(s);
        }
    if (const auto* t = art.Table(117))
        for (std::size_t i = 1; i < t->size(); ++i) {
            const auto& r = (*t)[i];
            if (r.size() < 7 || r[0].empty() || !std::isdigit(static_cast<unsigned char>(r[0][0])))
                continue;
            t117[Int(r, 0)] = {Plain(r[1]), Int(r, 4), Int(r, 5), Int(r, 6), Int(r, 3)};
        }
    if (const auto* t = art.Table(122))
        for (std::size_t i = 1; i < t->size(); ++i) {
            const auto& r = (*t)[i];
            if (r.size() < 4 || r[0].empty() || !std::isdigit(static_cast<unsigned char>(r[0][0])))
                continue;
            t122[{Int(r, 3), Int(r, 0)}] = StatShort(r[1]) + "+" + r[2];
        }
}

void Reader::ReadMenu(Mem& m, u64 gm) {
    menu_open = m.Get<u8>(gm + 0xF1) != 0;
    menu_pos = -1;
    menu_member = -1;
    if (!menu_open)
        return;
    const u64 sf = m.Statics(SlotMainMenu, "MainMenu", true);
    const u64 mc = sf ? m.Component(m.Ptr(sf), "MainMenu") : 0;
    if (!mc)
        return;
    menu_pos = m.Get<s32>(mc + 0x100);
    // the Set Skills / Learn Skills screens (menuPos 2xx) show pm[currentActiveSlot]
    if (menu_pos >= 200 && menu_pos < 300) {
        const s32 slot = m.Get<s32>(mc + 0xE8);
        const u64 pm = m.Ptr(mc + 0xF8);
        const u64 arr = Mem::Ok(pm) ? m.Ptr(pm + 0x10) : 0;
        const s32 n = Mem::Ok(pm) ? m.Get<s32>(pm + 0x18) : 0;
        if (slot >= 0 && slot < 16 && slot < n && Mem::Ok(arr) && m.Get<u64>(arr + 0x18) > static_cast<u64>(slot)) {
            const u64 p = m.Ptr(arr + 0x20 + 8 * static_cast<u64>(slot));
            if (m.ClassName(p) == "PartyMember")
                menu_member = m.Get<s32>(p + 0x10);
        }
    }
}

bool Reader::Refresh(const EdenDsmodHostApi& host) {
    Mem m{host, class_names};
    const u64 gm = m.Statics(SlotGameManager, "GameManager", false);
    if (!gm || !(std::fabs(m.Get<float>(gm + 0x48) - 1.41f) <= 0.005f)) // NaN fails too
        return false;
    if (const u64 sb = m.Statics(SlotBattle, "Battle", false);
        sb && m.Get<u8>(sb + 0x4D) && !m.Get<u8>(sb + 0x4E))
        return false; // battle: keep the model as it was
    ReadMenu(m, gm);
    const bool data_due = sample % (mirror ? DataEveryMirror : DataEvery) == 0 || refresh_now || !have;
    if (!data_due)
        return true;
    const u64 slot = m.Ptr(host.main_base + SlotSaveDb);
    const u64 kl = Mem::Ok(slot) ? m.Ptr(slot) : 0;
    if (!Mem::Ok(kl) || m.CStr(m.Ptr(kl + 0x10)) != "GetData")
        return false;
    const u64 sfd = m.Ptr(kl + 0xB8);
    const u64 sd = Mem::Ok(sfd) ? m.Ptr(sfd) : 0;
    if (m.ClassName(sd) != "SaveDataOwn")
        return false;
    LoadTables();

    // members: available characters in the formation (posInParty 0-7), native order
    std::vector<MemberData> mem;
    for (const u64 p : m.List(m.Ptr(sd + 0x40), 64)) {
        if (m.ClassName(p) != "PartyMember")
            continue;
        MemberData d;
        d.id = m.Get<s32>(p + 0x10);
        d.pos = m.Get<s32>(p + 0x94);
        if (d.id < 0 || d.id >= 100 || d.pos < 0 || d.pos > 7 || !m.Get<u8>(p + 0x110))
            continue;
        d.name = m.String(m.Ptr(p + 0x18));
        d.emblem = m.Get<s32>(p + 0xE8);
        d.sp = m.Get<s32>(p + 0x108);
        d.gs = m.Get<float>(p + 0x10C);
        if (d.name.empty() || d.sp < 0 || d.sp > 99999 || !std::isfinite(d.gs) || d.gs < 0 || d.gs > 999)
            continue;
        const u64 arr = m.Ptr(p + 0x100);
        const s32 n = Mem::Ok(arr) ? m.Get<s32>(arr + 0x18) : 0;
        if (n > 0 && n <= 64) {
            d.learnable.resize(static_cast<std::size_t>(n));
            if (!m.Read(arr + 0x20, d.learnable.data(), d.learnable.size() * 4))
                d.learnable.clear();
        }
        mem.push_back(std::move(d));
    }
    std::sort(mem.begin(), mem.end(), [](const auto& a, const auto& b) { return a.pos < b.pos; });
    if (mem.empty())
        return false;

    // skill list
    std::vector<Item> it;
    u64 item_klass = 0;
    for (const u64 s : m.List(m.Ptr(sd + 0x48), 8192)) {
        // one read per SkillItem: klass +0, fields +0x10..+0x29
        u8 raw[0x2A]{};
        if (!Mem::Ok(s) || !m.Read(s, raw, sizeof raw))
            continue;
        u64 klass;
        std::memcpy(&klass, raw, 8);
        if (klass != item_klass) {
            if (m.ClassName(s) != "SkillItem")
                continue;
            item_klass = klass;
        }
        s32 f[6];
        std::memcpy(f, raw + 0x10, sizeof f);
        Item x{f[0], f[1], f[2], f[3], f[4], f[5], raw[0x28] != 0, raw[0x29] != 0};
        if (x.type < 0 || x.type > 1 || x.lvl < 1 || x.lvl > 3 || x.slot < 0 || x.slot > 8 || x.sp < 0)
            continue;
        it.push_back(x);
    }
    // stat boosters
    std::map<std::pair<int, int>, bool> bo;
    for (const u64 b : m.List(m.Ptr(sd + 0x58), 4096)) {
        if (m.ClassName(b) != "StatBoost")
            continue;
        const s32 pos = m.Get<s32>(b + 0x10), user = m.Get<s32>(b + 0x24);
        if (pos >= 0 && pos < 32 && user >= 0 && user < 1000)
            bo[{user, pos}] = m.Get<u8>(b + 0x28) != 0;
    }
    // class emblems
    std::map<int, std::string> em;
    for (const u64 e : m.List(m.Ptr(sd + 0x60), 256))
        if (m.ClassName(e) == "ClassEmblem")
            em[m.Get<s32>(e + 0x10)] = Plain(m.String(m.Ptr(e + 0x18)));

    members = std::move(mem);
    items = std::move(it);
    data_new = true;
    boosts = std::move(bo);
    emblems = std::move(em);
    have = true;
    return true;
}

bool Reader::Available(const Item& it) const {
    // MainMenuSkills.LoadSkills / LoadPassives: a class skill is listed while its emblem is
    // equipped (classEmblemEquipped) or once it reached Lv 3
    return !it.cls || it.cls_eq || it.lvl == 3;
}

ce_page_skills::Skill Reader::MakeSkill(const Item& it, const MemberData& md) const {
    ce_page_skills::Skill s;
    s.present = true;
    s.id = it.id;
    s.lvl = it.lvl;
    int sp2 = 0, sp3 = 0;
    if (it.type == 0) {
        if (const auto r = t123.find(it.id); r != t123.end()) {
            s.name = r->second.name;
            s.icon = r->second.type;
            s.type = r->second.type >= 1 && r->second.type <= 6 ? TypeName[r->second.type] : "";
            s.tp = r->second.cost[std::clamp(it.lvl, 1, 3) - 1];
            sp2 = r->second.sp2, sp3 = r->second.sp3;
        }
    } else if (const auto r = t117.find(it.id); r != t117.end()) {
        s.name = r->second.name;
        s.icon = r->second.icon;
        sp2 = r->second.sp2, sp3 = r->second.sp3;
    }
    if (s.name.empty())
        s.name = "#" + std::to_string(it.id);
    // MainMenuSkills.ReturnSPLeft: Lv1 SP2 - invested, Lv2 SP2 + SP3 - invested, Lv3 0
    if (it.lvl == 1) {
        s.sp_left = std::max(0, sp2 - it.sp);
        s.frac = sp2 > 0 ? std::clamp(static_cast<float>(it.sp) / static_cast<float>(sp2), 0.0f, 1.0f) : 0.0f;
    } else if (it.lvl == 2) {
        s.sp_left = std::max(0, sp2 + sp3 - it.sp);
        s.frac = sp3 > 0 ? std::clamp(static_cast<float>(it.sp - sp2) / static_cast<float>(sp3), 0.0f, 1.0f)
                         : 0.0f;
    } else {
        s.sp_left = 0;
        s.frac = 1.0f;
    }
    // MainMenuSkills.EnoughSPToUpgrade: the owner's SP pool covers the SP left
    s.afford = it.lvl < 3 && s.sp_left <= md.sp;
    return s;
}

void Reader::Build() {
    using namespace ce_page_skills;
    SkillsView v;
    alarm = false;
    if (members.empty())
        return;
    // which member: the native screen's while mirroring, else the companion's own selection
    int show = sel_member;
    if (mirror && menu_member >= 0)
        show = menu_member;
    int sel = 0;
    for (std::size_t i = 0; i < members.size(); ++i)
        if (members[i].id == show)
            sel = static_cast<int>(i);
    if (!mirror)
        sel_member = members[static_cast<std::size_t>(sel)].id;
    // per-member alarm
    for (const auto& md : members) {
        // the native lists: character slots take character skills, the class slots (7-8) also take
        // the available class emblem skills
        bool spare_a = false, spare_ac = false, spare_p = false, spare_pc = false;
        std::array<bool, 9> used_a{}, used_p{};
        for (const auto& it : items) {
            if (it.user != md.id || !Available(it))
                continue;
            if (it.slot == 0)
                (it.type == 0 ? (it.cls ? spare_ac : spare_a) : (it.cls ? spare_pc : spare_p)) = true;
            else
                (it.type == 0 ? used_a : used_p)[static_cast<std::size_t>(it.slot)] = true;
        }
        bool empty_a = false, empty_ac = false, empty_p = false, empty_pc = false;
        for (int s = 1; s <= 8; ++s)
            (s >= 7 ? empty_ac : empty_a) = (s >= 7 ? empty_ac : empty_a) || !used_a[static_cast<std::size_t>(s)];
        for (int s : {1, 2, 3, 7, 8})
            (s >= 7 ? empty_pc : empty_p) = (s >= 7 ? empty_pc : empty_p) || !used_p[static_cast<std::size_t>(s)];
        Member m;
        m.id = md.id;
        m.name = md.name;
        m.sprite = SpriteFor(md.name);
        m.alarm = (empty_a && spare_a) || (empty_ac && (spare_a || spare_ac)) || (empty_p && spare_p) ||
                  (empty_pc && (spare_p || spare_pc));
        alarm = alarm || m.alarm;
        v.members.push_back(std::move(m));
    }
    v.sel = sel;
    const MemberData& md = members[static_cast<std::size_t>(sel)];
    v.gs = static_cast<int>(std::floor(md.gs));
    v.sp = md.sp;
    if (md.emblem >= 0)
        if (const auto e = emblems.find(md.emblem); e != emblems.end())
            v.emblem = e->second;
    int learned = 0;
    std::map<int, bool> learned_a, learned_p; // character skills the member knows
    for (const auto& it : items) {
        if (it.user != md.id)
            continue;
        if (!it.cls) {
            ++learned;
            (it.type == 0 ? learned_a : learned_p)[it.id] = true;
        }
        if (!Available(it))
            continue;
        const Skill s = MakeSkill(it, md);
        if (it.type == 0) {
            if (it.slot >= 1 && it.slot <= 6)
                v.actions[static_cast<std::size_t>(it.slot - 1)] = s;
            else if (it.slot >= 7)
                v.class_actions[static_cast<std::size_t>(it.slot - 7)] = s;
        } else {
            if (it.slot >= 1 && it.slot <= 3)
                v.passives[static_cast<std::size_t>(it.slot - 1)] = s;
            else if (it.slot >= 7)
                v.passives[static_cast<std::size_t>(it.slot - 4)] = s;
            else if (it.slot == 0) {
                Skill sp = s;
                sp.cls = it.cls;
                v.spare.push_back(std::move(sp));
            }
        }
    }
    // the native passive list order: by passive type (table 117), then the save's list order
    std::stable_sort(v.spare.begin(), v.spare.end(), [&](const Skill& a, const Skill& b) {
        const auto ta = t117.find(a.id), tb = t117.find(b.id);
        return (ta != t117.end() ? ta->second.type : 99) < (tb != t117.end() ? tb->second.type : 99);
    });
    const bool empty_char = !v.passives[0].present || !v.passives[1].present || !v.passives[2].present;
    const bool empty_class = !v.passives[3].present || !v.passives[4].present;
    for (auto& sp : v.spare) {
        sp.flag = (!sp.cls && (empty_char || empty_class)) || (sp.cls && empty_class);
        v.flag_spare = v.flag_spare || (empty_char && !sp.cls);
        v.flag_class = v.flag_class || empty_class;
    }
    // Stat Booster track and the learn-grid gate
    for (int pos = 0; pos < 32; ++pos) {
        const auto b = boosts.find({md.id, pos});
        if (b == boosts.end())
            continue;
        learned += b->second ? 1 : 0;
    }
    const auto unlocked = [&](int row) {
        const int tier = std::min(3, row / RowsPerTier);
        return md.id == 10 || learned >= TierNeed[static_cast<std::size_t>(tier)];
    };
    for (int pos = 0; pos < 32; ++pos) {
        const auto b = boosts.find({md.id, pos});
        if (b == boosts.end())
            continue;
        Booster bo;
        const auto lab = t122.find({md.id, pos});
        bo.label = lab != t122.end() ? lab->second : "?";
        bo.taken = b->second;
        bo.locked = !bo.taken && !unlocked(pos);
        v.boost_taken += bo.taken ? 1 : 0;
        if (!bo.taken && !bo.locked && v.boost_next.empty())
            v.boost_next = bo.label;
        v.boosters.push_back(std::move(bo));
    }
    for (std::size_t t = 1; t < TierNeed.size(); ++t)
        if (md.id != 10 && learned < TierNeed[t]) {
            v.learn_more = TierNeed[t] - learned;
            break;
        }
    // next learnable skills: the learn grid's action column (123 by learnOrder) and passive column
    // (PartyMember.learnablePassives), row by row, unlocked rows only
    {
        std::vector<std::pair<int, int>> acts; // (order, id)
        for (const auto& [id, r] : t123)
            if (r.user == md.id)
                acts.push_back({r.order, id});
        std::sort(acts.begin(), acts.end());
        const std::size_t rows = std::max(acts.size(), md.learnable.size());
        for (std::size_t r = 0; r < rows && v.learn_next.size() < 2; ++r) {
            if (!unlocked(static_cast<int>(r)))
                break;
            if (r < acts.size() && !learned_a.contains(acts[r].second))
                v.learn_next.push_back(t123[acts[r].second].name);
            if (v.learn_next.size() < 2 && r < md.learnable.size() && !learned_p.contains(md.learnable[r]))
                if (const auto p = t117.find(md.learnable[r]); p != t117.end())
                    v.learn_next.push_back(p->second.name);
        }
        if (v.gs < 1)
            v.learn_next.clear(); // nothing to spend: no "Learn next" line
    }
    // plans: tick entries whose passive is equipped now; drop stale ones
    for (auto& p : plans) {
        if (p.done)
            continue;
        bool known = false, equipped = false;
        for (const auto& it : items)
            if (it.user == p.member && it.type == 1 && it.id == p.id) {
                known = true;
                equipped = equipped || it.slot != 0;
            }
        if (equipped) {
            p.done = true;
            p.done_at = sample;
            ++tick_seq;
        } else if (!known) {
            p.done_at = 1; // the passive left the list: drop
        }
    }
    std::erase_if(plans, [&](const PlanEntry& p) {
        if (!p.done)
            return p.done_at == 1;
        return sample > p.done_at + DoneShow;
    });
    // a pending entry whose slot got filled by another passive no longer applies
    std::erase_if(plans, [&](const PlanEntry& p) {
        if (p.done || p.member != md.id)
            return false;
        return v.passives[static_cast<std::size_t>(p.slot)].present;
    });
    for (const auto& p : plans) {
        if (p.member != md.id)
            continue;
        Plan e;
        e.slot = p.slot;
        e.id = p.id;
        e.done = p.done;
        if (const auto r = t117.find(p.id); r != t117.end())
            e.name = r->second.name, e.icon = r->second.icon;
        v.plans.push_back(std::move(e));
    }
    v.mirror = mirror;
    view = std::move(v);
    view_dirty = true;
    ++build_seq;
}

std::string Reader::KeyFor(const ce_page_skills::SkillsView& v) {
    const std::string sig = ce_page_skills::Signature(v) + std::string{ce_draw::PictureStamp};
    const std::string key =
        "module:ce:sk:" + Hex(dsmod_sdk::Fnv1a64(reinterpret_cast<const u8*>(sig.data()), sig.size()));
    std::scoped_lock lock{mutex};
    if (!models.contains(key)) {
        models.emplace(key, v);
        order.push_back(key);
        while (order.size() > ModelKeep) {
            models.erase(order.front());
            order.pop_front();
        }
    }
    for (const auto& s : v.spare)
        row_info[s.id] = {s.icon, s.name};
    return key;
}

void Reader::Sample(const EdenDsmodHostApi& host, bool in_game) {
    if (in_game && (sample % MenuEvery == 0 || refresh_now)) {
        try {
            const bool forced = refresh_now;
            if (Refresh(host)) {
                refresh_now = false;
                mirror = menu_open && (menu_pos == 2 || (menu_pos >= 200 && menu_pos < 300));
                // rebuild on new data, a menu change, an action or an expiring tick (not every 100 ms)
                const int sig = (mirror ? 1 : 0) | (menu_member + 1) << 1;
                bool expiring = false;
                for (const auto& p : plans)
                    expiring = expiring || (p.done && sample > p.done_at + DoneShow);
                if (data_new || forced || sig != menu_sig || expiring) {
                    menu_sig = sig;
                    data_new = false;
                    Build();
                }
                // debug line on a model, menu or alarm change only (not at every 100 ms refresh)
                const std::array<long long, 4> mark{static_cast<long long>(build_seq), menu_pos,
                                                    alarm ? 1 : 0, mirror ? 1 : 0};
                if (host.log && have && mark != log_mark) {
                    log_mark = mark;
                    char b[200];
                    const auto& v = view;
                    int act = 0, pas = 0;
                    for (const auto& a : v.actions)
                        act += a.present;
                    for (const auto& a : v.class_actions)
                        act += a.present;
                    for (const auto& p : v.passives)
                        pas += p.present;
                    std::snprintf(b, sizeof b,
                                  "CE skills: member=%s gs=%d sp=%d actions=%d/8 passives=%d/5 spare=%zu "
                                  "boost=%d/%zu next=%s more=%d emblem=%s alarm=%d mirror=%d pos=%d plans=%zu",
                                  v.members.empty() ? "-" : v.members[static_cast<std::size_t>(v.sel)].name.c_str(),
                                  v.gs, v.sp, act, pas, v.spare.size(), v.boost_taken, v.boosters.size(),
                                  v.boost_next.c_str(), v.learn_more, v.emblem.c_str(), alarm ? 1 : 0,
                                  mirror ? 1 : 0, menu_pos, v.plans.size());
                    host.log(host.userdata, EDEN_DSMOD_LOG_DEBUG, b);
                }
            }
        } catch (...) {
        }
    }
    ++sample;
}

std::string Reader::Key() {
    if (!have || view.members.empty())
        return {};
    if (view_dirty || view_key.empty()) {
        view_key = KeyFor(view);
        view_dirty = false;
    }
    return view_key;
}

void Reader::PublishFor(const EdenDsmodHostApi& h, const std::string& shown, bool loading) {
    if (!h.publish_i64)
        return;
    const auto pi = [&](const std::string& n, s64 v) { h.publish_i64(h.userdata, n.c_str(), v); };
    if (alarm)
        pi("ce.sk.alarm", 1);
    if (!shown.starts_with("module:ce:sk:") || loading)
        return;
    ce_page_skills::SkillsView v;
    {
        std::scoped_lock lock{mutex};
        const auto it = models.find(shown);
        if (it == models.end())
            return;
        v = it->second;
    }
    pi("ce.sk", 1);
    if (v.mirror)
        return; // read-only mirror: no Back, picker or plan editing while the native menu has focus
    pi("ce.sk.back", 1);
    pi("ce.sk.swipe", 1);
    const int n = static_cast<int>(v.members.size());
    for (int i = 0; i < n && n <= ce_page_skills::geo::SkMaxMembers; ++i)
        if (i != v.sel)
            pi("ce.sk.m" + std::to_string(n) + "_" + std::to_string(i), 1);
    // plan editing: empty passive slots take a spare passive; planned slots clear on tap
    bool any_empty = false;
    for (int s = 0; s < 5; ++s) {
        if (v.passives[static_cast<std::size_t>(s)].present)
            continue;
        bool planned = false;
        for (const auto& p : v.plans)
            planned = planned || (p.slot == s && !p.done);
        pi((planned ? "ce.sk.pl" : "ce.sk.e") + std::to_string(s), 1);
        any_empty = any_empty || !planned;
    }
    if (!any_empty)
        return;
    const bool full = v.spare.size() <= static_cast<std::size_t>(ce_page_skills::SpareFullMax);
    const std::size_t shown_rows =
        full ? v.spare.size()
             : (v.spare.size() > static_cast<std::size_t>(ce_page_skills::SpareGridMax)
                    ? ce_page_skills::SpareGridMax - 1
                    : v.spare.size());
    for (std::size_t i = 0; i < shown_rows; ++i) {
        bool planned = false;
        for (const auto& p : v.plans)
            planned = planned || (p.id == v.spare[i].id && !p.done);
        if (planned)
            continue;
        pi((full ? "ce.sk.la" : "ce.sk.lb") + std::to_string(i), 1);
        pi("ce.sk.pid" + std::to_string(i), v.spare[i].id + 1000);
    }
}

bool Reader::Action(std::string_view name, s64 argument) {
    if (!name.starts_with("sk_"))
        return false;
    if (mirror || view.members.empty())
        return false; // read-only while the native menu has focus
    const int n = static_cast<int>(view.members.size());
    if (name == "sk_member") {
        if (argument < 0 || argument >= n)
            return false;
        sel_member = view.members[static_cast<std::size_t>(argument)].id;
    } else if (name == "sk_next" || name == "sk_prev") {
        const int d = name == "sk_next" ? 1 : n - 1;
        sel_member = view.members[static_cast<std::size_t>((view.sel + d) % n)].id;
    } else if (name.starts_with("sk_plan") && name.size() == 8) {
        const int slot = name[7] - '0';
        const int id = static_cast<int>(argument) - 1000;
        if (slot < 0 || slot > 4 || view.passives[static_cast<std::size_t>(slot)].present)
            return false;
        bool spare = false;
        for (const auto& s : view.spare)
            spare = spare || (s.id == id && (!s.cls || slot >= 3)); // class passives: class slots only
        if (!spare)
            return false;
        const int member = view.members[static_cast<std::size_t>(view.sel)].id;
        std::erase_if(plans, [&](const PlanEntry& p) {
            return p.member == member && !p.done && (p.slot == slot || p.id == id);
        });
        plans.push_back({member, slot, id, false, 0});
    } else if (name.starts_with("sk_unplan") && name.size() == 10) {
        const int slot = name[9] - '0';
        const int member = view.members[static_cast<std::size_t>(view.sel)].id;
        const auto before = plans.size();
        std::erase_if(plans, [&](const PlanEntry& p) { return p.member == member && !p.done && p.slot == slot; });
        if (plans.size() == before)
            return false;
    } else {
        return false;
    }
    Build();
    refresh_now = true;
    return true;
}

std::optional<ce_page_skills::SkillsView> Reader::Model(const std::string& key) {
    std::scoped_lock lock{mutex};
    if (const auto it = models.find(key); it != models.end())
        return it->second;
    return std::nullopt;
}

std::optional<ce_draw::Image> Reader::Row(std::string_view key) {
    // "a:<id>" full row / "b:<id>" compact cell, flagged state (only drawn while draggable)
    if (key.size() < 3 || (key[0] != 'a' && key[0] != 'b') || key[1] != ':')
        return std::nullopt;
    const auto parsed = ce_parse::Index(key.substr(2));
    if (!parsed)
        return std::nullopt;
    int id = *parsed;
    if (id >= 1000)
        id -= 1000; // the widgets format their payload (passive id + 1000)
    ce_page_skills::Skill s;
    {
        std::scoped_lock lock{mutex};
        const auto it = row_info.find(id);
        if (it == row_info.end())
            return std::nullopt;
        s.present = true, s.id = id, s.icon = it->second.first, s.name = it->second.second;
    }
    return ce_page_skills::ComposeSpareRow(art, s, key[0] == 'b');
}

} // namespace ce_skills
