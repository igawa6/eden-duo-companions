// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_abi.h"
#include "re4_map_state_tables.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>
#include <utility>

namespace re4 {
constexpr uint64_t Title = 0x010099A00BC1E000;
constexpr std::string_view Build = "82C2A04A21D3E0B850F273D4149C9003";
inline bool SupportsBuild(const char* s) {
    if (!s) return false;
    const std::string_view id{s};
    // NSO ids are 32 bytes; the researched 16-byte id has a zero-filled tail.
    if (id.size() != 32 && id.size() != 64) return false;
    for (size_t i=0; i<id.size(); ++i) {
        char c=id[i]; if(c>='a' && c<='f') c-=32;
        if (c != (i<Build.size() ? Build[i] : '0')) return false;
    }
    return true;
}
struct BuildLayout {
    uint64_t global, player; uint32_t money, pause, xyz, facing_a, facing_b;
    uint64_t items; uint32_t room, hp; uint64_t ashley;
    // Candidate save-identity fields (pG+), linearly extrapolated from the one real-binary
    // offset constant (+0x84) confirmed live elsewhere in the same struct; NOT independently
    // verified live for these three fields -- see
    // the r5-save-identity research notes. Bounds-checked and
    // used only to bucket per-save UI preferences, never gameplay state.
    uint32_t chapter, save_cnt, terminal_no;
    uint32_t scenario_progress; // verified Switch Scenario_flg[1], not GC +0x84
    // Switch Room_flg[0] (GC +0x174): per-room script bits. Read only in room 0x22C, where
    // r22c owns bit31 (range session) and bit29 (round running), analysis/20261114-gallery-sw.md.
    uint32_t room_flags;
};
inline std::optional<BuildLayout> Layout(const char* id) {
    if (!SupportsBuild(id)) return {};
    return BuildLayout{0xB25D78,0xB25D88,0x501C,0x5098,0x128,0x18,0x20,0xB25F60,0x5020,0x5028,0xB25E08,
        0x500E,0x5010,0x5016,0x5344,0x1AC};
}
enum class State { Unknown, Title, MainMenu, SaveSelect, Options, Loading, Cutscene,
    Exploration, WithAshley, Combat, AshleySeparated, AshleyHidden, AshleyPlayable, Boss,
    Qte, Vehicle, Merchant, Typewriter, Case, KeyItems, Map, GameOver=22,
    SeparateWays, SeparateWaysCase, AssignmentAda, MercSelect, MercRound, MercResults,
    Extras, Unsupported, Pause, Radio,
    ShootingRange }; // appended: re4.state 0..32 keep their numbers, this is 33
struct Presentation { const char* page; bool dim; };
// This policy is separate from detection: synthetic states never become live flags.
inline Presentation Select(State s) {
    switch(s) {
    case State::Title: case State::MainMenu: case State::SaveSelect: case State::Options:
    case State::Loading: case State::GameOver: case State::Extras: return {"standby",false};
    case State::Unsupported: return {"unsupported",false};
    case State::Cutscene: case State::Qte: case State::Typewriter: case State::Pause: return {"dimmed",true};
    // r22c range session/round: the case holds the range weapon only, never Field/Case.
    case State::ShootingRange: return {"dimmed",true};
    case State::Merchant: case State::Case: case State::SeparateWaysCase: return {"case",false};
    case State::KeyItems: return {"key_items",false};
    case State::Vehicle: return {"hud_only",false};
    case State::MercSelect: case State::MercRound: case State::MercResults: return {"mercenaries",false};
    case State::Unknown: return {"unavailable",false};
    default: return {"field",false};
    }
}
struct PlacedItem {
    uint16_t slot, id, count;
    int8_t centre_x2, centre_y2;
    uint8_t orientation;
    uint16_t magazine=0;
    // High nibble of record byte 7: the weapon's capacity level (0-6). Live records:
    // weapon 0x27 byte 0x52 -> level 5 = 35, weapons 0x2D/0x37/0x34 byte 0x6x -> level 6.
    uint8_t capacity_level=0;
    bool operator==(const PlacedItem&) const = default;
};
inline bool StripWeapon(uint16_t id) {
    switch(id) {
    case 3: case 0x21: case 0x40: case 0x23: case 0x25: case 0x27:
    case 0x29: case 0x2A: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
    case 0x30: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
    case 0x94: case 0x17: case 0x6D: case 0x3E: case 0x52:
    case 1: case 2: case 14: case 0x41: case 0x10: case 0x47: case 0x53:
        return true;
    default: return false;
    }
}
// Native wep_info bullet_id table; part-attached variants share their base ammo.
// Covers every StripWeapon()-eligible id (the complete 30-id shipped/live-verified weapon
// set, analysis/20261101-r5-inventory.md S2b) plus every decomp wep_info row. Two ids that
// looked like gaps turned out, on inspection of their own shipped weapon-NN.png case-model
// renders (packages/ResidentEvil4/dualscreen/assets/), not to be ammo-using handguns at all:
// 0x41's render is a blue/silver beam weapon (not a handgun silhouette like
// 0x23/0x40's renders), and 0x0A is outside StripWeapon() entirely, has a 1x1 case footprint
// identical to the grenade ids 0x08/0x09 (analysis/20261101-r5-inventory.md S1), and decomp
// wep_info marks it bullet_id 0xFF (no ammo) -- see analysis/20261109-reserve-ammo.md for the
// full evidence trail. Both stay unavailable, matching this function's own "infinite-ammo
// bonus weapons / grenades with no ammo item stay unavailable" contract.
inline std::optional<uint16_t> AmmoItem(uint16_t weapon) {
    switch(weapon) {
    case 0x03: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
    case 0x26: case 0x27: case 0x28: case 0x40: return 0x04; // handgun
    case 0x2C: case 0x2D: case 0x94: case 0x47: return 0x18; // shotgun (0x47: own render is a 4th, distinct pump shotgun)
    case 0x2E: case 0x2F: case 0x51: case 0x6B: case 0x6C: case 0x99: return 0x07; // rifle
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x3E: return 0x20; // TMP
    case 0x29: case 0x2A: case 0x2B: return 0x00; // magnum (zero is a real item id)
    case 0x36: case 0xAB: return 0x46; // mine darts
    case 0x34: case 0x53: return 0x6A; // weapon 0x34 (0x53: byte-identical render + footprint to 0x34)
    case 0x37: return 0x1A; // weapon 0x37
    case 0x52: return 0x72; // bowgun
    // 0x10 is StripWeapon()-eligible (a distinct crossbow/bow render, not the 0x52 bowgun's
    // own render) but its ammo item is unresolved this session -- no live case/merchant
    // encounter reached it. Left unavailable rather than guessed; see
    // analysis/20261109-reserve-ammo.md open items.
    default: return {}; // knife, rockets, grenades, weapon 0x41, unknown ids: no reserve item
    }
}
// Native wep_info charge_num rows (decomp item.cpp wep_info): loaded-round capacity per
// capacity level. 0 = the level does not exist; 0xFFFF = infinite, where the native HUD
// draws dashes instead of a digit (weapons 0x37 and 0x34 at level 6, live).
inline std::optional<std::array<uint16_t,7>> MagazineCapacity(uint16_t weapon) {
    using C=std::array<uint16_t,7>;
    switch(weapon) {
    case 0x23: case 0x24: return C{10,13,16,19,22,25,0};   // handgun
    case 0x25: case 0x26: return C{8,10,12,15,18,22,0};    // handgun
    case 0x03: return C{15,18,21,24,27,30,100};           // handgun (level 6 = 100)
    case 0x21: case 0x22: case 0x40: return C{10,13,16,20,24,28,0}; // handgun
    case 0x27: case 0x28: return C{15,18,21,25,30,35,0};   // handgun
    case 0x29: return C{6,8,10,12,0,0,0};                  // magnum
    case 0x2A: case 0x2B: return C{7,10,14,0,0,0,0};       // magnum
    case 0x2C: return C{6,8,10,12,15,18,0};                // shotgun
    case 0x94: return C{7,9,11,13,15,17,0};                // shotgun
    case 0x2D: return C{12,14,16,20,24,28,100};            // shotgun (level 6 = 100)
    case 0x2E: case 0x6B: case 0x99: return C{5,7,9,12,15,18,0};   // bolt-action rifle
    case 0x2F: case 0x6C: case 0x51: return C{10,12,14,17,20,24,0}; // semi-automatic rifle
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x3E: return C{30,50,100,150,200,250,0}; // TMP
    case 0x36: case 0xAB: return C{5,7,10,0,0,0,0};        // mine darts
    case 0x34: return C{20,25,30,35,40,50,0xFFFF};         // weapon 0x34
    case 0x37: return C{3,4,5,6,8,10,0xFFFF};              // weapon 0x37
    default: return {};
    }
}
// Weapons whose loaded count u16(record+8)>>3 matched the native aiming-HUD digit live, before
// and after one shot and after a reload, on two boots (analysis/20261114-ammo-mirror.md).
// Everything else (infinite/special weapons, unreached ids) stays unavailable.
inline bool MagazineProven(uint16_t weapon) {
    switch(weapon) {
    case 0x23: case 0x25: case 0x21: case 0x27: case 0x03: // handguns
    case 0x2C: case 0x2D: case 0x94:                        // shotguns
    case 0x2E: case 0x2F:                                   // rifles
    case 0x30:                                              // TMP
    case 0x29: case 0x2A:                                   // magnums
    case 0x36:                                              // mine darts
        return true;
    default: return false;
    }
}
// Loaded rounds for a proven weapon: never above the capacity of its recorded level, and never
// for an infinite or nonexistent level.
inline std::optional<uint16_t> LoadedRounds(uint16_t weapon,uint8_t level,uint16_t magazine) {
    const auto capacity=MagazineCapacity(weapon);
    if(!MagazineProven(weapon) || !capacity || level>6)return {};
    const uint16_t max=(*capacity)[level];
    if(!max || max==0xFFFF || magazine>max)return {};
    return magazine;
}
inline std::optional<uint16_t> LoadedRounds(const PlacedItem& item) {
    return LoadedRounds(item.id,item.capacity_level,item.magazine);
}
// The manager's equipped id names the attached-part variant (decomp item.cpp combine table)
// while the case record keeps the base weapon: live, the bolt-action rifle with its scope equips as 0x6B with
// record 0x2E. Only live-observed variants are mapped.
inline uint16_t EquippedRecordId(uint16_t equipped) {
    switch(equipped) {
    case 0x6B: return 0x2E;
    default: return equipped;
    }
}
inline std::optional<uint32_t> ReserveAmmo(uint16_t weapon,const std::vector<PlacedItem>& items) {
    const auto ammo=AmmoItem(weapon);
    if(!ammo)return {};
    uint32_t sum=0; // At most 384 native u16 stacks; cannot overflow u32.
    for(const auto& item:items)if(item.id==*ammo)sum+=item.count;
    return sum;
}
struct OwnedItem {uint16_t slot,id,count; bool operator==(const OwnedItem&) const = default;};
struct Snapshot {
    // Eight scenario words followed by the five key-unlock words used by this build's map.
    std::optional<std::array<uint32_t,13>> map_flags;
    // 0/1 = known native passed flag, 255 = invalid room-record identity.
    std::optional<std::array<uint8_t,MapSaveCount>> map_passed;
    std::optional<uint8_t> map_mode;
    std::optional<std::array<uint32_t,2>> map_open;
    std::optional<std::vector<OwnedItem>> owned_items;
    // Signed native current/max pair, acquired together. Zero current means dead;
    // unavailable is distinct. Ashley's stale absent values are never inferred.
    std::optional<std::array<int16_t,2>> health;
    std::optional<uint32_t> money;
    // The native HUD escort predicate; not an HP or Wait/Follow reader.
    std::optional<bool> ashley_escort;
    std::optional<uint8_t> ashley_command;
    std::optional<bool> paused;
    std::optional<std::array<float,3>> position;
    std::optional<std::array<float,2>> facing;
    std::optional<uint16_t> room, weapon, ammo;
    std::optional<uint32_t> reserve_ammo;
    // Raw room high byte; 0x200 is stage 2 on both sides of castle entry.
    std::optional<uint8_t> stage;
    // Native map selector's SCF_ST2_IN: Switch pG+0x5344 bit 23.
    // The room script sets this at castle entry; chapter/save metadata is not used.
    // Live event bracket and shipped selector: analysis/20261108-room200.md.
    std::optional<bool> castle_entered;
    std::optional<uint16_t> equipped_slot;
    std::optional<std::array<uint8_t,2>> case_capacity;
    std::optional<bool> village_treasure_map;
    std::optional<bool> medallion_note, merchant_contact;
    std::optional<uint16_t> medallions_unbroken;
    // Same ownership read, keyed to the castle (0x54) / island (0x55) Treasure Map item
    // ids instead of village's 0xA9 (the maps research report
    // "Treasure Map ownership item ids"). Independent of which area is current: owning
    // one map does not imply owning another.
    std::optional<bool> castle_treasure_map;
    std::optional<bool> island_treasure_map;
    // Separate Ways map items 0x10B..0x10F (bit n = item 0x10B+n) owned by the current
    // character, for the native SW treasure gate only (stage 5).
    std::optional<uint8_t> sw_treasure_maps;
    // Raw placements; capacity is separate. Full grid and direct case actions remain unavailable.
    std::optional<std::vector<PlacedItem>> placed_items;
    // Ashley's own signed current/max pair, pG+0x502C/+0x502E -- four bytes past Leon's
    // already-HIGH-confidence pG+0x5028/+0x502A pair (same GLOBAL_WK, adjacent GC fields
    // per decomp's pl_life/ashley_life naming). Gated strictly on ashley_escort==true:
    // this field is known to hold a stale nonzero value while Ashley is absent (LIVE-
    // READERS.md's own "960/960 while Ashley absent" caution) -- never published outside
    // a live-confirmed escort.
    std::optional<std::array<int16_t,2>> ashley_health;
    // Candidate per-save identity {chapter, save_cnt, terminal_no}, bounds-checked but NOT
    // independently confirmed live (the r5-save-identity research notes): used only
    // to bucket UI-preference persistence (never gameplay state),
    // and only ever read, never trusted blind -- absent whenever any one field looks
    // implausible, which simply falls back to unbucketed/default behaviour.
    std::optional<std::array<uint32_t,3>> save_identity;
    // Item manager's own character selector (`*m+0x1B`, the same byte the inventory-grid
    // reader below already reads to restrict itself to Leon) -- exposed unconditionally here
    // (unlike the grid reader, which only trusts value 0/Leon) so callers that don't need the
    // grid, such as re4.merc.best's per-character persistence bucket, can key on whichever
    // Mercenaries character is actually live without re-deriving this read. Raw byte, no
    // decoded id->name table exists yet (undecoded names stay hidden, same rule as
    // WeaponName/KeyItems elsewhere).
    std::optional<uint8_t> character;
    // Other mode/state flags and collection state beyond the readers above remain unavailable.
    State state=State::Unknown;
};
// The decoded village/castle/island tables are deliberately closed: stage 4/5 have no
// map (see the maps research report's "Unresolved" section -- stage 4's
// marker ratio is unverified and no Treasure Map item names either one).
inline const char* MapArea(uint16_t room, std::optional<bool> castle_entered = {}) {
    switch(room) {
    case 0x100: return "village-100";
    case 0x101: return "village-101";
    case 0x102: return "village-102";
    case 0x103: return "village-103";
    case 0x104: return "village-104";
    case 0x105: return "village-105";
    case 0x106: return "village-106";
    case 0x107: return "village-107";
    case 0x108: return "village-108";
    case 0x109: return "village-109";
    case 0x10A: return "village-10a";
    case 0x10B: return "village-10b";
    case 0x10C: return "village-10c";
    case 0x10D: return "village-10d";
    case 0x10E: return "village-10e";
    case 0x10F: return "village-10f";
    case 0x117: return "village-117";
    case 0x11C: return "village-11c";
    case 0x11D: return "village-11d";
    case 0x11E: return "village-11e";
    case 0x11F: return "village-11f";
    // r200 straddles the village/castle story boundary. Both meshes overlap;
    // follow the native map's progress bit, and refuse when it cannot be read.
    case 0x200: return castle_entered ? (*castle_entered ? "castle-200" : "village-200") : "";
    // ---- castle / st2 ----
    case 0x201: return "castle-201";
    case 0x202: return "castle-202";
    case 0x203: return "castle-203";
    case 0x204: return "castle-204";
    case 0x205: return "castle-205";
    case 0x206: return "castle-206";
    case 0x207: return "castle-207";
    case 0x208: return "castle-208";
    case 0x209: return "castle-209";
    case 0x20A: return "castle-20a";
    case 0x20B: return "castle-20b";
    case 0x20C: return "castle-20c";
    case 0x20D: return "castle-20d";
    case 0x20E: return "castle-20e";
    case 0x20F: return "castle-20f";
    case 0x210: return "castle-210";
    case 0x211: return "castle-211";
    case 0x212: return "castle-212";
    case 0x213: return "castle-213";
    case 0x214: return "castle-214";
    case 0x215: return "castle-215";
    case 0x216: return "castle-216";
    case 0x217: return "castle-217";
    case 0x218: return "castle-218";
    case 0x219: return "castle-219";
    case 0x21A: return "castle-21a";
    case 0x21B: return "castle-21b";
    case 0x21D: return "castle-21d";
    case 0x220: return "castle-220";
    case 0x221: return "castle-221";
    case 0x222: return "castle-222";
    case 0x223: return "castle-223";
    case 0x224: return "castle-224";
    case 0x225: return "castle-225";
    case 0x226: return "castle-226";
    case 0x227: return "castle-227";
    case 0x228: return "castle-228";
    case 0x229: return "castle-229";
    case 0x22A: return "castle-22a";
    // ---- island / st3 (36 unique rooms, map_obj3a..3f.dat; 0x306 shared 3B/3C) ----
    case 0x300: return "island-300";
    case 0x301: return "island-301";
    case 0x303: return "island-303";
    case 0x304: return "island-304";
    case 0x305: return "island-305";
    case 0x306: return "island-306";
    case 0x307: return "island-307";
    case 0x308: return "island-308";
    case 0x309: return "island-309";
    case 0x30A: return "island-30a";
    case 0x30B: return "island-30b";
    case 0x30C: return "island-30c";
    case 0x30D: return "island-30d";
    case 0x30E: return "island-30e";
    case 0x30F: return "island-30f";
    case 0x310: return "island-310";
    case 0x311: return "island-311";
    case 0x312: return "island-312";
    case 0x315: return "island-315";
    case 0x316: return "island-316";
    case 0x317: return "island-317";
    case 0x318: return "island-318";
    case 0x31A: return "island-31a";
    case 0x31B: return "island-31b";
    case 0x31C: return "island-31c";
    case 0x31D: return "island-31d";
    case 0x320: return "island-320";
    case 0x321: return "island-321";
    case 0x325: return "island-325";
    case 0x326: return "island-326";
    case 0x327: return "island-327";
    case 0x329: return "island-329";
    case 0x330: return "island-330";
    case 0x331: return "island-331";
    case 0x332: return "island-332";
    case 0x333: return "island-333";
    // Separate Ways: Switch-native mapDataInit areas 18..25 (analysis/20261114-gallery-sw.md B1;
    // 18 rooms also live-verified, analysis/20261109-sw-map-2.md). 0x511 has no native row and
    // 0x51F/0x520 are placeholder squares: they stay unmapped.
    case 0x500: return "stage5-500";
    case 0x501: return "stage5-501";
    case 0x502: return "stage5-502";
    case 0x503: return "stage5-503";
    case 0x504: return "stage5-504";
    case 0x505: return "stage5-505";
    case 0x506: return "stage5-506";
    case 0x507: return "stage5-507";
    case 0x508: return "stage5-508";
    case 0x509: return "stage5-509";
    case 0x50a: return "stage5-50a";
    case 0x50b: return "stage5-50b";
    case 0x50c: return "stage5-50c";
    case 0x50d: return "stage5-50d";
    case 0x50e: return "stage5-50e";
    case 0x50f: return "stage5-50f";
    case 0x510: return "stage5-510";
    case 0x512: return "stage5-512";
    case 0x513: return "stage5-513";
    case 0x514: return "stage5-514";
    case 0x515: return "stage5-515";
    case 0x516: return "stage5-516";
    case 0x517: return "stage5-517";
    case 0x518: return "stage5-518";
    case 0x519: return "stage5-519";
    case 0x51a: return "stage5-51a";
    case 0x51b: return "stage5-51b";
    case 0x51c: return "stage5-51c";
    case 0x51d: return "stage5-51d";
    case 0x51e: return "stage5-51e";
    default: return "";
    }
}
// Separate Ways (stage5) native pause-Map plate. Native selector main+0x4C25F4 (table
// +0xA12904): area 18/19 VILLAGE, 20 CASTLE, 21..25 ISLAND, area 0 none. Live-verified for
// 5a, 5b, 5c, 5e-5h (analysis/20261109-sw-label-and-zip.md); 5d is static.
inline char Stage5Label(uint16_t room) {
    switch(room) {
    case 0x500: case 0x501: case 0x502: case 0x503: return 'v';            // area 18, 5a
    case 0x504: case 0x505: case 0x506: case 0x507: case 0x508: case 0x509:
    case 0x50a: case 0x50b: return 'v';                                    // area 19, 5b
    case 0x50c: case 0x50d: case 0x50e: case 0x50f: case 0x510: return 'c'; // area 20, 5c
    case 0x512: case 0x514: case 0x513: case 0x515: case 0x516: case 0x517:
    case 0x518: case 0x519: case 0x51a: case 0x51b: case 0x51c:
    case 0x51d: case 0x51e: return 'i';                                    // areas 21-25
    default: return 0;
    }
}
inline double MapHeading(const std::array<float,2>& facing) {
    // Live walking established dx=component B, dz=component A.
    // Map north is -Z (the image's top); runtime rotates clockwise from up.
    return std::atan2(facing[1], -facing[0])*180.0/3.14159265358979323846;
}
// Native command-status getter main+331D60. Low bits: 1 Follow, 2 Wait;
// additional bits are native restrictions, never permission to issue a command.
inline uint8_t AshleyCommandStatus(uint16_t flags, std::array<uint8_t,2> routine,
                                  uint8_t status_a, uint8_t status_b) {
    uint8_t result=(flags&2) || (routine[0]==0 && routine[1]==0x10)?2:1;
    if(status_a&0x80)return result|0x20;
    if(status_b&1)return result|8;
    if((flags&8) || routine[0]==5)return result|0x20;
    if(routine[0]==0) {
        if(routine[1]==0x13 || routine[1]==0x14)return result|0x10;
        if(routine[1]==0x12)return result|0x20;
    }
    return result;
}
struct MapReadCache {
    State state=State::Unknown;
    uint64_t records=0;
    std::optional<uint16_t> room;
    std::optional<std::array<uint32_t,3>> save;
    std::optional<std::array<uint8_t,MapSaveCount>> passed;
};
class Resolver {
    const EdenDsmodHostApi& h;
    std::optional<BuildLayout> layout;
public:
    Resolver(const EdenDsmodHostApi& host,const char* id):h(host),layout(Layout(id)){}
    static bool Range(uint64_t a,size_t n) {
        return a>=0x10000 && a<0x8000000000ULL && n && n<=0x8000000000ULL-a;
    }
    template<class T> std::optional<T> Read(uint64_t a) const {
        T v{};
        if(!Range(a,sizeof v)||!h.is_mapped||!h.read_memory||
           !h.is_mapped(h.userdata,a,sizeof v)||!h.read_memory(h.userdata,a,&v,sizeof v)) return {};
        return v;
    }
    std::optional<uint64_t> Single(uint64_t offset) const {
        if(!layout || h.main_size<8 || offset>h.main_size-8 ||
           h.main_base>std::numeric_limits<uint64_t>::max()-offset) return {};
        auto p=Read<uint64_t>(h.main_base+offset);
        if(!p || (*p&7) || !Range(*p,0x40))return {};
        return p;
    }
    // Same bounds/overflow/mapped guard as Single(), but without requiring the value
    // read to itself look like a pointer -- used to tell "the main module is mapped and
    // readable here, just holding a null/stale slot" (a live game process, pre-game) apart
    // from "nothing is mapped at all" (no process, or the host hasn't attached yet).
    // Uses is_mapped only (no read_memory call), so it never counts as a host memory read.
    bool MainReadable(uint64_t offset,size_t n) const {
        return layout && h.main_size>=n && offset<=h.main_size-n &&
            h.main_base<=std::numeric_limits<uint64_t>::max()-offset &&
            Range(h.main_base+offset,n) && h.is_mapped && h.is_mapped(h.userdata,h.main_base+offset,n);
    }
    std::optional<uint64_t> Object(uint64_t offset) const {
        auto slot=Single(offset);
        if(!slot)return {};
        auto ptr=Read<uint64_t>(*slot);
        if(!ptr || (*ptr&7) || !Range(*ptr,0x5400))return {};
        return ptr;
    }
    // pG (GLOBAL_WK*), re-resolved fresh rather than cached: callers that must re-check a
    // live guard at the moment of an action (re4_equip.h) never trust a prior sample.
    std::optional<uint64_t> Global() const { return layout?Object(layout->global):std::nullopt; }
    // Switch checkSubMissionTarget (main+0x4E0620) -> GetEtcFlgPtr
    // (+0x205120) -> getRoomSavePtr (+0x495D80). RoomData GOT +0xB26080,
    // records pointer +0x30, stride 0xD8, EtcModelFlg +0x28 (u16, bit 0).
    // Static Room_data_tbl (+0xB18058) assigns these four record indexes.
    std::optional<uint16_t> Medallions() const {
        auto room_data=Single(0xB26080);
        if(!room_data)return {};
        auto records=Read<uint64_t>(*room_data+0x30);
        if(!records || (*records&7) || !Range(*records,21*0xD8))return {};
        constexpr std::array<uint8_t,4> indexes{3,18,8,20};
        constexpr std::array<uint8_t,4> rooms{0x03,0x13,0x08,0x18};
        std::array<std::array<uint8_t,0xD8>,4> data{};
        for(size_t n=0;n<data.size();++n) {
            auto record=Read<std::array<uint8_t,0xD8>>(*records+indexes[n]*0xD8);
            if(!record || (*record)[0]!=rooms[n] || (*record)[1]!=1)return {};
            data[n]=*record;
        }
        // st1_target_tbl: first seven farm targets, then eight cemetery targets.
        constexpr std::array<uint8_t,15> targets{15,16,17,18,20,19,21,4,5,6,7,9,8,10,11};
        uint16_t mask=0;
        for(size_t n=0;n<targets.size();++n) {
            const size_t pair=n<7?0:2, offset=0x28+2*targets[n];
            if(!((data[pair][offset]|data[pair+1][offset])&1))mask|=1u<<n;
        }
        return mask;
    }
    std::optional<std::array<uint8_t,MapSaveCount>> MapPassed(MapReadCache* cache=nullptr, const Snapshot* snapshot=nullptr) const {
        auto room_data=Single(0xB26080);
        if(!room_data)return {};
        auto records=Read<uint64_t>(*room_data+0x30);
        if(!records || (*records&7) || !Range(*records,MapSaveCount*0xD8))return {};
        if(cache && snapshot && cache->state==snapshot->state && cache->records==*records && cache->room==snapshot->room &&
           cache->save==snapshot->save_identity && cache->passed &&
           h.is_mapped && h.is_mapped(h.userdata,*records,MapSaveCount*0xD8))return cache->passed;
        auto data=Read<std::array<uint8_t,MapSaveCount*0xD8>>(*records);
        if(!data)return {};
        std::array<uint8_t,MapSaveCount> result;
        result.fill(255);
        for(const auto& row:MapSaveRooms) {
            const auto* r=data->data()+row.index*0xD8;
            if((r[0]|(r[1]<<8))==row.room)result[row.index]=(r[2]&0x80)!=0;
        }
        if(cache && snapshot){cache->state=snapshot->state;cache->records=*records;cache->room=snapshot->room;
            cache->save=snapshot->save_identity;cache->passed=result;}
        return result;
    }
    Snapshot Poll(MapReadCache* cache=nullptr, bool map_shown=true) const {
        Snapshot s;
        if(!layout){s.state=State::Unsupported;return s;}
        const auto global=Object(layout->global);
        // Resolved at most once per Poll(): the Title fallback below and the position/
        // facing block further down both need the player object, and previously each
        // called Object(layout->player) separately (2 extra host reads every sample
        // whenever the first short-circuit operand -- STA_TITLE -- was false, i.e.
        // essentially all of live gameplay). Lazy so a true title screen (where the
        // bit alone already answers the Title check) still never pays for it.
        bool player_resolved=false;
        std::optional<uint64_t> player_slot;
        auto player=[&]()->const std::optional<uint64_t>& {
            if(!player_resolved){player_slot=Object(layout->player);player_resolved=true;}
            return player_slot;
        };
        if(auto p=global) {
            auto money=Read<uint32_t>(*p+layout->money);
            if(money && *money<=99999999)s.money=money;
            const auto status2=Read<uint32_t>(*p+layout->pause);
            if(status2)s.paused=(*status2&0x04000000)!=0;
            if(auto r=Read<uint16_t>(*p+layout->room); r && (*r>>8)>=1 && (*r>>8)<=7) {
                s.room=r;
                s.stage=static_cast<uint8_t>(*r>>8);
            }
            if(s.room) {
                if(auto flags=Read<uint32_t>(*p+layout->scenario_progress)) {
                    s.castle_entered=(*flags&0x00800000)!=0;
                    // scf_check_submission: pG+0x5346 bit 6 = word 1 bit 22.
                    s.merchant_contact=(*flags&0x00400000)!=0;
                }

            }
            auto chapter=Read<uint8_t>(*p+layout->chapter);
            auto save_cnt=Read<uint16_t>(*p+layout->save_cnt);
            auto terminal_no=Read<uint8_t>(*p+layout->terminal_no);
            if(chapter && save_cnt && terminal_no && *chapter<=20 && *save_cnt<=9999 && *terminal_no<=50)
                s.save_identity=std::array<uint32_t,3>{*chapter,*save_cnt,*terminal_no};
            // Switch GameTask dispatches on pG+0x28 (GC's +0x20 does not shift by +0x84).
            // 4 door-demo -> 1 stage-init -> 2 room-init -> 3 main loop, verified across
            // three native door loads / two boots. analysis/20261108-loading.md.
            const auto game_routine=Read<uint8_t>(*p+0x28);
            const auto status1=Read<uint32_t>(*p+0x5094);
            const auto ctrl=Read<uint32_t>(*p+0x1A8);
            // Lazy: the shop's open-type lives behind its own Single()+Read() host round
            // trip, paid only when every earlier branch (Loading/Cutscene) already missed
            // and the cheap paired-status-bits check hasn't already ruled Merchant out.
            auto is_merchant=[&] {
                if(!status2 || (*status2&0x04040000)!=0x04040000)return false;
                const auto subscreen=Single(0xB26C98);
                const auto open_type=subscreen?Read<uint32_t>(*subscreen+0x2C):std::nullopt;
                return open_type==0x10;
            };
            // Only these dispatch states mean a room load. Zero is also used at title;
            // 5/6 are ending/options. Retired STA_72 and the UI word are not triggers.
            if(game_routine && (*game_routine==1 || *game_routine==2 || *game_routine==4))
                s.state=State::Loading;
            // STA_EVENT_SYSYTEM (+0x90000, incl. its unnamed companion bit) / STA_SUSPEND /
            // the scripted camera-freeze control word -- any one means a cutscene or
            // scripted sequence currently owns player control.
            else if((status2 && (*status2&0x00090000)) || (status1 && (*status1&0x10000000)) ||
                    (ctrl && (*ctrl&0x80000000)))
                s.state=State::Cutscene;
            // The island shop sets STA_INTO_SHOP + STA_SSCRN_REQUEST together.
            // INTO_SHOP persists after exit and in Case: also require SS_OPEN_SHOP.
            // Live bracket: analysis/20261108-live-checks.md.
            else if(is_merchant())
                s.state=State::Merchant;
            // STA_TITLE (live-verified, exact bit match at the genuine title screen) OR no
            // live player object yet (MainMenu/SaveSelect/Options -- no dedicated bit found
            // this round; Select() already presents these identically to Title).
            else if((status2 && (*status2&0x00008000)) || !player())
                s.state=State::Title;
            // Merchant's shooting range, the single native room 0x22C (st2/r22c, entered from
            // 0x204/0x211/0x220/0x305/0x31D; R22cInit main+0x3F6364). Room_flg[0] at pG+0x1AC:
            // bit31 = range session (itemSave main+0x3F7BAC sets, gameEnd +0x3FB2A4 clears; the
            // inventory is swapped for the range weapon meanwhile), bit29 = round task
            // (startShootingGame +0x3F6CE8 sets, shootEnd +0x3FA89C clears). Room_flg bits are
            // per-room script bits (r11C reuses bit29), so the room gate is required. An
            // unreadable word fails safe to the dimmed range state.
            else if(s.room && *s.room==0x22C) {
                const auto room_flags=Read<uint32_t>(*p+layout->room_flags);
                if(!room_flags || (*room_flags&0xA0000000u))s.state=State::ShootingRange;
            }
            // Mercenaries round -- LIVE-VERIFIED 2026-11-03 (analysis/20261103-merc.md), zero
            // new readers: the already-shipped room/stage field above (same pG+0x5020 this
            // reader already decodes for the campaign map) reads stage byte 4 while a live
            // Mercenaries round is in progress (village stage; confirmed via the native HUD
            // countdown timer, score counter and "MISSION START" after Extras > The
            // Mercenaries > Game Start > Leon > stage 1, reproduced across two independent
            // boots). The campaign only ever uses stage 1-3 (MapArea's village/castle/island);
            // stage 4 is unused by the base game, so this is a safe extension of an existing
            // HIGH-confidence field rather than a new memory address. Lowest priority of this
            // whole cascade (after Loading/Cutscene/Merchant/Title) so the mission-briefing
            // cutscene and loading screens inside a Mercenaries round keep classifying
            // correctly; Pause/GameOver below still have the final say, same as every other
            // branch here. Restricted to exactly stage==4 (bug found 2026-11-04, analysis/
            // 20261104-separate-ways-2.md section 1 "Dead/unused state label"): the previous
            // `>=4` catch-all also matched stage5's Separate Ways rooms (0x500-0x51F),
            // misclassifying live SW gameplay as the Mercenaries page. Only stage 4 (village)
            // is live-confirmed; 5-7 (the other three native Mercenaries maps, if present)
            // are no longer accepted here since stage5 is now known to collide with a real,
            // different game mode -- a future session should live-confirm 6/7 specifically
            // before widening this again, not restore the blanket bound.
            else if(s.room && (*s.room>>8)==4)
                s.state=State::MercRound;
            // Separate Ways (Ada) -- stage5, LIVE-VERIFIED 2026-11-04 (analysis/
            // 20261104-separate-ways-2.md: re4.room=1280/0x500, re4.stage=5, reproduced on
            // 4 independent live samples). Previously fell into the MercRound catch-all above
            // (same bug). SeparateWaysCase reuses the same bit Merchant/Case key on (SW's
            // Directives/Map/attache-case sub-screens set the identical STA_SSCRN_REQUEST bit
            // this reader already reads as `s.paused` -- analysis/20261104-separate-ways-2.md's
            // "Why this reads as Pause" finding), but unlike Merchant/Case it is reached via an
            // independent, reliable signal first (the room/stage field), so the final Pause
            // check below carves out an explicit exception for it rather than re-clobbering it
            // the way Merchant/Case still are. Plain SW gameplay (not paused) is unaffected
            // either way and reaches Select()'s default {"field",false} presentation.
            else if(s.room && (*s.room>>8)==5)
                s.state=(s.paused && *s.paused)?State::SeparateWaysCase:State::SeparateWays;
        } else if(MainReadable(layout->global,8)) {
            // The pG pointer chain itself didn't resolve, but the main module's own
            // anchor slot IS mapped and readable -- the game process is alive with no
            // game session loaded yet: pre-game (Title/MainMenu/SaveSelect/Options all
            // collapse to the same {"standby",false} presentation already, so one Title
            // tag covers all four without a further discriminator). Distinct from the
            // module itself being unmapped/unreadable (nothing running at all, or the
            // host hasn't attached yet), which stays Unknown exactly as before.
            s.state=State::Title;
        }
        if(auto p=player()) {
            auto xyz=Read<std::array<float,3>>(*p+layout->xyz);
            if(xyz && std::isfinite((*xyz)[0]) && std::isfinite((*xyz)[1]) &&
               std::isfinite((*xyz)[2]) && std::abs((*xyz)[0])<=10000000 &&
               std::abs((*xyz)[1])<=10000000 && std::abs((*xyz)[2])<=10000000)s.position=xyz;
            auto a=Read<float>(*p+layout->facing_a), b=Read<float>(*p+layout->facing_b);
            if(a && b && std::isfinite(*a) && std::isfinite(*b) &&
               std::abs((*a)*(*a)+(*b)*(*b)-1.f)<0.15f)s.facing=std::array<float,2>{*a,*b};
        }
        // A room value is cached outside gameplay. Never use it to select a state.
        if(!s.position){s.room.reset();s.stage.reset();s.castle_entered.reset();s.money.reset();}
        if(s.position && s.room)if(auto slot=Single(layout->ashley)) {
            auto ptr=Read<uint64_t>(*slot);
            if(ptr && *ptr==0)s.ashley_escort=false;
            else if(ptr && !(*ptr&7) && Range(*ptr,0x119)) {
                if(auto mode=Read<uint8_t>(*ptr+0x118)) {
                    s.ashley_escort=*mode==3 && (*s.room>>8)!=5;
                    if(*s.ashley_escort && global) {
                        auto flags=Read<uint16_t>(*ptr+0x4F0);
                        auto routine=Read<std::array<uint8_t,2>>(*ptr+0x114);
                        auto a=Read<uint8_t>(*global+0x509A), b=Read<uint8_t>(*global+0x5096);
                        if(flags && routine && a && b)
                            s.ashley_command=AshleyCommandStatus(*flags,*routine,*a,*b);
                        // Ashley current/max HP -- only ever read while escort is positively
                        // confirmed this same sample (see the Snapshot field comment: this
                        // address holds a stale value while Ashley is absent).
                        if(auto hp=Read<std::array<int16_t,2>>(*global+0x502C))
                            if((*hp)[0]>=0 && (*hp)[1]>0 && (*hp)[1]<=9999 && (*hp)[0]<=(*hp)[1])
                                s.ashley_health=hp;
                    }
                }
            }
        }
        if(s.position)if(auto m=Single(layout->items)) {
            const auto array=Read<uint64_t>(*m+0x20), equipped=Read<uint64_t>(*m+0x10);
            const auto count=Read<int32_t>(*m+0x30);
            const auto character=Read<uint8_t>(*m+0x1B);
            s.character=character;
            // Damage -> native green herb, repeated on two relocated boots.
            // The healing dispatcher caps Leon's maximum at 2400. Restrict this
            // reader to the positively tested character selector and live player.
            if(global && character && *character==0) {
                auto hp=Read<std::array<int16_t,2>>(*global+layout->hp);
                if(hp && (*hp)[0]>=0 && (*hp)[1]>0 && (*hp)[1]<=2400 && (*hp)[0]<=(*hp)[1])
                    s.health=hp;
            }
            // Only Leon's inventory selector has a positive live bracket. Separate Ways also
            // reads the same records for its map items (below), never for Leon's outputs.
            const bool leon=character && *character==0, sw=s.stage==5;
            if(array && count && character && (leon || sw) && *count>0 && *count<=384 &&
               !(*array&1) && Range(*array,static_cast<size_t>(*count)*14)) {
                // One bounded acquisition instead of up to 384 host crossings at
                // 60 Hz. Decode placement and equipped ammo from the same sample.
                std::array<uint8_t,384*14> records{};
                const size_t bytes=static_cast<size_t>(*count)*14;
                const bool acquired=h.is_mapped && h.read_memory &&
                    h.is_mapped(h.userdata,*array,bytes) &&
                    h.read_memory(h.userdata,*array,records.data(),bytes);
                auto record=[&](size_t n) {
                    std::array<uint8_t,14> r{};
                    std::memcpy(r.data(),records.data()+n*14,r.size());
                    return r;
                };
                // Separate Ways map items 0x10B..0x10F: the native SW search main+0x294FC0 matches
                // the alive bit, owner == ItemMgr+0x1B (the current character) and the id; the
                // count is not tested.
                if(sw && acquired) {
                    uint8_t mask=0;
                    for(int32_t n=0;n<*count;++n) {
                        const auto r=record(static_cast<size_t>(n));
                        const uint16_t id=r[0]|(r[1]<<8);
                        if((r[4]&1) && r[5]==*character && id>=0x10B && id<=0x10F)mask|=1u<<(id-0x10B);
                    }
                    s.sw_treasure_maps=mask;
                }
                if(leon) {
                std::vector<PlacedItem> placed;
                std::vector<OwnedItem> owned;
                bool valid=acquired;
                uint16_t case_id=0;
                bool treasure_map=false, treasure_map_castle=false, treasure_map_island=false, medallion_note=false;
                for(int32_t n=0;n<*count;++n) {
                    if(!acquired)break;
                    const auto r=record(static_cast<size_t>(n));
                    if(!(r[4]&1) || r[5]!=*character)continue;
                    const uint16_t item_id=r[0]|(r[1]<<8);
                    const uint16_t item_count=r[2]|(r[3]<<8);
                    if(item_count)owned.push_back({static_cast<uint16_t>(n),item_id,item_count});
                    // Native sub-screen initializer main+4AE4D0 searches these
                    // case ids in order and uses the highest owned tier.
                    if(item_id==0xB0 && (r[2] || r[3]))medallion_note=true;
                    if(item_id==0xA9 && (r[2] || r[3]))treasure_map=true;
                    if(item_id==0x54 && (r[2] || r[3]))treasure_map_castle=true;
                    if(item_id==0x55 && (r[2] || r[3]))treasure_map_island=true;
                    if(item_id>=0x7C && item_id<=0x7F && item_id>case_id)case_id=item_id;
                    if(r[13]!=1)continue;
                    const auto x=static_cast<int8_t>(r[10]), y=static_cast<int8_t>(r[11]);
                    if(x<0 || x>29 || y<0 || y>15 || r[12]>1){valid=false;continue;}
                    placed.push_back({static_cast<uint16_t>(n),
                        static_cast<uint16_t>(r[0]|(r[1]<<8)),
                        static_cast<uint16_t>(r[2]|(r[3]<<8)),x,y,r[12],
                        static_cast<uint16_t>((r[8]|(r[9]<<8))>>3),
                        static_cast<uint8_t>(r[7]>>4)});
                }
                if(acquired){s.medallion_note=medallion_note;s.village_treasure_map=treasure_map;s.castle_treasure_map=treasure_map_castle;
                    s.island_treasure_map=treasure_map_island;s.owned_items=std::move(owned);}
                if(valid)s.placed_items=std::move(placed);
                // Capacity ownership does not depend on whether every item's
                // placement/orientation has a supported display contract.
                constexpr std::array<std::array<uint8_t,2>,4> sizes{{{10,6},{11,7},{12,8},{15,8}}};
                // The native initializer forces playable Ashley to the small
                // board. Only the positively observed Leon type0 is supported.
                // Skip the host round trip entirely when no case is owned (case_id==0,
                // the common case for every fixture/sample with no attache case item).
                std::optional<uint8_t> player_type;
                if(case_id && global)player_type=Read<uint8_t>(*global+0x503C);
                if(case_id && player_type && *player_type==0)s.case_capacity=sizes[case_id-0x7C];
                // The fixture starts with no weapon (id 0800, null pointer). Never decode
                // that sentinel or arbitrary pointers as a magazine. Only live-proven weapons
                // publish a magazine (MagazineProven/LoadedRounds).
                const auto equipped_id=Read<uint16_t>(*m+0x18);
                const auto id=equipped_id?std::optional<uint16_t>(EquippedRecordId(*equipped_id)):std::nullopt;
                if(acquired && equipped && id && StripWeapon(*id) && *equipped>=*array &&
                   *equipped-*array<static_cast<uint64_t>(*count)*14 &&
                   (*equipped-*array)%14==0) {
                    const auto r=record(static_cast<size_t>((*equipped-*array)/14));
                    if((r[4]&1) && r[5]==*character && (r[0]|(r[1]<<8))==*id) {
                        const uint16_t ammo=static_cast<uint16_t>((r[8]|(r[9]<<8))>>3);
                        s.weapon=id;
                        s.equipped_slot=static_cast<uint16_t>((*equipped-*array)/14);
                        s.ammo=LoadedRounds(*id,static_cast<uint8_t>(r[7]>>4),ammo);
                        if(s.placed_items && (r[2] || r[3]))
                            s.reserve_ammo=ReserveAmmo(*id,*s.placed_items);
                    }
                }
                }
            }
        }
        // STA_SSCRN_REQUEST is shared by menus. Preserve the independently detected
        // island Merchant and stage-5 subscreens; otherwise use generic Pause.
        // Title is also excluded: it is reached either by STA_TITLE itself (live-verified
        // with no other bits set, analysis/20261102-fix-c.md) or by the no-player-object
        // fallback that stands in for MainMenu/SaveSelect/Options -- screens that are
        // themselves part of the same sub-screen family as every other member this project
        // has had to carve an exception for (the shop, SW's chapter/Directives/Map cards).
        // Found by priority-order code audit, not live-tested this session (no harness
        // access); worst case if wrong is a no-op, since no live sample has shown the two
        // bits coexisting. Flagged for live confirmation in a future session.
        if(s.paused && *s.paused && s.state!=State::SeparateWaysCase &&
           s.state!=State::Merchant && s.state!=State::Loading &&
           s.state!=State::Title)s.state=State::Pause;
        // Native death/retry capture: validated Leon HP reaches zero. This
        // selects a defeated standby throughout the death animation and retry.
        if(s.health && (*s.health)[0]==0)s.state=State::GameOver;
        // Warm during title/loading only when native pose/room data exists. State
        // changes invalidate passed bits too: a same-room reload can update them.
        // Pause, room/save changes, relocation and mapping loss still invalidate.
        const bool field=map_shown && s.room && s.position && s.facing && *MapArea(*s.room,s.castle_entered) &&
            (s.state==State::Unknown || s.state==State::SeparateWays ||
             s.state==State::Loading || s.state==State::Title);
        if(field && global) {
            s.map_flags=Read<std::array<uint32_t,13>>(*global+0x5340);
            if(s.map_flags) {
                s.map_passed=MapPassed(cache,&s);
                if(auto sub=Single(0xB26C98)) {
                    s.map_mode=Read<uint8_t>(*sub+0x448);
                    s.map_open=Read<std::array<uint32_t,2>>(*sub+0x2C);
                }
                if(*s.room<0x200 || (*s.room==0x200 && s.castle_entered==false))
                    s.medallions_unbroken=Medallions();
            }
            if(cache && !s.map_passed)*cache={};
        } else if(cache)*cache={};
        return s;
    }
};
}
