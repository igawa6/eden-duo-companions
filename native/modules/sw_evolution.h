// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Evolution chain decode: `romfs:/bin/pml/evolution/evo_%03d.bin`, one 72-byte file per
// PersonalTable row (the same row-is-species-id convention sw_dex_index.h already documents for
// `personal_total.bin`'s own 1192 rows). Format confirmed by direct byte inspection of the real
// extracted romfs, not guessed or ported from a written spec:
//
//   evo_220.bin (Swinub):    04 00 00 00 DD 00 00 21 | (zero) x8
//   evo_221.bin (Piloswine): 15 00 F6 00 D9 01 00 00 | (zero) x8
//   evo_473.bin (Mamoswine): (all zero -- a fully-evolved species has no further entries)
//
// i.e. a fixed 72-byte file holding up to 9 entries of 8 bytes each: u16 method, u16 argument,
// u16 target species, u8 target form, u8 level. The first all-zero entry (method==0 &&
// species==0) ends the list early for a species with fewer than 9 real evolutions -- matches
// PKHeX's own `EvolutionSet8` (`EvolutionSet8.cs`, fetched live from GitHub kwsch/PKHeX:
// SIZE=8 per entry). Decoding Swinub's bytes with this layout gives method=4 (Level), species=
// 0x00DD=221 (Piloswine), level=0x21=33 -- "Swinub evolves into Piloswine at level 33", matching
// the approved D2 mockup's own worked example exactly. Decoding Piloswine's bytes gives method=21
// (KnowsMove), argument=0x00F6=246 (the move id "Ancient Power" resolves to via
// `sw_move_names.h`), species=0x01D9=473 (Mamoswine) -- "Piloswine evolves into Mamoswine while
// knowing Ancient Power", also matching the D2 mockup.
//
// Method ids are the generic, cross-generation PKHeX `EvolutionType` enum (the same numbering
// this fork's `lp_dex.h` already uses for Luminescent Platinum's own evolution chains) -- SwSh's
// own table only exercises a subset. Scanned all 1192 real files this cycle: ids
// {1,2,3,4,5,6,7,8,9,10,11,14,16,17,18,19,20,21,22,23,24,28,29,30,31,32,33,34,36,40} match
// `lp_dex::Method`'s names; ids {43,44,45,46,47,48,49} are Gen8-only additions (e.g. Sirfetch'd/
// Mr. Rime's "level up + an in-battle special condition" evolutions) this header does not name
// individually -- `MethodText`'s fallback ("Special condition") is honest, not a guess, for those.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_strings.h"

namespace sw_evolution {

using u8 = std::uint8_t;
using u16 = std::uint16_t;

constexpr std::size_t kEntrySize = 8;
constexpr std::size_t kMaxEntries = 9; // 72 / 8
constexpr std::size_t kFileSize = kEntrySize * kMaxEntries;
constexpr int kMaxSpecies = 898; // same bound sw_dex_index.h/sw_species_types.h/sw_pk8.h use

struct Edge {
    int method = 0;
    int argument = 0;
    int species = 0; // the evolved-INTO species
    int form = 0;
    int level = 0;
    bool operator==(const Edge&) const = default;
};

/// Pure decode of one already-in-hand 72-byte evo_NNN.bin buffer (no I/O). Stops at the first
/// all-zero entry or the buffer's end, whichever comes first -- never reads past `size`.
inline std::vector<Edge> DecodeFile(const u8* data, std::size_t size) {
    std::vector<Edge> out;
    for (std::size_t i = 0; i + kEntrySize <= size && out.size() < kMaxEntries; i += kEntrySize) {
        const u16 method = static_cast<u16>(data[i] | (data[i + 1] << 8));
        const u16 arg = static_cast<u16>(data[i + 2] | (data[i + 3] << 8));
        const u16 species = static_cast<u16>(data[i + 4] | (data[i + 5] << 8));
        const u8 form = data[i + 6];
        const u8 level = data[i + 7];
        if (method == 0 && species == 0) break; // a blank entry ends the real list (PKHeX convention)
        out.push_back({method, arg, species, form, level});
    }
    return out;
}

/// `forward[species]` = that species' own evo_NNN.bin edges (species that evolve from it);
/// `parent[species]` = the one species whose own edge list targets it (0 = no known predecessor,
/// i.e. a first-stage species) -- the first predecessor found wins ties, which never happens in
/// practice (no real SwSh species evolves from two different parents).
struct Table {
    std::array<std::vector<Edge>, kMaxSpecies + 1> forward{};
    std::array<u16, kMaxSpecies + 1> parent{};
};

inline void AddToTable(Table& t, int species, const std::vector<Edge>& edges) {
    if (species <= 0 || species > kMaxSpecies) return;
    t.forward[static_cast<std::size_t>(species)] = edges;
    for (const auto& e : edges) {
        if (e.species > 0 && e.species <= kMaxSpecies &&
            t.parent[static_cast<std::size_t>(e.species)] == 0) {
            t.parent[static_cast<std::size_t>(e.species)] = static_cast<u16>(species);
        }
    }
}

/// Reads and decodes one species' own evo_NNN.bin into `out`. False when the file could not be read
/// (that species keeps an empty edge list -- fail-soft per entry, like sw_dex_index.h).
inline bool LoadSpecies(const EdenDsmodHostApi& host, Table& out, int s) {
    if (!host.read_romfs || s <= 0 || s > kMaxSpecies) return false;
    char path[96];
    std::snprintf(path, sizeof path, "romfs:/bin/pml/evolution/evo_%03d.bin", s);
    std::array<u8, kFileSize> buf{};
    const std::size_t got = host.read_romfs(host.userdata, path, 0, buf.data(), buf.size());
    if (got != buf.size()) return false;
    AddToTable(out, s, DecodeFile(buf.data(), buf.size()));
    return true;
}

/// One member of a chain in display order: its species, its depth from the first stage (0 = base),
/// and the edge that led to it from its parent (default-constructed, species==0, at depth 0).
struct Member {
    int species = 0;
    int depth = 0;
    Edge via{};
};

// Display slots on the Pokedex detail card's evolution row. A chain can be longer (Eevee: 9
// members), so the horizontal card allows all nine and puts the selected branch beside its
// known root. BuildChain walks the whole chain (up to kMaxChainWalk); VisibleChain remains
// bounded for malformed or future data longer than the real game's largest family.
constexpr int kMaxChainMembers = 9;
constexpr int kMaxChainWalk = 16;

/// Pure chain build over an already-loaded Table (no I/O, unit-testable with a hand-built Table).
/// Walks back to the first stage via `parent` (bounded to 4 steps, cycle-guarded by construction --
/// `parent` can never point back to a species already visited without looping, since each step
/// strictly changes `root` or stops), then forward via `forward` (breadth-first in table order),
/// capped at kMaxChainWalk members and depth 5.
inline std::vector<Member> BuildChain(const Table& t, int species) {
    std::vector<Member> out;
    if (species <= 0 || species > kMaxSpecies) return out;

    int root = species;
    for (int step = 0; step < 4; ++step) {
        const int p = t.parent[static_cast<std::size_t>(root)];
        if (p == 0 || p == root) break;
        root = p;
    }

    std::vector<int> placed;
    struct Frame {
        int species;
        int depth;
        Edge via;
    };
    std::vector<Frame> queue{{root, 0, Edge{}}};
    while (!queue.empty() && static_cast<int>(out.size()) < kMaxChainWalk) {
        Frame f = queue.front();
        queue.erase(queue.begin());
        if (f.species <= 0 || f.species > kMaxSpecies) continue;
        bool dup = false;
        for (int p : placed) {
            if (p == f.species) { dup = true; break; }
        }
        if (dup) continue;
        placed.push_back(f.species);
        out.push_back({f.species, f.depth, f.via});
        if (f.depth < 5) {
            for (const auto& e : t.forward[static_cast<std::size_t>(f.species)]) {
                if (e.species > 0) queue.push_back({e.species, f.depth + 1, e});
            }
        }
    }
    return out;
}

/// The members the card shows: the whole chain when it fits in kMaxChainMembers; otherwise the
/// first stage plus a run of consecutive later members that contains `species` (the selected
/// entry is always on its own chain).
inline std::vector<Member> VisibleChain(const std::vector<Member>& chain, int species) {
    const int n = static_cast<int>(chain.size());
    if (n <= kMaxChainMembers) return chain;
    int sel = 0;
    for (int i = 0; i < n; ++i)
        if (chain[static_cast<std::size_t>(i)].species == species) sel = i;
    constexpr int kTail = kMaxChainMembers - 1;
    int start = 1;
    if (sel >= start + kTail) start = sel - kTail + 1;
    std::vector<Member> out{chain[0]};
    for (int i = start; i < start + kTail && i < n; ++i) out.push_back(chain[static_cast<std::size_t>(i)]);
    return out;
}

/// Evolution discovery is save-gated: omit every unseen member BEFORE applying the display
/// window. Silhouette slots and a full chain count would disclose undiscovered later stages.
/// The selected entry is already known by the detail-card gate and always stays in the window.
template <typename SeenFn>
inline std::vector<Member> KnownChain(const Table& table, int species, SeenFn&& seen) {
    std::vector<Member> known;
    for (const auto& member : BuildChain(table, species))
        if (member.species == species || seen(member.species)) known.push_back(member);
    // A scrollable branch family starts with its known root and the selected child. This
    // keeps Eevee plus Leafeon/Glaceon/Sylveon in the first three visible cells, without
    // dropping or inventing any discovered sibling or pretending siblings evolve together.
    const auto selected = std::find_if(known.begin(), known.end(),
                                       [&](const Member& member) { return member.species == species; });
    if (known.size() > 1 && selected != known.end() && selected != known.begin() &&
        table.parent[static_cast<std::size_t>(species)] == known.front().species &&
        table.forward[static_cast<std::size_t>(known.front().species)].size() > 1)
        std::rotate(known.begin() + 1, selected, selected + 1);
    return VisibleChain(known, species);
}

/// A separator/method describes only an actual adjacent parent -> child pair. Filtering an
/// unknown middle stage must never imply a direct evolution between the two known endpoints;
/// similarly, two Eevee branches are siblings, not consecutive evolutionary steps.
inline bool ConsecutiveEvolution(const Table& table, const std::vector<Member>& chain, int index) {
    return index > 0 && index < static_cast<int>(chain.size()) &&
           table.parent[static_cast<std::size_t>(chain[static_cast<std::size_t>(index)].species)] ==
               chain[static_cast<std::size_t>(index - 1)].species;
}

/// A branch's own method can still be shown when its known parent is elsewhere in the
/// same row. It is not a separator: only ConsecutiveEvolution may draw a connecting arrow.
inline bool KnownParent(const Table& table, const std::vector<Member>& chain, int index) {
    if (index < 0 || index >= static_cast<int>(chain.size())) return false;
    const int parent = table.parent[static_cast<std::size_t>(chain[static_cast<std::size_t>(index)].species)];
    return parent != 0 && std::any_of(chain.begin(), chain.end(),
                                    [&](const Member& member) { return member.species == parent; });
}

/// Human-readable evolution-method text (shown next to the chevron INTO a member), using real game
/// text where available (item/move names, via the caller-supplied lookups into already-loaded
/// sw_item_names.h/sw_move_names.h tables) and short generic English for the rest. See this file's
/// header comment for the full scanned-method-id coverage; an id outside the switch below falls
/// back to "Special condition" -- never fabricated, never silently blank.
template <typename ItemNameFn, typename MoveNameFn>
inline std::string MethodText(const Edge& e, ItemNameFn&& item_name, MoveNameFn&& move_name) {
    using sw_str::Id;
    const auto S = [](Id id) { return std::string(sw_str::S(id)); };
    const auto lv = [&](int level) { return sw_str::Format(Id::kEvoLevel, level); };
    const std::string level = e.level > 0 ? lv(e.level) : S(Id::kEvoLevelUp);
    switch (e.method) {
        case 1: return S(Id::kEvoFriendship);
        case 2: return S(Id::kEvoFriendshipDay);
        case 3: return S(Id::kEvoFriendshipNight);
        case 4: return level;
        case 5: return S(Id::kEvoTrade);
        case 6: {
            std::string item;
            if (item_name(e.argument, item) && !item.empty()) return sw_str::Format(Id::kEvoTradeHolding, item.c_str());
            return S(Id::kEvoTradeItem);
        }
        case 7: return S(Id::kEvoTradeShelmet);
        case 8:
        case 17:
        case 18: {
            std::string item;
            if (!item_name(e.argument, item) || item.empty()) item = S(Id::kEvoUseItem);
            return item + (e.method == 17 ? S(Id::kEvoMale) : e.method == 18 ? S(Id::kEvoFemale) : std::string{});
        }
        case 9: return level + S(Id::kEvoAtkGtDef);
        case 10: return level + S(Id::kEvoAtkEqDef);
        case 11: return level + S(Id::kEvoAtkLtDef);
        case 14: return level; // Ninjask (the hidden Shedinja companion isn't in this table)
        case 16: return S(Id::kEvoHighBeauty);
        case 19:
        case 20: {
            std::string item;
            if (!item_name(e.argument, item) || item.empty()) item = S(Id::kEvoItem);
            return level + S(Id::kEvoHold) + item + S(e.method == 19 ? Id::kEvoDay : Id::kEvoNight);
        }
        case 21: {
            std::string move;
            if (move_name(e.argument, move) && !move.empty()) return move;
            return S(Id::kEvoKnowsMove);
        }
        case 22: return S(Id::kEvoPartySpecies);
        case 23: return level + S(Id::kEvoMale);
        case 24: return level + S(Id::kEvoFemale);
        case 28: return level + S(Id::kEvoUpsideDown);
        // Sword1.3.2 method29: native783404/783510 requires friendship>=160, then
        // 783520..7835A0 compares each known move's type against the evo argument.
        // evo_133.bin supplies argument17 (Fairy) for Sylveon. The historical generic
        // enum name mentions affection50; that is not Sword's actual threshold.
        case 29: return S(e.argument == 17 ? Id::kEvoFairyMove : Id::kEvoMoveType);
        // Native783324 scans the current party for type16 (Dark), rather than a move type.
        case 30: return level + S(Id::kEvoDarkParty);
        case 31: return level + S(Id::kEvoRain);
        case 32: return level + S(Id::kEvoDay);
        case 33: return level + S(Id::kEvoNight);
        case 34: return level + S(Id::kEvoFemale);
        case 36: return level + S(Id::kEvoVersion);
        case 40: return level + S(Id::kEvoDusk);
        default: return S(Id::kEvoSpecial);
    }
}

} // namespace sw_evolution
