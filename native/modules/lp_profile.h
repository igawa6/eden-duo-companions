// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace lp_profile {
using u64 = std::uint64_t;
struct Reloc { u64 slot, target; };
struct GuestRvas {
    u64 IsRunningEvent, UiOnUseFieldItem, UseFieldItem, UiOnFieldWaza, UiSelectWaza,
        UiOnWazaFly, CanUseHidenWaza, UseHidenWaza;
};
struct Profile {
    u64 title_id;
    std::string_view build_id;
    u64 EntityManagerTypeInfo, EvDataManagerTypeInfo, PlayerWorkTypeInfo, FieldManagerTypeInfo,
        BattleViewCoreGetInstance, PoketchWindowTypeInfo, SwayGrassTypeInfo, ItemWorkTypeInfo;
    std::array<Reloc, 4> Fingerprints;
    GuestRvas guest;
    u64 AudioManagerTypeInfo, AudioPlaySe;
    u64 ConfirmIrekae;
    u64 DemoSceneManagerTypeInfo;
};

// Exact 1.3.0 base executables. LP retains these build IDs. Metadata roots and method RVAs
// are joined by name AND signature in IL2CPP dumps; overloads must not be matched by name only.
// PlayerWorkTypeInfo names the PlayerWork.SaveData_TypeInfo slot used by the existing reader.
inline constexpr Profile Diamond{
    UINT64_C(0x0100000011D90000),
    "94CEAE325C205C4B9D6F7235552F28FD00000000000000000000000000000000",
    0x4C59D10, 0x4C59C50, 0x4C64DC0, 0x4C5A638, 0x4C676C0, 0x4C5EEB8, 0x4C5AC58, 0x4C59C20,
    {{{0x4932BA0, 0x1020}, {0x49AE2E8, 0x1984340}, {0x4ABD130, 0x1BDBC0}, {0x4C52418, 0x172FE60}}},
    {0x2C423A0, 0x17A1750, 0x17A17D0, 0x17A2320, 0x17A24D0, 0x17A3410,
     0x1DBC040, 0x1DBBC90},
    0x4C59B78, 0x21EB8D0, 0x1F5A310, 0x4C5E5A0
};
inline constexpr Profile Pearl{
    UINT64_C(0x010018E011D92000),
    "38F59CBDA2EB9C44B72F94C4D25935A200000000000000000000000000000000",
    0x4E70DE8, 0x4E70D28, 0x4E7BE98, 0x4E71710, 0x4E7E798, 0x4E75F90, 0x4E71D30, 0x4E70CF8,
    // Four R_AARCH64_RELATIVE .data pointers into .text, sampled across the relocation table.
    {{{0x46DA3A0, 0x23CE4}, {0x49AEC58, 0x1E324D0}, {0x4ABD948, 0x258720}, {0x4C52418, 0x172FE50}}},
    {0x1B04F90, 0x1BD49E0, 0x1BD4A60, 0x1BD55B0, 0x1BD5760, 0x1BD66A0,
     0x22249C0, 0x2224610},
    0x4E70C50, 0x25EE490, 0x23BB7D0, 0x4E75678
};

#if defined(LP_PEARL) && LP_PEARL
inline constexpr const Profile& Active = Pearl;
#else
inline constexpr const Profile& Active = Diamond;
#endif
} // namespace lp_profile
