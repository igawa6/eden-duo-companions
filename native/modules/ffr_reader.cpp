// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ffr_reader.h"

#include <cmath>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace ffr {
namespace {

struct BuildRoots {
    const char* build;   // first 16 hex digits of the main NSO build id
    uint64_t name_blocks;  // FNamePool Blocks[] (u32 CurrentBlock, u32 cursor just before)
    uint64_t object_array; // FUObjectArray::ObjObjects
};
// Update 1.0.1. Found by shape in .bss and checked live (tools/ffr/NOTES.md).
constexpr BuildRoots kBuilds[] = {
    {"84E182F72EE1DA4F", 0x7c54978, 0x7c8e6a0},
};

// Reflection offsets (UE 5.6 dump of /Script/mucha, tools/ffr/NOTES.md)
constexpr uint64_t kSaveMgrGameSave = 0x60;
constexpr uint64_t kSaveMap = 0x1a8, kSavePerMap = 0x1c8;  // CPP_MuchaGameSaveData.Map, SaveData_Map.perMapSaveDataMap
constexpr int32_t kWorldMapId = 1000;  // the world map (DA_WldMapParameter.WldMapId)
constexpr uint64_t kSavePlayer = 0x28, kSaveUnitParams = 0x60, kSaveParty = 0x90, kSaveInventory = 0xa8, kSaveVisions = 0x118;
constexpr size_t kUnitParamSize = 0xa0;
constexpr uint64_t kMiniAreaMgr = 0x80, kMiniPropKey = 0x88, kMiniPlayerLoc = 0x170;
constexpr uint64_t kAreaMgrMapDA = 0x38, kAreaMgrCurrent = 0x90;
constexpr uint64_t kUiMapLandCaptureDA = 0x30, kLandCaptureMap = 0x30;
constexpr uint64_t kBtlUnitMgr = 0x48, kBtlUnitList = 0x28, kBtlUnitParam = 0x838;
// CPP_UnitParameter (not reflected; found live): unit id +0xf8, level +0x100, HP/MP/BP +0x10c..,
// base max HP/MP/BP +0x120.., Attack/Defence/Intelligence/Mind/Divine/Agility +0x12c..
constexpr uint64_t kUpId = 0xf8, kUpLevel = 0x100, kUpHp = 0x10c, kUpMaxHp = 0x120, kUpStats = 0x12c;
constexpr uint64_t kPcPawn = 0x350, kActorRoot = 0x1b8, kSceneLoc = 0x140, kSceneRot = 0x158;
constexpr uint64_t kAssetDataTables = 0xd0, kDataTableRows = 0x30;
constexpr uint8_t kDtItemConsumables = 39, kDtSkillData = 25, kDtItemEquipment = 44, kDtSkillEffect = 29, kDtPassiveSkill = 26, kDtVisionAwakening = 46, kDtVisionSynchro = 47;

uint64_t NowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct TArr {
    uint64_t data;
    int32_t num, max;
};

} // namespace

uint64_t GameState::Hash() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(&save_ok, 1);
    mix(&in_game, 1);
    mix(&money, 4);
    for (int p : party) mix(&p, 4);
    mix(&guest, 4);
    for (auto& u : units) {
        mix(&u.id, sizeof(int) * 8), mix(&u.level, sizeof(int) * 3), mix(u.stats, sizeof u.stats);
        for (int a : u.accessories) mix(&a, 4);
    }
    for (auto& i : inventory) mix(&i, sizeof i);
    mix(&map.valid, 1);
    mix(&map.map_id, 4);
    mix(&map.area_id, 4);
    mix(&map.world, 1);
    mix(map.name.data(), map.name.size());
    mix(&in_battle, 1);
    mix(&tracked_quest, 4);
    for (auto& k : quests) { mix(&k.id, 4), mix(&k.prog, 4), mix(&k.can_track, 1); const size_t n = k.log.size(), nm = k.name.size() + k.name_src.size(); mix(&n, sizeof n), mix(&nm, sizeof nm); }
    mix(&menu.valid, 1), mix(&menu.open, 1), mix(&menu.state, 4), mix(&menu.cursor, 4), mix(&menu.actor, 4);
    for (auto& e : menu.entries) mix(&e, sizeof e);
    for (auto& e : menu.path) mix(&e, sizeof e);
    for (uint64_t t : menu.targets) mix(&t, 8);
    for (uint64_t t : menu.sub) mix(&t, 8);
    for (auto& f : menu.finish) {
        const int v[5] = {f.enable, f.skill, f.vision, f.uid, f.dir};
        mix(v, sizeof v), mix(&f.unit, 8);
    }
    for (auto& g : map.goals) mix(&g.main, 1), mix(&g.side, 1);  // (how many and which kind; their positions go out every tick)
    for (auto& b : battle) {
        const int v[5] = {b.id, b.hp, b.max_hp, b.mp, b.max_mp};
        mix(v, sizeof v), mix(&b.enemy, 1), mix(&b.staggered, 1), mix(&b.owner, 8), mix(&b.uid, 4), mix(&b.face, 4);
    }
    mix(&turns_valid, 1), mix(&acting, 4), mix(&acting_phase, 4);
    for (auto& t : turns) mix(&t, sizeof t);
    mix(&in_dialog, 1);
    for (auto& d : dialog) mix(d.speaker.data(), d.speaker.size()), mix(d.text.data(), d.text.size()), mix(d.portrait.data(), d.portrait.size());
    return h;
}

bool Reader::Init(const EdenDsmodHostApi* host, const char* build_hex, std::string& why) {
    g_.Set(host);
    main_ = host->main_base;
    const BuildRoots* r = nullptr;
    for (auto& b : kBuilds)
        if (build_hex && std::strncmp(build_hex, b.build, 16) == 0) r = &b;
    if (!r) { why = "unsupported build"; return false; }
    blocks_ = main_ + r->name_blocks;
    objarr_ = main_ + r->object_array;
    if (Name(0) != "None") { why = "name pool did not validate"; return false; }
    const int32_t mx = g_.Get<int32_t>(objarr_ + 16), num = g_.Get<int32_t>(objarr_ + 20);
    if (mx < 0x10000 || num <= 1000 || num > mx) { why = "object array did not validate"; return false; }
    return true;
}

std::string Reader::Name(uint32_t id) {
    auto it = names_.find(id);
    if (it != names_.end()) return it->second;
    const uint32_t block = id >> 16, off = (id & 0xffff) * 2;
    std::string s;
    const uint32_t cur = g_.Get<uint32_t>(blocks_ - 8);
    if (block <= cur && block < 8192) {
        const uint64_t base = g_.Get<uint64_t>(blocks_ + 8ull * block);
        const uint16_t hdr = g_.Get<uint16_t>(base + off);
        const uint32_t len = hdr >> 6;
        if (base && len && len < 1024) {
            if (hdr & 1) {
                std::vector<uint16_t> w(len);
                if (g_.Read(base + off + 2, w.data(), len * 2))
                    for (uint16_t c : w) s += c < 0x80 ? char(c) : '?';
            } else {
                s.resize(len);
                if (!g_.Read(base + off + 2, s.data(), len)) s.clear();
            }
        }
    }
    if (names_.size() > 200000) names_.clear();
    names_[id] = s;
    return s;
}

bool Reader::FindNames(const EdenDsmodHostApi* host, const std::vector<std::string>& want, std::vector<uint32_t>& ids) {
    g_.Set(host);
    if (ids.size() != want.size()) ids.assign(want.size(), 0), find_block_ = 0;
    if (std::all_of(ids.begin(), ids.end(), [](uint32_t i) { return i != 0; })) return true;
    if (!blocks_) return false;
    const uint32_t cur = g_.Get<uint32_t>(blocks_ - 8, ~0u), cursor = g_.Get<uint32_t>(blocks_ - 4);
    if (cur >= 8192) return false;
    if (find_block_ > cur) return true;  // searched (a missing one stays 0)
    const uint32_t block = find_block_++;
    const uint32_t size = block == cur ? std::min<uint32_t>(cursor, 0x20000) : 0x20000;
    const uint64_t base = g_.Get<uint64_t>(blocks_ + 8ull * block);
    find_buf_.resize(size);
    if (!base || !size || !g_.Read(base, find_buf_.data(), size)) return false;
    for (uint32_t off = 0; off + 2 <= size;) {
        const uint16_t hdr = uint16_t(find_buf_[off] | find_buf_[off + 1] << 8);
        const uint32_t len = hdr >> 6, bytes = len * ((hdr & 1) ? 2 : 1);
        if (!len || off + 2 + bytes > size) break;
        if (!(hdr & 1))
            for (size_t i = 0; i < want.size(); ++i)
                if (!ids[i] && want[i].size() == len && std::memcmp(find_buf_.data() + off + 2, want[i].data(), len) == 0)
                    ids[i] = block << 16 | off / 2;
        off += (2 + bytes + 1) & ~1u;
    }
    return find_block_ > cur || std::all_of(ids.begin(), ids.end(), [](uint32_t i) { return i != 0; });
}

std::string Reader::ClassName(uint64_t cls) {
    auto it = class_names_.find(cls);
    if (it != class_names_.end()) return it->second;
    std::string n = Name(g_.Get<uint32_t>(cls + 0x18));
    class_names_[cls] = n;
    return n;
}

// The class, or a blueprint subclass of it, is the field or card game mode (UStruct::SuperStruct at +0x40).
bool Reader::IsGameplayMode(uint64_t cls) {
    auto it = mode_classes_.find(cls);
    if (it != mode_classes_.end()) return it->second;
    bool yes = false;
    uint64_t c = cls;
    for (int depth = 0; c && depth < 16 && !yes; ++depth) {
        const std::string n = Name(g_.Get<uint32_t>(c + 0x18));
        yes = n == "CPP_MuchaGameModeField" || n == "CPP_MuchaGameModeCardgame";
        c = g_.Get<uint64_t>(c + 0x40);
    }
    mode_classes_[cls] = yes;
    return yes;
}

bool Reader::Alive(const Obj& o) const {
    if (!o.addr) return false;
    struct { uint64_t vt; int32_t flags, index; uint64_t cls; } h{};
    return g_.Read(o.addr, &h, sizeof h) && h.cls == o.cls && uint32_t(h.index) == o.index;
}

void Reader::Scan() {
    ++scans_;
    const uint64_t objs = g_.Get<uint64_t>(objarr_);
    const int32_t num = g_.Get<int32_t>(objarr_ + 20), nchunks = g_.Get<int32_t>(objarr_ + 28);
    if (!objs || num <= 0 || nchunks <= 0 || nchunks > 64) return;
    Obj save, quest, mini, btl, pc, am, mode, vm, mgr, wsub, wvm, wlist, dlg, mmvm, gwm, wpar, sact, stgt, backlog;
    std::vector<Obj> ups, maps, lists;
    std::unordered_map<std::string, Obj> tables;
    std::vector<uint8_t> items;
    for (int32_t c = 0; c < nchunks; ++c) {
        const uint64_t chunk = g_.Get<uint64_t>(objs + 8ull * c);
        const int32_t n = std::min<int32_t>(0x10000, num - c * 0x10000);
        if (!chunk || n <= 0) continue;
        items.resize(size_t(n) * 0x18);
        if (!g_.Read(chunk, items.data(), items.size())) continue;
        for (int32_t i = 0; i < n; ++i) {
            uint64_t a;
            std::memcpy(&a, &items[size_t(i) * 0x18], 8);
            if (!a) continue;
            struct { uint64_t vt; int32_t flags, index; uint64_t cls; uint32_t name, number; } h{};
            if (!g_.Read(a, &h, sizeof h) || !h.cls) continue;
            const std::string& cn = ClassName(h.cls);
            if (cn.find("GameMode") != std::string::npos && IsGameplayMode(h.cls)) {
                if (Name(h.name).rfind("Default__", 0) != 0) mode = {a, h.cls, uint32_t(h.index)};
                continue;
            }
            if (cn == "DataTable") {
                if (Name(h.name).rfind("DT_MapData_", 0) == 0) maps.push_back({a, h.cls, uint32_t(h.index)});
                continue;
            }
            if (cn.find("BtlCmd") != std::string::npos) {  // the battle command menu (CPP_VM_BtlCmd, its lists, the manager)
                if (Name(h.name).rfind("Default__", 0) == 0) continue;
                // the game may use blueprint subclasses (VM_BtlCmd_C ...): match on the native ancestor
                if (IsA(h.cls, "CPP_VM_BtlCmd")) vm = {a, h.cls, uint32_t(h.index)};
                else if (IsA(h.cls, "CPP_UI_BtlCmdManager")) mgr = {a, h.cls, uint32_t(h.index)};
                else if (IsA(h.cls, "CPP_BtlCmdListBase")) lists.push_back({a, h.cls, uint32_t(h.index)});
                continue;
            }
            if (cn.find("WldMap") != std::string::npos) {  // the world map menu's view models (blueprint subclasses), its subsystem
                if (Name(h.name).rfind("Default__", 0) == 0) continue;
                if (cn == "CPP_DA_WldMapParameter") { if (Name(h.name) == "DA_WldMapParameter") wpar = {a, h.cls, uint32_t(h.index)}; }
                else if (cn == "CPP_UI_WldMapSubsystem") wsub = {a, h.cls, uint32_t(h.index)};
                else if (cn.find("MiniMap") == std::string::npos && IsA(h.cls, "CPP_VM_WldMap")) wvm = {a, h.cls, uint32_t(h.index)};
                else if (IsA(h.cls, "CPP_VM_WldMapLandmarkList")) wlist = {a, h.cls, uint32_t(h.index)};
                continue;
            }
            if (cn == "CPP_DA_LandmarkList") {
                if (Name(h.name) == "DA_LandmarkList") lm_list_ = {a, h.cls, uint32_t(h.index)};
                continue;
            }
            if (cn == "CPP_GameWidgetManagerSubsystem") {
                if (Name(h.name).rfind("Default__", 0) != 0) gwm = {a, h.cls, uint32_t(h.index)};
                continue;
            }
            if (cn == "CPP_VM_MainMenu") {
                if (Name(h.name).rfind("Default__", 0) != 0) mmvm = {a, h.cls, uint32_t(h.index)};
                continue;
            }
            if (cn == "CPP_DialogManager") {
                if (Name(h.name).rfind("Default__", 0) != 0) dlg = {a, h.cls, uint32_t(h.index)};
                continue;
            }
            if (cn == "CPP_DA_UI_SkillAction" || cn == "CPP_DA_UI_SkillTargeting") {
                const std::string n = Name(h.name);
                if (n == "DA_UI_SkillAction") sact = {a, h.cls, uint32_t(h.index)};
                else if (n == "DA_UI_SkillTargeting") stgt = {a, h.cls, uint32_t(h.index)};
                continue;
            }
            if (cn == "StringTable") { tables[Name(h.name)] = {a, h.cls, uint32_t(h.index)}; continue; }
            if (cn == "CPP_BacklogSubsystem") {
                if (Name(h.name).rfind("Default__", 0) != 0) backlog = {a, h.cls, uint32_t(h.index)};
                continue;
            }
            const bool up = cn == "CPP_UnitParameter" || cn == "CPP_BtlUnitParameter";
            Obj* slot = cn == "CPP_MuchaSaveDataManager" ? &save : cn == "CPP_QuestSubsystem" ? &quest : cn == "CPP_MiniMapSubsystem" ? &mini : cn == "CPP_BtlManager" ? &btl
                      : cn == "CPP_MuchaPlayerController" ? &pc : cn == "CPP_AppAssetManager" ? &am : nullptr;
            if (!slot && !up) continue;
            if (Name(h.name).rfind("Default__", 0) == 0) continue;
            if (up) { if (cn == "CPP_UnitParameter") ups.push_back({a, h.cls, uint32_t(h.index)}); continue; }
            *slot = {a, h.cls, uint32_t(h.index)};  // the last (newest) one wins
        }
    }
    save_mgr_ = save;
    backlog_ = backlog;
    quest_sub_ = quest;
    string_tables_ = std::move(tables);
    minimap_ = mini;
    battle_ = btl;
    controller_ = pc;
    assets_ = am;
    unit_params_ = std::move(ups);
    map_tables_ = std::move(maps);
    game_mode_ = mode;
    cmd_vm_ = vm;
    cmd_mgr_ = mgr;
    cmd_lists_ = std::move(lists);
    wld_sub_ = wsub;
    wld_vm_ = wvm;
    wld_list_ = wlist;
    dialogs_ = dlg;
    main_vm_ = mmvm;
    widgets_ = gwm;
    wld_param_ = wpar;
    skill_action_ = sact, skill_targeting_ = stgt;
}

bool Reader::IsA(uint64_t cls, const char* base) {
    const auto key = std::make_pair(cls, std::string(base));
    auto it = isa_.find(key);
    if (it != isa_.end()) return it->second;
    bool yes = false;
    uint64_t c = cls;
    for (int depth = 0; c && depth < 16 && !yes; ++depth) {
        yes = Name(g_.Get<uint32_t>(c + 0x18)) == base;
        c = g_.Get<uint64_t>(c + 0x40);  // UStruct::SuperStruct
    }
    isa_[key] = yes;
    return yes;
}

uint64_t Reader::Resolve(int32_t index, int32_t serial) {
    (void)serial;
    if (index <= 0) return 0;
    const uint64_t objs = g_.Get<uint64_t>(objarr_);
    const uint64_t chunk = objs ? g_.Get<uint64_t>(objs + 8ull * uint32_t(index >> 16)) : 0;
    const uint64_t o = chunk ? g_.Get<uint64_t>(chunk + 0x18ull * uint32_t(index & 0xffff)) : 0;
    // The object must still be the one in that slot (UObject::InternalIndex)
    return o && g_.Get<int32_t>(o + 0xc) == index ? o : 0;
}

namespace {
// TMap element walk (TSparseArray + TBitArray<TInlineAllocator<4>>), stride = element size including the hash links.
template <class F> void ForEachMap(const Guest& g, uint64_t map, size_t stride, F f) {
    const uint64_t data = g.Get<uint64_t>(map);
    const int32_t num = g.Get<int32_t>(map + 8);
    if (!data || num <= 0 || num > 100000) return;
    const int32_t nfree = g.Get<int32_t>(map + 0x34);
    const uint64_t secondary = g.Get<uint64_t>(map + 0x20);
    const int32_t maxbits = g.Get<int32_t>(map + 0x2c);
    const uint64_t words = secondary && maxbits > 128 ? secondary : map + 0x10;
    for (int32_t i = 0; i < num; ++i) {
        if (nfree > 0 && !((g.Get<uint32_t>(words + 4ull * (i / 32)) >> (i % 32)) & 1)) continue;
        if (!f(data + stride * size_t(i))) return;
    }
}
} // namespace

void Reader::ReadSave(GameState& s) {
    const uint64_t save = g_.Get<uint64_t>(save_mgr_.addr + kSaveMgrGameSave);
    if (!save) { s.status = "no save object"; return; }
    s.money = g_.Get<int32_t>(save + kSavePlayer);
    s.play_time = g_.Get<float>(save + kSavePlayer + 0x10);
    s.difficulty = g_.Get<uint8_t>(save + kSavePlayer + 0x20);
    TArr party = g_.Get<TArr>(save + kSaveParty);
    const int32_t guest = g_.Get<int32_t>(save + kSaveParty + 0x10);  // SaveData_Party.guestUnitId (-1 or 0: none)
    s.guest = guest > 0 ? guest : 0;
    if (party.num > 0 && party.num <= 8) {
        s.party.resize(size_t(party.num));
        g_.Read(party.data, s.party.data(), s.party.size() * 4);
    }
    TArr units = g_.Get<TArr>(save + kSaveUnitParams);
    if (units.num > 0 && units.num <= 64) {
        std::vector<uint8_t> b(size_t(units.num) * kUnitParamSize);
        if (g_.Read(units.data, b.data(), b.size()))
            for (int i = 0; i < units.num; ++i) {
                const uint8_t* e = &b[size_t(i) * kUnitParamSize];
                UnitState u;
                std::memcpy(&u.id, e, 4);
                std::memcpy(&u.xp, e + 4, 4);
                std::memcpy(&u.hp, e + 8, 4);
                std::memcpy(&u.mp, e + 12, 4);
                std::memcpy(&u.vision, e + 0x20, 4);
                std::memcpy(&u.weapon, e + 0x24, 4);
                std::memcpy(&u.armor, e + 0x28, 4);
                TArr acc;
                std::memcpy(&acc, e + 0x30, sizeof acc);
                if (acc.num > 0 && acc.num <= 8) {
                    u.accessories.resize(size_t(acc.num));
                    g_.Read(acc.data, u.accessories.data(), u.accessories.size() * 4);
                }
                TArr sk;
                std::memcpy(&sk, e + 0x40, sizeof sk);
                if (sk.num > 0 && sk.num <= 32) {
                    u.skills.resize(size_t(sk.num));
                    g_.Read(sk.data, u.skills.data(), u.skills.size() * 4);
                }
                u.join = e[0x80];
                u.usable = e[0x81] & 1;
                s.units.push_back(std::move(u));
            }
    }
    vision_points_.clear();
    TArr vis = g_.Get<TArr>(save + kSaveVisions);  // SaveData_Vision (0x58): ID, awakeningPoint, synchroPoints Map<int, int>
    if (vis.num > 0 && vis.num <= 512) {
        std::vector<uint8_t> b(size_t(vis.num) * 0x58);
        if (g_.Read(vis.data, b.data(), b.size()))
            for (int i = 0; i < vis.num; ++i) {
                const uint8_t* e = &b[size_t(i) * 0x58];
                int32_t id, awk;
                std::memcpy(&id, e, 4), std::memcpy(&awk, e + 4, 4);
                VisionPoints& vp = vision_points_[id];
                vp.awakening = awk;
                ForEachMap(g_, vis.data + 0x58ull * size_t(i) + 8, 0x10, [&](uint64_t p) {
                    vp.synchro[g_.Get<int32_t>(p)] = g_.Get<int32_t>(p + 4);
                    return true;
                });
            }
    }
    TArr inv = g_.Get<TArr>(save + kSaveInventory);
    if (inv.num > 0 && inv.num <= 4096) {
        s.inventory.resize(size_t(inv.num));
        g_.Read(inv.data, s.inventory.data(), s.inventory.size() * sizeof(ItemStack));
    }
    s.save_ok = !s.units.empty();
}

std::string Reader::SoftPath(uint64_t soft) {
    // TPersistentObjectPtr<FSoftObjectPath>: FWeakObjectPtr (8), then PackageName FName, AssetName FName
    const uint32_t pkg = g_.Get<uint32_t>(soft + 8), number = g_.Get<uint32_t>(soft + 12);
    std::string p = Name(pkg);
    if (number) p += "_" + std::to_string(number - 1);  // FName instance number (Town_01Gra_22 + 6 = Town_01Gra_22_5)
    if (p.rfind("/Game/", 0) == 0) return "FFRS/Content/" + p.substr(6) + ".uasset";
    return "";
}

bool Reader::ReadPlayer(const EdenDsmodHostApi* host, MapState& m) {
    g_.Set(host);
    if (!minimap_.addr) return false;
    struct { double x, y, z; } loc{}, rot{};
    // (the map id too: ReadMap and the 30 Hz read share the heading's history, and a map change starts it over)
    m.map_id = g_.Get<int32_t>(minimap_.addr + kMiniPropKey);
    bool ok = g_.Read(minimap_.addr + kMiniPlayerLoc, &loc, sizeof loc);
    if (controller_.addr) {  // the live pawn: PlayerController.AcknowledgedPawn -> RootComponent
        const uint64_t pawn = g_.Get<uint64_t>(controller_.addr + kPcPawn);
        const uint64_t root = pawn ? g_.Get<uint64_t>(pawn + kActorRoot) : 0;
        if (root && g_.Read(root + kSceneLoc, &loc, sizeof loc)) {
            g_.Read(root + kSceneRot, &rot, sizeof rot);  // pitch, yaw, roll
            ok = true;
        }
    }
    if (!ok || !std::isfinite(loc.x) || !std::isfinite(loc.y)) return false;
    // The land-capture layout is world space turned a quarter: land x = world Y, land y = -world X.
    m.px = loc.y, m.py = -loc.x, m.pz = loc.z;
    Heading(m, rot.y);
    return true;
}

void Reader::Heading(MapState& m, double yaw_fallback) {
    // The heading: the direction the player last moved in. The root component keeps one yaw while the character walks (the game turns
    // its mesh, not the actor), so the root's yaw is only the fallback before the first step. Land (dx, dy) with y down: the icon
    // points up at 0 degrees and turns clockwise, so the angle is atan2(dx, -dy). A jump of more than 20 m (a warp or a new map)
    // starts over; a step under 20 cm (standing, or jitter) keeps the last heading, and short steps add up until they pass it
    // (the world map's small character moves only a few cm a tick).
    {
        const double dx = m.px - last_px_, dy = m.py - last_py_, d2 = dx * dx + dy * dy;
        if (!have_pos_ || d2 > 2000.0 * 2000.0 || m.map_id != last_map_) {
            have_pos_ = true, last_px_ = m.px, last_py_ = m.py, last_map_ = m.map_id;
        } else if (d2 > 20.0 * 20.0) {
            heading_ = std::atan2(dx, -dy) * 180.0 / 3.14159265358979, have_heading_ = true;
            last_px_ = m.px, last_py_ = m.py;
        }
    }
    m.yaw = have_heading_ ? heading_ : yaw_fallback;
}

void Reader::ReadMap(GameState& s) {
    MapState& m = s.map;
    const uint64_t mini = minimap_.addr;
    m.map_id = g_.Get<int32_t>(mini + kMiniPropKey);
    ReadPlayer(g_.host(), m);
    const uint64_t area_mgr = g_.Get<uint64_t>(mini + kMiniAreaMgr);
    if (area_mgr) {
        m.area_id = g_.Get<int32_t>(area_mgr + kAreaMgrCurrent);
        const uint64_t ui_map = g_.Get<uint64_t>(area_mgr + kAreaMgrMapDA);
        const uint64_t land = ui_map ? g_.Get<uint64_t>(ui_map + kUiMapLandCaptureDA) : 0;
        if (land) {
            // Map<int, MapLandCaptureData(0x70)>: key 0, value 8, element 0x80
            ForEachMap(g_, land + kLandCaptureMap, 0x80, [&](uint64_t e) {
                const int32_t key = g_.Get<int32_t>(e);
                if (key == kWorldMapId && m.map_id != kWorldMapId) {  // the world's islands, for the world view from here
                    ForEachMap(g_, e + 8 + 0x20, 0x130, [&](uint64_t a) {
                        Island is;
                        is.area = g_.Get<int32_t>(a);
                        struct { double x, y; } wl{}, bs{};
                        g_.Read(a + 8 + 0x50, &wl, sizeof wl);
                        g_.Read(a + 8 + 0x60, &bs, sizeof bs);
                        is.wx = wl.x, is.wy = wl.y, is.bw = bs.x, is.bh = bs.y;
                        ForEachMap(g_, a + 8, 0x38, [&](uint64_t t) {
                            is.texture = SoftPath(t + 8);
                            return is.texture.empty();
                        });
                        if (!is.texture.empty() && is.bw > 0 && is.bh > 0 && is.bw < 1e8 && is.bh < 1e8 && is.texture.find("/MapCapture/Wld/") != std::string::npos)
                            m.world_islands.push_back(is);
                        return m.world_islands.size() < 64;
                    });
                    return true;
                }
                if (key != m.map_id) return true;
                // MapLandCaptureData.AreaLandCaptureDataMap (+0x20): Map<int, LandCaptureData(0x120)>: element 0x130.
                // Every area of the map: the current one is the town or dungeon picture; on the world map each island is one.
                ForEachMap(g_, e + 8 + 0x20, 0x130, [&](uint64_t a) {
                    Island is;
                    is.area = g_.Get<int32_t>(a);
                    const uint64_t v = a + 8;
                    struct { double x, y; } wl{}, bs{};
                    g_.Read(v + 0x50, &wl, sizeof wl);
                    g_.Read(v + 0x60, &bs, sizeof bs);
                    is.wx = wl.x, is.wy = wl.y, is.bw = bs.x, is.bh = bs.y;
                    // LandTextureMap: Map<enum, SoftObject<Texture2D>(0x28)>: value 8, element 0x38
                    ForEachMap(g_, v, 0x38, [&](uint64_t t) {
                        is.texture = SoftPath(t + 8);
                        return is.texture.empty();
                    });
                    if (is.texture.empty() || !(is.bw > 0 && is.bh > 0 && is.bw < 1e8 && is.bh < 1e8)) return true;
                    if (is.area == m.area_id) m.texture = is.texture, m.wx = is.wx, m.wy = is.wy, m.bw = is.bw, m.bh = is.bh;
                    if (is.texture.find("/MapCapture/Wld/") != std::string::npos) m.islands.push_back(is);
                    return m.islands.size() < 64;
                });
                return true;  // (go on: the world map's entry may come after)
            });
        }
    }
    // The world map: its areas are the islands' captures. Some islands have story variants at the same place (Wld_7 / 17 / 18,
    // near-identical pictures): one per place, the first.
    auto uniq = [](std::vector<Island>& v) {
        std::sort(v.begin(), v.end(), [](const Island& a, const Island& b) { return a.area < b.area; });
        std::vector<Island> u;
        for (auto& is : v) {
            bool dup = false;
            for (auto& o : u) dup = dup || (std::abs(o.wx - is.wx) < 300 && std::abs(o.wy - is.wy) < 300 && std::abs(std::max(o.bw, o.bh) - std::max(is.bw, is.bh)) < 600);
            if (!dup) u.push_back(is);
        }
        v = std::move(u);
    };
    uniq(m.islands);
    uniq(m.world_islands);
    m.world = !m.islands.empty();
    if (m.world) m.world_islands = m.islands;
    m.valid = m.map_id > 0;
    if (m.valid) m.name = MapName(m.map_id);
    if (m.valid) ReadIcons(m);  // (on the world map only the objectives: its other icons are dozens of towns)
    if (m.valid) ReadLandmarks(m);
    if (m.valid && m.world) last_here_ = -1;  // out on the world map: no town to remember
}

// The places the world map shows, whether each was visited, and the one the player is in.
void Reader::ReadLandmarks(MapState& m) {
    std::unordered_map<int, uint8_t> flags;  // CPP_MuchaGameSaveData.Map.perMapSaveDataMap: Map<int, SaveData_PerMap{known, visited, cleared}>
    if (const uint64_t save = Alive(save_mgr_) ? g_.Get<uint64_t>(save_mgr_.addr + kSaveMgrGameSave) : 0)
        ForEachMap(g_, save + kSaveMap + kSavePerMap, 0x10, [&](uint64_t e) {
            const uint8_t b[2] = {g_.Get<uint8_t>(e + 4), g_.Get<uint8_t>(e + 5)};
            flags[g_.Get<int32_t>(e)] = uint8_t((b[0] & 1) | (b[1] & 1) << 1);
            return flags.size() < 4096;
        });
    auto flag = [&](const std::vector<int>& ids, uint8_t bit) {
        for (int i : ids) { auto it = flags.find(i); if (it != flags.end() && (it->second & bit)) return true; }
        return false;
    };
    auto add = [&](int key, int id, std::vector<int> ids, double wx, double wy) {
        Landmark l;
        l.key = key, l.map_id = id;
        l.x = wy, l.y = -wx;  // land = (world Y, -world X)
        for (int j : Mates(id)) if (std::find(ids.begin(), ids.end(), j) == ids.end()) ids.push_back(j);
        l.ids = std::move(ids);
        l.visited = flag(l.ids, 2);
        const MapRow& r = MapInfo(id);
        l.type = r.type, l.name = r.name;
        for (size_t j = 0; l.name.empty() && j < l.ids.size(); ++j) l.name = MapInfo(l.ids[j]).name;  // (a variant row may lack it)
        if (l.name.empty()) {
            l.name_ns = r.name_ns, l.name_src = r.name_src;
            for (size_t j = 0; l.name_src.empty() && j < l.ids.size(); ++j) {
                const MapRow& v = MapInfo(l.ids[j]);
                l.name_ns = v.name_ns, l.name_src = v.name_src;
            }
        }
        m.landmarks.push_back(std::move(l));
    };
    // The world map's own table (CPP_UI_WldMapSubsystem.FldObjDataMap: the known places), filled once its menu has opened:
    // Map<int, StaticFieldObjectData(0x50)>: value +8, element 0x60. RelatedMapId +0x18, Location +0x30.
    if (Alive(wld_sub_))
        ForEachMap(g_, wld_sub_.addr + 0x48, 0x60, [&](uint64_t e) {
            const uint64_t v = e + 8;
            const int id = g_.Get<int32_t>(v + 0x18);
            struct { double x, y, z; } loc{};
            if (id > 0 && g_.Read(v + 0x30, &loc, sizeof loc) && std::isfinite(loc.x) && std::isfinite(loc.y)) {
                add(g_.Get<int32_t>(e), id, {id}, loc.x, loc.y);
                m.landmarks.back().landing = g_.Get<int32_t>(v + 0x48, -1);  // LandingPointId
            }
            return m.landmarks.size() < 128;
        });
    // Before that: the game's landmark list (DA_LandmarkList.DataList +0x38, MAP_LANDMARK_DATA_LIST 0x50: landmarkDataList +0x08
    // {Condition, mapId +0x10} 0x18 each, Location +0x18). A row is a place the player knows when one of its maps (or of their
    // group) is known in the save; it is named by the first such map.
    if (m.landmarks.empty() && Alive(lm_list_)) {
        TArr rows = g_.Get<TArr>(lm_list_.addr + 0x38);
        for (int32_t i = 0; rows.data && i < rows.num && i < 512; ++i) {
            const uint64_t e = rows.data + 0x50ull * uint32_t(i);
            TArr ld = g_.Get<TArr>(e + 8);
            std::vector<int> ids;
            for (int32_t j = 0; ld.data && j < ld.num && j < 8; ++j) {
                const int id = g_.Get<int32_t>(ld.data + 0x18ull * uint32_t(j) + 0x10);
                if (id > 0) ids.push_back(id);
            }
            int pick = -1;
            for (int id : ids) if (pick < 0 && flag({id}, 1)) pick = id;
            for (int id : ids) if (pick < 0 && flag(Mates(id), 1)) pick = id;
            struct { double x, y, z; } loc{};
            if (pick > 0 && g_.Read(e + 0x18, &loc, sizeof loc) && std::isfinite(loc.x) && std::isfinite(loc.y)) add(10000 + i, pick, ids, loc.x, loc.y);
        }
    }
    std::sort(m.landmarks.begin(), m.landmarks.end(), [](const Landmark& a, const Landmark& b) { return a.key < b.key; });
    if (m.world) return;
    if (m.map_id > 0) m.map_type = MapInfo(m.map_id).type;  // (DA_WldMapParameter.FastTravelMapTypeEnableMap {Dungeon: 0})
    // Where the player is: the place that is this map (or one of its group: a dungeon's floors); inside a building with a map of
    // its own, the last place found
    for (size_t i = 0; i < m.landmarks.size() && m.here < 0; ++i) {
        const auto& ids = m.landmarks[i].ids;
        if (std::find(ids.begin(), ids.end(), m.map_id) != ids.end()) m.here = int(i);
    }
    for (size_t i = 0; i < m.landmarks.size() && m.here < 0; ++i)
        if (m.landmarks[i].map_id == last_here_) m.here = int(i);
    if (m.here >= 0) last_here_ = m.landmarks[size_t(m.here)].map_id;
}

bool Reader::ReadUi(const EdenDsmodHostApi* host, UiState& u) {
    g_.Set(host);
    u = UiState{};
    if (!Alive(widgets_)) {
        const uint64_t now = NowMs();
        if (now - last_wm_scan_ms_ > 1500) { last_wm_scan_ms_ = now; Scan(); last_scan_ms_ = now; }
        if (!Alive(widgets_)) return false;
    }
    const uint64_t lay = g_.Get<uint64_t>(widgets_.addr + 0x30);  // CPP_EU_GameLayoutSubsystem.PrimaryGameLayout
    if (!lay) return false;
    if (lay != layout_ || layers_.empty()) {  // Layers (+0x3a0): Map<GameplayTag, CommonActivatableWidgetContainerBase*>, element 0x18
        layers_.clear();
        ForEachMap(g_, lay + 0x3a0, 0x18, [&](uint64_t e) {
            layers_.push_back({Name(g_.Get<uint32_t>(e)), g_.Get<uint64_t>(e + 8)});
            return layers_.size() < 32;
        });
        layout_ = lay;
        if (layers_.empty()) return false;
    }
    const uint64_t cur = g_.Get<uint64_t>(lay + 0x540);  // CurrentInputTargetLayer
    for (auto& [tag, stack] : layers_) {
        const int32_t n = g_.Get<int32_t>(stack + 0x190 + 8);  // WidgetList.Num
        if (stack == cur) u.input = tag;
        if (tag == "UI.Layer.Event") u.event = n > 0;
        if (tag == "UI.Layer.Save") u.save = n > 0;
    }
    if (cur && u.input.empty()) u.input = "?";
    u.valid = true;
    return true;
}

std::string Reader::ObjectPackage(uint64_t obj) {
    uint64_t o = obj;
    for (int i = 0; i < 8 && o; ++i) {
        const uint64_t outer = g_.Get<uint64_t>(o + 0x20);
        if (!outer) return Name(g_.Get<uint32_t>(o + 0x18));
        o = outer;
    }
    return "";
}

// The conversation. The game's backlog (CPP_BacklogSubsystem +0x30: TArray<BacklogData 0x48>, SpeakerName FText +0, Text FText +0x10)
// keeps the newest 30 lines; the line in the talk window is its newest. The talk window: the Event layer's talk layout
// (a CPP_WBP_Talk: WindowManager +0x598) -> m_pWindow (+0x190, a CPP_WBP_TalkWindow) -> m_pTextBlock (+0x558, text FText +0x188) and
// m_pSpeaker (+0x5a0, a CPP_WBP_TalkSpeaker) -> m_pTexture (+0x348): the speaker's bust, /Game/Chara/Face/<id>_<emotion>_BU.
constexpr uint64_t kBacklogList = 0x30, kBacklogStride = 0x48, kTalkWm = 0x598, kWmWindow = 0x190, kWinText = 0x558, kWinSpeaker = 0x5a0,
                   kSpeakerTex = 0x348, kTextBlockText = 0x188;
bool Reader::ReadTalk(const EdenDsmodHostApi* host, TalkState& t) {
    g_.Set(host);
    t = TalkState{};
    UiState u;
    if (ReadUi(host, u) && u.event)
        for (auto& [tag, stack] : layers_) {
            if (tag != "UI.Layer.Event") continue;
            const TArr wl = g_.Get<TArr>(stack + 0x190);
            for (int i = 0; i < std::min(wl.num, 8); ++i) {
                const uint64_t w = g_.Get<uint64_t>(wl.data + 8ull * size_t(i));
                const uint64_t cls = w ? g_.Get<uint64_t>(w + 0x10) : 0;
                if (!cls || !IsA(cls, "CPP_WBP_Talk")) continue;
                const uint64_t wm = g_.Get<uint64_t>(w + kTalkWm);
                const uint64_t win = wm ? g_.Get<uint64_t>(wm + kWmWindow) : 0;
                if (!win) continue;
                t.active = true;
                const uint64_t tb = g_.Get<uint64_t>(win + kWinText);
                if (tb) t.text = FTextString(tb + kTextBlockText, 1024);
                const uint64_t sp = g_.Get<uint64_t>(win + kWinSpeaker);
                const uint64_t tex = sp ? g_.Get<uint64_t>(sp + kSpeakerTex) : 0;
                const std::string pkg = tex ? ObjectPackage(tex) : "";
                if (pkg.rfind("/Game/Chara/Face/", 0) == 0) t.portrait = "FFRS/Content" + pkg.substr(5) + ".uasset";
            }
        }
    if (!Alive(backlog_)) {
        const uint64_t now = NowMs();
        if (now - last_wm_scan_ms_ > 1500) { last_wm_scan_ms_ = now; Scan(); last_scan_ms_ = now; }
        if (!Alive(backlog_)) return false;
    }
    const TArr bl = g_.Get<TArr>(backlog_.addr + kBacklogList);
    if (bl.num < 0 || bl.num > 256) return false;
    for (int i = 0; i < bl.num; ++i) {
        const uint64_t e = bl.data + kBacklogStride * uint64_t(i);
        t.backlog.push_back({FTextString(e, 128), FTextString(e + 0x10, 1024)});
    }
    t.valid = true;
    return true;
}

bool Reader::TravelAsset(const EdenDsmodHostApi* host, uint64_t& da, uint64_t& cls) {
    g_.Set(host);
    if (!Alive(wld_param_)) {
        const uint64_t now = NowMs();
        if (now - last_wm_scan_ms_ > 1500) { last_wm_scan_ms_ = now; Scan(); last_scan_ms_ = now; }
        if (!Alive(wld_param_)) return false;
    }
    da = wld_param_.addr, cls = wld_param_.cls;
    return true;
}

bool Reader::ItemAssets(const EdenDsmodHostApi* host, uint64_t& action, uint64_t& action_cls, uint64_t& target, uint64_t& target_cls) {
    g_.Set(host);
    if (!Alive(skill_action_)) {
        const uint64_t now = NowMs();
        if (now - last_wm_scan_ms_ > 1500) { last_wm_scan_ms_ = now; Scan(); last_scan_ms_ = now; }
        if (!Alive(skill_action_)) return false;
    }
    action = skill_action_.addr, action_cls = skill_action_.cls;
    const bool t = Alive(skill_targeting_);
    target = t ? skill_targeting_.addr : 0, target_cls = t ? skill_targeting_.cls : 0;
    return true;
}

int Reader::PlayerMapId(const EdenDsmodHostApi* host) {
    g_.Set(host);
    return Alive(minimap_) ? g_.Get<int32_t>(minimap_.addr + kMiniPropKey) : -1;
}

bool Reader::ReadWorldMenu(const EdenDsmodHostApi* host, WorldMenu& w, bool scan) {
    g_.Set(host);
    w = WorldMenu{};
    if (scan && !(Alive(wld_vm_) && Alive(wld_list_) && Alive(dialogs_) && Alive(main_vm_))) {
        const uint64_t now = NowMs();
        if (now - last_wm_scan_ms_ > 700) {
            last_wm_scan_ms_ = now;
            Scan();
            last_scan_ms_ = now;
        }
    }
    if (Alive(dialogs_)) w.dialog = g_.Get<uint64_t>(dialogs_.addr + 0x40) != 0;  // DialogWidget
    if (Alive(main_vm_)) {  // SceneDataStack (+0x80): the main menu's scenes, empty when it is closed
        const int32_t n = g_.Get<int32_t>(main_vm_.addr + 0x80 + 8);
        w.main_menu = n > 0 && n < 64 ? n : 0;
    }
    if (!Alive(wld_vm_) || !Alive(wld_list_)) return false;
    w.found = true;
    w.list_open = Name(g_.Get<uint32_t>(wld_vm_.addr + 0x180)) == "UI.WldMap.Scene.Location";  // SceneTag
    w.focus = g_.Get<int32_t>(wld_vm_.addr + 0x3a0 + 0x18);  // FocusObjectData.RelatedMapId
    w.can_travel = g_.Get<uint8_t>(wld_vm_.addr + 0x408) & 1;  // FastTravelBehavior.bCanTravel
    w.travel_dialog = Name(g_.Get<uint32_t>(wld_vm_.addr + 0x40c));  // FastTravelBehavior.DialogTag
    w.category = g_.Get<uint32_t>(wld_list_.addr + 0x90) != 0;  // DisplayCategory (None: the map is closed)
    TArr ids = g_.Get<TArr>(wld_list_.addr + 0xa8);  // DisplayLandmarkIdList
    if (ids.data && ids.num > 0 && ids.num < 256) {
        w.list.resize(size_t(ids.num));
        if (!g_.Read(ids.data, w.list.data(), w.list.size() * 4)) w.list.clear();
    }
    return true;
}

namespace {
// The game's interact tags (Field.Object.Interact.Type.*) and the minimap icon each shows (UI/Textures/Icon/MAP).
const char* IconForTag(const std::string& tag) {
    static const std::pair<const char*, const char*> kTags[] = {
        {"Shop.Weapon", "ICON_MAP_L_Weapon"},     {"Shop.Armor", "ICON_MAP_L_Armar"},     {"Shop.Accessory", "ICON_MAP_L_AccessoryShop"},
        {"Shop.Inn", "ICON_MAP_L_Bed"},           {"Shop.Tool", "ICON_MAP_L_Shop"},       {"Shop.Vision", "ICON_MAP_L_VisionUpdate"},
        {"Shop.Chocobo", "ICON_MAP_L_Chocobo"},   {"Shop.Race", "ICON_MAP_L_Chocobo"},    {"Shop.DLC", "ICON_MAP_L_DLCshop"},
        {"Shop.Bank", "ICON_MAP_L_Shop"},         {"Shop.Fortunetelling", "ICON_MAP_L_Shop"}, {"Shop.Music", "ICON_MAP_L_Shop"},
        {"Shop", "ICON_MAP_L_Shop"},              {"Save", "ICON_MAP_L_SavePoint"},
        {"Colosseum", "ICON_MAP_L_ColosseumRank"}, {"MythicalBeast", "ICON_MAP_L_Beast"}, {"Landmark", "ICON_MAP_L_Town"},
    };
    const std::string pre = "Field.Object.Interact.Type.";
    if (tag.rfind(pre, 0) != 0) return nullptr;
    const std::string t = tag.substr(pre.size());
    for (auto& [k, icon] : kTags)
        if (t == k || t.rfind(std::string(k) + ".", 0) == 0) return icon;
    return nullptr;
}
} // namespace

// The quests (CPP_QuestSubsystem +0x38 QuestDataMap: Map<int, QuestData(0x98)>, element 0xa8). In QuestData: ID +0x10,
// Category +0x14 (EQuestCategory: 1 Main, 2 Sub, 3 Mini), Progression +0x68 (EQuestProgression: 1 Start, 2 Progress, 3 Complete).
// The journal: every quest of QuestDataMap (Map<int, QuestData 0x98>: value +8, element 0xa8). Its texts are read again only
// when the quest moves on (its progression or the length of its summary list).
void Reader::ReadQuestList(GameState& s) {
    if (!Alive(quest_sub_)) return;
    s.tracked_quest = g_.Get<int32_t>(quest_sub_.addr + 0x9c);  // PreferredQuestId
    const uint64_t now = NowMs();
    if (now - last_quest_list_ms_ >= 1000 || quest_list_.empty()) {
        last_quest_list_ms_ = now;
        std::vector<Quest> list;
        ForEachMap(g_, quest_sub_.addr + 0x38, 0xa8, [&](uint64_t e) {
            const uint64_t v = e + 8;
            uint8_t q[0x98];
            if (!g_.Read(v, q, sizeof q)) return true;
            Quest k;
            std::memcpy(&k.id, q + 0x10, 4);
            std::memcpy(&k.priority, q + 0x0c, 4);
            k.cat = q[0x14], k.prog = q[0x68];
            k.can_track = q[0x08] & 1, k.tracked = q[0x80] & 1;
            if (k.id <= 0 || k.cat < 1 || k.cat > 3 || k.prog > 3) return true;
            TArr sum{};
            std::memcpy(&sum, q + 0x58, sizeof sum);
            QuestTexts& t = quest_texts_[k.id];
            // (read again while the name is missing: the game loads a quest's texts some time after the quest itself)
            if (t.prog != k.prog || t.summaries != sum.num || (t.q.name.empty() && now - t.read_ms >= 3000)) {
                t.prog = k.prog, t.summaries = sum.num, t.read_ms = now;
                t.q.name = FTextString(v + 0x18), t.q.subtitle = FTextString(v + 0x28), t.q.place = FTextString(v + 0x38);
                t.q.name_ns.clear(), t.q.name_src.clear();
                if (t.q.name.empty()) StringTableSource(v + 0x18, t.q.name_ns, t.q.name_src);
                t.q.desc = FTextString(v + 0x48, 2048);
                t.q.log.clear();
                for (int32_t i = 0; sum.data && i < sum.num && i < 24; ++i) {
                    std::string line = FTextString(sum.data + 0x10ull * uint32_t(i), 2048);
                    if (!line.empty()) t.q.log.push_back(std::move(line));
                }
            }
            k.name_ns = t.q.name_ns, k.name_src = t.q.name_src;
            k.name = t.q.name, k.subtitle = t.q.subtitle, k.place = t.q.place, k.desc = t.q.desc, k.log = t.q.log;
            list.push_back(std::move(k));
            return list.size() < 1024;
        });
        quest_list_ = std::move(list);
    }
    s.quests = quest_list_;
    for (Quest& k : s.quests) k.tracked = k.id == s.tracked_quest;
}

bool Reader::QuestSystem(const EdenDsmodHostApi* host, uint64_t& sub, uint64_t& cls) {
    g_.Set(host);
    if (!Alive(quest_sub_)) return false;
    sub = quest_sub_.addr, cls = quest_sub_.cls;
    return true;
}

void Reader::ReadQuests() {
    const uint64_t now = NowMs();
    if (now - last_quest_ms_ < 1000 && !quests_.empty()) return;
    last_quest_ms_ = now;
    quests_.clear();
    if (!quest_sub_.addr) return;
    ForEachMap(g_, quest_sub_.addr + 0x38, 0xa8, [&](uint64_t e) {
        uint8_t q[0x70];
        if (!g_.Read(e + 8, q, sizeof q)) return true;
        int32_t id;
        std::memcpy(&id, q + 0x10, 4);
        if (id > 0 && q[0x14] <= 3 && q[0x68] <= 3) quests_[id] = {q[0x14], q[0x68]};
        return quests_.size() < 4096;
    });
}

// The minimap's entries (CPP_MiniMapSubsystem.EntryDataMap): the interact tag says what it is, the reference actor where it is.
// Only the current area's (indoors, the town's entries stay in the map), and never treasure: the game's map does not show it.
// An entry is an objective when the game marks it as the quest target, or when one of its quests (FieldObjectCoreData
// RelatedQuestIdList) is a side quest in progress.
void Reader::ReadIcons(MapState& m) {
    // Map<int, MiniMapEntryData(0xd8)>: value +8, element 0xe8. In the value: ObjectData +0x60 (FieldObjectData: InteractTag +0x10,
    // RelatedQuestIdList +0x20, ReferenceActor soft pointer +0x30, whose FWeakObjectPtr is {index, serial}), BelongAreaId +0xb8,
    // bIsMarkedQuestTarget +0xbc.
    constexpr size_t kMaxGoals = 16;
    m.goals.clear();
    ReadQuests();
    ForEachMap(g_, minimap_.addr + 0xc0, 0xe8, [&](uint64_t e) {
        const uint64_t v = e + 8;
        struct { int32_t area; uint8_t goal; } tail{};
        g_.Read(v + 0xb8, &tail, 5);
        // its quests in progress: the main story's, side quests' (Sub, Mini); the marked one with unknown quests counts as the story's
        bool main_on = false, side_on = false, known = false;
        TArr rel = g_.Get<TArr>(v + 0x60 + 0x20);
        if (rel.data && rel.num > 0 && rel.num <= 16) {
            int32_t ids[16];
            if (g_.Read(rel.data, ids, 4ull * size_t(rel.num)))
                for (int i = 0; i < rel.num; ++i) {
                    auto it = quests_.find(ids[i]);
                    if (it == quests_.end()) continue;
                    const QuestInfo q = it->second;
                    const bool on = q.prog == 1 || q.prog == 2;
                    known = true;
                    if (q.cat == 1) main_on = main_on || on;
                    else if (q.cat == 2 || q.cat == 3) side_on = side_on || on;
                }
        }
        if ((tail.goal & 1) && !known) main_on = true;
        const int kind = (main_on ? 1 : 0) | (side_on ? 2 : 0);  // bits: 1 main, 2 side
        const bool goal = kind && m.goals.size() < kMaxGoals;
        const char* icon = m.world ? nullptr : IconForTag(Name(g_.Get<uint32_t>(v + 0x60 + 0x10)));
        if (!icon && !goal) return true;
        if (!m.world && tail.area > 0 && m.area_id > 0 && tail.area != m.area_id) return true;
        struct { int32_t index, serial; } weak{};
        g_.Read(v + 0x60 + 0x30, &weak, sizeof weak);
        const uint64_t actor = Resolve(weak.index, weak.serial);
        const uint64_t root = actor ? g_.Get<uint64_t>(actor + kActorRoot) : 0;
        struct { double x, y, z; } loc{};
        if (!root || !g_.Read(root + kSceneLoc, &loc, sizeof loc) || !std::isfinite(loc.x) || !std::isfinite(loc.y)) return true;
        const double x = loc.y, y = -loc.x;
        // an entry without an area: keep it when it lies in the current area's box
        if (!m.world && tail.area <= 0 && m.bw > 0 && (std::abs(x - m.wx) > m.bw / 2 || std::abs(y - m.wy) > m.bh / 2)) return true;
        if (goal) {
            bool dup = false;  // one marker per spot (an NPC can be both a main and a side quest's)
            for (auto& g : m.goals)
                if (std::abs(g.x - x) < 50 && std::abs(g.y - y) < 50) dup = true, g.main = g.main || (kind & 1), g.side = g.side || (kind & 2);
            if (!dup) m.goals.push_back({x, y, (kind & 1) != 0, (kind & 2) != 0});
        }
        if (icon) m.icons.push_back({x, y, icon});
        return m.icons.size() < 96;
    });
    // the main story's first, so the gold one is drawn over the others
    std::stable_sort(m.goals.begin(), m.goals.end(), [](const MapState::Goal& a, const MapState::Goal& b) { return a.main > b.main; });
}

namespace {
// CPP_UnitParameter from the unit id to the break gauge in one read: id +0xf8 .. HP +0x10c, MP +0x110, BP +0x114
// (UNIT_PARAMETER's order, the live object 0xc8 further on: Level 0x38 -> 0x100, HitPoint 0x44 -> 0x10c, Breakpoint 0x4c -> 0x114,
// MaxBreakPoint 0x60 -> 0x128)
struct UpHead {
    int32_t id, pad0, level, pad1, pad2, hp, mp, bp;
};
static_assert(sizeof(UpHead) == 0x20 && offsetof(UpHead, hp) == kUpHp - kUpId && offsetof(UpHead, level) == kUpLevel - kUpId);
constexpr uint64_t kUpMaxBp = 0x128;
// The unit's table row (UNIT_PARAMETER) is held at +0xc8: its FaceIconId (+0x188) and IsBoss (+0x150)
constexpr uint64_t kUpRow = 0xc8, kUpFaceIcon = kUpRow + 0x188, kUpIsBoss = kUpRow + 0x150;
void SetBreak(BattleUnit& b, int bp, int max_bp) {
    b.bp = bp, b.max_bp = max_bp;
    b.staggered = b.enemy && b.hp > 0 && max_bp > 0 && max_bp < 1000000 && bp <= 0;
}
constexpr uint64_t kBtlVisionOwner = 0x370;  // CPP_BtlUnitParameter.m_pVisionOwner: a vision fights for its owner's side
} // namespace

void Reader::ReadBattle(GameState& s) {
    const uint64_t um = g_.Get<uint64_t>(battle_.addr + kBtlUnitMgr);
    if (!um) return;
    TArr list = g_.Get<TArr>(um + kBtlUnitList);
    if (list.num <= 0 || list.num > 32) return;
    std::vector<uint64_t> units(size_t(list.num));
    if (!g_.Read(list.data, units.data(), units.size() * 8)) return;
    std::vector<BattleUnit> allies, enemies;
    std::unordered_map<uint64_t, bool> ally_unit;  // CPP_BtlUnit -> on the party's side
    struct Pending { BattleUnit b; uint64_t owner; };
    std::vector<Pending> visions;
    for (uint64_t u : units) {
        const uint64_t param = u ? g_.Get<uint64_t>(u + kBtlUnitParam) : 0;
        if (!param) continue;
        UpHead hd{};
        if (!g_.Read(param + kUpId, &hd, sizeof hd)) continue;
        BattleUnit b;
        b.param = param;
        b.unit = u;
        b.id = hd.id;
        b.hp = hd.hp;
        b.mp = hd.mp;
        int mx[3] = {-1, -1, -1};
        g_.Read(param + kUpMaxHp, mx, sizeof mx);
        b.max_hp = mx[0];
        b.max_mp = mx[1];
        b.max_bp = g_.Get<int32_t>(param + kUpMaxBp);
        b.bp = hd.bp;
        if (b.id <= 0 || b.hp < 0 || b.hp > 10000000) continue;
        b.face = g_.Get<int32_t>(param + kUpFaceIcon);
        if (b.face < 0 || b.face > 99999) b.face = 0;
        b.boss = g_.Get<uint8_t>(param + kUpIsBoss) & 1;
        b.enemy = !(s.guest && b.id == s.guest);  // a guest (a temporary ally) is not in the save's unit list
        for (auto& x : s.units)
            if (x.id == b.id) {
                b.enemy = false;
                if (x.max_hp > 0) b.max_hp = std::max(b.max_hp, x.max_hp);
                if (x.max_mp > 0) b.max_mp = std::max(b.max_mp, x.max_mp);
            }
        const uint64_t owner = g_.Get<uint64_t>(param + kBtlVisionOwner);
        if (b.enemy && owner) { visions.push_back({b, owner}); continue; }
        ally_unit[u] = !b.enemy;
        SetBreak(b, b.bp, b.max_bp);
        (b.enemy ? enemies : allies).push_back(b);
    }
    for (auto& v : visions) {  // a vision equipped to a party member stands with the party, beside its owner
        auto it = ally_unit.find(v.owner);
        v.b.enemy = !(it != ally_unit.end() && it->second);
        if (!v.b.enemy) v.b.owner = v.owner;
        SetBreak(v.b, v.b.bp, v.b.max_bp);
        (v.b.enemy ? enemies : allies).push_back(v.b);
    }
    // the party in its own order (the game's formation), then anyone else on its side
    // (a vision ranks as its owner, after it)
    std::unordered_map<uint64_t, int> id_of;
    for (auto& a : allies) id_of[a.unit] = a.id;
    std::stable_sort(allies.begin(), allies.end(), [&](const BattleUnit& a, const BattleUnit& b) {
        auto rank = [&](const BattleUnit& u) {
            const int id = u.owner && id_of.count(u.owner) ? id_of[u.owner] : u.id;
            return int(std::find(s.party.begin(), s.party.end(), id) - s.party.begin()) * 2 + (u.owner ? 1 : 0);
        };
        return rank(a) < rank(b);
    });
    s.battle = std::move(enemies);
    s.battle.insert(s.battle.end(), allies.begin(), allies.end());
    s.in_battle = !s.battle.empty();
}

bool Reader::RefreshBattle(const EdenDsmodHostApi* host, GameState& s) {
    g_.Set(host);
    for (BattleUnit& b : s.battle) {
        UpHead hd{};
        if (!b.param || !g_.Read(b.param + kUpId, &hd, sizeof hd) || hd.id != b.id || hd.hp < 0 || hd.hp > 10000000) return false;
        b.hp = hd.hp;
        b.mp = hd.mp;
        SetBreak(b, hd.bp, b.max_bp);
    }
    return true;
}

namespace {
// CPP_VM_BtlCmd (reflection): CommandStack TArray<BtlCmdStackData 0x68> +0x88, FocusCmdData (BtlCmd_EntryValue) +0x100, bDisplayCmd +0x199.
// BtlCmdStackData: state +0, ActionKey {u8 category +4, int id +8}, Index +0xc, EntryIndex +0x18. BtlCmd_EntryValue: state +0,
// EntryIndex +4, Value.ActionKey +8, Value.TargetIdList +0x10. A list (CPP_BtlCmdListBase): BaseStackData +0x30, ActionKeyList +0x98.
constexpr uint64_t kVmStack = 0x88, kVmFocus = 0x100, kVmDisplay = 0x199;
constexpr uint64_t kListKeys = 0x98;  // (a list's BtlCmdVM is at +0x28)
constexpr uint64_t kUnitVmParam = 0x1a0, kUnitVmTarget = 0x1ba;  // CPP_VM_BtlUnitParam: BtlUnitParam, TargetSelectState
struct Focus {
    uint8_t state, pad[3];
    int32_t entry;
    uint8_t cat, pad2[3];
    int32_t id;
    TArr targets;
};
static_assert(sizeof(Focus) == 0x20);
} // namespace

// The cheap part, every refresh: whether the window is up, the cursor and focused entry, the path, whose turn, the targets
void Reader::ReadMenuHead(GameState& s) {
    BattleMenu& mn = s.menu;
    const uint64_t vm = mn.vm;
    mn.open = g_.Get<uint8_t>(vm + kVmDisplay) & 1;
    Focus f{};
    if (!g_.Read(vm + kVmFocus, &f, sizeof f)) { mn.open = false; return; }
    mn.cursor = f.entry;
    mn.focus = {f.cat, f.id};
    mn.path.clear();
    TArr st = g_.Get<TArr>(vm + kVmStack);
    if (st.num > 0 && st.num <= 8) {
        std::vector<uint8_t> b(size_t(st.num) * 0x68);
        if (g_.Read(st.data, b.data(), b.size()))
            for (int i = 0; i < st.num; ++i) {
                ActionRef r;
                r.cat = b[size_t(i) * 0x68 + 4];
                std::memcpy(&r.id, &b[size_t(i) * 0x68 + 8], 4);
                if (r.id) mn.path.push_back(r);
            }
    }
    // CPP_BtlUnitManager: m_pCmdSelectUnit +0x38 (whose command). (Its m_TargetUnitList is filled once a target is confirmed
    // and kept into the next turn: not the pointer.)
    mn.actor = 0;
    mn.targets.clear();
    mn.sub.clear();
    if (mn.units) {
        const uint64_t sel = g_.Get<uint64_t>(mn.units + 0x38);
        for (auto& b : s.battle)
            if (sel && b.unit == sel) mn.actor = b.id;
    }
    // Choosing a target: the path ends in an action (a skill, Attack, Defend, an item), not a command list (cat 1, id < 1000)
    const bool action = !mn.path.empty() && !(mn.path.back().cat == 1 && mn.path.back().id < 1000);
    mn.state = mn.open && action ? 1 : 0;
    if (!mn.state) return;
    // The pointer: each unit's CPP_VM_BtlUnitParam.TargetSelectState (+0x1ba: 1 Main, the pointed unit; 2 Sub, the area's others)
    bool missing = false;
    for (auto& b : s.battle) {
        auto it = unit_vms_.find(b.param);
        if (it == unit_vms_.end() || g_.Get<uint64_t>(it->second + kUnitVmParam) != b.param) { missing = true; continue; }
        const uint8_t t = g_.Get<uint8_t>(it->second + kUnitVmTarget);
        if (t == 1) mn.targets.push_back(b.unit);
        else if (t == 2) mn.sub.push_back(b.unit);
    }
    const uint64_t now = NowMs();
    if (missing && now - last_vm_find_ms_ > 500) last_vm_find_ms_ = now, FindUnitVms(s);
}

// The battle units' view models (CPP_VM_BtlUnitParam, BtlUnitParam +0x1a0): one pass over the object array, at most twice a second
void Reader::FindUnitVms(const GameState& s) {
    const uint64_t objs = g_.Get<uint64_t>(objarr_);
    const int32_t num = g_.Get<int32_t>(objarr_ + 20), nchunks = g_.Get<int32_t>(objarr_ + 28);
    if (!objs || num <= 0 || nchunks <= 0 || nchunks > 64) return;
    std::unordered_map<uint64_t, bool> want;
    for (auto& b : s.battle) want[b.param] = true;
    unit_vms_.clear();
    std::vector<uint8_t> items;
    for (int32_t c = 0; c < nchunks; ++c) {
        const uint64_t chunk = g_.Get<uint64_t>(objs + 8ull * c);
        const int32_t n = std::min<int32_t>(0x10000, num - c * 0x10000);
        if (!chunk || n <= 0) continue;
        items.resize(size_t(n) * 0x18);
        if (!g_.Read(chunk, items.data(), items.size())) continue;
        for (int32_t i = 0; i < n; ++i) {
            uint64_t a;
            std::memcpy(&a, &items[size_t(i) * 0x18], 8);
            if (!a) continue;
            struct { uint64_t vt; int32_t flags, index; uint64_t cls; } h{};
            if (!g_.Read(a, &h, sizeof h) || !h.cls) continue;
            auto it = unit_vm_class_.find(h.cls);
            if (it == unit_vm_class_.end()) {
                const int kind = IsA(h.cls, "CPP_VM_BtlUnitParam") ? 1 : IsA(h.cls, "CPP_VM_BtlTimeLine") ? 2 : 0;
                it = unit_vm_class_.emplace(h.cls, kind == 1).first;
                if (kind == 2) timeline_class_[h.cls] = true;
            }
            if (timeline_class_.count(h.cls)) {
                if (Name(g_.Get<uint32_t>(a + 0x18)).rfind("Default__", 0) != 0) timeline_vm_ = {a, h.cls, uint32_t(h.index)};
                continue;
            }
            if (!it->second) continue;
            const uint64_t p = g_.Get<uint64_t>(a + kUnitVmParam);
            if (want.count(p)) unit_vms_[p] = a;
        }
    }
}

// The turn order: CPP_VM_BtlTimeLine.TurnVMMap (+0xd8, Map<EUIBtlTurnTimeLine, CPP_VM_BtlTurn*>, element 0x18) holds a view model per
// line (0 this turn, 1 the next, 2 an additional one), each with its EntryDataList (+0xa8, BtlTimelineEntryData 0x14: UnitUniqueId +0,
// DuplicateCount +4, BelongTimeline +8, BelongPhaseType +9) and bDisplay (+0xc9). The units' unique ids come from their view models
// (CPP_VM_BtlUnitParam.UniqueID +0x198).
constexpr uint64_t kTlTurnMap = 0xd8, kTurnEntries = 0xa8;
void Reader::ReadTimeline(GameState& s) {
    bool missing = false;
    for (auto& b : s.battle) {
        auto it = unit_vms_.find(b.param);
        if (it == unit_vms_.end() || g_.Get<uint64_t>(it->second + kUnitVmParam) != b.param) { missing = true; continue; }
        b.uid = g_.Get<int32_t>(it->second + 0x198);  // UniqueID
    }
    const uint64_t now = NowMs();
    if ((missing || !Alive(timeline_vm_)) && now - last_vm_find_ms_ > 1000) last_vm_find_ms_ = now, FindUnitVms(s);
    if (!Alive(timeline_vm_)) return;
    struct Line { int line; uint64_t vm; };
    std::vector<Line> lines;
    ForEachMap(g_, timeline_vm_.addr + kTlTurnMap, 0x18, [&](uint64_t e) {
        const int k = g_.Get<uint8_t>(e);
        const uint64_t vm = g_.Get<uint64_t>(e + 8);
        if (vm && k <= 2) lines.push_back({k, vm});
        return lines.size() < 4;
    });
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.line < b.line; });
    for (auto& l : lines) {
        const TArr arr = g_.Get<TArr>(l.vm + kTurnEntries);
        if (arr.num <= 0 || arr.num > 64) continue;
        std::vector<uint8_t> b(size_t(arr.num) * 0x14);
        if (!g_.Read(arr.data, b.data(), b.size())) continue;
        for (int i = 0; i < arr.num; ++i) {
            TurnEntry t;
            std::memcpy(&t.uid, &b[size_t(i) * 0x14], 4);
            t.line = l.line;
            t.phase = b[size_t(i) * 0x14 + 9];
            for (size_t u = 0; u < s.battle.size(); ++u)
                if (s.battle[u].uid >= 0 && s.battle[u].uid == t.uid) t.unit = int(u);
            s.turns.push_back(t);
        }
    }
    s.turns_valid = !lines.empty();
    // Who acts now. This turn's list (line 0) holds those still to act: a unit leaves it as its turn begins. So the one acting is
    // the first of the order last seen that is no longer listed: the previous read's list, or, as a new turn's list appears, the
    // previous read's next-turn list (it becomes this turn's).
    // An entry is its unit and its phase: a unit may act again in the extra phase (1), and the finish phase (2) is the
    // visions' resonance, with no unit (-1).
    std::vector<std::pair<int, int>> now_ids, next_ids;
    for (auto& t : s.turns) (t.line == 0 ? now_ids : next_ids).push_back({t.uid, t.phase});
    if (s.turns_valid && now_ids != turn_now_) {
        const auto& expect = turn_now_.empty() ? turn_next_ : turn_now_;
        for (auto& e : expect)
            if (std::find(now_ids.begin(), now_ids.end(), e) == now_ids.end()) { acting_ = e; break; }
    }
    if (s.turns_valid) turn_now_ = now_ids, turn_next_ = next_ids;
    s.acting_phase = acting_.second;
    for (size_t u = 0; u < s.battle.size(); ++u)
        if (acting_.first >= 0 && s.battle[u].uid == acting_.first) s.acting = int(u);
}

// The resonance wheel (the game's Finish command). CPP_UI_BtlCmdManager.CmdType +0x30 (eBtlCmdType: Normal 0, Additional 1,
// Finish 2). CPP_VM_FinishCmd.EntryDataList +0x70: FinishCmdEntryData (0x20): bEnable +0, UnitUniqueId +4, VisionId +8, SkillId +0xc,
// DirectionProperty.RelatedNavigation +0x10 (EUINavigation). A unit's unique id: CPP_VM_BtlUnitParam.UniqueID +0x198.
namespace {
constexpr uint64_t kMgrCmdType = 0x30, kFinishEntries = 0x70, kUnitVmUid = 0x198;
}

void Reader::ReadFinish(GameState& s) {
    BattleMenu& mn = s.menu;
    mn.finish.clear();
    if (!mn.open || !mn.path.empty() || !Alive(cmd_mgr_) || g_.Get<uint8_t>(cmd_mgr_.addr + kMgrCmdType) != 2) return;
    mn.state = 2;
    mn.entries.clear();
    const uint64_t now = NowMs();
    if (!Alive(finish_vm_) && now - last_finish_find_ms_ > 500) {
        last_finish_find_ms_ = now;
        finish_vm_ = {};
        const uint64_t objs = g_.Get<uint64_t>(objarr_);
        const int32_t num = g_.Get<int32_t>(objarr_ + 20), nchunks = g_.Get<int32_t>(objarr_ + 28);
        std::vector<uint8_t> items;
        for (int32_t c = 0; objs && num > 0 && c < nchunks && c < 64; ++c) {
            const uint64_t chunk = g_.Get<uint64_t>(objs + 8ull * c);
            const int32_t n = std::min<int32_t>(0x10000, num - c * 0x10000);
            if (!chunk || n <= 0) continue;
            items.resize(size_t(n) * 0x18);
            if (!g_.Read(chunk, items.data(), items.size())) continue;
            for (int32_t i = 0; i < n; ++i) {
                uint64_t a;
                std::memcpy(&a, &items[size_t(i) * 0x18], 8);
                if (!a) continue;
                struct { uint64_t vt; int32_t flags, index; uint64_t cls; uint32_t name, number; } h{};
                if (!g_.Read(a, &h, sizeof h) || !h.cls) continue;
                auto it = finish_class_.find(h.cls);
                if (it == finish_class_.end()) it = finish_class_.emplace(h.cls, IsA(h.cls, "CPP_VM_FinishCmd")).first;
                if (it->second && Name(h.name).rfind("Default__", 0) != 0 && (!finish_vm_.addr || uint32_t(h.index) >= finish_vm_.index))
                    finish_vm_ = {a, h.cls, uint32_t(h.index)};
            }
        }
    }
    if (!Alive(finish_vm_)) return;
    TArr e = g_.Get<TArr>(finish_vm_.addr + kFinishEntries);
    if (e.num <= 0 || e.num > 8) return;
    std::vector<uint8_t> b(size_t(e.num) * 0x20);
    if (!g_.Read(e.data, b.data(), b.size())) return;
    std::unordered_map<int, uint64_t> by_uid;  // unique id -> CPP_BtlUnit
    bool missing = false;
    for (auto& u : s.battle) {
        auto it = unit_vms_.find(u.param);
        if (it == unit_vms_.end() || g_.Get<uint64_t>(it->second + kUnitVmParam) != u.param) { missing = true; continue; }
        by_uid[g_.Get<int32_t>(it->second + kUnitVmUid)] = u.unit;
    }
    if (missing && now - last_vm_find_ms_ > 500) last_vm_find_ms_ = now, FindUnitVms(s);
    for (int i = 0; i < e.num; ++i) {
        const uint8_t* d = &b[size_t(i) * 0x20];
        BattleMenu::Finish f;
        f.enable = d[0] & 1;
        std::memcpy(&f.uid, d + 4, 4);
        std::memcpy(&f.vision, d + 8, 4);
        std::memcpy(&f.skill, d + 0xc, 4);
        f.dir = d[0x10] <= 3 ? d[0x10] : -1;
        if (auto it = by_uid.find(f.uid); it != by_uid.end()) f.unit = it->second;
        if (f.skill > 0) mn.finish.push_back(f);
    }
}

// The open list holds the game's focused entry at the cursor (a list of an earlier turn or menu level does not)
bool Reader::ListMatches(const BattleMenu& mn) {
    if (!mn.list || mn.cursor < 0) return false;
    TArr keys = g_.Get<TArr>(mn.list + kListKeys);
    if (keys.num <= 0 || keys.num > 64 || mn.cursor >= keys.num || g_.Get<uint64_t>(mn.list + 0x28) != mn.vm) return false;
    uint8_t e[8];
    if (!g_.Read(keys.data + 8ull * uint32_t(mn.cursor), e, 8)) return false;
    int32_t id;
    std::memcpy(&id, e + 4, 4);
    return e[0] == mn.focus.cat && id == mn.focus.id;
}

void Reader::FindList(BattleMenu& mn) {
    // The lists are created as the player opens them: look over the object array for this menu's (CPP_BtlCmdListBase,
    // BtlCmdVM +0x28 == the menu) and take the one that holds the focused entry at the cursor; the newest wins a tie.
    const uint64_t objs = g_.Get<uint64_t>(objarr_);
    const int32_t num = g_.Get<int32_t>(objarr_ + 20), nchunks = g_.Get<int32_t>(objarr_ + 28);
    if (!objs || num <= 0 || nchunks <= 0 || nchunks > 64) return;
    std::vector<Obj> lists;
    std::vector<uint8_t> items;
    for (int32_t c = 0; c < nchunks; ++c) {
        const uint64_t chunk = g_.Get<uint64_t>(objs + 8ull * c);
        const int32_t n = std::min<int32_t>(0x10000, num - c * 0x10000);
        if (!chunk || n <= 0) continue;
        items.resize(size_t(n) * 0x18);
        if (!g_.Read(chunk, items.data(), items.size())) continue;
        for (int32_t i = 0; i < n; ++i) {
            uint64_t a;
            std::memcpy(&a, &items[size_t(i) * 0x18], 8);
            if (!a) continue;
            struct { uint64_t vt; int32_t flags, index; uint64_t cls; } h{};
            if (!g_.Read(a, &h, sizeof h) || !h.cls) continue;
            auto it = list_class_.find(h.cls);
            if (it == list_class_.end()) it = list_class_.emplace(h.cls, IsA(h.cls, "CPP_BtlCmdListBase")).first;
            if (it->second && g_.Get<uint64_t>(a + 0x28) == mn.vm) lists.push_back({a, h.cls, uint32_t(h.index)});
        }
    }
    cmd_lists_ = std::move(lists);
    uint64_t best = 0;
    uint32_t best_index = 0;
    for (const Obj& o : cmd_lists_) {
        BattleMenu t = mn;
        t.list = o.addr;
        if (ListMatches(t) && (!best || o.index >= best_index)) best = o.addr, best_index = o.index;
    }
    mn.list = best;
}

void Reader::ReadMenu(GameState& s) {
    BattleMenu& mn = s.menu;
    if (!Alive(cmd_vm_)) return;
    mn.vm = cmd_vm_.addr;
    mn.units = g_.Get<uint64_t>(battle_.addr + kBtlUnitMgr);
    mn.list = menu_list_;
    ReadMenuHead(s);
    ReadFinish(s);
    if (mn.state == 2) {  // the resonance wheel, not a list (and no list search meanwhile)
        mn.list = 0;
        mn.valid = !mn.finish.empty();
        return;
    }
    if (!ListMatches(mn)) {
        mn.list = 0;
        for (const Obj& o : cmd_lists_) {  // the lists known already
            BattleMenu t = mn;
            t.list = o.addr;
            if (Alive(o) && ListMatches(t)) { mn.list = o.addr; break; }
        }
        const uint64_t now = NowMs();
        if (!mn.list && mn.open && now - last_find_ms_ > 100) last_find_ms_ = now, FindList(mn);
    }
    menu_list_ = mn.list;
    mn.entries.clear();
    if (mn.list) {
        TArr keys = g_.Get<TArr>(mn.list + kListKeys);
        std::vector<uint8_t> b(size_t(std::max(0, std::min(keys.num, 64))) * 8);
        if (!b.empty() && g_.Read(keys.data, b.data(), b.size()))
            for (size_t i = 0; i < b.size() / 8; ++i) {
                ActionRef r;
                r.cat = b[i * 8];
                std::memcpy(&r.id, &b[i * 8 + 4], 4);
                mn.entries.push_back(r);
            }
    }
    mn.valid = mn.open && ((mn.state == 1) || (!mn.entries.empty() && mn.cursor >= 0 && mn.cursor < int(mn.entries.size())));
}

bool Reader::RefreshMenu(const EdenDsmodHostApi* host, GameState& s) {
    g_.Set(host);
    if (!s.menu.vm) return true;
    ReadMenu(s);
    return true;
}

std::string Reader::Debug() {
    char b[160];
    std::snprintf(b, sizeof b, "[vm %llx alive %d, mgr %llx, lists %zu, scans %u]", (unsigned long long)cmd_vm_.addr, Alive(cmd_vm_),
                  (unsigned long long)cmd_mgr_.addr, cmd_lists_.size(), scans_);
    return b;
}

void Reader::LoadEquipment() {
    if (equipment_loaded_ || !assets_.addr) return;
    // CPP_AppAssetManager.m_DataTableMap: Map<eDataTableType(u8), DataTable*>: value +8, element 0x18
    uint64_t dt = 0;
    ForEachMap(g_, assets_.addr + kAssetDataTables, 0x18, [&](uint64_t e) {
        if (g_.Get<uint8_t>(e) != kDtItemEquipment) return true;
        dt = g_.Get<uint64_t>(e + 8);
        return false;
    });
    if (!dt) return;
    // UDataTable.RowMap: Map<FName, uint8*>: value +8, element 0x18. Rows are EQUIPMENT_ITEM_DATA.
    ForEachMap(g_, dt + kDataTableRows, 0x18, [&](uint64_t e) {
        const uint64_t row = g_.Get<uint64_t>(e + 8);
        if (!row) return true;
        EquipInfo info;
        const int id = g_.Get<int32_t>(row + 8);
        info.type = g_.Get<uint8_t>(row + 0x7d);
        info.detail = g_.Get<uint8_t>(row + 0x7e);
        // params: Map<eUnitParameterType(u8), int>: value +4, element 0x10
        ForEachMap(g_, row + 0x80, 0x10, [&](uint64_t p) {
            const uint8_t k = g_.Get<uint8_t>(p);
            if (k < 21) info.params[k] = g_.Get<int32_t>(p + 4);
            return true;
        });
        TArr pl = g_.Get<TArr>(row + 0xd0);  // passiveList
        if (pl.num > 0 && pl.num <= 16) {
            info.passives.resize(size_t(pl.num));
            g_.Read(pl.data, info.passives.data(), info.passives.size() * 4);
        }
        if (id > 0) equipment_[id] = info;
        return true;
    });
    equipment_loaded_ = equipment_.size() > 10;
}

// The consumables (eDataTableType ItemConsumablesData 39: CONSUMABLES_ITEM_DATA) and their skills (SkillData 25: SKILL_DATA), as
// the game's item menus use them: checked live, Potion (1) is usableScene Any, skill 100010 HitPoint Single; Phoenix Down (4)
// targets the dead (defaultTargetState Dead); the party potions are TargetType Group; bombs are BattleOnly.
void Reader::LoadConsumables() {
    if (consumables_loaded_ || !assets_.addr) return;
    uint64_t items = 0, skills = 0;
    ForEachMap(g_, assets_.addr + kAssetDataTables, 0x18, [&](uint64_t e) {
        const uint8_t type = g_.Get<uint8_t>(e);
        if (type == kDtItemConsumables) items = g_.Get<uint64_t>(e + 8);
        if (type == kDtSkillData) skills = g_.Get<uint64_t>(e + 8);
        return true;
    });
    if (!items || !skills) return;
    std::unordered_map<int, uint64_t> skill_rows;
    ForEachMap(g_, skills + kDataTableRows, 0x18, [&](uint64_t e) {
        if (const uint64_t row = g_.Get<uint64_t>(e + 8)) skill_rows[g_.Get<int32_t>(row + 8)] = row;
        return true;
    });
    ForEachMap(g_, items + kDataTableRows, 0x18, [&](uint64_t e) {
        const uint64_t row = g_.Get<uint64_t>(e + 8);
        if (!row) return true;
        ConsumableInfo c;
        const int id = g_.Get<int32_t>(row + 8);
        const uint8_t scene = g_.Get<uint8_t>(row + 0x7d);  // eItemUsableScene: BattleOnly 1, MapOnly 2, Any 3
        c.field = scene == 2 || scene == 3;
        c.skill = g_.Get<int32_t>(row + 0x80);
        if (auto it = skill_rows.find(c.skill); it != skill_rows.end()) {
            const uint64_t sk = it->second;
            const uint8_t ptype = g_.Get<uint8_t>(sk + 0x7d), target = g_.Get<uint8_t>(sk + 0x7f), state = g_.Get<uint8_t>(sk + 0x83);
            c.kind = state == 1 ? ConsumableInfo::kRevive : ptype == 1 ? ConsumableInfo::kHp : ptype == 2 ? ConsumableInfo::kMp : ConsumableInfo::kOther;
            c.party = target == 2;  // eSkillTargetType Group
        }
        if (id > 0) consumables_[id] = c;
        return true;
    });
    consumables_loaded_ = consumables_.size() > 10;
}

void Reader::LoadSkillTables() {
    if (skills_loaded_ || !assets_.addr) return;
    auto table = [&](uint8_t type) {
        uint64_t dt = 0;
        ForEachMap(g_, assets_.addr + kAssetDataTables, 0x18, [&](uint64_t e) {
            if (g_.Get<uint8_t>(e) != type) return true;
            dt = g_.Get<uint64_t>(e + 8);
            return false;
        });
        return dt;
    };
    const uint64_t effects = table(kDtSkillEffect), passives = table(kDtPassiveSkill), awaken = table(kDtVisionAwakening),
                   synchro = table(kDtVisionSynchro);
    if (!effects || !passives || !awaken || !synchro) return;
    // SKILL_EFFECT_DATA: ID +8, EffectType +0xc, ParamList +0x38. A lasting parameter change (ParameterContinuousVariation, 3)
    // holds {value, eUnitParameterType, 2 = percent}: Tronn's "+10% MP" turns a base max MP of 68 into the 74 the game shows.
    std::unordered_map<int, std::pair<int, int>> pct_effects;
    ForEachMap(g_, effects + kDataTableRows, 0x18, [&](uint64_t e) {
        const uint64_t row = g_.Get<uint64_t>(e + 8);
        if (!row || g_.Get<uint8_t>(row + 0xc) != 3) return true;
        TArr pl = g_.Get<TArr>(row + 0x38);
        int32_t p[3];
        if (pl.num >= 3 && g_.Read(pl.data, p, sizeof p) && p[2] == 2 && p[1] > 0 && p[1] < 21 && p[0] > -100 && p[0] <= 1000)
            pct_effects[g_.Get<int32_t>(row + 8)] = {p[1], p[0]};
        return true;
    });
    // PASSIVE_SKILL_DATA: ID +8, effectBundleList +0x50 (SKILL_EFFECT_BUNDLE 0x18: effectId +0), bBattleOnly +0x60
    std::unordered_map<int, std::vector<std::pair<int, int>>> pct;
    ForEachMap(g_, passives + kDataTableRows, 0x18, [&](uint64_t e) {
        const uint64_t row = g_.Get<uint64_t>(e + 8);
        if (!row || (g_.Get<uint8_t>(row + 0x60) & 1)) return true;
        TArr bl = g_.Get<TArr>(row + 0x50);
        for (int k = 0; k < bl.num && k < 8; ++k) {
            auto it = pct_effects.find(g_.Get<int32_t>(bl.data + 0x18ull * size_t(k)));
            if (it != pct_effects.end()) pct[g_.Get<int32_t>(row + 8)].push_back(it->second);
        }
        return true;
    });
    // VISION_MASTERY_DATA: vision ID +8, masteryPoint +0x10 (points needed), detailData +0x18 (0x18 each: parameterType +0,
    // params +8). Types: 2 a passive skill {id}, 5 a base parameter {eUnitParameterType, value}.
    auto mastery = [&](uint64_t dt, std::unordered_map<int, std::vector<MasteryRow>>& out) {
        ForEachMap(g_, dt + kDataTableRows, 0x18, [&](uint64_t e) {
            const uint64_t row = g_.Get<uint64_t>(e + 8);
            if (!row) return true;
            MasteryRow m;
            m.pts = g_.Get<int32_t>(row + 0x10);
            TArr dl = g_.Get<TArr>(row + 0x18);
            for (int k = 0; k < dl.num && k < 16; ++k) {
                const uint64_t d = dl.data + 0x18ull * size_t(k);
                TArr pl = g_.Get<TArr>(d + 8);
                int32_t p[2] = {-1, -1};
                if (pl.num >= 1) g_.Read(pl.data, p, 4 * size_t(std::min(pl.num, 2)));
                const uint8_t type = g_.Get<uint8_t>(d);
                if (type == 2 && p[0] > 0) m.passives.push_back(p[0]);
                if (type == 5 && pl.num >= 2 && p[0] > 0 && p[0] < 21) m.flats.push_back({p[0], p[1]});
            }
            out[g_.Get<int32_t>(row + 8)].push_back(std::move(m));
            return true;
        });
    };
    mastery(awaken, awakening_);
    mastery(synchro, synchro_);
    passive_pct_ = std::move(pct);
    skills_loaded_ = !passive_pct_.empty() && !awakening_.empty();
}

// An FText from a string table, as displayed: TextData -> string table history (+0x20) -> shared display string (+0x40),
// whose FString is at +8 (this build; checked on MAP_DATA.Name).
std::string Reader::FTextString(uint64_t ftext, int max_chars) {
    const uint64_t td = g_.Get<uint64_t>(ftext);
    const uint64_t rec = td ? g_.Get<uint64_t>(td + 0x20) : 0;
    const uint64_t disp = rec ? g_.Get<uint64_t>(rec + 0x40) : 0;
    struct { uint64_t data; int32_t num, max; } fs{};
    const bool localized = disp && g_.Read(disp + 8, &fs, sizeof fs) && fs.data && fs.num > 1 && fs.num <= max_chars && fs.max >= fs.num;
    // A generated text (a formatted one: the quests' "Chapter 1 - ...") holds its string itself, an FString at +0x18
    if (!localized && !(td && g_.Read(td + 0x18, &fs, sizeof fs) && fs.data && fs.num > 1 && fs.num <= max_chars && fs.max >= fs.num && fs.max < 65536))
        return "";
    std::vector<uint16_t> w(size_t(fs.num));
    if (!g_.Read(fs.data, w.data(), w.size() * 2)) return "";
    std::string out;
    for (size_t i = 0; i + 1 < w.size(); ++i) {
        const uint32_t c = w[i];
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

// An FString (data, num, max) as UTF-8
std::string Reader::FStringAt(uint64_t fstring, int max_chars) {
    struct { uint64_t data; int32_t num, max; } fs{};
    if (!g_.Read(fstring, &fs, sizeof fs) || !fs.data || fs.num <= 1 || fs.num > max_chars || fs.max < fs.num) return "";
    std::vector<uint16_t> w(size_t(fs.num));
    if (!g_.Read(fs.data, w.data(), w.size() * 2)) return "";
    std::string out;
    for (size_t i = 0; i + 1 < w.size(); ++i) {
        const uint32_t c = w[i];
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

// A string table text the game has not looked up yet (no display string; the quest journal does that when it opens): its
// reference (TextData +0x20) names the table (FName +0x20) and the key (FTextKey index +0x28). The loaded UStringTable of that
// name holds its FStringTable at +0x28, whose KeysToEntries set (+0x20: elements of 0x20, the key's index at +0, the entry at
// +8) gives the entry, and the entry its source text (FString +0x10, the game's Japanese). Checked live on side quest 14:
// ST_EventQuestTitle, key 0x10b09, 「きれいごとを胸に抱いて」, which the game's ja locres has as EventQuestTitle_032.
bool Reader::StringTableSource(uint64_t ftext, std::string& ns, std::string& src) {
    const uint64_t td = g_.Get<uint64_t>(ftext);
    const uint64_t rec = td ? g_.Get<uint64_t>(td + 0x20) : 0;
    if (!rec) return false;
    const uint32_t table = g_.Get<uint32_t>(rec + 0x20), key = g_.Get<uint32_t>(rec + 0x28);
    const std::string path = Name(table);  // /Game/.../ST_EventQuestTitle.ST_EventQuestTitle
    const auto it = string_tables_.find(path.substr(path.rfind('.') + 1));
    if (it == string_tables_.end() || !Alive(it->second)) return false;
    const uint64_t st = g_.Get<uint64_t>(it->second.addr + 0x28);
    const TArr els = st ? g_.Get<TArr>(st + 0x20) : TArr{};
    if (!els.data || els.num <= 0 || els.num > 65536) return false;
    std::vector<uint8_t> b(size_t(els.num) * 0x20);
    if (!g_.Read(els.data, b.data(), b.size())) return false;
    for (size_t i = 0; i < size_t(els.num); ++i) {
        uint32_t k;
        uint64_t entry;
        std::memcpy(&k, &b[i * 0x20], 4);
        std::memcpy(&entry, &b[i * 0x20 + 8], 8);
        if (k != key || !entry) continue;
        src = FStringAt(entry + 0x10, 512);
        ns = it->first;
        return !src.empty() && !ns.empty();
    }
    return false;
}

void Reader::LoadMapRows() {
    if (rows_loaded_) return;
    size_t n = 0;
    for (const Obj& t : map_tables_) {
        if (!Alive(t)) continue;
        ForEachMap(g_, t.addr + kDataTableRows, 0x18, [&](uint64_t e) {
            const uint64_t row = g_.Get<uint64_t>(e + 8);
            if (!row) return true;
            MapRow& r = map_rows_[g_.Get<int32_t>(row + 8)];
            r.found = true, r.row = row;
            r.group = g_.Get<int32_t>(row + 0x10);  // GroupId (a dungeon's floors share one)
            r.same = g_.Get<int32_t>(row + 0x18);   // sameMapId
            r.type = g_.Get<uint8_t>(row + 0x101);  // MapType
            ++n;
            return true;
        });
    }
    rows_loaded_ = n > 100;  // (the tables are all there: a few hundred maps)
}

const Reader::MapRow& Reader::MapInfo(int id) {
    LoadMapRows();
    MapRow& r = map_rows_[id];
    if (r.found && r.name.empty() && NowMs() - r.name_ms > 3000) {  // the name: looked up again until the game has it
        r.name_ms = NowMs();
        r.name = FTextString(r.row + 0x20);  // MAP_DATA.Name
        // (a place the player is not in: the game has not looked its name up, so its string table entry, for the locres)
        if (r.name.empty() && r.name_src.empty()) StringTableSource(r.row + 0x20, r.name_ns, r.name_src);
    }
    return r;
}

std::vector<int> Reader::Mates(int id) {
    std::vector<int> out{id};
    const MapRow& m = MapInfo(id);
    for (auto& [j, r] : map_rows_) {
        if (j == id || !r.found) continue;
        if ((m.group > 0 && (r.group == m.group || j == m.group)) || (m.same > 0 && (r.same == m.same || j == m.same))) out.push_back(j);
    }
    return out;
}

std::string Reader::MapName(int id) { return MapInfo(id).name; }

void Reader::ReadUnitParams(GameState& s) {
    std::unordered_map<int, uint64_t> by_id;
    for (const Obj& o : unit_params_)
        if (Alive(o)) by_id[g_.Get<int32_t>(o.addr + kUpId)] = o.addr;
    for (UnitState& u : s.units) {
        auto it = by_id.find(u.id);
        if (it == by_id.end()) continue;
        const uint64_t a = it->second;
        int32_t lv = g_.Get<int32_t>(a + kUpLevel), base[3] = {}, st[6] = {};
        g_.Read(a + kUpMaxHp, base, sizeof base);
        g_.Read(a + kUpStats, st, sizeof st);
        if (lv <= 0 || lv > 999 || base[0] <= 0) continue;
        int bonus[21] = {};
        auto add = [&](int id) {
            auto e = s.equipment.find(id);
            if (e != s.equipment.end())
                for (int k = 0; k < 21; ++k) bonus[k] += e->second.params[k];
        };
        add(u.weapon), add(u.armor), add(u.vision);
        for (int x : u.accessories) add(x);
        // Passive skills from the gear, the equipped skills and the vision's mastery raise a parameter by a percentage; the
        // vision's synchro mastery also adds flat parameters.
        int pct[21] = {};
        auto passive = [&](int id) {
            auto p = passive_pct_.find(id);
            if (p != passive_pct_.end())
                for (auto [type, v] : p->second) pct[type] += v;
        };
        for (int id : {u.weapon, u.armor}) if (auto e = s.equipment.find(id); e != s.equipment.end()) for (int p : e->second.passives) passive(p);
        for (int x : u.accessories) if (auto e = s.equipment.find(x); e != s.equipment.end()) for (int p : e->second.passives) passive(p);
        for (int sk : u.skills) if (sk > 0) passive(sk);
        if (auto vp = vision_points_.find(u.vision); vp != vision_points_.end()) {
            if (auto rows = awakening_.find(u.vision); rows != awakening_.end())
                for (const MasteryRow& m : rows->second)
                    if (m.pts <= vp->second.awakening) for (int p : m.passives) passive(p);
            const auto sp = vp->second.synchro.find(u.id);
            const int points = sp == vp->second.synchro.end() ? 0 : sp->second;
            if (auto rows = synchro_.find(u.vision); rows != synchro_.end())
                for (const MasteryRow& m : rows->second)
                    if (m.pts <= points) {
                        for (int p : m.passives) passive(p);
                        for (auto [type, v] : m.flats) bonus[type] += v;
                    }
        }
        auto scale = [&](int v, int type) { return int(int64_t(v) * (100 + pct[type]) / 100); };
        u.level = lv;
        u.max_hp = scale(base[0] + bonus[1], 1);
        u.max_mp = scale(base[1] + bonus[2], 2);
        // Attack, Defense, Magic, Spirit, Divine, Agility: base + equipment (PhysicalAttack 12, PhysicalDefense 13,
        // MagicalAttack 14, MagicalDefense 15, Divine 16, Agility 9)
        u.stats[0] = scale(st[0] + bonus[5] + bonus[12], 5);
        u.stats[1] = scale(st[1] + bonus[6] + bonus[13], 6);
        u.stats[2] = scale(st[2] + bonus[7] + bonus[14], 7);
        u.stats[3] = scale(st[3] + bonus[8] + bonus[15], 8);
        u.stats[4] = st[4] + bonus[16];
        u.stats[5] = scale(st[5] + bonus[9], 9);
    }
}

void Reader::Update(const EdenDsmodHostApi* host, GameState& out) {
    g_.Set(host);
    out = GameState{};
    const uint64_t now = NowMs();
    const bool stale = !Alive(save_mgr_) || !Alive(game_mode_) || (minimap_.addr && !Alive(minimap_)) || (battle_.addr && !Alive(battle_)) ||
                       (controller_.addr && !Alive(controller_));
    // The periodic rescan walks the whole object array (tens of thousands of reads): in a battle, where HP
    // must keep up, only when an object went away, or every 10 s.
    const bool battle_before = was_battle_;
    was_battle_ = false;
    // In a battle the command menu's objects appear after it starts: look again every 2 s until they are found
    const uint64_t period = battle_before ? (cmd_vm_.addr && Alive(cmd_vm_) ? 10000u : 2000u) : 3000u;
    if (stale || now - last_scan_ms_ > period) {
        Scan();
        last_scan_ms_ = now;
    }
    if (!save_mgr_.addr) { out.status = "waiting for the game"; return; }
    out.in_game = Alive(game_mode_);
    if (!out.in_game) return;  // title / boot: the save object holds default data until a game is loaded
    ReadSave(out);
    LoadEquipment();
    LoadSkillTables();
    LoadConsumables();
    out.equipment = equipment_;
    out.consumables = consumables_;
    ReadQuestList(out);
    if (out.save_ok) ReadUnitParams(out);
    if (minimap_.addr) ReadMap(out);
    if (battle_.addr) ReadBattle(out);
    if (out.in_battle) ReadMenu(out);
    if (out.in_battle) ReadTimeline(out);
    else turn_now_.clear(), turn_next_.clear(), acting_ = {-1, -1};
    was_battle_ = out.in_battle;
}

} // namespace ffr
