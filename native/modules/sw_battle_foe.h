// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Every ACTIVE battler of a regular (non-raid) single or double battle, by battle position, from
// the native battle roster -- singles, one-trainer doubles, two-trainer doubles (1 vs 2) and
// multi battles (2 vs 2 with an NPC partner).
//
// Roster (same as the raid reader, sw_battle_raid.h): client+0x58 -> main, client+0x70 -> pass,
// pass+8 -> roster; roster+0 == main; roster+8+client*0x38 = that client's battle party
// {u64 bpp ptrs[6], u8 count at +0x30} (main+0x7DF3C0(roster, client)); roster+0x120 = the
// pokeID -> bpp table (30 entries). PokeIDs are NOT client*6 (a raid placed client 1 at pokeID 12
// and client 2 at pokeID 6), so they are read and validated, never derived.
//
// Positions (analysis/20261026-review-fixes-re, Ghidra main+0x7EE550(rule, multiMode, pos) =
// the client owning battle position pos; main+0x7F9310 counts a client's positions):
//   rule 0 (single)            [0, 1]
//   rule 1 mode 0 (double)     [0, 1, 0, 1]   one trainer per side, two positions each
//   rule 1 modes 1..3 (multi)  [0, 1, 2, 3]   four clients, one position each
//   rule 1 mode 4              [0, 1, 0, 3]   you (two positions) vs two trainers
//   rule 1 mode 5              [0, 1, 2, -]   (sw_battle_field.h's table)
// rule = main+0x150, multiMode = u8 setup+0x3B9 (setup = main+0x88). Even positions are side 0,
// odd side 1; your side is your client's (client+0x2A6) first position. A client's party lists
// its active battlers first, in position order (live Tower doubles 2026-11-01: party[0] = pos 1,
// party[1] = pos 3; owner Thor 2026-11-01 two-trainer double: two foe parties of one Pokemon).
// On screen (target select and the top HP plates), the far row is pos 3 (left), pos 1 (right) and
// the near row pos 0 (left), pos 2 (right), for your side 0 (mirrored for side 1).
//
// Every value is validated and the read is fail closed: the own party pointer (client+0x340) must
// be the own client's roster party, every member pointer must match the roster's own pokeID table
// at its decoded pokeID, members are unique across all parties read, and the roster/pass/main
// chain must re-read identically. An unknown rule/mode or anything else -> ready=false (the
// caller keeps its fixed-lead fallback).

#pragma once

#include <array>
#include <cstdint>

#include "sw_battle.h"

namespace sw_battle_foe {

inline constexpr std::size_t kRosterBytes = 0x210;
inline constexpr int kClients = 5;
inline constexpr int kPositions = 4;
inline constexpr int kPokeIds = 30;

inline std::uint64_t U64(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(p[i]) << (i * 8);
    return value;
}

// main+0x7EE550 for rules 0 and 1: the client owning each position, -1 = no battler there.
// Returns false for a rule/mode this reader does not know (raids have their own reader).
inline bool PositionClients(int rule, int mode, std::array<int, kPositions>& out) {
    if (rule == 0) out = {0, 1, -1, -1};
    else if (rule == 1 && mode == 0) out = {0, 1, 0, 1};
    else if (rule == 1 && mode >= 1 && mode <= 3) out = {0, 1, 2, 3};
    else if (rule == 1 && mode == 4) out = {0, 1, 0, 3};
    else if (rule == 1 && mode == 5) out = {0, 1, 2, -1};
    else return false;
    return true;
}

// Screen slots: 0 far-left, 1 far-right, 2 near-left, 3 near-right (the target-select order,
// sw_battle_cursor.h) -> battle position, for your side.
inline int PositionForSlot(int slot, int own_side) {
    static constexpr int kSide0[4] = {3, 1, 0, 2};
    if (slot < 0 || slot > 3 || own_side < 0 || own_side > 1) return -1;
    return own_side == 0 ? kSide0[slot] : kSide0[slot] ^ 1;
}

struct Member {
    bool valid = false;
    sw_battle::Mon mon;
    std::uint64_t address = 0; // the bpp the roster lists
    unsigned poke_id = 0;
};

struct Party {
    int count = 0;     // members, actives included (0 = the client owns no position)
    int positions = 0; // how many positions the client holds (its first `positions` members are active)
    std::array<Member, 6> members{};
};

struct Battler {
    bool valid = false;
    int client = -1;
    int index = -1; // index in its client's party
    Member member;
};

struct Snapshot {
    bool ready = false;
    int rule = -1, mode = -1;
    int own_client = -1, own_side = -1;
    std::array<int, kPositions> clients{-1, -1, -1, -1};
    std::array<Battler, kPositions> positions{}; // by battle position
    std::array<Party, kClients> parties{};       // by client (only clients that own a position)

    const Battler& Slot(int slot) const { // screen slot (PositionForSlot)
        static const Battler kNone{};
        const int pos = PositionForSlot(slot, own_side);
        return pos >= 0 && pos < kPositions ? positions[static_cast<std::size_t>(pos)] : kNone;
    }
    bool IsFoeClient(int client) const {
        for (int p = 0; p < kPositions; ++p)
            if (clients[static_cast<std::size_t>(p)] == client) return (p & 1) != own_side;
        return false;
    }
    // The own client's partner (multi battles): the near battler another client owns.
    const Battler& Partner() const {
        for (const int slot : {2, 3}) {
            const auto& b = Slot(slot);
            if (b.valid && b.client != own_client) return b;
        }
        static const Battler kNone{};
        return kNone;
    }
};

template <typename ReadFn>
inline Snapshot Read(std::uint64_t client, ReadFn&& read) {
    Snapshot out;
    std::uint64_t main = 0, pass = 0, roster = 0, own_party = 0, setup = 0, recheck = 0;
    std::uint32_t rule = 0;
    std::uint8_t own_client = 0xFF, mode = 0;
    if (!client || !read(client + 0x58, &main, sizeof main) || !main ||
        !read(client + 0x70, &pass, sizeof pass) || !pass ||
        !read(pass + 8, &roster, sizeof roster) || !roster ||
        !read(client + 0x340, &own_party, sizeof own_party) || !own_party ||
        !read(client + 0x2A6, &own_client, 1) || own_client >= kClients ||
        !read(main + 0x150, &rule, sizeof rule) || rule > 1) return out;
    bool mode_known = rule == 0;
    if (rule == 1 && read(main + 0x88, &setup, sizeof setup) && setup && read(setup + 0x3B9, &mode, 1))
        mode_known = true;
    std::array<std::uint8_t, kRosterBytes> raw{}, again{};
    if (!read(roster, raw.data(), raw.size()) || U64(raw.data()) != main) return out;
    // The own party pointer must be the own client's roster party: proves the layout and the id.
    if (roster + 8 + static_cast<std::uint64_t>(own_client) * 0x38 != own_party) return out;
    const auto party_count = [&](int c) { return static_cast<int>(raw[8 + c * 0x38 + 0x30]); };
    // The table must match the roster: every client owning a position has a party. A double
    // whose setup is unreadable, or whose table disagrees with the roster, is accepted only as
    // multiMode 0 with no party beyond clients 0 and 1 (fail closed otherwise).
    const auto consistent = [&](int m, std::array<int, kPositions>& clients) {
        if (!PositionClients(static_cast<int>(rule), m, clients)) return false;
        for (const int c : clients)
            if (c >= 0 && (party_count(c) < 1 || party_count(c) > 6)) return false;
        return true;
    };
    if (!mode_known || !consistent(mode, out.clients)) {
        for (int c = 2; c < kClients; ++c)
            if (party_count(c) != 0) return out;
        mode = 0;
        if (!consistent(mode, out.clients)) return out;
    }
    out.rule = static_cast<int>(rule);
    out.mode = mode;
    out.own_client = own_client;
    for (int p = 0; p < kPositions; ++p)
        if (out.clients[static_cast<std::size_t>(p)] == own_client) { out.own_side = p & 1; break; }
    if (out.own_side < 0) return out;

    std::array<std::uint64_t, 4 * 6> seen{};
    int seen_n = 0;
    for (int p = 0; p < kPositions; ++p) {
        const int c = out.clients[static_cast<std::size_t>(p)];
        if (c < 0) continue;
        auto& party = out.parties[static_cast<std::size_t>(c)];
        if (party.positions++ > 0) continue; // already read for its first position
        const auto* entry = raw.data() + 8 + c * 0x38;
        party.count = entry[0x30];
        if (party.count < 1 || party.count > 6) return out;
        for (int i = 0; i < party.count; ++i) {
            const auto ptr = U64(entry + i * 8);
            for (int j = 0; j < seen_n; ++j)
                if (seen[static_cast<std::size_t>(j)] == ptr) return out;
            seen[static_cast<std::size_t>(seen_n++)] = ptr;
            std::array<std::uint8_t, sw_battle::kStructBytes> mon_raw{};
            if (!ptr || !read(ptr, mon_raw.data(), mon_raw.size())) return out;
            const unsigned id = mon_raw[0x7D]; // native7E0A00 pokeID
            Member m;
            m.mon = sw_battle::Decode(mon_raw.data());
            if (!m.mon.valid || id >= kPokeIds || U64(raw.data() + 0x120 + id * 8) != ptr) return out;
            m.valid = true;
            m.address = ptr;
            m.poke_id = id;
            party.members[static_cast<std::size_t>(i)] = m;
        }
    }
    for (int p = 0; p < kPositions; ++p) {
        const int c = out.clients[static_cast<std::size_t>(p)];
        if (c < 0) continue;
        int index = 0;
        for (int q = 0; q < p; ++q) index += out.clients[static_cast<std::size_t>(q)] == c ? 1 : 0;
        const auto& party = out.parties[static_cast<std::size_t>(c)];
        auto& b = out.positions[static_cast<std::size_t>(p)];
        if (index >= party.count) continue; // e.g. a doubles side down to its last Pokemon
        b.valid = true;
        b.client = c;
        b.index = index;
        b.member = party.members[static_cast<std::size_t>(index)];
    }
    if (!read(roster, again.data(), again.size()) || raw != again ||
        !read(pass + 8, &recheck, sizeof recheck) || recheck != roster ||
        !read(client + 0x70, &recheck, sizeof recheck) || recheck != pass ||
        !read(client + 0x58, &recheck, sizeof recheck) || recheck != main) return out;
    out.ready = true;
    return out;
}

} // namespace sw_battle_foe
