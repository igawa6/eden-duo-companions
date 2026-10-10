// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Read-only field party reader for Dragon Quest III HD-2D Remake, Switch 1.1.0.0
// (build 4F41309B39EEBE5E). Research contracts: FIELD-PARTY.md, FIELD-PARTY-LOWLEVEL.md,
// FIELD-STATUS.md, FIELD-STATUS-CODE.md, CHEAT-LEADS.md (research/dragon-quest-iii-hd-2d-remake).
//
// Routes (main-relative code offsets; every word below is checked before anything is read):
//   0x87437c  GD accessor:  adrp x8,G@page; add x8,x8,G@off; ldr x8,[x8]; ldr x8,[x8,#0x528];
//             add x0,x8,#0x18; ret          -> G slot (decoded), P = [[[slot]] + 0x528] + 0x18]
//   0x8bc1c8  HealPartyMember: ldr x0,[x8,#0x220]; ldr x21,[x8,#0x228]  -> PM = [P + 0x220]
//   0x8b10e0  member list:  ldr x27,[x22,#0x10]!; ldrsw x8,[x22,#8]   -> roster {PM+0x10, PM+0x18}
//             16-byte TSharedPtr entries {C, controller}
//   0x8b11b4  ldrb w9,[x8,#0xc0]; sub w9,w9,#1; lsr w9,w21,w9; tbz w9,#0  -> membership C+0xC0
//             (1 = active party, flag bit 0); 0x8b11d4 ldr w25,[x8,#0xc4] -> party slot C+0xC4
//   0x8d08d0  status builder: ldr x8,[x21,#0x58]; cbz; ldr w8,[x8,#0x8c]; tbnz w8,#0 (dead);
//             and w9,w8,#4 (paralysis); tbz w8,#1 / orr #2 (poison)  -> F = [C+0x58], F+0x8C
//   0x89d30c / 0x89d5ac  GetMaxHP / GetMaxMP simple path: clamp(F+0x28 + F+0x68, 0, 999) and
//             clamp(F+0x30 + F+0x6C, 0, 999)
//   0x7240fc  ldr w0,[x8,#0x1c]!       -> level F+0x1C
//   0x716624  ldr w8,[x8,#0x2c]        -> HP F+0x2C (battle write-back)
//   0x7165fc  ldr w8,[x8,#0x34]        -> MP F+0x34
//   0x112d84c FName::ToString: adrp x23,Pool@page; add x23,x23,Pool@off; ... add x8,x23,w8,uxtw #3;
//             ldr x8,[x8,#0x28]  -> FNamePool (decoded), Blocks[] at Pool+0x28
//   0x112b938 entry = Blocks[index >> 16] + (index & 0xffff) * 2; header u16: bit 0 = UTF-16,
//             length = header >> 6 (0x112b970)
// The name FString at C+0x28 {char16_t* data, s32 num incl. NUL, s32 max} is verified live
// (native menus, two boots) but has no code pin yet.
//
// Scene gate (UE4 reflection layout, class/object names verified live, contracts/LOCATION.md):
// the title screen already holds the autosave's roster, so a party is shown only while the
// viewport world [[[G+0x20 Outer = GameEngine]+0x7A0 GameViewport]+0x70 World] is named
// "FieldTop" (the persistent gameplay world; "title" on the title screen) AND the local player
// controller [[[G+0x38 LocalPlayers][0]]+0x30] possesses a pawn (+0x2A0 AcknowledgedPawn: town /
// field walker, ship, Ramia or battle pawn; null on the title and while a save loads).
// Map transitions (contracts/LOCATION.md): the world stays the same FieldTop object; the world
// subsystem LevelLoadingManager (TMap at W+0x6E8 {data, num}, 0x18-byte entries {UClass*, object,
// hash}) holds a load-state byte at +0x1B1, set to 1 by the map-change start (main+0x91d1b0 /
// 0x91d5ec) and cleared when it completes (0x91d5d4, 0x91ee04); the game tests it at 0x91df80.
// The three setters at 0x91d5d4 (clear / 1 / 2, each through the LLM getter 0x9181b4) are pinned.
// Live: 1 from the MapID change until the arrival map is in (town/field, Zoom, battle in and out,
// save load), 2 briefly after a save load; the party is held while it is non-zero or while the
// pawn is absent in the same FieldTop world.
//
// Active party = roster entries with C+0xC0 == 1 ordered by C+0xC4 (verified by a Line-Up swap);
// at most 4 with distinct slots 0..3, else the snapshot is rejected. Displayed max HP/MP here is
// the base-plus-bonus path. Native equipment HP effects resolve the item definition's FName
// to a GOP_Item row (row +0xC9 == 7, +0xCA == ADD, +0xCC value); this is not an item offset.
// All 301 rows in the pinned 1.1.0.0 archive have no HP/MP effects, so that sum is zero here.
// A KO'd member stays in the party (F+0x8C bit 0); the game's own UI code reports dead alone and
// poison/paralysis only for a living member, and so does this reader.
//
// Looks / job / HP state (slice 1 polish, dq3_sprites.h):
//   0x8d0398  GetLooksId: ldrb w8,[x0,#0xaa] (vocation); class FString table = ADRP/ADD 0x8d03a8 +
//             0xe08 (ldr x9,[x9,#0xe08], num +0xe10), index vocation - 1; ldr w2,[x20,#0xb8] (looks);
//             coffin when w1 bit 0 and F+0x8C bit 0
//   0x8d03fc  equipped armour: B = [C+0x78]; map elements [B+0x38] (32 bytes, key byte +0, next +0x18),
//             key 4 = armour; element +0x08 -> slot, [slot+0x08] -> item def, FName at def+0x08
//             (contracts/INVENTORY.md; the reader walks the allocation bits instead of the hash)
//   0xce252c  HP state: HP < 1 -> 2; GetMaxHP < 1 -> 0; r = (float)HP / (float)max; r <= 0.3f -> 3;
//             r <= 0.5f -> 4; else 0 (dead -> 2 before that, 0x8a23b0)
//   0x84a858  Erdrick title: bit 0x8b of the bitset at P+0x128+0xC8 (0x877570: index <= 0xac,
//             qword [x + 0xc8 + 8 * (i >> 6)] >> (i & 63))
//   0xce2740  GetJobTextId: jump table (main+0x4909fba) by vocation 1..10; the target strings are
//             read back from the guest at resolve time and must equal dq3::JobTextId's ids.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dq3 {

using s32 = std::int32_t;

using GuestRead = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

namespace detail {
/// One guest value; a null address fails.
template <class T>
inline bool Get(const GuestRead& read, std::uint64_t at, T& out) {
    return at != 0 && read(at, &out, sizeof(T));
}
} // namespace detail

struct CodePin {
    std::uint64_t offset; ///< main-relative
    std::vector<std::uint32_t> words;
    std::vector<std::uint32_t> masks{}; ///< empty = exact match
};
const std::vector<CodePin>& Pins();

/// Checks every pin against the main image: "<label> outside main", "<label> read" or
/// "<label> main+0x<offset> word <i> = <word>" on failure, empty when all match. `on_match` sees
/// each matching pin with the words read (to decode ADRP / ADD immediates).
std::string CheckPins(const GuestRead& read, std::uint64_t main_base, std::uint64_t main_size,
                      std::span<const CodePin> pins, std::string_view label,
                      const std::function<void(const CodePin&, std::span<const std::uint32_t>)>& on_match = {});
/// The page an ADRP at `pc` addresses (imm = immhi:immlo, 21 bits signed, in 4 KiB pages).
std::uint64_t AdrpPage(std::uint64_t pc, std::uint32_t adrp);

struct Roots {
    std::uint64_t g_slot{};      ///< &G (main+0x56262e8 on 1.1.0)
    std::uint64_t name_pool{};   ///< FNamePool (main+0x5b04940 on 1.1.0)
    std::uint64_t class_table{}; ///< FString[] of class names (main+0x5626248 on 1.1.0)
    bool job_ids_ok{};           ///< the job-name code's strings equal dq3::JobTextId's
};

/// Checks every pin and decodes the G slot and the FName pool from the accessors' ADRP + ADD.
/// Empty string on success, else the reason.
std::string Resolve(const GuestRead& read, std::uint64_t main_base, std::uint64_t main_size,
                    Roots& roots);

/// FName text (ASCII or UTF-16 entries, "_<number - 1>" suffix); nullopt when unreadable.
/// Successful reads are cached by (pool, index, number): UE's FNamePool is append-only, an entry
/// never changes once published (failures are never cached).
std::optional<std::string> ReadFName(const GuestRead& read, std::uint64_t name_pool,
                                     std::uint32_t index, std::uint32_t number);
/// Drops the ReadFName cache (a new pool after a failed resolve; tests that reuse pool addresses).
void ClearFNameCache();

/// GD = [[G slot] + 0x528] and P = [GD + 0x18] (dq3_layout.h); false unless both are non-null.
/// `g` (optional) receives the game instance.
bool ReadGdP(const GuestRead& read, const Roots& roots, std::uint64_t& gd, std::uint64_t& p,
             std::uint64_t* g = nullptr);

struct SceneState {
    std::string world;          ///< viewport world name ("FieldTop", "title", ...)
    std::uint64_t world_ptr{};  ///< the viewport UWorld (FieldTop persists across map changes)
    bool pawn{};                ///< the local player controller possesses a pawn
    bool llm_found{};           ///< the world's LevelLoadingManager subsystem was found
    std::uint8_t loading{};     ///< LevelLoadingManager+0x1B1 (0 idle; set while a map loads)
    std::uint64_t pawn_ptr{};   ///< the possessed pawn (0 = none)
    std::uint64_t llm{};        ///< the LevelLoadingManager (0 = not found)
    bool InAdventure() const {
        return world == "FieldTop" && pawn;
    }
    bool Transition() const {
        return world == "FieldTop" && (!pawn || loading != 0);
    }
};
/// Per-world cache of the LevelLoadingManager lookup (the world's subsystem map, by class name).
struct SceneCache {
    std::uint64_t world_ptr{};
    std::uint64_t llm{};
    std::string world_name;
    std::uint32_t world_fname[2]{};
};
/// false when the engine objects are unreadable (treated as "no adventure"). `cache` keeps the
/// world name and the LevelLoadingManager of the current world object between calls.
bool ReadScene(const GuestRead& read, const Roots& roots, SceneState& out,
               SceneCache* cache = nullptr);

struct PartyMember {
    std::u32string name;
    s32 level{};
    s32 hp{}, hp_max{}, mp{}, mp_max{};
    /// GetMaxHP / GetMaxMP inputs: base F+0x28 / +0x30, bonus F+0x68 / +0x6C. hp_max / mp_max above are
    /// DisplayedMax(base, bonus); the module adds the equipment term (dq3_info.h EquipMaxParam) when the
    /// game data has MAXHP / MAXMP items.
    s32 hp_base{}, hp_bonus{}, mp_base{}, mp_bonus{};
    std::uint32_t status{}; ///< raw F+0x8C
    std::uint32_t slot{};   ///< C+0xC4
    std::uint8_t vocation{}; ///< C+0xAA (1 Hero .. 10 Monster Wrangler)
    std::uint32_t looks{};   ///< C+0xB8
    bool armour_read{};      ///< the equipment map was readable
    std::uint32_t armour[2]{}; ///< equipped armour FName {index, number}; {0, 0} = none
    std::uint64_t field{};     ///< F = [C+0x58] (links the battle unit's [+0x78])
    bool Dead() const {
        return (status & 1) != 0;
    }
    bool Poisoned() const {
        return !Dead() && (status & 2) != 0;
    }
    bool Paralysed() const {
        return !Dead() && (status & 4) != 0;
    }
    bool operator==(const PartyMember&) const = default;
};

struct PartySnapshot {
    std::vector<PartyMember> members; ///< Line-Up order
    s32 roster_count{};
    bool roto{}; ///< Erdrick title flag (hero job name "Erdrick")
    std::uint32_t flag9c_entries{}; ///< roster entries with C+0x9C != 0 (never seen; diagnostic)
    bool operator==(const PartySnapshot&) const = default;
};

enum class PartyState {
    Ok,      ///< members valid (possibly empty)
    NoGame,  ///< no progress record / party manager / roster yet (title, boot)
    Invalid, ///< structure failed a check: publish nothing
};

/// One pass over the structures. `why` explains NoGame / Invalid.
PartyState ReadPartyOnce(const GuestRead& read, std::uint64_t g_slot, PartySnapshot& out,
                         std::string* why = nullptr);

/// Two passes that must agree (torn-read rejection); a disagreement reports Invalid "torn".
PartyState ReadParty(const GuestRead& read, std::uint64_t g_slot, PartySnapshot& out,
                     std::string* why = nullptr);

/// GetMaxHP/MP simple path: clamp(base + bonus, 0, 999) in the game's order (min 999, then max 0).
s32 DisplayedMax(s32 base, s32 bonus);
/// a + b saturated to the s32 range (base + equipment term of guest-read values, before DisplayedMax).
s32 AddClamped(s32 a, s32 b);

/// The menus' HP state (main+0xce24b0): 0 normal, 2 dead, 3 HP <= 30 %, 4 HP <= 50 %.
int HpState(const PartyMember& m);
/// The same rule for a current / max HP pair (HP < 1 -> 2; no dead-flag check).
int HpState(s32 hp, s32 max);

/// Class name of a vocation from the guest's FString table; nullopt when unreadable/out of range.
std::optional<std::string> ReadClassName(const GuestRead& read, const Roots& roots,
                                         std::uint8_t vocation);

/// GetLooksId's row for `m` (dead -> coffin row; costume -> "_MIZUGI" / NUIGURUMI row). `armour`
/// is the equipped armour's FName text ("" = none).
std::string LooksId(const PartyMember& m, std::string_view class_name, std::string_view armour);
/// The four armours whose GOP_Item spec is ItemEffectType::COSUTUME_CHANGE (GOP_Item_Spec data).
bool IsCostumeArmour(std::string_view armour);

} // namespace dq3
