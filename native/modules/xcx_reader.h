// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_abi.h"
#include "xcx_profile.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace xcx {
inline constexpr std::uint64_t TitleId = 0x0100453019AA8000;
inline constexpr std::string_view Build =
    "2A720C7CE5C84905C1C6847211F670FBB31842B6000000000000000000000000";
// The character object's name field: char[0x40] at +0x248, NUL-terminated UTF-8 (contracts/AVATAR.md).
inline constexpr std::size_t NameOffset = 0x248, NameBytes = 0x40;
// The name in `field`, or empty when it is unterminated, empty, has control bytes or bad UTF-8.
std::string DecodeActorName(const std::uint8_t *field, std::size_t n);
// The 0x90-byte character info block of party kind 1 / id 0 at saved-records root + 0xf8.
inline constexpr std::uint64_t AvatarBlockOffset = 0xf8;
inline constexpr std::size_t AvatarBlockSize = 0x90;
struct AvatarLook {
    bool valid{};
    std::uint16_t face{}, hair{}, sex{}, color0{}, color5{};
    bool operator==(const AvatarLook &) const = default;
};
// Validates the block against the avatar actor's name and returns its appearance. Anything that is
// not exactly the avatar's own record (kind 1, id 0, same name and length, sane ids) is refused.
AvatarLook DecodeAvatarBlock(std::span<const std::uint8_t> block, std::string_view actor_name);
// The map-up clockwise heading (radians, in (-pi, pi]) of the game's yaw float (actor+0x64).
float HeadingFromRaw(float raw);
struct Actor {
    bool present{};
    std::string name; // player-party members only (type 2); empty when absent or malformed
    std::uint32_t handle{};
    std::uint16_t id{};
    std::int32_t hp{}, max_hp{}, points{}, max_points{};
    float fuel{}, max_fuel{};
    std::array<float, 3> xyz{};
    // Heading in radians, map-up clockwise (0 = -z, +x right), from the float32 at actor+0x64:
    // yaw = wrap(-(f + pi)) (contracts/PLAYER-HEADING.md). `driven` is the driven-actor bit (+0xd0 & 0x20).
    bool yaw_valid{}, driven{};
    float yaw{};
    // The actor's state machine value [actor+0x1e0]+0x34: 1 while this actor fights (the game's own
    // IsBattle at main+0x3d8330 reads it), 2 otherwise; -1 when unreadable (BATTLE-LIVE-1/-2).
    std::int32_t battle_state{-1};
    // Party members (type 2): the status component's block 2 stats (Party page profile).
    profile::Stats stats;
};
struct Member {
    Actor pc, skell;
    bool controlled{}, owned{};
    // The party record's byte +10 is 0 (with a Skell handle): the member rides its Skell. Correlated
    // live with one board and one exit for all three Skell-owning slots (LIVE-CHECK-4); the guest
    // helper's Return gate reads the same byte. Inferred meaning, not a traced field.
    bool mounted{};
    std::uint8_t owned_index{255}, insurance{};
    std::uint16_t frame{};
    std::string skell_name;
    // The record's six float4 colour groups at +0x364 (UNVERIFIED as the player's paint). Kept raw;
    // only the opt-in player-paint path reads them.
    std::array<float, 24> paint{};
    // Item ids of the record's ten weapon blocks (0x34 bytes from +0x24; 0 = empty), confirmed
    // against the native Skell Weapons screen (SKELL-STILL-MODULE.md); drawn on the Skell picture.
    std::array<std::uint16_t, 10> weapons{};
    // The member's saved character record: level, class, rank, ground gear (xcx_profile.h). !valid
    // when the record is unreadable or fails its checks (the Party page then shows "--").
    profile::Record profile;
};
// FrontierNav segment flags in the saved data (contracts/MAP-REVEAL.md): the flag area F =
// *(main+0x1dc1408) + 0x72e78; category 1 is 1 bit per id at F + (id >> 3) (0x2000 bytes),
// category 2 is 2 bits per id at F + 0x2000 + (id >> 4) * 4 (0x800 bytes). Only these 0x2800
// bytes are read.
inline constexpr std::uint64_t FlagAreaOffset = 0x72e78;
inline constexpr std::size_t FlagBytes = 0x2800, FlagCat2Offset = 0x2000;
struct Snapshot {
    // nla: legacy "zone 1401" flag; the Map page now picks its picture with xcx_map.h (KindForZone).
    bool valid{}, nla{};
    std::uint32_t zone{};
    std::array<Member, 4> party{};
    AvatarLook avatar; // the player's appearance (xcx_avatar.h); !valid -> placeholder portrait
    int controlled{-1};
    std::string area;
    // The segment flag bytes (FlagBytes long) when readable, else empty: the Map page then lights
    // only the continent the player is on.
    std::vector<std::uint8_t> flags;
    // Diagnostics only (never drawn): the field HUD's party rows, H = *(main+0x1df5978), row s at
    // H + 0x1dfb4 + 0xdd0 * s = {hp, max hp, kind (2 TP, 4 GP), points, max points} (BATTLE-LIVE-2).
    struct HudRow {
        std::int32_t hp{-1}, max_hp{-1}, kind{-1}, points{-1}, max_points{-1};
    };
    std::array<HudRow, 4> hud{};
    // Observation only: valid actors can survive loading. No action readiness is inferred.
    bool operator==(const Snapshot &) const = delete;
};
bool SupportsBuild(std::string_view);
bool SupportsBuild(const std::uint8_t *);
Snapshot Read(const EdenDsmodHostApi &);
} // namespace xcx
