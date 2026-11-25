// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pokémon Legends Z-A (title 0100F43008C44000) save reader.
//
// The game's save file is one SwishCrypto ("MemeCrypto V2") container: a flat list of typed,
// XOR-obfuscated blocks, the whole payload obfuscated with a fixed 0x7F-byte pad and closed by
// SHA256(intro || payload || outro). Inside, the party and box blocks hold standard Gen-9
// entities (PA9), each one encrypted with the usual PID-seeded LCG + block shuffle.
//
// Everything here is read-only and format-only: block keys, sizes and the entity layout come
// from the community's open documentation of the format (PKHeX's SAV9ZA / PA9 / SwishCrypto).
// No game asset is embedded: the module decodes the player's own save at runtime.
//
// The parser is deliberately total: every read is bounds-checked against the block it belongs
// to, a malformed block list ends the walk instead of throwing, and a save that fails the hash
// check is reported as such rather than half-decoded.

#pragma once

#include <cstddef>
#include <array>
#include <memory>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace za {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

// ---- block keys (SwishCrypto block list) ------------------------------------------------------
inline constexpr u32 KParty = 0x3AA1A9AD;
inline constexpr u32 KBox = 0x0D66012C;
inline constexpr u32 KItem = 0x21C9BD44;
inline constexpr u32 KMyStatus = 0xE3E89BD1;
inline constexpr u32 KBoxLayout = 0x19722C89;
inline constexpr u32 KPlayTime = 0xEDAFF794;
inline constexpr u32 KPokedex = 0x2D87BE5C;
inline constexpr u32 KLastSaved = 0x1522C79C;
inline constexpr u32 KMoney = 0x4F35D0DD;
inline constexpr u32 KCurrentBox = 0x017C3CBB;
inline constexpr u32 KSaveRevision = 0x0926555A;
inline constexpr u32 KCoordinates = 0x910D381F;
inline constexpr u32 KPlayedSeconds = 0xCE3AF8F2;
inline constexpr u32 KStreetName = 0xBCCE00D6;
inline constexpr u32 KBoxesUnlocked = 0x71825204;
inline constexpr u32 KTimeSecondsPastMidnight = 0x7B70DA66;

// ---- entity layout ---------------------------------------------------------------------------
inline constexpr std::size_t kStoredSize = 0x148; // PA9 stored
inline constexpr std::size_t kPartySize = 0x158;  // PA9 party (stored + 0x10 party stats)
inline constexpr std::size_t kPartyGap = 0x88;    // 0x40 footer + 0x48 padding
inline constexpr std::size_t kPartySlotSize = kPartySize + kPartyGap; // 0x1E0
inline constexpr std::size_t kPartySlots = 6;
inline constexpr std::size_t kBoxSlotSize = kPartySize + 0x40; // 0x198: party format in boxes
inline constexpr std::size_t kBoxSlotCount = 30;
inline constexpr std::size_t kBoxCount = 32;

enum class PartyStatus : u32 { Healthy=0, Paralysis=1, Sleep=2, Frozen=3, Burn=4, Poison=5 };

inline constexpr std::size_t kDexEntrySize = 0x84;
inline constexpr std::size_t kDexEntries = 0x27068 / kDexEntrySize; // 1912 internal slots

// ---- one entity ------------------------------------------------------------------------------
struct Entity {
    bool present = false;
    u16 internal_species = 0; // the game's own species id
    u16 species = 0;          // National Dex id
    u16 form = 0;
    u16 held_item = 0;
    u8 level = 0;
    u8 nature = 0;
    u8 gender = 0; // 0 male, 1 female, 2 genderless
    u8 ball = 0;
    bool shiny = false;
    bool egg = false;
    u16 hp = 0;
    u16 hp_max = 0;
    u16 attack = 0;
    u16 defense = 0;
    u16 speed = 0;
    u16 sp_attack = 0;
    u16 sp_defense = 0;
    u32 exp = 0;
    u32 encryption_constant = 0;
    u32 pid = 0;
    u16 checksum = 0;
    bool checksum_ok = false;
    bool alpha = false;
    u32 status_condition = 0;
    u16 ability = 0;
    std::array<u16, 4> moves{};
    std::array<u8, 4> move_pp{};
    std::array<u8, 4> move_pp_ups{};
    std::array<u8, 6> evs{}; // HP, Attack, Defense, Speed, Sp. Attack, Sp. Defense
    std::array<u8, 6> ivs{};
    std::string nickname; // UTF-8
};

struct Trainer {
    u32 id32 = 0;
    u16 tid = 0;
    u16 sid = 0;
    u8 game = 0;
    u8 gender = 0;
    u8 language = 0;
    std::string name; // UTF-8
};

struct Save {
    bool valid = false;      // a save container was read and its block list walked
    bool hash_ok = false;    // the container's SHA256 trailer matched
    std::string status;      // short human-readable state, for the package's status label
    std::size_t file_size = 0;
    std::size_t block_count = 0;

    Trainer trainer;
    u32 money = 0;
    int research_level=-1,research_points=-1,hyperspace_points=-1,ticket_points=-1,royale_wins=-1;
    int infinite_points=-1,royale_medals=-1;
    std::unordered_map<u64,u64> system_work;
    u32 play_seconds = 0;
    double day_seconds = 0.0;

    int party_count = 0;
    Entity party[kPartySlots];

    int dex_seen = 0;   // species seen at least once (National Dex 1..1025)
    int dex_caught = 0; // species caught (National Dex 1..1025)
    // Caught counts per generation, index 0 = Gen 1 (National Dex 1-151) .. 8 = Gen 9.
    int dex_caught_by_gen[9] = {};
    std::array<u8, 1026> dex_state{}; // national id: 0 missing, 1 seen, 2 caught
    std::array<u16, 1026> owned{};    // current party + all boxes, by national id
    struct Item { u16 id; u32 count; u32 pouch; u32 flags; };
    std::vector<Item> items;
    std::unordered_map<u64,u64> quest_work;

    int box_used = 0; // slots holding an entity, across every box
    int boxes_unlocked = 0;
    int boxes_available = 0; // boxes the save actually holds (KBox block size / 30 slots)
    int current_box = 0;
    int boxes_used[kBoxCount] = {}; // slots holding an entity, per box
    struct BoxEntry {
        u16 box, slot, species, form;
        u32 pid, ec;
        std::string nickname;
        u8 gender=0;
        bool egg=false;
    };
    std::shared_ptr<const std::vector<BoxEntry>> box_entries;

    // The box the package is currently showing (clamped to a valid, unlocked box).
    int box_view_index = 0;
    Entity box_view[kBoxSlotCount];

    std::string map_name; // ASCII area id recorded with the position (e.g. "t1")
    std::string street;   // street name block, when the save has one
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float rotation = 0.0f;
};

// Decrypts and parses a whole save file. `bytes` is the raw file (encrypted container).
// `box_index` selects which box `box_view` holds (pass -1 for the box the game itself has
// selected); it is clamped to the save's valid range.
Save Parse(const std::vector<u8>& bytes, int box_index = -1);

// The same, for callers that already hold a decrypted block list: exposed for the tests. The walk
// decodes each block's own bytes in place (they are obfuscated with the block's key stream), so
// `payload` must be the mutable buffer the views point into.
struct BlockView {
    u32 key = 0;
    const u8* data = nullptr;
    std::size_t size = 0;
};
bool ReadBlocks(std::vector<u8>& payload, std::vector<BlockView>& out);
const BlockView* FindBlock(const std::vector<BlockView>& blocks, u32 key);

// Decrypts one PA9 in place. `size` is kPartySize (party) or kStoredSize/kBoxSlotSize.
bool DecryptEntity(const u8* raw, std::size_t size, Entity& out);
// Decode an already decrypted, unshuffled PA9 memory record. checksum_ok includes sanity==0.
// Does not reject a mismatched checksum: live readers must decide whether the candidate is valid.
bool ReadPlainEntity(const u8* raw, std::size_t size, Entity& out);

// Internal (game) species id -> National Dex id. Gen-9 numbering only diverges from 917 up.
u16 NationalSpecies(u16 internal);

// Generation (1..9) a National Dex id belongs to, or 0 when it is outside 1..1025.
int GenerationOf(u16 national);

// UTF-16LE fixed-width field (nickname, trainer name) -> UTF-8, stopping at the terminator.
std::string Utf16ToUtf8(const u8* bytes, std::size_t size);

} // namespace za
