// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ce_field_reader.h"

#include "ce_draw.h"
#include "ce_parse.h"
#include "core/mods/modules/dsmod_module_sdk.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ce_field {
namespace {

// TypeInfo slots, main-relative (1.41 only). Direct = the class pointer; usage = a pointer to the
// class pointer (research fixtures/usage-slot-map-boot1.json).
constexpr u64 SlotGameManager = 0x4930BB8; // direct (contracts/battle-readers.md)
constexpr u64 SlotBattle = 0x492F298;      // direct
constexpr u64 SlotPlayer = 0x4AF4A48;      // usage: Player_TypeInfo
constexpr u64 SlotSaveDb = 0x4AFB318;      // usage: GetData_TypeInfo, statics +0 = SaveDataOwn

// Map.OpenMap scale factors (float bits 0x3D4985F1 / 0x3DF6DDAE)
float BitsToFloat(u32 b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
const float KArea = BitsToFloat(0x3D4985F1);
const float KWorld = BitsToFloat(0x3DF6DDAE);

constexpr int RefreshEvery = 6;   // 10 Hz: position, context
constexpr int SaveEvery = 60;     // 1 Hz: quests, board, explored states
constexpr float MaskSpriteUnits = 0.1f; // reveal mask sprite size (160 map px at scale 1600)
constexpr std::size_t ModelKeep = 32;

// MainMenuSystem.ReturnAreaName (1.41 string literals; the English names the menus show)
constexpr std::pair<std::string_view, std::string_view> AreaNames[] = {
    {"br_tunnel", "Baalrut Tunnel"}, {"lt", "Leviathan's Trench"}, {"sw", "Sandworm"},
    {"emf", "Mushroom Forest"}, {"km", "Kindreld Monastary"}, {"kmr", "Kortara Mountain Range"},
    {"fp", "Dancing City of Farnsport"}, {"tpf", "Eternal Flame Temple"}, {"bf", "Wyrnshire Castle"},
    {"edl", "Deadlands"}, {"gr", "Grave of Reina"}, {"va_isleOfMessages", "Isle of Messages"},
    {"va", "Somewhere in Valandis"}, {"hi", "Hermit's Isle"}, {"as_aurora", "Flying Battleship Aurora"},
    {"fpp", "Farnsport Palace"}, {"sh", "Flying Continent Shambala"}, {"ml", "Marylea"},
    {"sr", "Sternenritt"}, {"eot", "Oldtown"}, {"elg", "Lost Gardens"}, {"basil", "Basil"},
    {"dr_first", "Wounded Mind"}, {"fw", "Fiorwoods"}, {"rf", "Rohlan Fields"},
    {"ns", "Narslene Sewers"}, {"rt", "Raminas Tower"}, {"ewl", "Wastelands"},
    {"el_dungeon", "Elrant Dungeon"}, {"po", "Phyon Oasis"}, {"mm", "Mana Machine"},
    {"ho", "The Hooge"}, {"is", "Inner Sanctum"}, {"ws", "New Wyrnshire"}, {"flandern", "Flandern"},
    {"trm", "Termina Caves"}, {"tc", "Tormund Castle"}, {"nh", "Magic Academy Nhysa"},
    {"aa", "Arkant Archipelago"}, {"tm", "City of Rain Tormund"}, {"wm", "Wygrand Mines"},
    {"mr", "Mount Rydell"}, {"rockbottom", "Rockbottom"}, {"el", "Elrant City"},
    {"dj", "Djerun Village"}, {"wr", "White Rose Inn"}, {"dr_glenn", "Glenn's Mind"},
    {"er", "Empyrean Ruins"}, {"og", "Ograne Grottos"}, {"el_isles_fish", "Sky Isles"},
    {"ffp", "Flower Fields of Perpetua"}};

std::string AreaName(std::string_view code) {
    for (const auto& [c, n] : AreaNames)
        if (c == code)
            return std::string{n};
    return {};
}

/// The CE font has no typographic quotes; the game's own tables use them in a few names.
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

std::string Replace(std::string s, std::string_view from, const std::string& to) {
    for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
        s.replace(p, from.size(), to);
    return s;
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
    std::string CStr(u64 at, std::size_t max = 64) const {
        char buf[64];
        if (!at || max > sizeof buf)
            return {};
        for (std::size_t off = 0; off < max; off += 16) {
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
    /// managed GameObject -> native GameObject (validated back-pointer)
    u64 Native(u64 go) const {
        const u64 nat = Ok(go) ? Ptr(go + 0x10) : 0;
        return Ok(nat) && Ptr(nat + 0x28) == go ? nat : 0;
    }
    /// managed component of a class on a managed GameObject
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
    /// native GameObject -> its Transform (ComponentPair[0])
    u64 TransformOfNative(u64 nat) const {
        const u64 arr = Ok(nat) ? Ptr(nat + 0x30) : 0;
        return Ok(arr) ? Ptr(arr + 8) : 0;
    }
};

// ---- reader ----------------------------------------------------------------------------------------

Reader::Reader(ce_draw::Art& a) : art{a} {}

std::string Reader::Translate(std::string_view meta, int row, std::string fallback) {
    // the BGDatabase text is read once for every reader (Art::BgText)
    const auto* column = art.BgText(meta);
    if (!column || row < 0 || row >= static_cast<int>(column->size()) ||
        (*column)[static_cast<std::size_t>(row)].empty())
        return fallback;
    return (*column)[static_cast<std::size_t>(row)];
}

std::optional<std::pair<float, float>> Reader::SceneWorldPos(Mem& m, u64 map_go) {
    if (const auto it = scene_world.find(map_go); it != scene_world.end())
        return it->second;
    const u64 tr = m.TransformOfNative(m.Native(map_go));
    if (!Mem::Ok(tr))
        return std::nullopt;
    const u64 h = m.Ptr(tr + 0x38);
    s32 idx = m.Get<s32>(tr + 0x40);
    if (!Mem::Ok(h))
        return std::nullopt;
    const u32 count = m.Get<u32>(h + 0x10);
    const u64 lt = m.Ptr(h + 0x18), par = m.Ptr(h + 0x20);
    if (count == 0 || count > 200000 || !Mem::Ok(lt) || !Mem::Ok(par))
        return std::nullopt;
    struct T {
        float t[12];
    };
    std::vector<T> chain;
    while (idx >= 0 && static_cast<u32>(idx) < count && chain.size() < 64) {
        T t{};
        if (!m.Read(lt + 48ull * static_cast<u64>(idx), t.t, 48))
            return std::nullopt;
        chain.push_back(t);
        idx = m.Get<s32>(par + 4ull * static_cast<u64>(idx));
    }
    if (chain.empty() || chain.size() >= 64)
        return std::nullopt;
    float x = 0, y = 0, sx = 1, sy = 1;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        // rotation is identity on these scene roots (checked: q = 0,0,0,1)
        if (std::fabs(it->t[7] - 1.0f) > 1e-3f)
            return std::nullopt;
        x += sx * it->t[0];
        y += sy * it->t[1];
        sx *= it->t[8];
        sy *= it->t[9];
    }
    if (!std::isfinite(x) || !std::isfinite(y))
        return std::nullopt;
    if (scene_world.size() > 256)
        scene_world.clear();
    scene_world[map_go] = {x, y};
    return std::pair{x, y};
}

bool Reader::ReadMap(Mem& m, u64 mo) {
    if (map.map_object == mo && map.ok)
        return true;
    map = MapCache{};
    map.map_object = mo;
    const u64 mc = m.Component(mo, "Map");
    if (!mc)
        return false;
    // the art is named after the map object ("map_ffp" -> ffp_map); Map.mapName is empty on some
    // areas (ffp), so it is only the fallback
    {
        const u64 nat = m.Native(mo);
        const std::string go_name = nat ? m.CStr(m.Ptr(nat + 0x60)) : std::string{};
        map.map_name = go_name.starts_with("map_") ? go_name.substr(4) : m.String(m.Ptr(mc + 0x78));
    }
    if (map.map_name.empty() || map.map_name.size() > 32)
        return false;
    map.geo.sprite = map.map_name + "_map";
    // the Map canvas hierarchy: every node's local position is a native map coordinate
    const u64 tr = m.TransformOfNative(m.Native(mo));
    if (!Mem::Ok(tr))
        return false;
    const u64 h = m.Ptr(tr + 0x38);
    const s32 root = m.Get<s32>(tr + 0x40);
    if (!Mem::Ok(h))
        return false;
    const u32 count = m.Get<u32>(h + 0x10);
    const u64 lt = m.Ptr(h + 0x18), par = m.Ptr(h + 0x20), ptrs = m.Ptr(h + 0x30);
    if (count == 0 || count > 4096 || !Mem::Ok(lt) || !Mem::Ok(par) || !Mem::Ok(ptrs) || root < 0 ||
        static_cast<u32>(root) >= count)
        return false;
    std::vector<s32> parent(count);
    std::vector<float> loc(count * 12);
    std::vector<u64> tptr(count);
    if (!m.Read(par, parent.data(), count * 4) || !m.Read(lt, loc.data(), count * 48) ||
        !m.Read(ptrs, tptr.data(), count * 8))
        return false;
    std::unordered_map<u64, u32> index;
    s32 masks_node = -1;
    bool image = false;
    for (u32 i = 0; i < count; ++i) {
        index[tptr[i]] = i;
        if (parent[i] != root)
            continue;
        const u64 go = Mem::Ok(tptr[i]) ? m.Ptr(tptr[i] + 0x30) : 0;
        const std::string name = Mem::Ok(go) ? m.CStr(m.Ptr(go + 0x60)) : std::string{};
        const float x = loc[i * 12], y = loc[i * 12 + 1];
        if (name == "MapImage") {
            map.geo.img_x = x, map.geo.img_y = y, image = true;
            // inactive on areas without map art (the game then prints "No Map")
            map.geo.art = m.Get<u8>(go + 0x56) != 0 && ce_page_field::HasMapArt(map.geo.sprite);
        } else if (name == "Map") {
            map.geo.bg = true, map.geo.bg_x = x, map.geo.bg_y = y;
        } else if (name == "Map Grid") {
            map.geo.grid = true, map.geo.grid_x = x, map.geo.grid_y = y;
        } else if (name == "Masks") {
            masks_node = static_cast<s32>(i);
        }
    }
    if (!image)
        return false;
    // reveal masks: children of "Masks" carrying a StateInfo (cell name = world state)
    const auto state_of_native = [&](u64 nat) -> std::string {
        const u64 arr = m.Ptr(nat + 0x30), n = m.Ptr(nat + 0x40);
        if (!Mem::Ok(arr) || n == 0 || n > 16)
            return {};
        for (u64 k = 0; k < n; ++k) {
            const u64 comp = m.Ptr(arr + 16 * k + 8);
            const u64 managed = Mem::Ok(comp) ? m.Ptr(comp + 0x28) : 0;
            if (Mem::Ok(managed) && m.Ptr(managed + 0x10) == comp &&
                m.ClassName(managed) == "StateInfo") {
                const u64 ws = m.Ptr(managed + 0x18);
                return m.ClassName(ws) == "WorldState" ? m.String(m.Ptr(ws + 0x18)) : std::string{};
            }
        }
        return {};
    };
    if (masks_node >= 0) {
        const auto mi = static_cast<std::size_t>(masks_node);
        const float px = loc[mi * 12], py = loc[mi * 12 + 1], ps = loc[mi * 12 + 8];
        for (u32 i = 0; i < count; ++i) {
            if (parent[i] != masks_node)
                continue;
            const u64 go = Mem::Ok(tptr[i]) ? m.Ptr(tptr[i] + 0x30) : 0;
            const std::string st = Mem::Ok(go) ? state_of_native(go) : std::string{};
            if (st.empty())
                continue;
            MapCache::Mask k;
            k.cx = px + ps * loc[i * 12];
            k.cy = py + ps * loc[i * 12 + 1];
            k.half = 0.5f * MaskSpriteUnits * ps * loc[i * 12 + 8];
            k.state = st;
            if (k.half > 0 && k.half < 1000)
                map.masks.push_back(std::move(k));
        }
    }
    // teleports (Map.teleports): position = the node's local position, state = its StateInfo
    for (const u64 t : m.List(m.Ptr(mc + 0x40), 64)) {
        const u64 nat = m.Native(t);
        const u64 ttr = m.TransformOfNative(nat);
        const auto it = index.find(ttr);
        if (!nat || it == index.end() || parent[it->second] != root)
            continue;
        MapCache::Tele tp;
        tp.x = loc[it->second * 12];
        tp.y = loc[it->second * 12 + 1];
        tp.state = state_of_native(nat);
        if (!tp.state.empty())
            map.teleports.push_back(std::move(tp));
    }
    map.ok = true;
    return true;
}

void Reader::ReadSave(Mem& m, u64 sd) {
    using ce_page_field::Chip;
    using ce_page_field::Tile;
    // explored map cells / discovered teleports: presence of the name in mapExplored
    std::set<std::string> ex;
    for (const u64 w : m.List(m.Ptr(sd + 0xA0), 4096))
        if (m.ClassName(w) == "WorldState")
            ex.insert(m.String(m.Ptr(w + 0x18)));
    explored = std::move(ex);

    // current main quest: started, not finished, mainQuest; objective = its open tasks
    const auto* t125 = art.Table(125);
    const auto* t128 = art.Table(128);
    const auto row_of = [](const std::vector<std::vector<std::string>>* t, int id) {
        if (!t)
            return -1;
        const std::string key = std::to_string(id);
        for (std::size_t i = 1; i < t->size(); ++i)
            if (!(*t)[i].empty() && (*t)[i][0] == key)
                return static_cast<int>(i) - 1;
        return -1;
    };
    int quest_id = -1, quest_order = 1 << 30;
    std::string quest;
    for (const u64 q : m.List(m.Ptr(sd + 0x70), 4096)) {
        if (m.ClassName(q) != "Quest")
            continue;
        const bool started = m.Get<u8>(q + 0x2C) != 0, finished = m.Get<u8>(q + 0x2D) != 0,
                   main_quest = m.Get<u8>(q + 0x34) != 0;
        const s32 order_v = m.Get<s32>(q + 0x28);
        if (started && !finished && main_quest && order_v < quest_order) {
            quest_order = order_v;
            quest_id = m.Get<s32>(q + 0x10);
            quest = m.String(m.Ptr(q + 0x20));
        }
    }
    std::string objective;
    if (quest_id >= 0) {
        quest = Translate("tr_QUESTS_NAME", row_of(t125, quest_id), quest);
        int best = 1 << 30;
        for (const u64 t : m.List(m.Ptr(sd + 0x68), 8192)) {
            if (m.ClassName(t) != "QuestTask" || m.Get<s32>(t + 0x14) != quest_id)
                continue;
            if (!m.Get<u8>(t + 0x18) || m.Get<u8>(t + 0x2C))
                continue;
            const s32 ord = m.Get<s32>(t + 0x28);
            if (ord < best) {
                best = ord;
                const s32 id = m.Get<s32>(t + 0x10);
                objective = Translate("tr_QUESTTASKS", row_of(t128, id), m.String(m.Ptr(t + 0x20)));
            }
        }
    }
    view.quest = Plain(quest);
    view.objective = Plain(objective);

    // Reward Board: the save's task list (only unlocked tasks are present)
    std::map<int, std::string> enemy_names;
    for (const u64 es : m.List(m.Ptr(sd + 0x78), 4096))
        if (m.ClassName(es) == "EnemySingle")
            enemy_names[m.Get<s32>(es + 0x10)] = m.String(m.Ptr(es + 0x18));
    const auto* t121 = art.Table(121);
    const auto* t115 = art.Table(115);
    const auto cell = [](const std::vector<std::vector<std::string>>* t, int row, int col) -> int {
        if (!t || row < 0 || static_cast<std::size_t>(row + 1) >= t->size())
            return -1;
        const auto& r = (*t)[static_cast<std::size_t>(row + 1)];
        if (static_cast<std::size_t>(col) >= r.size())
            return -1;
        return ce_parse::Leading(r[static_cast<std::size_t>(col)]).value_or(0);
    };
    std::vector<Tile> tiles;
    for (const u64 r : m.List(m.Ptr(sd + 0xB8), 1024)) {
        if (m.ClassName(r) != "RewardBoardTask" || !m.Get<u8>(r + 0x45))
            continue;
        Tile t;
        t.id = m.Get<s32>(r + 0x10);
        t.x = m.Get<s32>(r + 0x14);
        t.y = m.Get<s32>(r + 0x18);
        t.loc = m.String(m.Ptr(r + 0x20));
        t.type = m.Get<s32>(r + 0x54);
        t.done = m.Get<u8>(r + 0x44) != 0;
        t.claimed = m.Get<u8>(r + 0x58) != 0;
        t.claimable = t.done && !t.claimed;
        if (t.id < 0 || t.id > 4096 || t.x < 0 || t.x > 63 || t.y < 0 || t.y > 255 || t.loc.empty() ||
            t.type < 0 || t.type > 6)
            continue;
        const s32 var = m.Get<s32>(r + 0x48), var2 = m.Get<s32>(r + 0x4C), enemy = m.Get<s32>(r + 0x50);
        std::string text = m.String(m.Ptr(r + 0x28));
        const int row = row_of(t121, t.id);
        text = Translate("tr_REWARDBOARD", row, text);
        // target: %y% = Variable 2; otherwise the literal number after '/' ("Find %x%/5 ...")
        t.prog = std::max(0, var);
        t.target = var2;
        if (text.find("%y%") == std::string::npos) {
            const auto slash = text.find("%x%/");
            t.target = slash != std::string::npos
                           ? ce_parse::Leading(std::string_view{text}.substr(slash + 4)).value_or(0)
                           : 0;
        }
        std::string name = "???";
        if (const auto it = enemy_names.find(enemy); it != enemy_names.end() && !it->second.empty())
            name = it->second;
        text = Replace(text, "%x%", std::to_string(var));
        text = Replace(text, "%y%", std::to_string(var2));
        text = Replace(text, "%z%", name);
        t.text = Plain(text);
        if (row >= 0) {
            t.gold = std::max(0, cell(t121, row, 5));
            t.sp = std::max(0, cell(t121, row, 6));
            t.cp = std::max(0, cell(t121, row, 7));
            const int item = cell(t121, row, 8);
            const int irow = item >= 0 && item < 9999 ? row_of(t115, item) : -1;
            if (irow >= 0 && t115) {
                t.item = (*t115)[static_cast<std::size_t>(irow + 1)].size() > 1
                             ? (*t115)[static_cast<std::size_t>(irow + 1)][1]
                             : std::string{};
                t.item = Plain(t.item);
                t.item_icon = cell(t115, irow, 5);
            }
        }
        tiles.push_back(std::move(t));
    }
    std::sort(tiles.begin(), tiles.end(), [](const Tile& a, const Tile& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    view.tiles = std::move(tiles);

    // longest chain of adjacent finished tiles (MainMenuRewardBoard.CalculateChain; native 11 on
    // the fixture = this rule)
    std::set<std::pair<int, int>> fin;
    for (const auto& t : view.tiles)
        if (t.done)
            fin.insert({t.x, t.y});
    std::set<std::pair<int, int>> seen, best;
    for (const auto& p : fin) {
        if (seen.contains(p))
            continue;
        std::set<std::pair<int, int>> comp;
        std::vector<std::pair<int, int>> stack{p};
        while (!stack.empty()) {
            const auto q = stack.back();
            stack.pop_back();
            if (!comp.insert(q).second)
                continue;
            for (const auto& d : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                const std::pair n{q.first + d.first, q.second + d.second};
                if (fin.contains(n) && !comp.contains(n))
                    stack.push_back(n);
            }
        }
        seen.insert(comp.begin(), comp.end());
        if (comp.size() > best.size())
            best = std::move(comp);
    }
    view.chain.assign(best.begin(), best.end());
}

bool Reader::Refresh(const EdenDsmodHostApi& host) {
    Mem mm{host, class_names};
    const u64 gm = mm.Statics(SlotGameManager, "GameManager", false);
    if (!gm || !(std::fabs(mm.Get<float>(gm + 0x48) - 1.41f) <= 0.005f)) // NaN fails too
        return false;
    if (const u64 sb = mm.Statics(SlotBattle, "Battle", false);
        sb && mm.Get<u8>(sb + 0x4D) && !mm.Get<u8>(sb + 0x4E))
        return false; // battle: ce_reader owns the screen; keep the field model as it was
    const std::string area = mm.String(mm.Ptr(gm + 0x88));
    const u64 scene = mm.Ptr(gm + 0xB0), mo = mm.Ptr(gm + 0x70), lvl_go = mm.Ptr(gm + 0x78);
    if (area.empty() || area.size() > 32 || mm.ClassName(scene) != "MapScene")
        return false;
    // quiet only while the event object is alive and active (a reference left behind by a finished
    // or destroyed event does not count) or a skit runs
    {
        const u64 ev = mm.Ptr(gm + 0x58);
        const u64 nat = ev ? mm.Ptr(ev + 0x10) : 0;
        const bool live = nat && mm.Ptr(nat + 0x28) == ev && mm.Get<u8>(nat + 0x56) != 0;
        view.quiet = live || mm.Get<u8>(gm + 0x61) != 0; // currentEvent / currentlySkit
    }
    if (area != area_code) {
        area_code = area;
        view_set = false;
        section.clear();
        sel_tile = -1;
    }
    view.area_name = AreaName(area);
    if (view.area_name.empty())
        view.area_name = area;

    // player position on the native map (Map.OpenMap)
    bool map_ok = Mem::Ok(mo) && ReadMap(mm, mo);
    const float k = area == "va" ? KWorld : KArea;
    u64 use_scene = scene;
    float wx{}, wy{};
    const float lockx = mm.Get<float>(scene + 0x40), locky = mm.Get<float>(scene + 0x44);
    interior = lockx != 0.0f;
    if (interior) {
        // interior: the parent scene of LvlManager.mapScenes[parentMapNr] and the locked marker
        const u64 lvl = mm.Component(lvl_go, "LvlManager");
        const s32 parent = mm.Get<s32>(scene + 0x48);
        const auto scenes = mm.List(lvl ? mm.Ptr(lvl + 0x58) : 0, 512);
        if (parent < 0 || static_cast<std::size_t>(parent) >= scenes.size() ||
            mm.ClassName(scenes[static_cast<std::size_t>(parent)]) != "MapScene") {
            map_ok = false;
        } else {
            use_scene = scenes[static_cast<std::size_t>(parent)];
        }
        wx = lockx, wy = locky;
    } else {
        const u64 ps = mm.Statics(SlotPlayer, "Player", true);
        bool ok1 = false, ok2 = false;
        wx = ps ? mm.Get<float>(ps + 0x28, &ok1) : 0;
        wy = ps ? mm.Get<float>(ps + 0x2C, &ok2) : 0;
        const s32 dir = ps ? mm.Get<s32>(ps + 0xC) : 0;
        facing = dir >= 0 && dir <= 4 ? dir : 0;
        if (!ok1 || !ok2 || !std::isfinite(wx) || !std::isfinite(wy))
            map_ok = false;
    }
    if (map_ok) {
        const auto world = SceneWorldPos(mm, mm.Ptr(use_scene + 0x10));
        const float mpx = mm.Get<float>(use_scene + 0x38), mpy = mm.Get<float>(use_scene + 0x3C);
        if (!world) {
            map_ok = false;
        } else {
            player_x = mpx + (wx - world->first) * k;
            player_y = -(mpy + (world->second - wy) * k);
            if (!std::isfinite(player_x) || !std::isfinite(player_y) || std::fabs(player_x) > 5000 ||
                std::fabs(player_y) > 5000)
                map_ok = false;
        }
    }

    if (sample % SaveEvery == 0 || save_stamp == 0) {
        const u64 slot = mm.Ptr(host.main_base + SlotSaveDb);
        const u64 kl = Mem::Ok(slot) ? mm.Ptr(slot) : 0;
        if (Mem::Ok(kl) && mm.CStr(mm.Ptr(kl + 0x10)) == "GetData") {
            const u64 sf = mm.Ptr(kl + 0xB8);
            const u64 sd = Mem::Ok(sf) ? mm.Ptr(sf) : 0;
            if (mm.ClassName(sd) == "SaveDataOwn") {
                ReadSave(mm, sd);
                save_stamp = sample + 1;
            }
        }
    }

    // model: map geometry with the reveal state, discovered crystals only
    view.has_map = map_ok && map.ok && area != "va"; // world map art: not mapped yet
    if (view.has_map) {
        view.map = map.geo;
        view.map.cells.clear();
        for (const auto& c : map.masks)
            view.map.cells.push_back({c.cx, c.cy, c.half, explored.contains(c.state)});
        view.crystals.clear();
        for (const auto& t : map.teleports)
            if (explored.contains(t.state))
                view.crystals.push_back({t.x, t.y});
    } else {
        view.map = {};
        view.crystals.clear();
    }
    if (view.has_map && !view.map.art) {
        // "No Map": the game shows no player icon or crystals; centre the parchment
        view.crystals.clear();
        player_x = view.map.img_x, player_y = view.map.img_y;
        interior = false;
    }
    view.interior = interior && view.has_map;
    view.marker_x = player_x, view.marker_y = player_y;
    view.facing = facing;

    // the picture's view anchor: re-centre only when the marker leaves the safe area
    if (view.has_map) {
        using namespace ce_page_field::geo;
        const auto canvas = [&](float lx, float ly) {
            return std::pair{ViewAnchorX + (lx - view_cx) * FZoom, ViewAnchorY + (view_cy - ly) * FZoom};
        };
        const auto [cx, cy] = canvas(player_x, player_y);
        if (!view_set || cx < SafeX0 || cx > SafeX1 || cy < SafeY0 || cy > SafeY1) {
            view_cx = std::round(player_x);
            view_cy = std::round(player_y);
            view_set = true;
        }
        view.view_x = view_cx, view.view_y = view_cy;
    }

    // Reward Board badge / counts for the current area's section
    view.claim_area = 0;
    view.chips.clear();
    std::array<ce_page_field::Chip, 4> chips{};
    std::array<bool, 4> got{};
    for (const auto& t : view.tiles) {
        if (t.loc != area)
            continue;
        if (t.claimable)
            ++view.claim_area;
        int kind = -1;
        std::string_view s = t.text;
        // categories by the task text the board shows (table 121 "Find %x%/%y% ...")
        if (s.find("treasure chests") != std::string_view::npos)
            kind = 0;
        else if (s.find("hidden caves") != std::string_view::npos)
            kind = 1;
        else if (s.find("buried treasure") != std::string_view::npos)
            kind = 2;
        else if (s.find("collectibles") != std::string_view::npos)
            kind = 3;
        if (kind < 0 || t.target <= 0)
            continue;
        auto& c = chips[static_cast<std::size_t>(kind)];
        if (!got[static_cast<std::size_t>(kind)] || t.target > c.need) {
            c = {kind, std::min(t.prog, t.target), t.target};
            got[static_cast<std::size_t>(kind)] = true;
        }
    }
    for (std::size_t i = 0; i < 4; ++i)
        if (got[i])
            view.chips.push_back(chips[i]);

    // board view: section (default: the current area when it has tiles), window, selection
    {
        std::vector<std::string> secs;
        for (const auto& t : view.tiles)
            if (std::find(secs.begin(), secs.end(), t.loc) == secs.end())
                secs.push_back(t.loc);
        std::string cur = section;
        if (std::find(secs.begin(), secs.end(), cur) == secs.end())
            cur = std::find(secs.begin(), secs.end(), area) != secs.end() ? area
                  : secs.empty()                                       ? std::string{}
                                                                       : secs.front();
        section = cur;
        view.section = cur;
        view.section_name = AreaName(cur).empty() ? cur : AreaName(cur);
        const auto pos = std::find(secs.begin(), secs.end(), cur);
        view.sections_prev = pos != secs.end() && pos != secs.begin();
        view.sections_next = pos != secs.end() && pos + 1 != secs.end();
        int x0 = 99, x1 = -1, y0 = 999, y1 = -1, gw = 0, gh = 0;
        for (const auto& t : view.tiles) {
            gw = std::max(gw, t.x + 1), gh = std::max(gh, t.y + 1);
            if (t.loc != cur)
                continue;
            x0 = std::min(x0, t.x), x1 = std::max(x1, t.x), y0 = std::min(y0, t.y), y1 = std::max(y1, t.y);
        }
        using namespace ce_page_field::geo;
        if (x1 >= 0) {
            view.win_x0 = std::clamp(x0 - (BCols - (x1 - x0 + 1)) / 2, 0, std::max(0, gw - BCols));
            view.win_y0 = std::clamp(y0 - (BRows - (y1 - y0 + 1)) / 2, 0, std::max(0, gh - BRows));
            if (x0 < view.win_x0)
                view.win_x0 = x0;
            if (y0 < view.win_y0)
                view.win_y0 = y0;
        }
        // selection: keep a tile of the section; default = first claimable, else the first tile
        bool keep = false;
        for (const auto& t : view.tiles)
            keep = keep || (t.id == sel_tile && t.loc == cur);
        if (!keep) {
            sel_tile = -1;
            for (const auto& t : view.tiles)
                if (t.loc == cur && t.claimable) {
                    sel_tile = t.id;
                    break;
                }
            if (sel_tile < 0)
                for (const auto& t : view.tiles)
                    if (t.loc == cur) {
                        sel_tile = t.id;
                        break;
                    }
        }
        view.selected = sel_tile;
        win_ids.assign(static_cast<std::size_t>(BCols * BRows), -1);
        for (const auto& t : view.tiles) {
            const int cx = t.x - view.win_x0, cy = t.y - view.win_y0;
            if (cx >= 0 && cx < BCols && cy >= 0 && cy < BRows)
                win_ids[static_cast<std::size_t>(cy * BCols + cx)] = t.id;
        }
    }
    view.page = page;
    have = true;
    return true;
}

void Reader::Sample(const EdenDsmodHostApi& host, bool in_game) {
    if (in_game && (sample % RefreshEvery == 0 || refresh_now)) {
        refresh_now = false;
        ++view_gen; // Refresh may change the model even when it fails half-way
        try {
            const bool ok = Refresh(host);
            if (ok && host.log) {
                char b[256];
                std::snprintf(b, sizeof b,
                              "CE field: area=%s map=%s player=(%.1f,%.1f) dir=%d interior=%d quiet=%d "
                              "crystals=%zu cells=%zu claim=%d chips=%zu",
                              area_code.c_str(), view.has_map ? view.map.sprite.c_str() : "-", player_x,
                              player_y, facing, interior ? 1 : 0, view.quiet ? 1 : 0,
                              view.crystals.size(), view.map.cells.size(), view.claim_area,
                              view.chips.size());
                // log only on a context change (not every step or turn)
                const std::string ctx = b;
                const auto cut = ctx.find(" player=");
                const auto rest = ctx.find(" interior=");
                const std::string key = ctx.substr(0, cut) + ctx.substr(rest);
                if (key != last_log) {
                    last_log = key;
                    host.log(host.userdata, EDEN_DSMOD_LOG_INFO, b);
                }
            }
        } catch (...) {
        }
    }
    ++sample;
}

std::string Reader::KeyFor(const ce_page_field::FieldView& v) {
    const std::string sig = ce_page_field::Signature(v) + std::string{ce_draw::PictureStamp};
    const std::string key =
        "module:ce:fh:" + Hex(dsmod_sdk::Fnv1a64(reinterpret_cast<const u8*>(sig.data()), sig.size()));
    std::scoped_lock lock{mutex};
    if (!models.contains(key)) {
        models.emplace(key, v);
        key_view[key] = {v.view_x, v.view_y};
        order.push_back(key);
        while (order.size() > ModelKeep) {
            models.erase(order.front());
            key_view.erase(order.front());
            order.pop_front();
        }
    }
    return key;
}

std::string Reader::IdleKey() {
    if (!have)
        return {};
    // the native Skills menu shows the Skills page, the native Equipment / smith fusion menus the
    // Crystals page, as read-only mirrors; on close the page held before returns (UX-SPEC 2.2)
    const bool cr_mirror = crystals_mirror && crystals_mirror();
    const int pg = skills_mirror && skills_mirror() ? 2 : (cr_mirror ? 1 : page);
    if (pg == 1 && crystals_key)
        if (std::string k = crystals_key(); !k.empty())
            return k;
    if (pg == 2 && skills_key)
        if (std::string k = skills_key(); !k.empty())
            return k;
    const bool alarm = pg == 0 && skills_alarm && skills_alarm();
    if (idle_gen == view_gen && idle_pg == pg && idle_alarm == alarm && !idle_key.empty()) {
        std::scoped_lock lock{mutex};
        if (models.contains(idle_key))
            return idle_key; // the model is unchanged since the last call (no copy, no signature)
    }
    ce_page_field::FieldView v = view;
    v.page = pg;
    v.skills_alarm = alarm;
    // a sub-page stays put during dialogue; Home turns quiet
    v.quiet = view.quiet && pg == 0;
    if (pg != 3) {
        // the board data is not part of the other pictures' keys
        v.tiles.clear();
        v.chain.clear();
        v.selected = -1;
    }
    if (pg != 0) {
        v.map = {};
        v.has_map = false;
        v.crystals.clear();
    }
    // the live marker is a widget (ce.fmx / ce.fmy) except in the quiet and interior pictures
    if (pg == 0 && !v.quiet && !v.interior) {
        v.marker_x = v.marker_y = 0;
        v.facing = 0;
    }
    idle_key = KeyFor(v);
    idle_gen = view_gen;
    idle_pg = pg;
    idle_alarm = alarm;
    return idle_key;
}

void Reader::PublishFor(const EdenDsmodHostApi& h, const std::string& shown, bool loading) {
    if (!h.publish_i64 || !shown.starts_with("module:ce:fh:") || loading)
        return;
    const auto pi = [&](const char* n, s64 v) { h.publish_i64(h.userdata, n, v); };
    ce_page_field::FieldView v;
    bool have_art = true;
    std::pair<float, float> anchor{};
    {
        std::scoped_lock lock{mutex};
        const auto it = models.find(shown);
        if (it == models.end())
            return;
        v.page = it->second.page;
        v.quiet = it->second.quiet;
        v.interior = it->second.interior;
        v.has_map = it->second.has_map;
        have_art = it->second.map.art;
        anchor = key_view[shown];
    }
    if (v.page == 0 && !v.quiet) {
        pi("ce.fhome", 1);
        if (v.has_map && !v.interior && have_art) {
            using namespace ce_page_field::geo;
            const float cx = ViewAnchorX + (player_x - anchor.first) * FZoom;
            const float cy = ViewAnchorY + (anchor.second - player_y) * FZoom;
            if (cx > 0 && cx < 1240 && cy > 0 && cy < FMapH) {
                pi("ce.fmark", 1);
                pi("ce.fmx", static_cast<s64>(std::lround(cx)) - FMarkerBox / 2);
                pi("ce.fmy", static_cast<s64>(std::lround(cy)) - FMarkerBox / 2);
                pi("ce.fmd", facing);
            }
        }
    } else if (v.page != 0) {
        pi("ce.fsub", 1);
        if (v.page == 3) {
            pi("ce.fboard", 1);
            for (std::size_t i = 0; i < win_ids.size(); ++i)
                if (win_ids[i] >= 0) {
                    const std::string n = "ce.bt" + std::to_string(i);
                    pi(n.c_str(), 1);
                }
            if (view.sections_prev)
                pi("ce.bprev", 1);
            if (view.sections_next)
                pi("ce.bnext", 1);
        }
    }
}

bool Reader::Action(std::string_view name, s64 argument) {
    if (name == "fpage") {
        if (argument < 0 || argument > 3)
            return false;
        page = static_cast<int>(argument);
        if (page == 3) {
            section.clear(); // the board opens on the current area
            sel_tile = -1;
        }
        refresh_now = true;
        return true;
    }
    if (name == "btile") {
        if (page != 3 || argument < 0 || static_cast<std::size_t>(argument) >= win_ids.size() ||
            win_ids[static_cast<std::size_t>(argument)] < 0)
            return false;
        const int id = win_ids[static_cast<std::size_t>(argument)];
        sel_tile = id;
        for (const auto& t : view.tiles)
            if (t.id == id)
                section = t.loc;
        refresh_now = true;
        return true;
    }
    if (name == "bsec") {
        if (page != 3)
            return false;
        std::vector<std::string> secs;
        for (const auto& t : view.tiles)
            if (std::find(secs.begin(), secs.end(), t.loc) == secs.end())
                secs.push_back(t.loc);
        auto it = std::find(secs.begin(), secs.end(), section);
        if (it == secs.end())
            return false;
        if (argument > 0 && it + 1 != secs.end())
            ++it;
        else if (argument <= 0 && it != secs.begin())
            --it;
        section = *it;
        sel_tile = -1;
        refresh_now = true;
        return true;
    }
    return false;
}

std::optional<ce_page_field::FieldView> Reader::Model(const std::string& key) {
    std::scoped_lock lock{mutex};
    if (const auto it = models.find(key); it != models.end())
        return it->second;
    return std::nullopt;
}

} // namespace ce_field
