// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The Binding of Isaac: Repentance game-memory reader (see isaac_reader.h). Every offset below is
// an nro+X / G+X / P+X location from research/isaac/code/REPORT.md (+ verify corrections C1-C7)
// or from the routine named next to it; the routines whose rules are reproduced are
// fingerprinted in isaac_pins.inc.

#include "isaac_reader.h"
#include "isaac_map.h"
#include "isaac_mapreg.h"

#include "core/mods/modules/dsmod_module_sdk.h"
#include "isaac_text.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace isaac_reader {

using namespace dsmod_sdk::int_types;

namespace {
constexpr char BuildHex[] = "B6E5BDB9DC12E1D1A25CBFDA17F4BE24B4754EF5000000000000000000000000";
} // namespace

bool SupportsBuildHex(const char* hex) {
    return hex && std::strlen(hex) == 64 && std::memcmp(hex, BuildHex, 64) == 0;
}

bool SupportsBuildId(const std::uint8_t* id) {
    if (!id)
        return false;
    for (int i = 0; i < 32; ++i) {
        const auto hi = BuildHex[i * 2], lo = BuildHex[i * 2 + 1];
        const auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
        if (id[i] != static_cast<std::uint8_t>(nib(hi) * 16 + nib(lo)))
            return false;
    }
    return true;
}

// ---- pure rules -----------------------------------------------------------------------------
namespace detail {

std::string Seed2String(u32 seed, const char* a) {
    // Seeds::Seed2String (nro+0x4A102C)
    u32 c = 0;
    for (u32 x = seed; x; x >>= 5) {
        const u32 t = c + x;
        c = (t << 1) | ((t >> 7) & 1);
    }
    const u32 v = seed ^ 0x0FEF7FFDu;
    const u32 w = (c & 0xE0) | ((v & 0xFFFFFF) << 8);
    std::string s(9, ' ');
    s[0] = a[v >> 27];
    s[1] = a[(v >> 22) & 31];
    s[2] = a[(v >> 17) & 31];
    s[3] = a[(v >> 12) & 31];
    s[5] = a[(v >> 7) & 31];
    s[6] = a[(v >> 2) & 31];
    s[7] = a[(w >> 5) & 31];
    s[8] = a[c & 31];
    return s;
}

std::string TimeText(s32 t, s32 challenge) {
    if (challenge == 22) // Minimap::Render: challenge 22 counts down from 28800 frames
        t = std::max<s32>(0, 28800 - t);
    if (t < 0)
        t = 0;
    char b[32];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d", t / 108000, (t % 108000) / 1800, (t % 1800) / 30);
    return b;
}

std::string StatText(float v) {
    char b[12]; // StatHUD::Render: snprintf(buf, 0xC, ...) @0x3AF608
    std::snprintf(b, sizeof(b), "%.2f", static_cast<double>(v));
    return b;
}

int BagPreview(const unsigned char (&block)[12], bool same_twice, unsigned curses, bool seedfx_empty,
               bool result_valid) {
    int n = 0;
    for (int i = 0; i < 8; ++i)
        n += block[i] ? 1 : 0;
    s32 result;
    std::memcpy(&result, block + 8, 4);
    if (!same_twice || n != 8 || result <= 0 || !seedfx_empty || !result_valid)
        return 0;
    return (curses & 64) ? 2 : 1;
}

int ButtonFrame(u32 b) {
    // Manager::GetButtonFrame(unsigned) nro+0x3FB20C: b <= 0x11 via jump table 0x8BF826
    // (default target "mov w0, w1" for R/+/-), b + 0x54550000 <= 7 via 0x8BF81E, else -1
    static constexpr int Low[18] = {7, 6, 5, 4, 2, 1, 0, 3, 10, 12, 8, 11, 13, 9, 14, 15, 25, 26};
    static constexpr int Stick[8] = {19, 18, 17, 16, 23, 22, 21, 20};
    if (b <= 0x11)
        return Low[b];
    const u32 s = b + 0x54550000u; // 0xABAB0000 + n
    return s <= 7 ? Stick[s] : -1;
}

const char* ButtonAnim(int t) {
    switch (t) { // nro+0xA387F0 (4 entries), else "Switch" (0x3FB414)
    case 0:
    case 1: return "Switch_JoyCon";
    case 3: return "Switch_Pro";
    default: return "Switch";
    }
}

int ButtonFrameForType(int f, int t) {
    // 0x3FB460: type 1 and (f - 8) in {0, 8, 9, 10, 11} (mask 0xF01) -> table 0x8BF854
    static constexpr int Remap[12] = {9, 9, 9, 9, 9, 9, 9, 9, 20, 21, 22, 23};
    if (t == 1 && f >= 8 && f - 8 < 12 && ((0xF01u >> (f - 8)) & 1))
        return Remap[f - 8];
    return f;
}

int PocketAction(s32 type, bool has_twin, s32 self_19f0, s32 twin_19f0) {
    if (static_cast<u32>(type - 19) > 1 || !has_twin)
        return 10;
    // csel x9, twin, self, lt (twin < self) ; cmp x9, self ; cinc w1, #9, ne
    return twin_19f0 < self_19f0 ? 10 : 9;
}

bool BagHeld(const std::array<std::int32_t, 4>& active) {
    for (const std::int32_t id : active)
        if (id == BagOfCraftingId)
            return true;
    return false;
}

int StatTally(int stat, float v) {
    float r;
    switch (stat) {
    case 0: r = v * 4.5f + -2.0f; break;                                   // GetSpeedRating
    case 1: r = std::pow(30.0f / (v + 1.0f), 0.75f) * std::bit_cast<float>(0x4007B47Cu) + -2.0f;
        break;                                                              // GetTearDelayRating
    case 2: r = std::pow(v, std::bit_cast<float>(0x3F0F5C29u)) * std::bit_cast<float>(0x400ECBA3u) +
                -2.0f;
        break;                                                              // GetDamageRating
    case 3: r = (v + -230.0f) / 60.0f + 2.0f; break;                       // GetRangeRating
    case 4: r = v * 5.0f + -2.5f; break;                                   // GetShotSpeedRating
    case 5: r = v + 2.5f; break;                                           // GetLuckRating
    default: return 0;
    }
    // KAGE::Math::Clamp(lo=0, hi=7, r) @0x4FCF90: r < lo -> lo; hi < r -> hi (NaN passes through)
    if (r < 0.0f)
        r = 0.0f;
    if (7.0f < r)
        r = 7.0f;
    if (std::isnan(r))
        return 0; // fcvtzs(NaN) = 0
    return static_cast<int>(r);
}

std::string FloorSuffix(int stage, u32 curses, int difficulty, int challenge) {
    // Level::GetName(stage, type, curses, 0, jumble, mode): mode = RoomConfig::GetCurrentMode
    // (difficulty & ~1) == 2 -> greed (1): no suffix; challenge 0x2C: no suffix.
    if ((difficulty & ~1) == 2 || challenge == 0x2C)
        return {};
    if (curses & 2) // Curse of the Labyrinth
        return " XL";
    if (stage < 0 || stage > 8)
        return {};
    const u32 bit = 1u << stage;
    if (bit & 0xAA)
        return " I";
    if (bit & 0x154)
        return " II";
    return {};
}

std::string MapHex(const std::vector<MapRoom>& rooms, bool lost) {
    static constexpr char H[] = "0123456789abcdef";
    std::string s;
    s.reserve(2 + rooms.size() * 12);
    const auto put = [&](u8 b) {
        s.push_back(H[b >> 4]);
        s.push_back(H[b & 15]);
    };
    put(lost ? 1 : 0);
    if (lost)
        return s;
    for (const MapRoom& r : rooms) {
        put(r.grid);
        put(r.shape);
        put(r.type);
        put(r.dflags);
        put(r.flags);
        put(r.current);
    }
    return s;
}

u8 MapFlags(u32 flags, s32 type, s32 subtype) {
    u8 f = static_cast<u8>(flags & 1);                // CLEAR -> "RoomVisited" (0x41B4C8)
    if (flags & 0x400)                                // RED_ROOM tint (0x41B3C4)
        f |= 2;
    if (flags & 0x800)                                // treasure icon: ldrb [D+0x51] tst #8 (0x41C32C)
        f |= 4;
    if (type == 11 && subtype == 1)                   // challenge: [Data+0x10] == 1 (0x41C394)
        f |= 8;
    return f;
}

s32 MaxPocketItems(bool slot_item, const s32 kind[4], const s32 id[4]) {
    // Entity_Player::GetMaxPocketItems (nro+0x2B0970)
    s32 n = slot_item ? 2 : 1;
    for (int s = 0; s < 4; ++s)
        if (kind[s] == 2 && id[s] > 0)
            ++n;
    return n;
}

Insn Decode(u32 w, u64 at) {
    Insn i;
    if ((w & 0xFFE00000u) == 0x52800000u) { // MOVZ Wd, #imm16 (hw 0)
        i.kind = Insn::MovzW;
        i.rd = static_cast<int>(w & 31);
        i.imm = (w >> 5) & 0xFFFF;
    } else if ((w & 0xFC000000u) == 0x14000000u) { // B imm26
        i.kind = Insn::B;
        s64 off = static_cast<s64>(w & 0x03FFFFFF);
        if (off & 0x02000000)
            off -= 0x04000000;
        i.target = static_cast<s64>(at) + off * 4;
    }
    return i;
}

int NearestPedestal(float px, float py, const std::vector<NearCand>& c, s32 frame, float radius,
                    float* dist) {
    int best = -1;
    float best_d = 10000.0f; // EID.lastDist
    for (std::size_t i = 0; i < c.size(); ++i) {
        const NearCand& e = c[i];
        if ((e.flags >> 46) & 1) // QueryRadius: tbnz x9,#0x2e (nro+0x78F28)
            continue;
        const float dx = px - e.x, dy = py - e.y;
        const float r = e.size + radius; // fadd s9,s0,s8 (nro+0x78F3C)
        if (!(dx * dx + dy * dy < r * r)) // fcmp; b.pl skips (nro+0x78F50)
            continue;
        if (std::bit_cast<s32>(static_cast<u32>(frame) - static_cast<u32>(e.spawn)) <= 0) // EID: entity.FrameCount > 0
            continue;
        const float d = std::sqrt(dx * dx + dy * dy); // diff:Length()
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    if (dist)
        *dist = best >= 0 ? best_d : -1.0f;
    return best;
}

} // namespace detail

namespace {

using detail::MapRoom;

// ---- build-pinned locations (Repentance.nro 91C73FDD..., launcher B6E5BDB9...) ---------------
constexpr u64 MainGotRunRepentance = 0x10CC78; // JUMP_SLOT _Z14run_repentancev
constexpr u64 NroRunRepentance = 0x4C0840;
constexpr u32 NroFileSize = 0xAB1000;         // header +0x18
constexpr u64 NroImageSize = 0x1710970;       // text..bss end
constexpr u64 NroMod0 = 0x68D054;
constexpr u8 NroModuleId[20] = {0x91, 0xC7, 0x3F, 0xDD, 0x57, 0x50, 0x61, 0x31, 0x8D, 0x68,
                                0x88, 0x63, 0x16, 0xAF, 0xEA, 0xC7, 0x23, 0x88, 0xB2, 0xAB};
constexpr u64 NroGGame = 0xABB448, NroGotGGame = 0xAAC698;
constexpr u64 NroGManager = 0xABCCE0, NroGotGManager = 0xAAC648;
// round 9: KAGE::Input::g_Manager (object, .bss) + its GOT slot (Manager::GetButtonFrame(int,int,int)
// 0x3FB334), the default binding table (InputConfigOptions::s_ButtonActions / s_NumButtonActions /
// s_MaximumButtonsPerAction), the InputDeviceBase vtable slots the game calls and the player's
// controller id (Entity_Player::control_pocket_item 0x2CAC1C: Manager::IsActionTriggered(action,
// P+0x19EC) -> KAGE ManagerBase: the device whose InputDeviceBase::GetId() ([dev+8]) matches)
constexpr u64 NroInputMgr = 0xB06F60, NroGotInputMgr = 0xAAC768;
constexpr u64 NroDefButtonActions = 0xA385B8, NroDefNumButtonActions = 0xA385C0,
              NroMaxButtonsPerAction = 0xA385B4;
constexpr u64 NroDevGetActionButtons = 0x4F8518, NroDevGetDeviceType = 0x4FC898;
constexpr u64 VtGetActionButtons = 0x108, VtGetDeviceType = 0x138;
constexpr u64 DevId = 0x8, DevNumActions = 0x1C, DevActions = 0x20, DevType = 0x488;
constexpr u64 PController = 0x19EC, PTwinOrder = 0x19F0, PPocketTwin = 0x2580;
constexpr u64 NroGotRngShifts = 0xAAD2C8; // =&RNG::s_Shifts (RNG::SetSeed)
constexpr u64 NroSeedAlphabet = 0x8AE69C;
constexpr u64 NroZodiacTable = 0x8BCDBC; // GetZodiacEffect: Random(12) -> s32[12]
// ItemPool::GetPillEffect (nro+0x3C857C) remap sites
constexpr u64 PillFn = 0x3C857C, PillFnSize = 0x27C;
constexpr u64 PillGoodTable = 0x8BF30F, PillGoodBase = 0x3C8674; // index effect-1, <= 0x2E
constexpr u64 PillBadTable = 0x8BF2DE, PillBadBase = 0x3C86A8;   // index effect, <= 0x30
constexpr u64 PillLoadBlock = 0x3C8714; // w20 = s32 table[w8] (nro+0x8BF444), w21 = 0
constexpr u64 PillEffectTable = 0x8BF444;
constexpr u64 PillCanUse = 0x3C8724;

// Manager
constexpr u64 MgrState = 0x8, MgrItemConfig = 0x36538;
// Game (= Level at +0)
constexpr u64 GStage = 0x0, GRooms = 0x18, GGrid = 0x20D18, GRoomCount = 0x21510,
              GRoom = 0x21550, GCurRoom = 0x21558, GDim = 0x21560, GPillEffect = 0x24CEC,
              GPillIdent = 0x24D28, GMinimap = 0x24EE70, GFrame = 0x24F99C, GTimer = 0x24F9A0,
              GPlayers = 0x25C50, GSeed = 0x25D44, GStageSeeds = 0x25D58, GSeedFx = 0x25DA0,
              GPause = 0x1AE128, GChallenge = 0x26FA88, GChallengeParam = 0x26FB1C,
              GDifficulty = 0x2F0028;
constexpr u64 GBlockA = GMinimap, GBlockAEnd = GTimer + 4, GBlockPEnd = GSeedFx + 8;
constexpr int PillColors = 15; // ItemPool pill tables (colours 1..14 + 0)
// Entity_Player
constexpr u64 PBlock = 0x16B8, PBlockEnd = 0x24E0; // one bulk read (..history header)
constexpr u64 PVariant = 0x3C, PCoopParent = 0x390, PDisable = 0x27E0, PDisable2 = 0x284C,
              PTwinFlag = 0x1C0, PTwin = 0x2588, PInnate = 0x26D0, PInnateSize = 0x26D8,
              PSpecialItem = 0x2730, PTempFx = 0x18D8, PGolden = 0x1DC4, PBone = 0x2450,
              PBroken = 0x2470, PRotten = 0x2474, PHistory = 0x24D0, PBag = 0x2690;
constexpr u64 OMax = 0x16B8, ORed = 0x16BC, OEternal = 0x16C0, OSoul = 0x16C4, OBlack = 0x16C8,
              OKeys = 0x16D4, OGKey = 0x16D8, OGBomb = 0x16D9, OBombs = 0x16DC, OCoins = 0x16E0,
              OType = 0x1738, OFireDelay = 0x1834, OShotSpeed = 0x1838, ODamage = 0x1844,
              ORange = 0x1854, OSpeed = 0x194C, OLuck = 0x1950, OActive = 0x1964,
              OTrinket = 0x1AB0, OCounts = 0x1AB8, OPocket = 0x1C10;
// RoomDescriptor
constexpr u64 DStride = 0x100, DGrid = 0x0, DData = 0x10, DDisplay = 0x48, DVisited = 0x4C,
              DFlags = 0x50;
constexpr int MaxRoomsList = 507 + 18;
// Room entity list (Room+0x1950 EntityList: Entity** +0x78, u32 count +0x84)
constexpr u64 RoomEntityList = 0x1950;
// Entity fields for the nearest-pedestal rule (EntityList::QueryRadius nro+0x78D28,
// CellSpace::insert nro+0x78450, Entity::GetFrameCount nro+0x5A210)
constexpr u64 EFlags = 0x1B8, ESpawnFrame = 0x2F4, EPos = 0x310, ESize = 0x344;
constexpr u64 EBlock = EFlags, EBlockEnd = ESize + 4;
constexpr float EidRadius = 5 * 40.0f; // EID MaxDistance (eid_config.lua:61) * 40 (main.lua:1453)

enum class Domain : int { Core, Player, Items, Seed, Stats, Pill, Map, Room, Name, Near, Hud, Input, Count };
constexpr const char* DomainName[] = {"core", "player", "items", "seed", "stats", "pill",
                                      "map",  "room",   "name",  "near", "hud",   "input"};

struct HashPin {
    Domain domain;
    u64 offset;
    u64 size;
    u64 fnv;
    const char* label;
};
constexpr HashPin Pins[] = {
#include "isaac_pins.inc"
};

enum Page : int { PageMap = 0, PageItems = 1, PageRoom = 2 };

template <class T>
T At(const std::vector<u8>& b, u64 off) {
    T v{};
    if (off + sizeof(T) <= b.size())
        std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

std::string Key(std::string_view kind, int id) {
    return "module:isaac:" + std::string{kind} + "/" + std::to_string(id);
}

/// Buffered outputs (published together; the last good buffer is republished on a torn read).
struct Out {
    std::vector<std::pair<std::string, s64>> ints;
    std::vector<std::pair<std::string, std::string>> texts;
    std::vector<std::pair<std::string, double>> floats;
    void I(std::string k, s64 v) {
        ints.emplace_back(std::move(k), v);
    }
    void F(std::string k, double v) {
        floats.emplace_back(std::move(k), v);
    }
    void T(std::string k, std::string v) {
        texts.emplace_back(std::move(k), std::move(v));
    }
    void Clear() {
        ints.clear();
        texts.clear();
        floats.clear();
    }
    void Publish(const EdenDsmodHostApi& h) const {
        if (h.publish_i64)
            for (const auto& [k, v] : ints)
                h.publish_i64(h.userdata, k.c_str(), v);
        if (h.publish_text)
            for (const auto& [k, v] : texts)
                h.publish_text(h.userdata, k.c_str(), v.c_str());
        if (h.publish_f64)
            for (const auto& [k, v] : floats)
                h.publish_f64(h.userdata, k.c_str(), v);
    }
};

struct HistItem {
    s32 frame;
    u8 trinket;
    s32 id;
};
struct Pedestal {
    u64 entity;
    s32 id;
    s32 price;
    s32 opt;
    bool blind;
    detail::NearCand near;
};

} // namespace

// ---- Impl -----------------------------------------------------------------------------------
struct Reader::Impl {
    EdenDsmodHostApi host{};
    const isaac_text::GameText* text{};

    // resolve state
    u64 main_base{}, main_size{};
    u64 nro{};
    bool resolved{};
    int resolve_wait{};
    s64 dlc_available{-1}; // host asset-source availability, cached for this boot
    std::array<bool, static_cast<int>(Domain::Count)> dom_ok{};
    std::string bad_domains;
    std::string why_resolve{"not resolved"};
    char alphabet[33]{};
    std::vector<u8> pill_code;               // GetPillEffect bytes (hash-verified)
    std::array<s32, 15> pill_table{};        // nro+0x8BF444
    std::array<s32, 12> zodiac_table{};      // nro+0x8BCDBC
    std::array<u32, 3> rng_shift12{};        // RNG::s_Shifts[12]
    // decoded remaps: effect -> (new effect, w21); -1 = unchanged
    std::array<std::pair<s32, s32>, 0x31> pill_good{}, pill_bad{};
    bool pill_decoded{};

    /// Last result of a pure text function (key + a validity bit that includes text readiness).
    struct Memo {
        u64 key{~u64{0}};
        int on{-1};
        std::string val;
        template <class F>
        const std::string& Get(u64 k, bool enabled, F&& f) {
            const int e = enabled ? 1 : 0;
            if (k != key || e != on) {
                val = f();
                key = k;
                on = e;
            }
            return val;
        }
    };
    Memo memo_floor, memo_time, memo_seed;
    std::array<Memo, 6> memo_stat;
    std::array<Memo, 4> memo_act;
    std::array<Memo, 2> memo_tr;

    // ItemConfig collectible table (Item* per id), cached per Manager
    mutable std::vector<u64> item_cfg;
    mutable u64 item_cfg_m{};

    // per-floor cache: RoomConfig::Room* -> (type, subtype, shape)
    struct RoomData {
        s32 type, subtype, shape;
    };
    std::unordered_map<u64, RoomData> room_data_cache;
    u64 cache_game{};
    u32 cache_seed{};
    s32 cache_stage{}, cache_stage_type{};
    void ResetRun() {
        room_data_cache.clear();
        cache_game = 0;
        last_good.Clear();
        last_good.I("run.ready", 0);
        last_good.T("run.why", "waiting for a consistent run snapshot");
        isrc = ISrc::None;
        sel_ped = false;
        sel_entity = near_entity = tap_near = 0;
        room_key = ~u64{0};
        last_hist.clear();
        last_peds.clear();
        last_act.fill(0);
        last_tr.fill(0);
        last_pk.fill({-1, 0});
        pk_max = -1;
        item_cfg_m = 0;
    }
    void ObserveFloor(u32 seed, s32 stage, s32 type) {
        if (cache_game && (cache_game != G || cache_seed != seed))
            ResetRun();
        if (cache_game != G || cache_seed != seed || cache_stage != stage || cache_stage_type != type)
            room_data_cache.clear();
        cache_game = G;
        cache_seed = seed;
        cache_stage = stage;
        cache_stage_type = type;
    }

    // UI state
    int ui_page{PageMap};
    // round 6: the loading page's animation clock (samples while not in a run)
    s64 load_n{};
    bool in_run{};
    // ROOM selection: an explicit pedestal tap, else the nearest pedestal (EID rule), else the
    // first one. A tap holds until the nearest pedestal changes to a different one or the room
    // changes.
    bool sel_ped{};
    u64 sel_entity{};
    u64 near_entity{}; // last pedestal seen as the nearest one (kept through out-of-range gaps)
    u64 tap_near{};    // near_entity when the pedestal was tapped
    u64 room_key{~u64{0}};
    std::vector<Pedestal> last_peds;
    // ITEMS selection (toggled by taps; never changes the page)
    enum class ISrc { None = 0, Inv = 1, Act = 2, Tr = 3, Pk = 4, Item = 5 } isrc{ISrc::None};
    s32 isel_slot{}, isel_frame{}, isel_raw{}, isel_kind{};
    std::vector<HistItem> last_hist;
    std::array<s32, 4> last_act{};
    std::array<s32, 2> last_tr{};
    std::array<std::pair<s32, s32>, 4> last_pk{}; // (kind, raw id) as the slot holds it

    // Sampling epoch for bounded refresh of the immutable ItemConfig table.
    u64 sample_n{};
    Out out, last_good;
    s64 torn_count{};

    // per-sample reads
    u64 G{}, M{}, P{};

    bool Read(u64 at, void* dst, std::size_t n) const {
        return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, at, dst, n);
    }
    template <class T>
    std::optional<T> Get(u64 at) const {
        T v{};
        if (!Read(at, &v, sizeof(T)))
            return std::nullopt;
        return v;
    }
    bool Block(u64 at, std::size_t n, std::vector<u8>& b) const {
        b.resize(n);
        return n == 0 || Read(at, b.data(), n);
    }
    bool InNro(u64 at, u64 n) const {
        return dsmod_sdk::InMain(nro, NroImageSize, at, n);
    }
    bool Ok(Domain d) const {
        return dom_ok[static_cast<int>(d)];
    }

    bool Resolve();
    bool ResolveAfterbirth();
    void SampleAfterbirth(Out& o, bool& torn);
    bool DecodePill();
    void SampleInner(Out& o, bool& torn);

    // game rule replicas
    std::optional<u64> CollectibleConfig(s32 id) const;
    std::optional<bool> HasCollectible(u64 p, s32 id, int depth = 0) const;
    std::optional<bool> TempHasNull(u64 p, s32 null_id) const;
    std::optional<bool> SeedFxContains(s32 key) const;
    std::optional<s32> ZodiacEffect(u64 p) const;
    std::optional<bool> CanUsePill(u64 p, s32 effect) const;
    std::optional<s32> PillEffect(u64 p, s32 color) const;
    std::optional<s32> ActiveMaxCharge(u64 p, const std::vector<u8>& pb, int slot) const;
    std::optional<bool> AnyOwner(u64 players_begin, u64 players_end, s32 id) const;
    std::optional<s32> MaxPocketItems(u64 p, const std::vector<u8>& pb) const;
    struct ButtonGlyph {
        bool live{};
        s32 button{-1}, type{-1}, frame{-1};
    };
    std::optional<ButtonGlyph> LiveButton(s32 controller, s32 action) const;
    std::optional<s32> DefaultButton(s32 action) const;

    // Pocket slot count, refreshed every 15 samples. The removed minimap-icon page no longer
    // needs its five cross-player ownership walks or per-sample image-key publications.
    static constexpr u64 RefreshSamples = 15;
    u64 slow_n{};
    s32 pk_max{-1};

    std::string Name(isaac_text::Kind k, int id) const {
        if (!text || !text->Ready() || id < 0 || (id == 0 && k != isaac_text::Kind::PillEffect))
            return {};
        const auto info = text->Item(k, id);
        return info.valid ? info.name : std::string{};
    }
};

// The Afterbirth+ profile has its own offsets and executable fingerprints.
#include "isaac_afterbirth.inc"

// ---- resolve --------------------------------------------------------------------------------
bool Reader::Impl::Resolve() {
    resolved = false;
    dom_ok.fill(false);
    main_base = host.main_base;
    main_size = host.main_size;
    const auto slot = Get<u64>(main_base + MainGotRunRepentance);
    if (!slot) {
        why_resolve = "launcher GOT slot unreadable";
        return false;
    }
    // unbound slot = PLT0 inside main (run_repentance not called yet / AfterbirthPlus path)
    if (dsmod_sdk::InMain(main_base, main_size, *slot, 1) || *slot < NroRunRepentance) {
        why_resolve = "Repentance.nro not loaded";
        return false;
    }
    const u64 base = *slot - NroRunRepentance;
    if (base & 0xFFF) {
        why_resolve = "nro base not page aligned";
        return false;
    }
    u8 hdr[0x54];
    if (!Read(base, hdr, sizeof(hdr)) || std::memcmp(hdr + 0x10, "NRO0", 4) != 0) {
        why_resolve = "NRO0 magic mismatch";
        return false;
    }
    u32 fsize;
    std::memcpy(&fsize, hdr + 0x18, 4);
    if (fsize != NroFileSize || std::memcmp(hdr + 0x40, NroModuleId, 20) != 0) {
        why_resolve = "Repentance.nro size/module id mismatch";
        return false;
    }
    char mod0[4];
    if (!Read(base + NroMod0, mod0, 4) || std::memcmp(mod0, "MOD0", 4) != 0) {
        why_resolve = "MOD0 mismatch";
        return false;
    }
    nro = base;
    // the globals' GOT slots hold their relocated addresses
    const auto got_game = Get<u64>(nro + NroGotGGame), got_mgr = Get<u64>(nro + NroGotGManager);
    if (!got_game || *got_game != nro + NroGGame || !got_mgr || *got_mgr != nro + NroGManager) {
        why_resolve = "g_Game/g_Manager GOT mismatch";
        return false;
    }
    // fingerprints, per domain
    std::array<bool, static_cast<int>(Domain::Count)> ok;
    ok.fill(true);
    std::vector<u8> bytes;
    for (const HashPin& pin : Pins) {
        if (!Block(nro + pin.offset, pin.size, bytes) ||
            dsmod_sdk::Fnv1a64(bytes.data(), bytes.size()) != pin.fnv) {
            ok[static_cast<int>(pin.domain)] = false;
            if (host.log) {
                const std::string m = std::string{"Isaac DSMod: fingerprint mismatch "} + pin.label;
                host.log(host.userdata, EDEN_DSMOD_LOG_WARNING, m.c_str());
            }
        }
        if (pin.offset == PillFn && ok[static_cast<int>(Domain::Pill)])
            pill_code = bytes;
    }
    // round 9: the input manager's GOT slot must hold its relocated address, else Input fails closed
    const auto got_in = Get<u64>(nro + NroGotInputMgr);
    if (!got_in || *got_in != nro + NroInputMgr)
        ok[static_cast<int>(Domain::Input)] = false;
    dom_ok = ok;
    if (!Ok(Domain::Core)) {
        why_resolve = "core fingerprints mismatch";
        return false;
    }
    // Seed2String alphabet (fingerprinted in the Seed domain)
    if (Ok(Domain::Seed)) {
        if (!Read(nro + NroSeedAlphabet, alphabet, 32))
            dom_ok[static_cast<int>(Domain::Seed)] = false;
        alphabet[32] = 0;
    }
    if (Ok(Domain::Pill) && !DecodePill())
        dom_ok[static_cast<int>(Domain::Pill)] = false;
    bad_domains.clear();
    for (int d = 0; d < static_cast<int>(Domain::Count); ++d)
        if (!dom_ok[d])
            bad_domains += std::string{bad_domains.empty() ? "" : ","} + DomainName[d];
    resolved = true;
    why_resolve.clear();
    if (host.log) {
        char m[160];
        std::snprintf(m, sizeof(m), "Isaac DSMod: Repentance.nro at 0x%llx (bad domains: %s)",
                      static_cast<unsigned long long>(nro),
                      bad_domains.empty() ? "none" : bad_domains.c_str());
        host.log(host.userdata, EDEN_DSMOD_LOG_INFO, m);
    }
    return true;
}

/// Decodes ItemPool::GetPillEffect's two jump tables from the running NRO: for every effect the
/// remap target (MOVZ/B chains ending at the table load at 0x3C8714 or at the CanUsePill check
/// at 0x3C8724) and the w21 flag the CanUsePill fallback uses.
bool Reader::Impl::DecodePill() {
    if (pill_code.size() != PillFnSize)
        return false;
    if (!Read(nro + PillEffectTable, pill_table.data(), sizeof(pill_table)) ||
        !Read(nro + NroZodiacTable, zodiac_table.data(), sizeof(zodiac_table)))
        return false;
    const auto shifts = Get<u64>(nro + NroGotRngShifts);
    if (!shifts || !InNro(*shifts + 12 * 12, 12) ||
        !Read(*shifts + 12 * 12, rng_shift12.data(), sizeof(rng_shift12)))
        return false;
    const auto word = [&](u64 at) -> std::optional<u32> {
        if (at < PillFn || at + 4 > PillFn + PillFnSize || (at & 3))
            return std::nullopt;
        u32 w;
        std::memcpy(&w, pill_code.data() + (at - PillFn), 4);
        return w;
    };
    // run from `pc` with registers (w8, w20, w21) until the CanUsePill check
    const auto run = [&](u64 pc, s32 w8, s32 w20, s32 w21) -> std::optional<std::pair<s32, s32>> {
        for (int step = 0; step < 8; ++step) {
            if (pc == PillCanUse)
                return std::pair{w20, w21};
            if (pc == PillLoadBlock) {
                if (w8 < 0 || w8 >= static_cast<s32>(pill_table.size()))
                    return std::nullopt;
                return std::pair{pill_table[static_cast<std::size_t>(w8)], 0};
            }
            const auto w = word(pc);
            if (!w)
                return std::nullopt;
            const auto in = detail::Decode(*w, pc);
            if (in.kind == detail::Insn::MovzW) {
                const s32 v = static_cast<s32>(in.imm);
                if (in.rd == 8)
                    w8 = v;
                else if (in.rd == 20)
                    w20 = v;
                else if (in.rd == 21)
                    w21 = v;
                else
                    return std::nullopt;
                pc += 4;
            } else if (in.kind == detail::Insn::B) {
                pc = static_cast<u64>(in.target);
            } else {
                return std::nullopt;
            }
        }
        return std::nullopt;
    };
    std::array<u8, 0x31> good_t{}, bad_t{};
    if (!Read(nro + PillGoodTable, good_t.data(), 0x2F) ||
        !Read(nro + PillBadTable, bad_t.data(), 0x31))
        return false;
    for (s32 e = 0; e <= 0x30; ++e) {
        // good (PHD / Virgo / Lucky Foot): index effect-1 <= 0x2E, x8 = 0, w21 = 0
        pill_good[static_cast<std::size_t>(e)] = {e, 0};
        if (e >= 1 && e - 1 <= 0x2E) {
            const auto r = run(PillGoodBase + good_t[static_cast<std::size_t>(e - 1)] * 4u, 0, e, 0);
            if (!r)
                return false;
            pill_good[static_cast<std::size_t>(e)] = *r;
        }
        // bad (False PHD): index effect <= 0x30, w8 = effect, w21 = 1
        const auto r = run(PillBadBase + bad_t[static_cast<std::size_t>(e)] * 4u, e, e, 1);
        if (!r)
            return false;
        pill_bad[static_cast<std::size_t>(e)] = *r;
    }
    pill_decoded = true;
    return true;
}

// ---- game rule replicas ---------------------------------------------------------------------
/// ItemConfig::GetCollectible (nro+0x3C0C50): vector<Item*> at ItemConfig+0 (ids >= 0 only).
/// The Item* table is filled when the Manager loads items.xml; it is cached per Manager and
/// re-read when the Manager changes and once a second (Sample).
std::optional<u64> Reader::Impl::CollectibleConfig(s32 id) const {
    if (id < 0)
        return std::nullopt;
    if (item_cfg_m != M) {
        std::vector<u8> vb;
        if (!Block(M + MgrItemConfig, 0x10, vb))
            return std::nullopt;
        const u64 b = At<u64>(vb, 0), e = At<u64>(vb, 8);
        if (e < b || (e - b) / 8 > 0x10000)
            return std::nullopt;
        item_cfg.resize(static_cast<std::size_t>((e - b) / 8));
        if (!item_cfg.empty() && !Read(b, item_cfg.data(), item_cfg.size() * 8))
            return std::nullopt;
        item_cfg_m = M;
    }
    if (static_cast<u64>(id) >= item_cfg.size())
        return u64{0};
    return item_cfg[static_cast<std::size_t>(id)];
}

std::optional<bool> Reader::Impl::SeedFxContains(s32 key) const {
    const u64 end = G + GSeedFx;
    auto node = Get<u64>(end);
    if (!node)
        return std::nullopt;
    u64 best = end;
    for (int depth = 0; *node && depth < 64; ++depth) {
        const auto k = Get<s32>(*node + 0x1C);
        if (!k)
            return std::nullopt;
        const bool less = *k < key;
        if (!less)
            best = *node;
        node = Get<u64>(*node + (less ? 8 : 0));
        if (!node)
            return std::nullopt;
    }
    if (best == end)
        return false;
    const auto k = Get<s32>(best + 0x1C);
    if (!k)
        return std::nullopt;
    return *k == key;
}

/// Entity_Player::HasCollectible(id, false) (nro+0x27D3F4) for the item ids the pill and
/// charge rules ask about (46, 75, 303, 392, 619, 654 - none of the routine's per-id special
/// cases). Reproduced: co-op baby redirect, the disable flags with ItemConfig flag 0xC0 bit 15,
/// twin redirect, innate items map (P+0x26D0), P+0x2730, the four active slots, the owned count.
/// Not reproduced (fail closed = nullopt): the special-seed item roulette (seed effect 0x4F).
/// Not reproduced (documented): procedural (TMTRAINER) simulated items, item temporary-effect
/// instances, Metronome/0xBF held-item aliases.
std::optional<bool> Reader::Impl::HasCollectible(u64 p, s32 id, int depth) const {
    if (depth > 2 || id < 0)
        return std::nullopt;
    const auto item = CollectibleConfig(id);
    if (!item)
        return std::nullopt;
    const auto variant = Get<s32>(p + PVariant);
    if (!variant)
        return std::nullopt;
    if (*variant == 1) { // co-op baby: parent (type 1) or player 0
        const auto parent = Get<u64>(p + PCoopParent);
        if (!parent)
            return std::nullopt;
        u64 q = 0;
        if (*parent) {
            const auto t = Get<s32>(*parent + 0x38);
            if (t && *t == 1)
                q = *parent;
        }
        if (!q) {
            const auto first = Get<u64>(G + GPlayers);
            if (!first)
                return std::nullopt;
            const auto p0 = Get<u64>(*first);
            if (!p0)
                return std::nullopt;
            q = *p0;
        }
        if (q && q != p)
            return HasCollectible(q, id, depth + 1);
    }
    u8 flags_hi = 0, flags_c3 = 0;
    if (*item) {
        u8 f[4];
        if (!Read(*item + 0xC0, f, 4))
            return std::nullopt;
        flags_hi = f[1];
        flags_c3 = f[3];
    }
    const auto dis2 = Get<u8>(p + PDisable2);
    if (!dis2)
        return std::nullopt;
    if (*dis2 && !(flags_hi & 0x80))
        return false;
    const auto roulette = SeedFxContains(0x4F);
    if (!roulette || *roulette)
        return std::nullopt;
    if (*item) {
        const auto tf = Get<u8>(p + PTwinFlag);
        if (!tf)
            return std::nullopt;
        if (*tf && (flags_c3 & 0x40)) {
            const auto twin = Get<u64>(p + PTwin);
            if (!twin)
                return std::nullopt;
            if (*twin) {
                const auto r = HasCollectible(*twin, id, depth + 1);
                if (!r)
                    return std::nullopt;
                if (*r)
                    return true;
            }
        }
        const auto dis = Get<u8>(p + PDisable);
        if (!dis)
            return std::nullopt;
        if (*dis && id != 0x216 && !(flags_hi & 0x80))
            return false;
    }
    // innate items: libc++ map<int,int> (end node P+0x26D0, root = [end], size +0x8)
    {
        const u64 end = p + PInnate;
        auto node = Get<u64>(end);
        const auto size = Get<u64>(p + PInnateSize);
        if (!node || !size)
            return std::nullopt;
        if (*size && *node) {
            u64 best = end;
            for (int d = 0; *node && d < 64; ++d) {
                const auto k = Get<s32>(*node + 0x1C);
                if (!k)
                    return std::nullopt;
                const bool less = *k < id;
                if (!less)
                    best = *node;
                node = Get<u64>(*node + (less ? 8 : 0));
                if (!node)
                    return std::nullopt;
            }
            if (best != end) {
                const auto k = Get<s32>(best + 0x1C), v = Get<s32>(best + 0x20);
                if (!k || !v)
                    return std::nullopt;
                if (*k == id && *v > 0)
                    return true;
            }
        }
    }
    const auto special = Get<s32>(p + PSpecialItem);
    if (!special)
        return std::nullopt;
    if (*special && *special == id)
        return true;
    s32 act[4 * 7];
    if (!Read(p + OActive, act, sizeof(act)))
        return std::nullopt;
    for (int s = 0; s < 4; ++s)
        if (act[s * 7] == id)
            return true;
    u64 cv[2];
    if (!Read(p + OCounts, cv, sizeof(cv)) || cv[1] < cv[0])
        return std::nullopt;
    if (static_cast<u64>(id) >= (cv[1] - cv[0]) / 4)
        return false;
    const auto n = Get<s32>(cv[0] + static_cast<u64>(id) * 4);
    if (!n)
        return std::nullopt;
    return *n > 0;
}

/// TemporaryEffects::HasEffect(eNullItemID) (nro+0x4A6E14): P+0x18D8; +0x21 disabled byte;
/// vector<{Item*,..} 0x10> at +8/+0x10; match item type 0 (null item) and id.
std::optional<bool> Reader::Impl::TempHasNull(u64 p, s32 null_id) const {
    u8 hdr[0x28];
    if (!Read(p + PTempFx, hdr, sizeof(hdr)))
        return std::nullopt;
    if (hdr[0x21])
        return false;
    u64 b, e;
    std::memcpy(&b, hdr + 8, 8);
    std::memcpy(&e, hdr + 0x10, 8);
    if (e < b || (e - b) / 0x10 > 512)
        return std::nullopt;
    for (u64 r = b; r < e; r += 0x10) {
        const auto item = Get<u64>(r);
        if (!item)
            return std::nullopt;
        s32 ti[2];
        if (!*item || !Read(*item, ti, sizeof(ti)))
            return std::nullopt;
        if (ti[0] == 0 && ti[1] == null_id)
            return true;
    }
    return false;
}

/// Entity_Player::GetZodiacEffect (nro+0x29A938): Zodiac (392) -> RNG(stage seed of the
/// current stage, shift 12).Random(12) indexes the table at nro+0x8BCDBC; else 0.
std::optional<s32> Reader::Impl::ZodiacEffect(u64 p) const {
    const auto has = HasCollectible(p, 0x188);
    if (!has)
        return std::nullopt;
    if (!*has)
        return 0;
    const auto stage = Get<u32>(G + GStage);
    if (!stage || *stage > 64)
        return std::nullopt;
    const auto seed = Get<u32>(G + GStageSeeds + u64{*stage} * 4);
    if (!seed)
        return std::nullopt;
    u32 s = *seed; // RNG::Random: s ^= s >> a; s ^= s << b; s ^= s >> c
    s ^= s >> rng_shift12[0];
    s ^= s << rng_shift12[1];
    s ^= s >> rng_shift12[2];
    return zodiac_table[s % 12];
}

/// Entity_Player::CanUsePill (nro+0x2FBDF4).
std::optional<bool> Reader::Impl::CanUsePill(u64 p, s32 e) const {
    if (e != 6 && e != 1)
        return true;
    const auto te = TempHasNull(p, 0x70);
    const auto type = Get<s32>(p + OType);
    if (!te || !type)
        return std::nullopt;
    if (*te)
        return false;
    const u32 t = static_cast<u32>(*type);
    s32 hp;
    if (e == 1) {
        if (t == 0x27)
            return false;
        const auto red = Get<s32>(p + ORed), soul = Get<s32>(p + OSoul), bone = Get<s32>(p + PBone),
                   rot = Get<s32>(p + PRotten);
        if (!red || !soul || !bone || !rot)
            return std::nullopt;
        hp = *red + *soul + *bone - *rot;
    } else if (t > 0x27) {
        const auto m = Get<s32>(p + OMax);
        if (!m)
            return std::nullopt;
        hp = *m;
    } else if ((u64{1} << t) & 0x1803021010ULL) {
        const auto soul = Get<s32>(p + OSoul);
        if (!soul)
            return std::nullopt;
        hp = *soul;
    } else if ((u64{1} << t) & 0x8200004000ULL) {
        return false;
    } else if (t == 0x10) {
        const auto bone = Get<s32>(p + PBone);
        if (!bone)
            return std::nullopt;
        hp = *bone * 2;
    } else {
        const auto m = Get<s32>(p + OMax);
        if (!m)
            return std::nullopt;
        hp = *m;
    }
    return hp > 2;
}

/// ItemPool::GetPillEffect(color, player) (nro+0x3C857C).
std::optional<s32> Reader::Impl::PillEffect(u64 p, s32 color) const {
    if (!pill_decoded)
        return std::nullopt;
    const auto cp = Get<s32>(G + GChallengeParam);
    if (!cp)
        return std::nullopt;
    if (*cp == 0x2A)
        return 4;
    const s32 c = color & 0x7FF;
    if (c >= PillColors)
        return std::nullopt;
    const auto raw = Get<s32>(G + GPillEffect + static_cast<u64>(c) * 4);
    if (!raw)
        return std::nullopt;
    s32 eff = *raw, w21 = 0;
    const auto has = [&](s32 id) { return HasCollectible(p, id); };
    const auto phd = has(75), virgo = has(303), foot = has(46), fphd = has(654);
    if (!phd || !virgo || !foot || !fphd)
        return std::nullopt;
    enum { CanUse, Good, Bad } path = CanUse;
    if (*phd || *virgo || *foot) {
        path = *fphd ? CanUse : Good;
    } else {
        const auto z = ZodiacEffect(p);
        if (!z)
            return std::nullopt;
        if (*z == 303)
            path = *fphd ? CanUse : Good;
        else
            path = *fphd ? Bad : CanUse;
    }
    if (path == Good) {
        if (eff >= 1 && eff - 1 <= 0x2E)
            std::tie(eff, w21) = pill_good[static_cast<std::size_t>(eff)];
    } else if (path == Bad) {
        w21 = 1;
        if (eff >= 0 && eff <= 0x30)
            std::tie(eff, w21) = pill_bad[static_cast<std::size_t>(eff)];
    }
    const auto can = CanUsePill(p, eff);
    if (!can)
        return std::nullopt;
    if (!*can) {
        if (eff == 6)
            eff = w21 ? 8 : 7;
        else if (eff == 1)
            eff = w21 ? 8 : 5;
    }
    return eff;
}

/// Entity_Player::GetActiveMaxCharge(slot) (nro+0x27F828).
std::optional<s32> Reader::Impl::ActiveMaxCharge(u64 p, const std::vector<u8>& pb,
                                                 int slot) const {
    if (slot == 3)
        return 0;
    const auto a = [&](int s, u64 off) { return At<s32>(pb, OActive - PBlock + s * 0x1C + off); };
    const s32 item = a(slot, 0), var = a(slot, 0x18);
    if (slot == 2 && item == 0x244) {
        const auto ch = Get<s32>(G + GChallenge);
        if (!ch)
            return std::nullopt;
        if (*ch == 0x2C)
            return 0;
    }
    if (var >= 1 && (item == 0x11E || item == 0x107 || item == 0x15C))
        return var;
    const s32 type = At<s32>(pb, OType - PBlock);
    if (item == 0x2D2) {
        if ((type | 2) == 0x27) {
            const auto b = HasCollectible(p, 0x26B);
            if (!b)
                return std::nullopt;
            if (*b)
                return 0x12C;
        }
    } else if (item == 0x2C9) {
        if (type == 0x1A)
            return 0x1E;
    } else if (item == 0x1E9) {
        if (var & 0xFFFF)
            return var & 0xFFFF;
    } else if (item == 0x1A6) {
        if (var < 3)
            return 0;
    }
    const auto cfg = CollectibleConfig(item);
    if (!cfg)
        return std::nullopt;
    if (!*cfg)
        return 0;
    return Get<s32>(*cfg + 0x74);
}

/// PlayerManager::FirstCollectibleOwner(id, nullptr, true) (nro+0x4417AC) != null: a player of
/// variant 0 (P+0x3C) that HasCollectible(id); when the item's ItemConfig flags (+0xC0) bit 31 is
/// set, also its twin (P+0x2588). Players = the PlayerManager vector at G+0x25C50.
std::optional<bool> Reader::Impl::AnyOwner(u64 b, u64 e, s32 id) const {
    const auto item = CollectibleConfig(id);
    if (!item)
        return std::nullopt;
    bool twin_too = false;
    if (*item) {
        const auto f = Get<u32>(*item + 0xC0);
        if (!f)
            return std::nullopt;
        twin_too = (*f >> 31) != 0;
    }
    for (u64 at = b; at < e; at += 8) {
        const auto p = Get<u64>(at);
        if (!p || !*p)
            return std::nullopt;
        const auto variant = Get<s32>(*p + PVariant);
        if (!variant)
            return std::nullopt;
        if (*variant != 0)
            continue;
        const auto has = HasCollectible(*p, id);
        if (!has)
            return std::nullopt;
        if (*has)
            return true;
        if (twin_too) {
            const auto twin = Get<u64>(*p + PTwin);
            if (!twin)
                return std::nullopt;
            if (*twin) {
                const auto t = HasCollectible(*twin, id);
                if (!t)
                    return std::nullopt;
                if (*t)
                    return true;
            }
        }
    }
    return false;
}

/// Round 9: the button bound to `action` on the player's controller, read the way
/// Manager::RenderButtonIcon (nro+0x3FB3B8) resolves it: KAGE::Input::g_Manager (nro+0xB06F60)
/// devices vector [+8, +0x10) (ManagerBase::GetActionButtons 0x4FAEF8 walks it, matching
/// InputDeviceBase::GetId() = [dev+8] 0x4F6F54 against the controller id), then the device's
/// virtual GetActionButtons (vtable +0x108 = InputDeviceBase::GetActionButtons 0x4F8518:
/// action < [dev+0x1C], row = [[dev+0x20] + action*8], s_MaximumButtonsPerAction ints) and button
/// index 0; device type = virtual GetDeviceType (vtable +0x138 = DeviceNX::GetDeviceType 0x4FC898:
/// [dev+0x488]). Lock-free: the vector is re-read after the walk (torn -> nullopt) and the device's
/// two vtable slots must be the game's own functions. Controller -1 (any device) -> nullopt.
std::optional<Reader::Impl::ButtonGlyph> Reader::Impl::LiveButton(s32 controller, s32 action) const {
    if (controller < 0 || action < 0 || action > 63)
        return std::nullopt;
    const u64 mgr = nro + NroInputMgr;
    std::vector<u8> vec;
    if (!Block(mgr + 8, 16, vec))
        return std::nullopt;
    const u64 b = At<u64>(vec, 0), e = At<u64>(vec, 8);
    if (e < b || (e - b) % 8 || (e - b) / 8 == 0 || (e - b) / 8 > 16)
        return std::nullopt;
    std::vector<u8> devs;
    if (!Block(b, e - b, devs))
        return std::nullopt;
    u64 dev = 0, vptr = 0;
    for (u64 i = 0; i < (e - b) / 8 && !dev; ++i) {
        const u64 d = At<u64>(devs, i * 8);
        std::vector<u8> head;
        if (!d || !Block(d, 16, head))
            continue;
        if (At<s32>(head, DevId) == controller) {
            dev = d;
            vptr = At<u64>(head, 0);
        }
    }
    std::vector<u8> vec2;
    if (!dev || !Block(mgr + 8, 16, vec2) || vec2 != vec)
        return std::nullopt;
    if (!InNro(vptr, VtGetDeviceType + 8))
        return std::nullopt;
    const auto f1 = Get<u64>(vptr + VtGetActionButtons), f2 = Get<u64>(vptr + VtGetDeviceType);
    if (!f1 || !f2 || *f1 != nro + NroDevGetActionButtons || *f2 != nro + NroDevGetDeviceType)
        return std::nullopt;
    std::vector<u8> d;
    if (!Block(dev + DevNumActions, 12, d))
        return std::nullopt;
    const u32 nact = At<u32>(d, 0);
    const u64 tbl = At<u64>(d, 4);
    const auto maxb = Get<u32>(nro + NroMaxButtonsPerAction);
    if (nact > 64 || static_cast<u32>(action) >= nact || !tbl || !maxb || *maxb < 1 || *maxb > 4)
        return std::nullopt;
    const auto row = Get<u64>(tbl + static_cast<u64>(action) * 8);
    const auto button = row && *row ? Get<u32>(*row) : std::nullopt; // index 0 (RenderButtonIcon w3)
    const auto type = Get<s32>(dev + DevType);
    if (!button || !type)
        return std::nullopt;
    ButtonGlyph g;
    g.live = true;
    g.button = static_cast<s32>(*button);
    g.type = *type;
    g.frame = detail::ButtonFrameForType(detail::ButtonFrame(*button), *type);
    return g;
}

/// RenderButtonIcon's fallback (0x3FB48C..0x3FB4DC): the first (action, button) pair of
/// InputConfigOptions::s_ButtonActions (pointer nro+0xA385B8, count nro+0xA385C0; the Switch
/// table is IsaacRepentance::s_ButtonMap nro+0xA38300, 36 pairs) whose action matches.
std::optional<s32> Reader::Impl::DefaultButton(s32 action) const {
    const auto tbl = Get<u64>(nro + NroDefButtonActions);
    const auto n = Get<u32>(nro + NroDefNumButtonActions);
    if (!tbl || !n || *n == 0 || *n > 64 || !InNro(*tbl, u64{*n} * 8))
        return std::nullopt;
    std::vector<u8> pairs;
    if (!Block(*tbl, u64{*n} * 8, pairs))
        return std::nullopt;
    for (u32 i = 0; i < *n; ++i)
        if (At<s32>(pairs, u64{i} * 8) == action)
            return At<s32>(pairs, u64{i} * 8 + 4);
    return std::nullopt;
}

/// Entity_Player::GetMaxPocketItems (nro+0x2B0970): 2 with Starter Deck (251), Little Baggy
/// (252) or Polydactyly (454) (HasCollectible(id, false)), else 1; plus one per pocket slot 0..3
/// holding a pocket active (kind P+0x1C14+8i == 2 with id P+0x1C10+8i > 0).
std::optional<s32> Reader::Impl::MaxPocketItems(u64 p, const std::vector<u8>& pb) const {
    bool slot_item = false;
    for (const s32 id : {0xFB, 0xFC, 0x1C6}) {
        const auto has = HasCollectible(p, id);
        if (!has)
            return std::nullopt;
        if (*has) {
            slot_item = true;
            break;
        }
    }
    s32 kind[4], id[4];
    for (int s = 0; s < 4; ++s) {
        id[s] = At<s32>(pb, OPocket - PBlock + s * 8);
        kind[s] = At<s32>(pb, OPocket - PBlock + s * 8 + 4);
    }
    return detail::MaxPocketItems(slot_item, kind, id);
}

// ---- one sample -----------------------------------------------------------------------------
void Reader::Impl::SampleInner(Out& o, bool& torn) {
    torn = false;
    const auto not_ready = [&](std::string why) {
        in_run = false;
        o.I("run.ready", 0);
        o.T("run.why", std::move(why));
    };
    // Reads are merged into a few blocks (sample cost): G+0x24EE70..0x24F9A4 (minimap state,
    // frame counter, run timer), G+0x0..0x10, G+0x21508..0x21568, G+0x25C50..0x25DA8 (players
    // vector, seed, special seed set), P+0x16B8..0x24E0 (hearts, pickups, stats, slots, golden/
    // bone/broken/rotten hearts, history header). The frame counter, the level block, the first
    // 0x30 player bytes and the history header are re-read after the sample (torn reads).
    std::vector<u8> ga, gb0, gb1, gp, pb;
    if (!Block(G + GBlockA, GBlockAEnd - GBlockA, ga) || !Block(G + GStage, 0x10, gb0) ||
        !Block(G + 0x21508, 0x60, gb1) || !Block(G + GPlayers, GBlockPEnd - GPlayers, gp)) {
        not_ready("game unreadable");
        return;
    }
    const s32 frame0 = At<s32>(ga, GFrame - GBlockA);
    const u32 map_state = At<u32>(ga, GMinimap - GBlockA);
    const s32 timer = At<s32>(ga, GTimer - GBlockA);
    const s32 stage = At<s32>(gb0, 0), stype = At<s32>(gb0, 4);
    const u32 curses_raw = At<u32>(gb0, 0xC);
    const s32 room_count = At<s32>(gb1, GRoomCount - 0x21508);
    const u64 room_ptr = At<u64>(gb1, GRoom - 0x21508);
    const s32 cur = At<s32>(gb1, GCurRoom - 0x21508);
    const s32 dim = At<s32>(gb1, GDim - 0x21508);
    const u64 players[2] = {At<u64>(gp, 0), At<u64>(gp, 8)};
    const u32 seed = At<u32>(gp, GSeed - GPlayers);
    const bool seedfx_empty = At<u64>(gp, GSeedFx - GPlayers) == 0; // std::set root == null
    if (!players[0] || players[1] <= players[0] || (players[1] - players[0]) % 8 ||
        (players[1] - players[0]) / 8 > 16) {
        not_ready("no player");
        return;
    }
    const auto p0 = Get<u64>(players[0]);
    if (!p0 || !*p0) {
        not_ready("no player");
        return;
    }
    P = *p0;
    if (!Block(P + PBlock, PBlockEnd - PBlock, pb)) {
        not_ready("player unreadable");
        return;
    }
    const s32 golden = At<s32>(pb, PGolden - PBlock);
    const s32 bone = At<s32>(pb, PBone - PBlock), broken = At<s32>(pb, PBroken - PBlock),
              rotten = At<s32>(pb, PRotten - PBlock);
    const auto pause = Get<u32>(G + GPause);
    const auto challenge = Get<s32>(G + GChallenge);
    const auto difficulty = Get<s32>(G + GDifficulty);
    if (!pause || !challenge || !difficulty) {
        not_ready("game unreadable");
        return;
    }
    // history header (for the torn check and the ITEMS/INFO pages)
    const u64 hist[2] = {At<u64>(pb, PHistory - PBlock), At<u64>(pb, PHistory - PBlock + 8)};
    if (hist[1] < hist[0] || (hist[1] - hist[0]) % 0x1C ||
        hist[1] - hist[0] > 2048 * 0x1C) {
        not_ready("player unreadable");
        return;
    }

    ObserveFloor(seed,stage,stype);
    o.I("run.ready", 1);
    o.T("run.why", "");
    o.I("run.paused", *pause != 0 ? 1 : 0);
    o.I("run.map_held", map_state == 1 ? 1 : 0);

    // ---- header ----
    // Level::GetCurses = raw | GetSpecialSeedPermanentCurses & ~GetSpecialSeedBannedCurses; both
    // are 0 without special seed effects (their set at G+0x25DA0 is empty). Special seeds: the
    // raw value is published and floor.special = 1 (the name fails closed).
    const u32 curses = curses_raw;
    o.I("floor.stage", stage);
    o.I("floor.type", stype);
    o.I("floor.curses", curses);
    o.I("floor.special", seedfx_empty ? 0 : 1);
    const bool lost = (curses & 4) != 0; // Minimap::Render: tbnz w21,#2 skips the rooms
    o.I("floor.lost", lost ? 1 : 0);
    {
        const bool name_on =
            Ok(Domain::Name) && seedfx_empty && *challenge != 0x2C && text && text->Ready();
        const u64 key = u64{curses} | (u64{static_cast<u8>(stage)} << 32) |
                        (u64{static_cast<u8>(stype)} << 40) |
                        (u64{static_cast<u8>(*difficulty)} << 48) |
                        (u64{static_cast<u8>(*challenge)} << 56);
        o.T("floor.name", memo_floor.Get(key, name_on, [&] {
            std::string name;
            if (name_on) {
                const std::string base = text->StageName(stage, stype);
                if (!base.empty())
                    name = base + detail::FloorSuffix(stage, curses, *difficulty, *challenge);
            }
            return name;
        }));
    }
    o.T("time.text", memo_time.Get(u64{static_cast<u32>(timer)} | u64{static_cast<u32>(*challenge)} << 32,
                                   true, [&] { return detail::TimeText(timer, *challenge); }));
    // ---- pocket slot count (same game-derived calculation, cached for 15 samples) ----
    if (slow_n++ % RefreshSamples == 0 || pk_max < 0) {
        const bool owners_ok = Ok(Domain::Items) && Ok(Domain::Pill) && Ok(Domain::Hud);
        const auto mp = owners_ok ? MaxPocketItems(P, pb) : std::nullopt;
        pk_max = mp ? std::clamp(*mp, 1, 4) : 4;
    }
    o.I("pk.max", pk_max);
    o.T("seed.text", memo_seed.Get(seed, Ok(Domain::Seed),
                                   [&] { return Ok(Domain::Seed) ? detail::Seed2String(seed, alphabet)
                                                                 : std::string{}; }));

    // ---- pickups + hearts ----
    const auto pi = [&](u64 off) { return At<s32>(pb, off - PBlock); };
    if (Ok(Domain::Player)) {
        o.I("p.coins", pi(OCoins));
        o.I("p.bombs", pi(OBombs));
        o.I("p.keys", pi(OKeys));
        o.I("p.gkey", At<u8>(pb, OGKey - PBlock) ? 1 : 0);
        o.I("p.gbomb", At<u8>(pb, OGBomb - PBlock) ? 1 : 0);
        o.I("p.type", pi(OType));
        o.I("p.h.max", pi(OMax));
        o.I("p.h.red", pi(ORed));
        o.I("p.h.soul", pi(OSoul));
        o.I("p.h.black", std::popcount(static_cast<u32>(pi(OBlack))));
        o.I("p.h.eternal", pi(OEternal));
        o.I("p.h.golden", golden);
        o.I("p.h.bone", bone);
        o.I("p.h.broken", broken);
        o.I("p.h.rotten", rotten);
    }
    o.I("p.ready", Ok(Domain::Player) ? 1 : 0);

    // ---- stats (HUD::StatHUD::RecomputeStats bits 0..5 -> StatHUD::Render "%.2f") ----
    if (Ok(Domain::Stats)) {
        const auto f = [&](u64 off) { return At<float>(pb, off - PBlock); };
        const auto stat = [&](int i, float v) -> const std::string& {
            return memo_stat[i].Get(std::bit_cast<u32>(v), true, [&] { return detail::StatText(v); });
        };
        o.T("st.speed", stat(0, f(OSpeed)));
        o.T("st.tears", stat(1, 30.0f / (f(OFireDelay) + 1.0f)));
        o.T("st.damage", stat(2, f(ODamage)));
        o.T("st.range", stat(3, f(ORange) / 40.0f));
        o.T("st.shot", stat(4, f(OShotSpeed)));
        o.T("st.luck", stat(5, f(OLuck)));
        // pause-screen tallies (PauseScreen::Show 0x43274C) + text lengths (UI scale tiers)
        static constexpr u64 Raw[6] = {OSpeed, OFireDelay, ODamage, ORange, OShotSpeed, OLuck};
        static constexpr const char* TallyName[6] = {"st.0.tally", "st.1.tally", "st.2.tally",
                                                     "st.3.tally", "st.4.tally", "st.5.tally"};
        static constexpr const char* LenName[6] = {"st.0.len", "st.1.len", "st.2.len",
                                                   "st.3.len", "st.4.len", "st.5.len"};
        for (int i = 0; i < 6; ++i) {
            o.I(TallyName[i], detail::StatTally(i, f(Raw[i])));
            o.I(LenName[i], static_cast<int>(memo_stat[i].val.size()));
        }
    }
    o.I("st.tally_max", detail::StatTallyMax);
    o.I("st.ready", Ok(Domain::Stats) ? 1 : 0);

    // ---- active items, trinkets, pockets ----
    const bool text_on = text && text->Ready();
    const bool items_ok = Ok(Domain::Items);
    std::array<s32, 4> act_id{};
    for (int s = 0; s < 4; ++s) {
        const std::string k = "act." + std::to_string(s) + ".";
        const s32 id = At<s32>(pb, OActive - PBlock + s * 0x1C);
        const s32 charge = At<s32>(pb, OActive - PBlock + s * 0x1C + 4);
        const s32 battery = At<s32>(pb, OActive - PBlock + s * 0x1C + 8);
        act_id[static_cast<std::size_t>(s)] = id;
        std::optional<s32> max;
        if (items_ok && id > 0)
            max = ActiveMaxCharge(P, pb, s);
        o.I(k + "id", items_ok ? id : 0);
        o.I(k + "charge", items_ok && id > 0 ? charge : 0);
        o.I(k + "max", max ? *max : 0);
        o.I(k + "battery", items_ok && id > 0 ? battery : 0);
        o.T(k + "key", items_ok && id > 0 ? Key("coll", id) : std::string{});
        o.T(k + "name", memo_act[s].Get(static_cast<u32>(id), items_ok && id > 0 && text_on, [&] {
            return items_ok && id > 0 ? Name(isaac_text::Kind::Collectible, id) : std::string{};
        }));
        std::string bar;
        if (max && *max > 0) {
            bar = "module:isaac:charge/" + std::to_string(charge) + "/" + std::to_string(*max);
            if (battery > 0)
                bar += "/" + std::to_string(battery);
        }
        o.T(k + "bar_key", std::move(bar));
    }
    for (int t = 0; t < 2; ++t) {
        const std::string k = "tr." + std::to_string(t) + ".";
        const s32 raw = At<s32>(pb, OTrinket - PBlock + t * 4);
        const s32 id = raw & 0x7FFF;
        const bool gold = (raw & 0x8000) != 0;
        o.I(k + "id", items_ok ? id : 0);
        o.I(k + "gold", items_ok && gold ? 1 : 0);
        o.T(k + "key", items_ok && id ? Key("trink", id) + (gold ? "/g" : "") : std::string{});
        o.T(k + "name", memo_tr[t].Get(static_cast<u32>(id), items_ok && id && text_on, [&] {
            return items_ok && id ? Name(isaac_text::Kind::Trinket, id) : std::string{};
        }));
    }
    std::array<std::pair<s32, s32>, 4> pockets{}; // (kind, id) published
    for (int s = 0; s < 4; ++s) {
        const std::string k = "pk." + std::to_string(s) + ".";
        const s32 id = At<s32>(pb, OPocket - PBlock + s * 8);
        s32 kind = At<s32>(pb, OPocket - PBlock + s * 8 + 4);
        s32 pub_id = id;
        if (!items_ok || id == 0 || kind < 0 || kind > 2) { // empty slot = {0, 1} (C3)
            kind = -1;
            pub_id = 0;
        } else if (kind == 2) { // pocket active: id = active slot + 1 (RenderPocketItems)
            pub_id = id >= 1 && id <= 4 ? act_id[static_cast<std::size_t>(id - 1)] : 0;
            if (!pub_id)
                kind = -1;
        }
        pockets[static_cast<std::size_t>(s)] = {kind, pub_id};
        o.I(k + "kind", kind);
        o.I(k + "id", pub_id);
        o.T(k + "key", kind >= 0 ? "module:isaac:pocket/" + std::to_string(kind) + "/" +
                                       std::to_string(pub_id)
                                 : std::string{});
    }
    {
        // PlayerHUD::RenderPocketItems: pill -> identified[color & 0x7FF] ? PillEffect name :
        // "#QUESTION_MARKS_NAME"; card -> card name; pocket active -> item name
        const auto [kind, id] = pockets[0];
        std::string name;
        s64 ident = 0, effect = -1;
        if (kind == 0) {
            const s32 c = id & 0x7FF;
            const auto idf = c < PillColors ? Get<u8>(G + GPillIdent + static_cast<u64>(c))
                                            : std::nullopt;
            if (idf && *idf) {
                ident = 1;
                const auto eff = Ok(Domain::Pill) ? PillEffect(P, id) : std::nullopt;
                if (eff) {
                    effect = *eff;
                    name = Name(isaac_text::Kind::PillEffect, *eff);
                }
            } else if (idf && text && text->Ready()) {
                // unidentified: Manager::GetString(Manager+0x36CC0, "PocketItems",
                // "#QUESTION_MARKS_NAME") (0x3A67F4..0x3A6810) -> stringtable category
                // PocketItems, key QUESTION_MARKS_NAME ("???"); missing -> "" (fail closed)
                name = text->Text("PocketItems", "QUESTION_MARKS_NAME");
            }
        } else if (kind == 1) {
            name = Name(isaac_text::Kind::Card, id);
        } else if (kind == 2) {
            name = Name(isaac_text::Kind::Collectible, id);
        }
        o.T("pk.0.name", std::move(name));
        o.I("pk.0.ident", ident);
        o.I("pk.0.effect", effect); // the HUD's pill effect (identified pills only), else -1
    }
    {
        // round 9: the glyph of the button that uses pocket slot 0 (control_pocket_item's action on
        // the player's controller, resolved like Manager::RenderButtonIcon). Fail closed: no live
        // binding (Input domain off, unreadable / torn table, unknown button) -> the default table
        // (RenderButtonIcon's own fallback), else R (button 11 -> frame 11) on "Switch".
        const s32 type = At<s32>(pb, OType - PBlock);
        s32 action = 10;
        if (static_cast<u32>(type - 19) <= 1) { // Jacob / Esau
            const auto twin = Get<u64>(P + PPocketTwin);
            const auto t19 = twin && *twin ? Get<s32>(*twin + PTwinOrder) : std::nullopt;
            action = detail::PocketAction(type, t19.has_value(), At<s32>(pb, PTwinOrder - PBlock),
                                          t19.value_or(0));
        }
        std::optional<ButtonGlyph> g;
        if (Ok(Domain::Input) && Ok(Domain::Player))
            g = LiveButton(At<s32>(pb, PController - PBlock), action);
        s32 frame = g ? g->frame : -1, type_used = g ? g->type : -1, button = g ? g->button : -1;
        bool live = g && frame >= 0;
        if (frame < 0 && Ok(Domain::Input)) {
            if (const auto d = DefaultButton(action)) {
                button = *d;
                frame = detail::ButtonFrameForType(detail::ButtonFrame(static_cast<u32>(*d)), type_used);
            }
        }
        if (frame < 0) {
            button = 11;
            frame = detail::ButtonFrame(11);
            type_used = -1;
        }
        o.I("pk.btn_live", live ? 1 : 0);
        o.I("pk.btn_action", action);
        o.I("pk.btn_button", button);
        o.I("pk.btn_type", type_used);
        o.I("pk.btn_frame", frame);
        o.T("pk.btn_key", "module:isaac:anim/gfx/backdrop/controls_buttons.anm2#" +
                              std::string{detail::ButtonAnim(type_used)} + "#" + std::to_string(frame));
    }

    // ---- MAP page: rooms of the current dimension the minimap draws ----
    // the current dimension's 13x13 grid (list index per cell): whole on the map page
    std::vector<u8> grid;
    const bool dim_ok = dim >= 0 && dim < 3;
    const bool grid_ok = dim_ok && ui_page == PageMap &&
                         Block(G + GGrid + static_cast<u64>(dim) * 169 * 4, 169 * 4, grid);
    s32 cur_li = -1;
    if (cur >= 0 && cur < 169 && dim_ok) {
        const auto li = grid_ok ? std::optional<s32>{At<s32>(grid, static_cast<u64>(cur) * 4)}
                                : Get<s32>(G + GGrid + (static_cast<u64>(dim) * 169 + cur) * 4);
        if (li && *li >= 0 && *li < room_count)
            cur_li = *li;
    } else if (cur <= -1 && cur >= -18) {
        cur_li = 0x1FA - cur;
    }
    o.I("map.cur", cur_li);
    if (ui_page == PageMap) {
        std::vector<MapRoom> rooms;
        bool ok = Ok(Domain::Map) && room_count >= 0 && room_count <= MaxRoomsList && grid_ok;
        std::vector<u8> rb;
        ok = ok && Block(G + GRooms, static_cast<std::size_t>(room_count) * DStride, rb);
        if (ok) {
            std::vector<bool> seen(static_cast<std::size_t>(room_count), false);
            std::vector<s32> order;
            for (int c = 0; c < 169; ++c) {
                const s32 li = At<s32>(grid, static_cast<u64>(c) * 4);
                if (li >= 0 && li < room_count && !seen[static_cast<std::size_t>(li)]) {
                    seen[static_cast<std::size_t>(li)] = true;
                    order.push_back(li);
                }
            }
            std::sort(order.begin(), order.end());
            if (room_data_cache.size() > 2048)
                room_data_cache.clear();
            for (const s32 li : order) {
                const u64 d = static_cast<u64>(li) * DStride;
                const s32 gidx = At<s32>(rb, d + DGrid);
                const u64 data = At<u64>(rb, d + DData);
                const u32 disp = At<u32>(rb, d + DDisplay);
                const s32 visited = At<s32>(rb, d + DVisited);
                const u32 flags = At<u32>(rb, d + DFlags);
                const bool is_cur = li == cur_li;
                // Config::render: outline/fill need Data != 0 and DisplayFlags != 0 (or current)
                if (!data || (!disp && !is_cur) || gidx < 0 || gidx >= 169)
                    continue;
                auto it = room_data_cache.find(data);
                if (it == room_data_cache.end()) {
                    // RoomConfig::Room: +0x8 type, +0x10 subtype, +0x5C shape (one read)
                    u8 rd[0x58];
                    if (!Read(data + 0x8, rd, sizeof(rd))) {
                        ok = false;
                        break;
                    }
                    Impl::RoomData v{};
                    std::memcpy(&v.type, rd, 4);
                    std::memcpy(&v.subtype, rd + 0x8, 4);
                    std::memcpy(&v.shape, rd + 0x54, 4);
                    it = room_data_cache.emplace(data, v).first;
                }
                const Impl::RoomData& rd = it->second;
                MapRoom r{};
                r.grid = static_cast<u8>(gidx);
                r.shape = static_cast<u8>(rd.shape);
                r.type = static_cast<u8>(rd.type);
                r.dflags = static_cast<u8>((disp & 7) | (visited > 0 ? 0x80 : 0));
                r.flags = detail::MapFlags(flags, rd.type, rd.subtype);
                r.current = is_cur ? 1 : 0;
                rooms.push_back(r);
            }
        }
        o.T("map.key", ok ? "module:isaac:map/" + isaac_mapreg::Register(detail::MapHex(rooms, lost))
                         : std::string{});
        o.I("map.rooms", ok && !lost ? static_cast<s64>(rooms.size()) : 0);
        o.I("map.ok", ok ? 1 : 0);
        // the page's default map view (round 3): the image's own layout (isaac_map::PlanLayout,
        // the same function the asset side draws with): zoom MaxScale / k = rooms at the owner's
        // reference size (17x15 cells x6), centred on the current room; zoom 1 = the whole floor
        isaac_map::Layout lay;
        if (ok && !lost) {
            isaac_map::Floor fl;
            for (const MapRoom& r : rooms)
                fl.rooms.push_back({r.grid, r.shape, r.type, r.dflags, r.flags, r.current});
            lay = isaac_map::PlanLayout(fl);
        }
        o.F("map.vzoom", lay.ok ? static_cast<double>(isaac_map::MaxScale) / lay.k : 1.0);
        o.F("map.vcx", lay.ok ? lay.cx : 0.5);
        o.F("map.vcy", lay.ok ? lay.cy : 0.5);
        o.I("map.vsig", lay.ok ? static_cast<s64>(lay.sig) : 0);
        o.I("map.k", lay.ok ? static_cast<s64>(lay.k) : 0);
    }

    // ---- description of one item (ITEMS selection isel.*, ROOM selection info.*) ----
    // text kind (isaac_text::Kind; -1 = nothing), id, image key, name override ("" = game name)
    const auto describe = [&](const std::string& pre, s32 kind, s32 id, std::string key,
                              std::string name_override) {
        std::string name, quote, pools, eid;
        s64 quality = -1;
        if (kind >= 0 && (id > 0 || kind == 3) && text && text->Ready()) {
            const auto info = text->Item(static_cast<isaac_text::Kind>(kind), id);
            if (info.valid) {
                name = info.name;
                quote = info.quote;
                quality = info.quality;
                pools = info.pools;
            }
            eid = text->Eid(static_cast<isaac_text::Kind>(kind), id);
        }
        if (!name_override.empty())
            name = std::move(name_override);
        o.I(pre + "kind", kind);
        o.I(pre + "id", id);
        o.T(pre + "key", std::move(key));
        o.T(pre + "name", std::move(name));
        o.T(pre + "quote", std::move(quote));
        o.I(pre + "quality", quality);
        o.T(pre + "pools", std::move(pools));
        // text shape for the page's scroll box (paragraphs, code points without the breaks)
        s64 paras = eid.empty() ? 0 : 1, chars = 0;
        for (const char ch : eid) {
            if (ch == '\n')
                ++paras;
            else if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80)
                ++chars;
        }
        o.I(pre + "eid_paras", paras);
        o.I(pre + "eid_chars", chars);
        o.I(pre + "eid_on", eid.empty() ? 0 : 1);
        o.T(pre + "eid", std::move(eid));
    };
    // slot snapshots for the ITEMS actions
    last_act = act_id;
    for (int t = 0; t < 2; ++t)
        last_tr[static_cast<std::size_t>(t)] =
            items_ok ? At<s32>(pb, OTrinket - PBlock + t * 4) & 0xFFFF : 0;
    last_pk = pockets;
    // room change (every sample): drops the explicit ROOM selection
    {
        const u64 rk = u64{static_cast<u32>(cur)} | (u64{static_cast<u8>(dim)} << 32) |
                       (u64{static_cast<u8>(stage)} << 40) | (u64{static_cast<u8>(stype)} << 48);
        if (rk != room_key) {
            room_key = rk;
            sel_ped = false;
            near_entity = 0;
            tap_near = 0;
        }
    }

    // ---- Bag of Crafting (every page) ----
    {
        // Bag of Crafting: P+0x2690 u8[8] components + P+0x2698 s32 result. The result is
        // written only by Entity_Player::TryAddToBagOfCrafting (0x2FF794 str w0 =
        // get_crafting_output(bag) when the 8th component lands, 0x2FF79C str wzr otherwise),
        // cleared on craft (control_active_item 0x2C8CA0, UseActiveItem 0x30F9C4), restored by
        // RestoreGameState. Read as one 12-byte block, twice; a mismatch (torn: the game thread
        // was writing) or a not-full bag hides the preview.
        // Round 8: published on every page (the left card's BAG page) while a Bag of Crafting is held:
        // the collectible (710) in any active slot or Tainted Cain's pocket bag (710 in pocket active
        // slot 2; RenderPocketItems 0x3A6AE4 draws the crafting table for active id 0x2C6). bag.held 0
        // -> all bag.* zero / empty.
        const bool bag_held = items_ok && detail::BagHeld(act_id);
        o.I("bag.held", bag_held ? 1 : 0);
        u8 bag[12] = {}, bag2[12] = {};
        const bool bok = bag_held && Read(P + PBag, bag, sizeof(bag)) &&
                         Read(P + PBag, bag2, sizeof(bag2)) && std::memcmp(bag, bag2, sizeof(bag)) == 0;
        if (!bok)
            std::memset(bag, 0, sizeof(bag));
        int bag_n = 0;
        for (int i = 0; i < 8; ++i) {
            bag_n += bok && bag[i] ? 1 : 0;
            o.I("bag." + std::to_string(i) + ".id", bok ? bag[i] : 0);
        }
        o.I("bag.count", bag_n);
        {
            s32 result = 0;
            std::memcpy(&result, bag + 8, 4);
            // HUD::PlayerHUD::Update 0x3A2848..0x3A28C8 (the HUD crafting table's result image):
            // ItemConfig::GetCollectible(P+0x2698) == null -> no image (GameText valid = the
            // items.xml entry); Level::GetCurses() bit 6 (Curse of the Blind) -> the image is
            // "gfx/Items/Collectibles/questionmark.png" instead of the item (= isaac:coll/q). The
            // published curses are the raw value; with special-seed curses in play (floor.special)
            // GetCurses may differ -> fail closed (no preview).
            const bool known = result > 0 && text && text->Ready() &&
                               text->Item(isaac_text::Kind::Collectible, result).valid;
            const int pv = detail::BagPreview(bag, bok, curses, seedfx_empty, known);
            const bool valid = pv != 0, blind = pv == 2;
            o.I("bag.result", valid && !blind ? result : 0);
            o.I("bag.blind", blind ? 1 : 0);
            o.T("bag.result_key", !valid ? std::string{}
                                  : blind ? std::string{"module:isaac:coll/q"}
                                          : Key("coll", result));
            o.T("bag.result_name", valid && !blind
                                       ? Name(isaac_text::Kind::Collectible, result)
                                       : std::string{});
        }
    }

    // ---- ITEMS page: history, the items selection ----
    const u64 hist_n = (hist[1] - hist[0]) / 0x1C;
    std::vector<HistItem> history;
    if (ui_page == PageItems && items_ok && hist_n <= 2048) {
        std::vector<u8> hb;
        if (Block(hist[0], static_cast<std::size_t>(hist_n) * 0x1C, hb)) {
            for (u64 i = 0; i < hist_n; ++i)
                history.push_back({At<s32>(hb, i * 0x1C), At<u8>(hb, i * 0x1C + 4),
                                   At<s32>(hb, i * 0x1C + 8)});
        }
        last_hist = history;
    }
    if (ui_page == PageItems) {
        // the selection holds while its item is still where it was tapped
        s32 inv_sel = -1;
        if (isrc == ISrc::Inv) {
            const auto it = std::find_if(history.begin(), history.end(), [&](const HistItem& h) {
                return h.frame == isel_frame && h.id == isel_raw && (h.trinket ? 1 : 0) == isel_kind;
            });
            if (it == history.end())
                isrc = ISrc::None;
            else
                inv_sel = static_cast<s32>(it - history.begin());
        } else if (isrc == ISrc::Act) {
            if (act_id[static_cast<std::size_t>(isel_slot)] != isel_raw || isel_raw <= 0)
                isrc = ISrc::None;
        } else if (isrc == ISrc::Tr) {
            if (last_tr[static_cast<std::size_t>(isel_slot)] != isel_raw || !(isel_raw & 0x7FFF))
                isrc = ISrc::None;
        } else if (isrc == ISrc::Pk) {
            if (pockets[static_cast<std::size_t>(isel_slot)] != std::pair<s32, s32>{isel_kind, isel_raw} ||
                isel_kind < 0)
                isrc = ISrc::None;
        }
        u64 cv[2];
        std::vector<u8> counts;
        const bool cok = Read(P + OCounts, cv, sizeof(cv)) && cv[1] >= cv[0] &&
                         (cv[1] - cv[0]) <= 0x4000 &&
                         Block(cv[0], static_cast<std::size_t>(cv[1] - cv[0]), counts);
        o.I("inv.count", static_cast<s64>(history.size()));
        for (std::size_t i = 0; i < history.size(); ++i) {
            const std::string k = "inv." + std::to_string(i) + ".";
            const HistItem& h = history[i];
            o.I(k + "id", h.id);
            o.I(k + "trinket", h.trinket ? 1 : 0);
            o.I(k + "sel", static_cast<s32>(i) == inv_sel && isrc == ISrc::Inv ? 1 : 0);
            if (h.trinket) {
                const s32 tid = h.id & 0x7FFF;
                o.T(k + "key", Key("trink", tid) + ((h.id & 0x8000) ? "/g" : ""));
                o.I(k + "n", 1);
            } else {
                o.T(k + "key", Key("coll", h.id));
                o.I(k + "n", cok && h.id >= 0 && static_cast<u64>(h.id) * 4 + 4 <= counts.size()
                                 ? At<s32>(counts, static_cast<u64>(h.id) * 4)
                                 : 0);
            }
        }
        // the selected item's description (shown on the ITEMS page in place of the stats)
        s32 kind = -1, id = 0;
        std::string key, name_override;
        switch (isrc) {
        case ISrc::Inv:
        case ISrc::Tr: {
            const bool trinket = isrc == ISrc::Tr || isel_kind == 1;
            if (trinket) {
                kind = 1;
                id = isel_raw & 0x7FFF;
                key = Key("trink", id) + ((isel_raw & 0x8000) ? "/g" : "");
            } else {
                kind = 0;
                id = isel_raw;
                key = Key("coll", id);
            }
            break;
        }
        case ISrc::Act:
            kind = 0;
            id = isel_raw;
            key = Key("coll", id);
            break;
        case ISrc::Pk: {
            // RenderPocketItems: pill -> effect name when identified, else "#QUESTION_MARKS_NAME";
            // card -> card; pocket active -> the item (art = the HUD pocket icon, 32x32)
            key = "module:isaac:pocket/" + std::to_string(isel_kind) + "/" + std::to_string(isel_raw);
            if (isel_kind == 1) {
                kind = 2;
                id = isel_raw;
            } else if (isel_kind == 2) {
                kind = 0;
                id = isel_raw;
                key = Key("coll", id);
            } else if (isel_kind == 0) {
                kind = 3;
                id = -1;
                const s32 c = isel_raw & 0x7FF;
                const auto idf = c < PillColors ? Get<u8>(G + GPillIdent + static_cast<u64>(c))
                                                : std::nullopt;
                if (idf && *idf) {
                    const auto eff = Ok(Domain::Pill) ? PillEffect(P, isel_raw) : std::nullopt;
                    if (eff)
                        id = *eff;
                } else if (idf && text && text->Ready()) {
                    name_override = text->Text("PocketItems", "QUESTION_MARKS_NAME");
                }
            }
            break;
        }
        case ISrc::Item:
            kind = isel_kind;
            id = isel_raw;
            key = kind == 0   ? Key("coll", id)
                  : kind == 1 ? Key("trink", id)
                  : kind == 2 ? "module:isaac:pocket/1/" + std::to_string(id)
                              : std::string{};
            break;
        case ISrc::None:
            break;
        }
        o.I("isel.on", isrc != ISrc::None ? 1 : 0);
        o.I("isel.src", static_cast<s64>(isrc));
        o.I("isel.slot", isrc == ISrc::None ? -1 : isrc == ISrc::Inv ? inv_sel : isel_slot);
        o.I("isel.act", isrc == ISrc::Act ? isel_slot : -1);
        o.I("isel.tr", isrc == ISrc::Tr ? isel_slot : -1);
        o.I("isel.pk", isrc == ISrc::Pk ? isel_slot : -1);
        describe("isel.", kind, id, std::move(key), std::move(name_override));
    }

    // ---- ROOM page: pedestals, the nearest one (EID rule), the selection ----
    if (ui_page == PageRoom) {
        std::vector<Pedestal> peds;
        bool rok = Ok(Domain::Room) && room_ptr;
        const bool near_on = Ok(Domain::Near);
        u64 el[2] = {};
        if (rok) {
            u8 lb[0x10]{};
            rok = Read(room_ptr + RoomEntityList + 0x78, lb, sizeof(lb));
            std::memcpy(&el[0], lb, 8);
            u32 n;
            std::memcpy(&n, lb + 0xC, 4);
            el[1] = n;
            rok = rok && el[1] <= 1024;
        }
        std::vector<u64> ents(static_cast<std::size_t>(rok ? el[1] : 0));
        if (rok && !ents.empty())
            rok = Read(el[0], ents.data(), ents.size() * 8);
        const bool blind_curse = dim != 2 && (curses & 64); // Entity_Pickup::ReloadGraphics
        std::vector<u8> eb;
        for (std::size_t i = 0; rok && i < ents.size(); ++i) {
            s32 tvs[3];
            if (!ents[i] || !Read(ents[i] + 0x38, tvs, sizeof(tvs)))
                continue;
            if (tvs[0] != 5 || tvs[1] != 100 || tvs[2] == 0) // collectible, not taken (C4)
                continue;
            u8 pk[0x14];
            if (!Read(ents[i] + 0x55C, pk, sizeof(pk)))
                continue;
            Pedestal pd{};
            pd.entity = ents[i];
            pd.id = tvs[2];
            std::memcpy(&pd.opt, pk, 4);
            std::memcpy(&pd.price, pk + 8, 4);
            pd.blind = dim != 2 && (blind_curse || pk[6] != 0);
            pd.near.flags = u64{1} << 46; // unreadable -> never the nearest
            if (near_on && Block(ents[i] + EBlock, EBlockEnd - EBlock, eb)) {
                pd.near.flags = At<u64>(eb, EFlags - EBlock);
                pd.near.spawn = At<s32>(eb, ESpawnFrame - EBlock);
                pd.near.x = At<float>(eb, EPos - EBlock);
                pd.near.y = At<float>(eb, EPos - EBlock + 4);
                pd.near.size = At<float>(eb, ESize - EBlock);
            }
            peds.push_back(pd);
        }
        last_peds = peds;
        // EID: the closest describable entity to the player (EID.player = Isaac.GetPlayer(0))
        int near = -1;
        float near_d = -1.0f;
        float pp[2];
        if (near_on && !peds.empty() && Read(P + EPos, pp, sizeof(pp))) {
            std::vector<detail::NearCand> cands;
            for (const Pedestal& pd : peds)
                cands.push_back(pd.near);
            near = detail::NearestPedestal(pp[0], pp[1], cands, frame0, EidRadius, &near_d);
        }
        const u64 near_ent = near >= 0 ? peds[static_cast<std::size_t>(near)].entity : 0;
        int shown = -1;
        if (sel_ped) {
            // by entity: a pedestal whose item changes in place (D6 reroll, Glitched Crown /
            // Binge Eater cycling) stays the tapped one; a taken item (subtype 0) leaves the list
            const auto it = std::find_if(peds.begin(), peds.end(),
                                         [&](const Pedestal& p) { return p.entity == sel_entity; });
            // gone, or another pedestal became the nearest one (neither the tapped one nor the one
            // that was nearest at the tap; a pedestal whose FrameCount restarts when its item
            // cycles drops out of range for a frame, so "none" in between is not a change)
            if (it == peds.end() ||
                (near_ent && near_ent != sel_entity && near_ent != tap_near))
                sel_ped = false;
            else
                shown = static_cast<int>(it - peds.begin());
        }
        if (near_ent)
            near_entity = near_ent;
        const bool autosel = !sel_ped && near >= 0;
        if (!sel_ped)
            shown = near >= 0 ? near : (peds.empty() ? -1 : 0);

        o.I("ped.count", static_cast<s64>(peds.size()));
        for (std::size_t i = 0; i < peds.size(); ++i) {
            const std::string k = "ped." + std::to_string(i) + ".";
            const Pedestal& pd = peds[i];
            o.I(k + "id", pd.blind ? 0 : pd.id);
            o.T(k + "key", pd.blind ? std::string{} : Key("coll", pd.id));
            o.I(k + "price", pd.price);
            o.I(k + "blind", pd.blind ? 1 : 0);
            o.I(k + "opt", pd.opt);
            o.I(k + "near", static_cast<int>(i) == near ? 1 : 0);
            o.I(k + "sel", static_cast<int>(i) == shown ? 1 : 0);
            o.T(k + "name", pd.blind ? std::string{} : Name(isaac_text::Kind::Collectible, pd.id));
        }
        o.I("room.near", near);
        o.I("room.near_dist", near >= 0 ? static_cast<s64>(std::lround(near_d)) : -1);
        o.I("room.sel", shown);
        o.I("room.auto", autosel ? 1 : 0);
        if (shown >= 0) {
            const Pedestal& pd = peds[static_cast<std::size_t>(shown)];
            o.I("info.blind", pd.blind ? 1 : 0);
            if (pd.blind) // hidden item: the game's "?" art, no text (EID: question mark)
                describe("info.", 0, 0, "module:isaac:coll/q", {});
            else
                describe("info.", 0, pd.id, Key("coll", pd.id), {});
        } else {
            o.I("info.blind", 0);
            describe("info.", -1, 0, {}, {});
        }
    }

    // ---- torn-read check: nothing the sample keyed on moved while it was read ----
    const auto frame1 = Get<s32>(G + GFrame);
    std::vector<u8> gb1b, pb2, gp2;
    u64 hist2[2]{};
    const auto player1 = Get<u64>(players[0]);
    if (!frame1 || *frame1 != frame0 || !Block(G + 0x21508, 0x60, gb1b) || gb1b != gb1 ||
        !player1 || *player1 != P ||
        !Block(G + GPlayers, GBlockPEnd - GPlayers, gp2) || gp2 != gp ||
        !Block(P + PBlock, 0x30, pb2) ||
        std::memcmp(pb2.data(), pb.data(), 0x30) != 0 ||
        !Read(P + PHistory, hist2, sizeof(hist2)) || std::memcmp(hist2, hist, sizeof(hist)) != 0)
        torn = true;
}

// ---- Reader ---------------------------------------------------------------------------------
Reader::Reader(const EdenDsmodHostApi& host, const char*) : impl{std::make_unique<Impl>()} {
    impl->host = host;
}
Reader::~Reader() = default;

void Reader::SetText(const isaac_text::GameText* text) {
    impl->text = text;
}

void Reader::Sample(const EdenDsmodHostApi& h) {
    Impl& m = *impl;
    m.host = h;
    Out& o = m.out;
    if (m.sample_n % 60 == 0)
        m.item_cfg_m = 0; // re-read the ItemConfig table once a second
    o.Clear();
    // aoc: is the runtime's own enabled add-on source for this title. Unlike the launcher's
    // build id (shared by AB+ and Repentance), it distinguishes an absent/disabled DLC.
    if (m.dlc_available < 0 && h.get_i64)
        m.dlc_available = h.get_i64(h.userdata, "__source:aoc", -1);
    o.I("dlc.available", m.dlc_available);
    o.I("edition.abp", m.dlc_available == 0 ? 1 : 0);
    bool torn = false;
    if (!m.resolved || h.main_base != m.main_base || h.main_size != m.main_size) {
        if (m.resolve_wait > 0 && h.main_base == m.main_base) {
            --m.resolve_wait;
        } else if (!(m.dlc_available == 0 ? m.ResolveAfterbirth() : m.Resolve())) {
            m.resolve_wait = 60; // retry once a second
        }
    }
    o.I("nro.ok", m.resolved ? 1 : 0);
    o.T("nro.bad", m.resolved ? m.bad_domains : std::string{"all"});
    o.I("ui.page", m.ui_page);
    // 1 when the bundled EID file was loaded (GameText is immutable once set)
    const s64 eid_avail = m.text && m.text->HasEid() ? 1 : 0;
    o.I("eid.avail", eid_avail);
    m.in_run = false;
    if (!m.resolved) {
        o.I("run.ready", 0);
        o.T("run.why", m.why_resolve);
    } else {
        const auto g = m.Get<u64>(m.nro + (m.dlc_available == 0 ? 0x826AF0 : NroGGame));
        const auto mgr = m.Get<u64>(m.nro + (m.dlc_available == 0 ? 0x827850 : NroGManager));
        const auto state = mgr && *mgr ? m.Get<u32>(*mgr + (m.dlc_available == 0 ? 0 : MgrState)) : std::nullopt;
        if (!g || !*g || !state || *state != 2) {
            o.I("run.ready", 0);
            o.T("run.why", !g || !*g ? "no game" : "not in a run");
        } else {
            m.G = *g;
            m.M = *mgr;
            m.in_run = true;
            for (int attempt = 0; attempt < 2; ++attempt) {
                o.Clear();
                o.I("nro.ok", 1);
                o.T("nro.bad", m.bad_domains);
                o.I("ui.page", m.ui_page);
                o.I("eid.avail", eid_avail);
                o.I("dlc.available", m.dlc_available);
                o.I("edition.abp", m.dlc_available == 0 ? 1 : 0);
                if (m.dlc_available == 0)
                    m.SampleAfterbirth(o, torn);
                else
                    m.SampleInner(o, torn);
                if (!torn)
                    break;
            }
        }
    }
    if (!m.in_run)
        m.ResetRun();
    if (torn) {
        ++m.torn_count;
        m.last_good.Publish(h); // keep showing the last consistent sample
        // ui.page must follow the actions even while torn samples are held back
        if (h.publish_i64)
            h.publish_i64(h.userdata, "ui.page", m.ui_page);
    } else {
        o.Publish(h);
        std::swap(m.last_good, m.out); // keep it without a copy; out is cleared next sample
    }
    // Loading page clock (round 6): ld.t = anm2 time at the game's 30 fps (samples run at 60 Hz),
    // published only outside a run so an in-run page never redraws for it; restarts at 0 on each
    // return to the loading page.
    if (m.in_run) {
        m.load_n = 0;
    } else {
        if (h.publish_i64)
            h.publish_i64(h.userdata, "ld.t", (m.load_n / 2) % 100000);
        ++m.load_n;
    }
    if (h.publish_i64)
        h.publish_i64(h.userdata, "run.torn", m.torn_count);
    ++m.sample_n;
}

bool Reader::OnAction(const char* action, std::int64_t argument) {
    if (!action)
        return false;
    Impl& m = *impl;
    const std::string_view a{action};
    if (a == "ui_open") {
        if (argument < 0 || argument > 2)
            return false;
        m.ui_page = static_cast<int>(argument);
        return true;
    }
    // ITEMS selection: a tap on the same thing again clears it; the page never changes
    const auto toggle = [&](Impl::ISrc src, s32 slot, s32 kind, s32 raw, s32 frame) {
        if (m.isrc == src && m.isel_slot == slot && m.isel_kind == kind && m.isel_raw == raw &&
            m.isel_frame == frame) {
            m.isrc = Impl::ISrc::None;
            return true;
        }
        m.isrc = src;
        m.isel_slot = slot;
        m.isel_kind = kind;
        m.isel_raw = raw;
        m.isel_frame = frame;
        return true;
    };
    if (a == "info_sel") { // kind*100000 + id (0 collectible, 1 trinket, 2 card, 3 pill effect)
        if (argument < 0 || argument >= 4 * 100000)
            return false;
        return toggle(Impl::ISrc::Item, 0, static_cast<s32>(argument / 100000),
                      static_cast<s32>(argument % 100000), 0);
    }
    if (a == "info_inv") {
        if (argument < 0 || static_cast<std::size_t>(argument) >= m.last_hist.size())
            return false;
        const HistItem& h = m.last_hist[static_cast<std::size_t>(argument)];
        // the slot is re-found by (frame, id, trinket) every sample
        if (m.isrc == Impl::ISrc::Inv && m.isel_frame == h.frame && m.isel_raw == h.id &&
            m.isel_kind == (h.trinket ? 1 : 0)) {
            m.isrc = Impl::ISrc::None;
            return true;
        }
        return toggle(Impl::ISrc::Inv, static_cast<s32>(argument), h.trinket ? 1 : 0, h.id,
                      h.frame);
    }
    // the held slots (round 3: the left pane on every page): on ITEMS a tap toggles the
    // description; from MAP / ROOM it opens ITEMS with that description
    const auto slot_pick = [&](Impl::ISrc src, s32 slot, s32 kind, s32 raw) {
        if (m.ui_page != PageItems) {
            m.ui_page = PageItems;
            m.isrc = Impl::ISrc::None; // select, never toggle off
        }
        return toggle(src, slot, kind, raw, 0);
    };
    if (a == "info_act") {
        if (argument < 0 || argument > 3 || m.last_act[static_cast<std::size_t>(argument)] <= 0)
            return false;
        return slot_pick(Impl::ISrc::Act, static_cast<s32>(argument), 0,
                         m.last_act[static_cast<std::size_t>(argument)]);
    }
    if (a == "info_tr") {
        if (argument < 0 || argument > 1 ||
            !(m.last_tr[static_cast<std::size_t>(argument)] & 0x7FFF))
            return false;
        return slot_pick(Impl::ISrc::Tr, static_cast<s32>(argument), 1,
                         m.last_tr[static_cast<std::size_t>(argument)]);
    }
    if (a == "info_pk") {
        if (argument < 0 || argument > 3 || m.last_pk[static_cast<std::size_t>(argument)].first < 0)
            return false;
        const auto [kind, raw] = m.last_pk[static_cast<std::size_t>(argument)];
        return slot_pick(Impl::ISrc::Pk, static_cast<s32>(argument), kind, raw);
    }
    if (a == "isel_off") {
        m.isrc = Impl::ISrc::None;
        return true;
    }
    // ROOM selection: an explicit pedestal (holds until another pedestal becomes the nearest one
    // or the room changes)
    if (a == "info_ped") {
        if (argument < 0 || static_cast<std::size_t>(argument) >= m.last_peds.size())
            return false;
        const Pedestal& p = m.last_peds[static_cast<std::size_t>(argument)];
        m.sel_ped = true;
        m.sel_entity = p.entity;
        m.tap_near = m.near_entity;
        m.ui_page = PageRoom;
        return true;
    }
    return false;
}

} // namespace isaac_reader
