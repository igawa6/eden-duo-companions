// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Sword1.3.2 native roster: client+70 -> pass, pass+8 -> roster (831920).
// 7DF3C0 returns roster+8+participant*38; its six mon pointers precede count byte+30.
// 7DF3F0 indexes the separate pokeID pointer table at roster+120 (30 entries).
// IDs are not participant*6: the first real offline raid placed participant1 at pokeID12
// and participant2 at pokeID6. Read the actual pointers and validate their native IDs.
// Native7F8110: rule2=raid. This first supported offline fixture uses submode5, competitor0,
// comm0. Other raid formats stay withheld until their own native/live verification.

#include <array>
#include <cstdint>

#include "sw_battle.h"

namespace sw_battle_raid {
// Two independent raid boots (main80BC4000/805EC000), different NPC rosters including an
// ally/boss duplicate species. Native HP changes, ownDmax0->1 and elapsed0->1->2, and barrier
// inactive->raised->depleted observed live. Three-turn expiry is additionally
// established by native8A4930/8A3940; that raid ended before elapsed3 was observed.
inline constexpr bool kRuntimeVerified = true;
inline constexpr bool kDmaxVerified = true;
inline constexpr bool kShieldVerified = true;
inline constexpr std::size_t kRosterBytes = 0x210;
inline constexpr int kParticipants = 5;
inline constexpr int kBossParticipant = 4;
inline constexpr int kAllies = 3;

inline std::uint64_t U64(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(p[i]) << (i * 8);
    return value;
}
struct Member {
    sw_battle::Mon mon;
    std::uint64_t address = 0;
    std::uint64_t extension = 0;
    unsigned poke_id = 30;
    bool dmax = false;
    unsigned dmax_elapsed = 0;
    bool boss = false;
    bool valid = false;
};
inline Member DecodeMember(const std::uint8_t* data, std::uint64_t address) {
    Member member;
    member.mon = sw_battle::Decode(data);
    member.address = address;
    member.extension = U64(data + 0x278); // native7E0B60
    member.poke_id = data[0x7D]; // native7E0A00
    member.boss = data[0x273] == 1; // native7E0B50; excludes ordinary Dynamax expiry
    member.dmax = data[0x298] == 1; // native7E0A10, set/reset7E5920/7E5B10
    member.dmax_elapsed = data[0x299]; // native7E5C20/7E5C30, capped at native8A3940=3
    member.valid = address && member.mon.valid && member.poke_id < 30 && data[0x273] <= 1 &&
                   data[0x298] <= 1 && member.dmax_elapsed <= 3;
    return member;
}
struct Shield {
    bool valid = false;
    bool active = false;
    unsigned total = 0, remaining = 0;
};
inline Shield DecodeShield(const std::uint8_t* data) {
    Shield shield;
    if (data[0] > 1 || data[2] > data[1] || (data[0] && data[1] == 0)) return shield;
    shield.valid = true;
    // Native83DB60: the raised marker stays set after all segments break (83DB90).
    shield.active = data[0] == 1 && data[2] != 0;
    shield.total = data[1]; // native83DBB0
    shield.remaining = data[2]; // native83DBC0; UI adapter8C7B80 exposes this while active
    return shield;
}
struct Snapshot {
    bool kind_known = false;
    bool active = false;
    bool ready = false;
    unsigned own_participant = 5;
    std::array<Member, kParticipants> participants{};
    Shield shield;
};

// The caller supplies a stable read. A changed root/table invalidates the entire snapshot;
// no Pokémon is guessed by species or heap stride and no old raid data is retained.
template <typename ReadFn>
inline Snapshot Read(std::uint64_t client, ReadFn&& read) {
    Snapshot out;
    std::uint64_t main = 0, setup = 0, pass = 0, roster = 0, recheck = 0;
    std::uint32_t rule = 0, competitor = 0;
    std::uint8_t comm = 0, mode = 0, own = 0;
    if (!client || !read(client + 0x58, &main, sizeof main) || !main ||
        !read(main + 0x150, &rule, sizeof rule) || rule > 2) return out;
    if (rule != 2) { out.kind_known = true; return out; }
    // Recognize native rule2 even if its specific roster format is unsupported. That prevents
    // callers from falling back to a previous single battle's heap-relative Pokémon data.
    out.kind_known = true;
    out.active = true;
    if (!read(main + 0x88, &setup, sizeof setup) || !setup ||
        !read(setup, &competitor, sizeof competitor) || competitor != 0 ||
        !read(setup + 0x3B8, &comm, sizeof comm) || comm != 0 ||
        !read(setup + 0x3B9, &mode, sizeof mode) || mode != 5 ||
        !read(client + 0x2A6, &own, sizeof own) || own != 0) return out;
    out.own_participant = own;
    if (!read(client + 0x70, &pass, sizeof pass) || !pass ||
        !read(pass + 8, &roster, sizeof roster) || !roster) return out;
    std::array<std::uint8_t, kRosterBytes> raw{}, again{};
    if (!read(roster, raw.data(), raw.size()) || U64(raw.data()) != main) return out;
    for (int participant = 0; participant < kParticipants; ++participant) {
        const auto* party = raw.data() + 8 + participant * 0x38;
        // Exactly one member is the independently tested offline raid roster shape.
        if (party[0x30] != 1) return out;
        const auto pointer = U64(party);
        std::array<std::uint8_t, sw_battle::kStructBytes> mon_raw{};
        if (!pointer || !read(pointer, mon_raw.data(), mon_raw.size())) return out;
        auto member = DecodeMember(mon_raw.data(), pointer);
        if (!member.valid || (participant == 0 && member.poke_id >= 6) ||
            member.boss != (participant == kBossParticipant) ||
            U64(raw.data() + 0x120 + member.poke_id * 8) != pointer) return out;
        for (int previous = 0; previous < participant; ++previous)
            if (out.participants[previous].address == pointer) return out;
        out.participants[participant] = member;
    }
    if (!read(roster, again.data(), again.size()) || raw != again ||
        !read(pass + 8, &recheck, sizeof recheck) || recheck != roster ||
        !read(client + 0x70, &recheck, sizeof recheck) || recheck != pass ||
        !read(client + 0x58, &recheck, sizeof recheck) || recheck != main) return out;
    out.ready = true;
    const auto& boss = out.participants[kBossParticipant];
    std::uint64_t shield_pointer = 0;
    std::array<std::uint8_t, 6> shield_raw{};
    if (boss.extension && read(boss.extension, &shield_pointer, sizeof shield_pointer) && shield_pointer &&
        read(shield_pointer, shield_raw.data(), shield_raw.size()) &&
        read(boss.extension, &recheck, sizeof recheck) && recheck == shield_pointer &&
        read(boss.address + 0x278, &recheck, sizeof recheck) && recheck == boss.extension)
        out.shield = DecodeShield(shield_raw.data());
    return out;
}
} // namespace sw_battle_raid
