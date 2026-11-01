// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fly destinations (Flying Taxi landings), in the Town Map's own order. Generated 2026-10-26 from
// the live Town Map's destination table (town map object +0x39F8, stride 0x30: zoneHash,
// flyLandingHash; 45 entries, kind 0 Galar / 1 Isle of Armor / 2 Crown Tundra -- the same pairs
// romfs bin/appli/townmap/bin/map_data.prmb carries) on Pokemon Sword 1.3.2. zone = FNV-1a 64 of
// the zone id ("z_t0101"). No names (F13, 2026-11-01: no game text in the module): a row shows its
// zone's Town Map name from the player's romfs (script/place_name; z_r1002 under z_r1001's,
// z_c0306 under the Town Map facility "townmap_factory_33"), and `number` is the landing's place
// among its zone's landings, in table order (the Town Map itself shows the same place name for
// each; 2 and 3 get a " (2)" / " (3)").
// Which ones the player may fly to is never taken from here: the guest helper asks the game
// (landing lookup + the destination's own unlock flag, FLY_QUERY) -- only those are listed.

#include <array>
#include <cstdint>

#include "sw_strings.h"

namespace sw_fly {

using sw_str::Region;
struct Destination {
    std::uint64_t zone;
    std::uint64_t landing;
    std::uint8_t number; // 1 = the zone's first landing
    Region region;
};

inline constexpr std::array<Destination, 45> kDestinations{{
    {0x019B5DC049D8CB3Eull, 0x5EDD6BB072AE66DBull, 1, Region::kGalar}, // z_t0101
    {0xE80BCEC03B7D6E23ull, 0xCFF6996163EBAFCCull, 1, Region::kGalar}, // z_t0201
    {0x10355EFF1F4DB0B5ull, 0xA079099395D8B4DEull, 1, Region::kGalar}, // z_r0201
    {0xE0D6E5E78C91F4A7ull, 0xDB057456848C4478ull, 1, Region::kGalar}, // z_c0101
    {0xE0D6E5E78C91F4A7ull, 0x82521C7C2333D0CFull, 2, Region::kGalar}, // z_c0101
    {0xF12207C040C1CE88ull, 0xBB06FDCB8FE6C73Dull, 1, Region::kGalar}, // z_t0301
    {0xFC1954E79C5EAFE2ull, 0x6242689510BB7B17ull, 1, Region::kGalar}, // z_c0201
    {0xFC1954E79C5EAFE2ull, 0x957FD02CE2E7B36Bull, 2, Region::kGalar}, // z_c0201
    {0xFC1954E79C5EAFE2ull, 0x957FCF2CE2E7B1B8ull, 3, Region::kGalar}, // z_c0201
    {0x23D489C05D1C60CAull, 0x06F4FE3A8A9AAB9Full, 1, Region::kGalar}, // z_t0501
    {0x0AB1FAC04F1DD58Full, 0x5A11EBC739DDB9C0ull, 1, Region::kGalar}, // z_t0601
    {0x13C833C0546235F4ull, 0x149841D79FBAE6B1ull, 1, Region::kGalar}, // z_t0701
    {0x4BFDFBFF40EC7062ull, 0x7603F017C373C097ull, 1, Region::kGalar}, // z_r0903
    {0xF3033BE7971A85DDull, 0xF6D2435DD4EB7346ull, 1, Region::kGalar}, // z_c0301
    {0xF3033BE7971A85DDull, 0xA94B6DAAC6F64871ull, 2, Region::kGalar}, // z_c0301
    {0xF30334E7971A79F8ull, 0xFA6FBB61040CE20Dull, 1, Region::kGalar}, // z_c0306
    {0x776775717EA4468Bull, 0x460CF9907E1A6BB4ull, 1, Region::kWildArea}, // z_wr0101
    {0x776779717EA44D57ull, 0x5262127BB7E43408ull, 1, Region::kWildArea}, // z_wr0105
    {0x776E7E717EAA7484ull, 0x2375C86B5A9C4AE1ull, 1, Region::kWildArea}, // z_wr0122
    {0x7771EA717EAD61DFull, 0x65131852CCF38130ull, 1, Region::kWildArea}, // z_wr0134
    {0x194B97FF2492111Aull, 0x58E6C75D25F75040ull, 1, Region::kGalar}, // z_r0301
    {0xEDFC32FF0C0A1B29ull, 0x2F2AA741DC41885Aull, 1, Region::kGalar}, // z_r0601
    {0xF55F6BFF0FDCE70Eull, 0x5386FD96998C090Bull, 1, Region::kGalar}, // z_r0701
    {0x449AE0FF3D19D777ull, 0x698C5EA321B6F168ull, 1, Region::kGalar}, // z_r0801
    {0x4BFDF6FF40EC67E3ull, 0xE4C082430207EA8Cull, 2, Region::kGalar}, // z_r0904
    {0xB332920807F9D2D7ull, 0x9CC8529911FBF088ull, 1, Region::kGalar}, // z_r1002
    {0x1C7150C0594994E5ull, 0x6F3B39858EE2BC6Eull, 1, Region::kGalar}, // z_t0401
    {0xDBCF5CFF0180B073ull, 0x75E9D7C8F7595BFCull, 1, Region::kGalar}, // z_r0401
    {0xE4E595FF06C510D8ull, 0x3E0AF22F51E4EEB2ull, 1, Region::kGalar}, // z_r0501
    {0xE4E595FF06C510D8ull, 0xE4B672670516582Dull, 2, Region::kGalar}, // z_r0501
    {0x908A64718CA374E6ull, 0xF1731467F304CC63ull, 1, Region::kArmor}, // z_wr0201
    {0x908A64718CA374E6ull, 0x2D6A0F2CC555980Cull, 2, Region::kArmor}, // z_wr0201
    {0x908A69718CA37D65ull, 0x9E0FB81896B0B9EEull, 1, Region::kArmor}, // z_wr0204
    {0x908A67718CA379FFull, 0x764F22108611FA10ull, 1, Region::kArmor}, // z_wr0206
    {0x908A6C718CA3827Eull, 0xB66F70E03D0EB41Bull, 1, Region::kArmor}, // z_wr0209
    {0x87E14B7187BC1CC1ull, 0xD9735E8D79E03D32ull, 1, Region::kCrown}, // z_wr0301
    {0x87E14B7187BC1CC1ull, 0xEE2CC97AD63B2BE5ull, 2, Region::kCrown}, // z_wr0301
    {0x87E1487187BC17A8ull, 0x9FF31ABEA0B7701Dull, 1, Region::kCrown}, // z_wr0302
    {0x87E14E7187BC21DAull, 0x845A9ABB01878DCFull, 1, Region::kCrown}, // z_wr0304
    {0x87E14E7187BC21DAull, 0x6A43CD5D8C5C0A80ull, 2, Region::kCrown}, // z_wr0304
    {0x87E14C7187BC1E74ull, 0x436CC06AA788E431ull, 1, Region::kCrown}, // z_wr0306
    {0x87E1437187BC0F29ull, 0xE4D27BB715CF045Aull, 1, Region::kCrown}, // z_wr0309
    {0x87E4517187BE5CCAull, 0xC8163FE47A887F9Full, 1, Region::kCrown}, // z_wr0313
    {0x87DA417187B5ED15ull, 0x262213ECE8C9D9FEull, 1, Region::kCrown}, // z_wr0323
    {0x87E14C7187BC1E74ull, 0xF42C76B50056046Eull, 2, Region::kCrown}, // z_wr0306
}};

} // namespace sw_fly
