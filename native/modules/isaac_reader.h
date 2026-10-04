// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac: Afterbirth+ / Repentance (010021C000B6A000, update v524288): the
// game-memory reader. The game code is Repentance.nro, loaded by the launcher `main` through
// nn::ro; the NRO base comes from the launcher's lazily bound run_repentance GOT slot
// (main+0x10CC78 - 0x4C0840), validated by NRO0 / size / 20-byte module id / MOD0 and the
// global's GOT slot. FNV-1a 64 fingerprints (isaac_pins.inc) of every routine whose rule the
// reader reproduces guard each domain; a mismatch fails only that domain.
// Without DLC, isaac_afterbirth.inc validates and reads AfterbirthPlus.nro independently.
// Publishes the output names of research/isaac/CONTRACT.md (v1.2 + round 2: ITEMS selection
// isel.*, ROOM nearest pedestal room.* / ped.i.near, see research/isaac/r2/REPORT.md).
//
// Outputs (ints unless noted, "t" = text):
//   gate     dlc.available (host aoc: availability; -1 unknown, 0 absent/disabled, 1 present)
//            run.ready run.why(t) run.paused run.map_held nro.ok ui.page run.torn eid.avail (EID file loaded)
//            nro.bad(t: failed domains)
//   header   floor.stage floor.type floor.name(t) floor.curses floor.lost time.text(t) seed.text(t)
//   player   p.coins p.bombs p.keys p.gkey p.gbomb p.type
//            p.h.max p.h.red p.h.soul p.h.black p.h.eternal p.h.golden p.h.bone p.h.broken p.h.rotten
//   stats    st.speed st.tears st.damage st.range st.shot st.luck (t, "%.2f" = StatHUD::Render) st.ready
//   slots    act.N.id/.charge/.max/.battery/.key(t)/.bar_key(t)/.name(t) (N 0..3),
//            tr.N.id/.gold/.key(t)/.name(t),
//            pk.N.kind/.id/.key(t) (N 0..3), pk.0.name(t), pk.0.ident pk.0.effect
//            pk.btn_frame pk.btn_key(t) pk.btn_live pk.btn_action pk.btn_button pk.btn_type: the
//            controller glyph of the button that uses pocket slot 0 (round 9; see PocketButton)
//   map      map.key(t) map.rooms map.cur                                          (ui.page 0)
//   bag      bag.held bag.count bag.i.id bag.result bag.blind bag.result_key(t) bag.result_name(t)
//            (every page since round 8: the left card's BAG page; zero / empty while bag.held 0)
//   items    inv.count inv.i.id/.trinket/.key(t)/.n/.sel                         (ui.page 1)
//            isel.on/.src/.slot/.act/.tr/.pk/.kind/.id/.key(t)/.name(t)/.quote(t)/.quality/
//            .pools(t)/.eid(t)/.eid_on/.eid_paras/.eid_chars: the ITEMS-page selection (info_inv / info_act /
//            info_tr / info_pk / info_sel toggle it; isel_off clears it)
//   room     info.kind/.id/.key(t)/.name(t)/.quote(t)/.quality/.pools(t)/.eid(t)/.eid_on/.blind
//            .eid_paras/.eid_chars
//            ped.count ped.i.id/.key(t)/.price/.blind/.opt/.name(t)/.near/.sel
//            room.near room.near_dist room.sel room.auto                          (ui.page 2)
//            (info.* = the ROOM selection: a tapped pedestal, else the nearest one by EID's
//            rule, else the first one; kind -1 with no pedestal)
// Image keys are published with the "module:" prefix (src_bind ready).

#include "core/mods/dsmod_module_abi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace isaac_text {
class GameText;
}

namespace isaac_reader {

/// Launcher build B6E5BDB9DC12E1D1A25CBFDA17F4BE24B4754EF5 + 12 zero bytes only.
bool SupportsBuildHex(const char* hex);
bool SupportsBuildId(const std::uint8_t* build_id32);

class Reader {
public:
    Reader(const EdenDsmodHostApi& host, const char* config_json);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /// Game text (names/quotes) owned by the module; may be null or not Ready() yet.
    void SetText(const isaac_text::GameText* text);

    void Sample(const EdenDsmodHostApi& host);
    bool OnAction(const char* action, std::int64_t argument);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Pure rules reproduced from the game code (unit-tested in isaac_reader_test.cpp).
namespace detail {

/// Seeds::Seed2String (nro+0x4A102C) with the game's 32-letter alphabet (nro+0x8AE69C).
std::string Seed2String(std::uint32_t seed, const char* alphabet32);
/// Minimap::Render (nro+0x41E004..0x41E0F8): "%02d:%02d:%02d" of the run timer in frames;
/// challenge 22 counts down from 28800 (never below 0).
std::string TimeText(std::int32_t time_counter, std::int32_t challenge);
/// HUD::StatHUD::Render (nro+0x3AF634): snprintf(buf, 12, "%.2f", (double)value) - the game's
/// 12-byte buffer (`mov w1,#0xc` @0x3AF608) caps the text at 11 characters.
std::string StatText(float value);
/// Pause-screen stat tally (marks drawn = PauseScreenStats.anm2 "Idle" layer stat+1, frame =
/// this value; frame k shows k solid marks over the paper's 7 faded ones): PauseScreen::Show
/// (nro+0x43274C) stores (int)KAGE::Math::Clamp(0, 7, PlayerUtils::Get<Stat>Rating(field)) per stat
/// (fcvtzs = toward zero; NaN -> 0), PauseScreen::Update (0x432AD8..0x432B34) SetLayerFrame(1..6).
/// stat 0..5 = speed, tears, damage, range, shot speed, luck; `raw` = the player field the game
/// passes (speed P+0x194C, MaxFireDelay P+0x1834, damage P+0x1844, range P+0x1854 (not /40),
/// shot speed P+0x1838, luck P+0x1950). Ratings: speed 4.5v-2 (0x443690), tears
/// (30/(v+1))^0.75*2.1203909-2 (0x4436D0), damage v^0.56*2.231179-2 (0x443780), range
/// (v-230)/60+2 (0x443800), shot speed 5v-2.5 (0x443860), luck v+2.5 (0x4438A0).
int StatTally(int stat, float raw);
inline constexpr int StatTallyMax = 7;
/// Bag of Crafting preview (HUD::PlayerHUD::Update 0x3A2848..0x3A28C8): block = P+0x2690 u8[8]
/// components + P+0x2698 s32 result (two equal reads), valid = the result has an items.xml entry.
/// 0 = hidden (bag not full, no/invalid result, torn, special-seed curses), 1 = the item,
/// 2 = Curse of the Blind (curses bit 6): the game's question-mark art.
int BagPreview(const unsigned char (&block)[12], bool same_twice, unsigned curses, bool seedfx_empty,
               bool result_valid);
/// Bag of Crafting held (round 8): collectible 710 in any active slot 0..3 (P+0x1964 + s*0x1C), which
/// covers Tainted Cain's pocket bag (710 in pocket active slot 2, RenderPocketItems 0x3A6AE4 id 0x2C6).
inline constexpr std::int32_t BagOfCraftingId = 710;
bool BagHeld(const std::array<std::int32_t, 4>& active);
/// Level::GetName (nro+0x3E3CD0..0x3E3D38) suffix for the stage name: " XL" with Curse of the
/// Labyrinth, else " I" / " II" for stages 1..8 (odd / even); none in greed mode or challenge 44.
std::string FloorSuffix(int stage, std::uint32_t curses, int difficulty, int challenge);

/// One room of the drawn map (CONTRACT "isaac:map/<hex>": 6 bytes per room).
struct MapRoom {
    std::uint8_t grid;
    std::uint8_t shape;
    std::uint8_t type;
    std::uint8_t dflags;   // DisplayFlags bits 0..2; bit 7 = VisitedCount > 0 (icon pass rule)
    std::uint8_t flags;    // MapFlags()
    std::uint8_t current;
};
/// The map hex flags byte from RoomDescriptor Flags (D+0x50) and RoomConfig::Room type/subtype
/// (Data+0x8 / +0x10), as Minimap::Config::render reads them: bit0 CLEAR ("RoomVisited",
/// 0x41B4C8), bit1 RED_ROOM 0x400 (tint, 0x41B3C4), bit2 Flags 0x800 (type 4 icon
/// "IconTreasureRoomRed": ldrb [D+0x51], tst #8 @0x41C32C), bit3 type 11 with subtype 1 (icon
/// "IconBossAmbushRoom": ldr [Data+0x10], cmp #1 @0x41C394; jump table nro+0x8BFBCC).
std::uint8_t MapFlags(std::uint32_t flags, std::int32_t type, std::int32_t subtype);
/// Header byte (bit0 Curse of the Lost) + 6 bytes per room, lowercase hex. Lost -> header only.
std::string MapHex(const std::vector<MapRoom>& rooms, bool lost);

/// One room pedestal as the nearest-description rule sees it (Entity fields).
struct NearCand {
    float x, y;          // Entity+0x310 Position
    float size;          // Entity+0x344 Size
    std::uint64_t flags; // Entity+0x1B8 (bit 46 = skipped by EntityList::QueryRadius)
    std::int32_t spawn;  // Entity+0x2F4 (Entity::GetFrameCount = G+0x24F99C - this)
};
/// External Item Descriptions' pick (main.lua 1447-1471, MaxDistance 5 * 40): candidates are
/// Isaac.FindInRadius(player.Position, 200, ...) = EntityList::QueryRadius (nro+0x78D28):
/// flag bit 46 clear and DistanceSquared(pos, e.Position) < (e.Size + radius)^2; EID then keeps
/// entities with FrameCount > 0 (Entity::GetFrameCount nro+0x5A210) and takes the first one whose
/// distance is strictly below the best so far (start 10000). Returns the index or -1; *dist =
/// the chosen distance.
int NearestPedestal(float px, float py, const std::vector<NearCand>& c, std::int32_t frame,
                    float radius, float* dist);

/// Entity_Player::GetMaxPocketItems (nro+0x2B0970): 2 with Starter Deck / Little Baggy /
/// Polydactyly (slot_item), else 1; +1 per pocket slot holding a pocket active (kind 2, id > 0).
std::int32_t MaxPocketItems(bool slot_item, const std::int32_t kind[4], const std::int32_t id[4]);

/// Round 9: the controller glyph of the PILLCARD button (the one that uses pocket slot 0).
/// IsaacRepentance::Manager::GetButtonFrame(unsigned) (nro+0x3FB20C, jump tables 0x8BF826 /
/// 0x8BF81E): KAGE button id -> gfx/backdrop/controls_buttons.anm2 frame; -1 = no glyph.
/// D-pad L/R/U/D 0..3 -> 7/6/5/4, A 4 -> 2, B 5 -> 1, X 6 -> 0, Y 7 -> 3, L 8 -> 10, ZL 9 -> 12,
/// LStick 10 -> 8, R 11 -> 11, ZR 12 -> 13, RStick 13 -> 9, + 14 -> 14, - 15 -> 15, SL 16 -> 25,
/// SR 17 -> 26; stick directions 0xABAB0000..7 -> 19 / 18 / 17 / 16 / 23 / 22 / 21 / 20.
int ButtonFrame(std::uint32_t button);
/// Manager::RenderButtonIcon(int dev, int action, int idx, Vector2, bool) (nro+0x3FB3B8): the
/// animation by Input::Manager::GetDeviceType (DeviceNX+0x488): table nro+0xA387F0 [0]
/// "Switch_JoyCon", [1] "Switch_JoyCon", [2] "Switch", [3] "Switch_Pro", anything else "Switch"
/// (0x3FB414); device type 1 remaps frames 8 / 16..19 through nro+0x8BF854 (9, 20..23)
/// (0x3FB460..0x3FB4D0).
const char* ButtonAnim(int device_type);
int ButtonFrameForType(int frame, int device_type);
/// Entity_Player::control_pocket_item (nro+0x2CAB38): the action it tests is PILLCARD (10)
/// (0x2CABD4) except for Jacob / Esau (types 19 / 20, 0x2CAB98) with a twin (P+0x2580): the twin
/// with the lower P+0x19F0 of the pair (self on a tie) uses ITEM (9), the other PILLCARD (10)
/// (0x2CABAC..0x2CABC4).
int PocketAction(std::int32_t player_type, bool has_twin, std::int32_t self_19f0,
                 std::int32_t twin_19f0);

/// One AArch64 instruction the pill-remap decoder understands.
struct Insn {
    enum Kind { Other, MovzW, B } kind{Other};
    int rd{};
    std::uint32_t imm{};
    std::int64_t target{}; // B: absolute offset
};
Insn Decode(std::uint32_t word, std::uint64_t at);

} // namespace detail

} // namespace isaac_reader
