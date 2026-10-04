// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Animal Crossing: New Horizons 3.0.3 live reader (live lane). Resolves the game's roots from its
// own code (pinned instruction words in acnh_live_pins.inc, globals decoded from ADRP+ADD/LDR) and
// samples them into a LiveSnapshot. Every domain fails closed on its own; research notes:
// research/acnh/impl/live/ROOTS.md.
//
// Roots (main-relative code offsets of the 3.0.3 main, build ff1d1c05670db602...):
//   save        SaveDataMgr singleton (accessor 0x6e19d8): mgr+0x10 = save set; set[0] (or, with
//               mgr+0x453 bit 0, set+0x130) = GSaveMain object; its vtable slot 2 (0x260bd28,
//               "ldr x0,[x0,#0x10]") is the decrypted main.dat the game reads and writes; slot 7
//               (0x260bd50) the field-handle tree whose handles point at the same bytes.
//   personal    GetPersonal (0x25f9208): mgr+0x90+i*8 {player id, valid} -> set+0x60+i*8 -> +0x10
//   player no   0x679ad4: override byte / network session / local byte (offline: 0)
//   clock       TimeMgr (0xef9754): CalendarTime (u16 y, u8 m, d, h, min, s) at +0x4060
//   language    0x280720c: [[g]] u32 = index into the game's folder table (0x1adf0b0: "USen", ...)
//   position    0x241b3c0: per-player record, world x,y,z at +0xc
//   phone apps  app table 0x2e59d30 (main+0x55c6818, 18 x 0x38), display order list 0x2e5d6dc
//               (main+0x55c67d0), install rule 0x2e5a0e0, Type frame table 0x2e5b21c
//               (main+0x408148c); home seq = [[0x2b0c0a0 global]+0x90] (vtable 0x2e5a5c0): count
//               +0x228, list +0x230, cursor +0x308, page +0x30c, pages +0x2b4 (0x2e5efb0)
//   scene       NookPhone sequence state machine (0x2b74c10), the player's action state machine
//               (0x2287068) and the camera's CamState machine (0x80499c / 0x7aa4e8); state names
//               come from the machines' own name tables (0x3dbb0d8).
// The save layouts (offsets below) are the game's Smmh 655400_655362 schema (romfs System/Smmh),
// research/acnh/schema/SCHEMA.md.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

namespace acnh::live {

// ---- save layout (GSaveMain = main.dat, GSavePersonal = personal.dat; 3.0.x schema) ----------
namespace M {
inline constexpr uint32_t Size = 0x9B0E90;
inline constexpr uint32_t Villager = 0x120, VillagerStride = 0x13230, VillagerCount = 10;
inline constexpr uint32_t VillagerBirthDate = 0x12674; // Animal.BirthDate GSaveDate {u16 y,u8 m,d}
inline constexpr uint32_t IslandName = 0x1E393C;       // Land.LandId.Name char16[10]
inline constexpr uint32_t Weather = 0x1E3700;          // Land.Weather
inline constexpr uint32_t WeatherToday = Weather + 0xC, WeatherArea = Weather + 0x14;
inline constexpr uint32_t LandMaking = 0x30AB00, LandMakingSize = 112u * 96u * 0xEu;
inline constexpr uint32_t Structures = 0x32F700, StructureSize = 47u * 0x14u;
inline constexpr uint32_t FieldBlocks = 0x32FAAC, FieldBlockSize = 0x90;
inline constexpr uint32_t ShopKabu = 0x490770; // KaburibaKabuka, ShopKabuka[14], Pattern
inline constexpr uint32_t Museum = 0x491220;   // MuseumLevel, Date[1024], Item[1024]
inline constexpr uint32_t MuseumItems = 0x492224, MuseumCount = 1024;
inline constexpr uint32_t VisitorNpc = 0x494624; // s32[7]
} // namespace M
namespace P {
inline constexpr uint32_t Size = 0x74A40;
inline constexpr uint32_t PlayerId = 0xC248;    // LandId (+4 name), BaseId (+0x20 name)
inline constexpr uint32_t EventFlag = 0xC280;   // Player.EventFlag.Flags.ValueArray u16[2048]
inline constexpr uint32_t LifeSupport = 0xD280; // FlagsDaily u16[512] ...
inline constexpr uint32_t EntryDaily = 0xD680, RewardDaily = 0xDA80, BonusDaily = 0xDEC0;
inline constexpr uint32_t DailyOrder = 0xE0C4; // u16[8] (unresolved name, LEAD)
inline constexpr uint32_t MilesNow = 0x12718, MilesTotal = 0x12720;
inline constexpr uint32_t JpegSize = 0x13550, Jpeg = 0x13554, JpegMax = 0x23000;
inline constexpr uint32_t ProfileBirthday = 0x36598, ProfileFruit = 0x3659C, ProfileStamp = 0x3660C;
inline constexpr uint32_t ItemBag = 0x37BF0, ItemPocket = 0x37CA8; // Item[20] + u32 count each
inline constexpr uint32_t BagFav = 0x37C94, PocketFav = 0x37D4C;   // s8[20] `_7c3cae33` each
inline constexpr uint32_t Wallet = 0x37D60, ExpandBaggage = 0x37D68;
inline constexpr uint32_t Chest = 0x37D6C, ChestCount = 9000;
inline constexpr uint32_t Savings = 0x6527C;
inline constexpr uint32_t RecipeCollect = 0x6528C, RecipeMade = 0x6538C, RecipeNew = 0x6548C,
                          RecipeFav = 0x6558C;
inline constexpr uint32_t Fish = 0x67BA0, Insect = 0x67C76,
                          Dive = 0x67D4C; // u16 ids + flags + count
} // namespace P

struct Item {
    uint16_t id = 0xFFFE;
    uint8_t sys = 0, add = 0;
    uint32_t free = 0;
    int8_t fav = -1; ///< favourite order (s8 per slot after each holder; -1 = none; LIVE-checked)
    bool Empty() const {
        return id == 0xFFFE;
    }
};

struct StateName {
    int id = -1;
    std::string name; ///< the machine's own state name ("cNormalField"), empty when unknown
};

struct LiveSnapshot {
    uint64_t serial = 0;
    // ---- readiness (each domain fails closed on its own) ----
    bool code_ok = false; ///< every pin matched
    bool save_ok = false; ///< main.dat located and sane
    bool personal_ok = false;
    bool clock_ok = false, lang_ok = false, pos_ok = false;
    bool phone_ok = false, player_ok = false, camera_ok = false;
    bool order_ok =
        false;        ///< the game's list orders (Map residents, Critterpedia) are the pinned code
    std::string diag; ///< why something is not ready (short, ';'-separated)
    // ---- diagnostics (acnh.diag.*: the hidden overlay and the log line, acnh_publish_diag.cpp) ----
    std::string dead_pins;     ///< the pins whose words did not match (',' separated)
    uint32_t dead_domains = 0; ///< live::Domain bits closed by those pins
    uint64_t main_base = 0, main_size = 0, heap_lo = 0, heap_hi = 0;
    uint64_t main_dat = 0; ///< main.dat's bytes (a heap object) when save_ok
    int64_t reloc = 0;     ///< the host's "__relocation_delta"

    // ---- clock (TimeMgr CalendarTime = what the HUD shows) ----
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, wday = 0; ///< wday 0 = Sunday

    // ---- language ----
    std::string lang_folder; ///< "USen" ... read from the game's table

    // ---- island / players ----
    int player_no = -1, personal_slot = -1;
    std::u16string island_name, player_name;
    int hemi = -1; ///< Land.Weather.WeatherArea (0 north, 1 south: LEAD)
    struct Resident {
        bool present = false;
        uint8_t species = 0, variant = 0, personality = 0;
        bool born_ok = false; ///< BirthDate read (the Map app's resident sort key)
        uint16_t born_y = 0;
        uint8_t born_m = 0, born_d = 0;
    };
    std::array<Resident, M::VillagerCount> residents{};
    std::vector<uint8_t> structures; ///< StructureList (47 x 0x14) for house positions / icons

    // ---- position ----
    float px = 0, py = 0, pz = 0;
    /// The game's current scene id (u16 at the PlayerPos pin's scene global; the position record
    /// carries the same id), -1 unread.
    int scene_id = -1;
    std::string stage_name; ///< current stage SafeString, read directly; empty when unavailable

    // ---- pockets (display order: ItemPocket then ItemBag), money ----
    std::array<Item, 40> pockets{};
    int pocket_slots = 0; ///< 20 / 30 / 40 (ExpandBaggage)
    int bag_cursor = -1;  ///< the game's pocket menu cursor slot while it is open, else -1
    int64_t wallet = -1, miles = -1, bank = -1;
    std::vector<Item> chest; ///< storage (ItemChest, read only while the DIY page needs it)
    uint64_t chest_hash = 0;
    uint8_t birth_m = 0, birth_d = 0;
    uint16_t fruit = 0xFFFE;
    uint16_t reg_y = 0;
    uint8_t reg_m = 0, reg_d = 0;

    // ---- critterpedia (catch order item ids) / museum ----
    std::array<std::vector<uint16_t>, 3> caught; ///< 0 insects, 1 fish, 2 sea creatures
    std::vector<uint16_t> donated;               ///< museum donation item ids

    // ---- recipes (bit index = RecipeCraftParam.UniqueID) ----
    std::array<uint8_t, 0x100> recipe_collect{}, recipe_made{}, recipe_new{}, recipe_fav{};

    // ---- Nook Miles+ ----
    std::vector<uint16_t> nmp_order;       ///< the 5 entries (u16 order table, LEAD)
    std::array<uint16_t, 512> nmp_flags{}; ///< FlagsDaily
    std::array<uint8_t, 512> nmp_reward{}, nmp_bonus{};

    // ---- shop / weather / visitors ----
    uint32_t kabu_sun = 0;
    std::array<uint32_t, 14> kabu{};
    int32_t weather_today = -1;
    /// Today's visitor as the game decides it (TodayVisitor 0x27d6b0c): 0 none, 1..11 (labels
    /// from the game's table), -1 unknown. visitor_wday = the weekday index it used.
    bool visitor_ok = false;
    int visitor_today = -1, visitor_wday = -1;
    std::string visitor_label;

    // ---- island bytes for the map lane ----
    uint32_t island_rev = 0; ///< bumps when the terrain / structure / acre bytes change
    std::vector<uint8_t> land_making,
        field_blocks; ///< of revision island_rev (kept between samples)

    // ---- NookPhone apps (code tables, read from the running game) ----
    struct AppDef {
        int id = -1;
        std::string label; ///< LayoutMsg/MenuDevice label ("0006")
        std::string flag;  ///< EventFlagsPlayerParam Key that unlocks it ("" = always)
        int frame = 0;     ///< LMenuDeviceBtn_Type frame
    };
    std::vector<AppDef> app_defs;             ///< in the home grid's order (all 18)
    std::array<uint16_t, 2048> event_flags{}; ///< Player.EventFlag ValueArray (need.phone)
    bool flags_ok = false;
    struct PhoneUi {
        bool ok = false; ///< the home seq exists (phone home grid built)
        int count = 0, cursor = -1, page = -1, pages = 0;
        std::vector<int> ids; ///< the game's own installed list (phone order)
    } phone_ui;

    // ---- guest addresses for the Bag page's direct writes (acnh_bag.h; 0 = not resolved) ----
    uint64_t personal_at = 0; ///< personal.dat bytes of the local player (GetPersonal)
    uint64_t actor_at = 0;    ///< the PlayerActor (ActorList / PlayerActorName)

    // ---- text ----
    /// The game's capitalisation table (CaseTable pin): u16 {lower, upper} x 188, null if unread.
    std::shared_ptr<const std::vector<uint16_t>> case_table;

    // ---- scene ----
    StateName phone, player, camera;
    bool scene_ok = false;
    bool free = false, phone_open = false, bag_open = false, loading = false;
};

/// Which parts a sample needs (lazy publishing).
struct Need {
    bool pockets = true, money = true, critters = false, recipes = false, today = false,
         island = true, residents = true, scene = true, profile = false, chest = false,
         phone = false;
};

/// Guest memory over the host API (bounded, fail-closed reads).
class Guest {
public:
    explicit Guest(const EdenDsmodHostApi& h) : host{h} {}
    bool Read(uint64_t at, void* out, size_t n) const;
    template <class T>
    bool Get(uint64_t at, T& v) const {
        return Read(at, &v, sizeof v);
    }
    bool InMain(uint64_t at, uint64_t n) const {
        return host.main_base && at >= host.main_base && host.main_size >= n &&
               at - host.main_base <= host.main_size - n;
    }
    uint64_t Base() const {
        return host.main_base;
    }
    /// The host's data-vs-text relocation delta (NCE; 0 under Dynarmic and on older runtimes).
    int64_t DataDelta() const {
        return host.get_i64 ? host.get_i64(host.userdata, "__relocation_delta", 0) : 0;
    }
    /// The guest heap as the running process laid it out (page table; NCE puts it elsewhere than
    /// Dynarmic). Diagnostics only: object pointers are checked by IsHeapPtr (39-bit, aligned),
    /// which holds under both backends.
    uint64_t HeapBegin() const {
        return host.get_heap_begin ? host.get_heap_begin(host.userdata) : 0;
    }
    uint64_t HeapEnd() const {
        return host.get_heap_end ? host.get_heap_end(host.userdata) : 0;
    }
    const EdenDsmodHostApi& host;
};

class Reader {
public:
    /// Re-resolves when the main image changes. False = nothing usable (diag says why).
    bool Resolve(const Guest& g, std::string& diag);
    /// One sample. Fills `out` (keeps island bytes between calls through `out.island_rev`).
    void Sample(const Guest& g, const Need& need, LiveSnapshot& out);
    /// The passport JPEG of the local player (timing thread, LiveSample; the asset worker only
    /// gets the copy, LivePassportJpeg).
    bool PassportJpeg(const Guest& g, std::vector<uint8_t>& jpeg);

    // Decoded roots (absolute guest addresses).
    struct Roots {
        uint64_t save_mgr = 0;   ///< address of the SaveDataMgr pointer
        uint64_t time_mgr = 0;   ///< address of the TimeMgr pointer
        uint64_t lang = 0;       ///< address of the language object pointer
        uint64_t lang_table = 0; ///< GOT slot of the folder table
        uint64_t pno_override = 0, pno_session = 0, pno_local = 0;
        uint64_t pos_table = 0, pos_fallback = 0, pos_scene = 0, stage_name = 0;
        uint64_t device_mgr = 0; ///< address of the NookPhone device manager pointer
        uint64_t actor_list = 0; ///< address of the actor list manager pointer
        uint64_t save_vt_slot2 = 0, save_vt_slot7 = 0;
        uint64_t player_actor_name = 0;
        uint64_t camera_slot = 0; ///< code of the camera component accessor (vtable identity)
        uint64_t app_table = 0, app_order = 0, app_frames = 0; ///< NookPhone app tables
        uint64_t phone_root = 0;   ///< address of the UI root pointer (home seq at +0x90)
        uint64_t phone_seq_vt = 0; ///< vtable of the home seq
        uint64_t visitor_labels =
            0;                    ///< GOT slot of the visitor label table (labels at +0x38 + v*8)
        uint64_t visitor_gst = 0; ///< "gstA" (the label the game uses for visitor 4)
        uint64_t pocket_vt = 0;   ///< vtable of the pocket menu ([UI root]+0x88)
        uint64_t case_table = 0;  ///< the text writer's {lower, upper}[188] capitalisation table
    } roots;

private:
    bool CheckPins(const Guest& g, std::string& diag);
    bool SaveData(const Guest& g, uint64_t& main_dat, std::string& diag) const;
    bool PersonalData(const Guest& g, int player_no, uint64_t& personal, int& slot) const;
    int LocalPlayerNo(const Guest& g) const;
    bool ReadClock(const Guest& g, LiveSnapshot& out) const;
    bool ReadLanguage(const Guest& g, LiveSnapshot& out) const;
    bool ReadPosition(const Guest& g, int player_no, LiveSnapshot& out) const;
    void ReadScene(const Guest& g, LiveSnapshot& out);
    bool StateOf(const Guest& g, uint64_t sm, StateName& out) const;
    uint64_t FindPlayerActor(const Guest& g, std::string& why) const;
    uint64_t FindCamera(const Guest& g, uint64_t actor) const;
    void ReadIsland(const Guest& g, uint64_t main_dat, LiveSnapshot& out);
    void ReadPhone(const Guest& g, LiveSnapshot& out);
    void ReadVisitor(const Guest& g, LiveSnapshot& out) const;
    void ReadPocketCursor(const Guest& g, LiveSnapshot& out) const;
    std::vector<LiveSnapshot::AppDef> app_defs; ///< cached once valid
    bool app_defs_sent = false;                 ///< app_defs copied into the snapshot

    bool resolved = false;
    uint32_t dead_domains = 0; ///< domains whose pins did not match
    std::string dead_pins;     ///< their names (diagnostics)
    uint64_t main_base = 0, main_size = 0;
    std::shared_ptr<const std::vector<uint16_t>> case_table; ///< read once per resolve
    mutable uint64_t save_tree = 0; ///< the save object's field-handle tree (SaveData)
    uint64_t chest_last_ms = 0, chest_source = 0;
    std::array<uint8_t, M::MuseumCount * 8> museum_buf{};
    std::vector<uint8_t> chest_buf; ///< kept across the storage refreshes

    // island change detection: the bytes of revision island_rev (land / blocks in the snapshot)
    std::vector<uint8_t> island_structures;
    std::array<std::vector<uint8_t>, 4> island_buf; ///< read buffers, kept between reads
    uint32_t island_rev = 0;
    uint64_t island_last_ms = 0, island_source = 0;
};

/// EncryptedInt32 (u32 enc, u16 adjust, u8 shift, u8 check) -> value; false when the check byte
/// does not match (NHSE LEAD formula, checksum validated on real saves).
bool DecodeEncryptedInt(const uint8_t* p, int64_t& value);
/// Day of week (0 = Sunday) of a Gregorian date.
int Weekday(int y, int m, int d);
/// Monotonic milliseconds.
uint64_t NowMs();

// ---- scenes (acnh_live_scene.cpp, r9 indoor) ----
/// Island structure associated with a current-stage name; zero outside island buildings.
int SceneStructure(std::string_view name);

} // namespace acnh::live
