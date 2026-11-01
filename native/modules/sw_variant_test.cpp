// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-only tests for the game-variant record (sw_variant.h / the generated sw_variant_data.h):
// the Sword record's identity, the build gate that reads it, and every main offset the module
// compares against pinned at the value it had before the variant seam existed (Sword 1.3.2), so
// a regenerated record that moved one fails here, not on a live boot. Built a second time with
// SW_VARIANT_SHIELD=1 (dsmod-pokemon-shield-variant): the same checks for the Shield record, its
// offsets pinned at the values the Shield 1.3.2 scan verified (sw_variants.py SHIELD_132).

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "sw_addresses.h"
#include "sw_battle_cursor.h"
#include "sw_build.h"
#include "sw_dex_text.h"
#include "sw_overworld.h"
#include "sw_route.h"
#include "sw_trainer_portrait.h"
#include "sw_variant.h"

namespace {

using sw_variant::kActive;
using sw_variant::kBuild;
using sw_variant::kMain;

#if !(defined(SW_VARIANT_SHIELD) && SW_VARIANT_SHIELD)
void TestSwordRecord() {
    static_assert(&kActive == &sw_variant::kSword, "Sword is the default (and only) variant");
    static_assert(kActive.title_id == UINT64_C(0x0100ABF008968000));
    static_assert(sw_build::kTitleId == sw_build::kSwordTitleId);
    static_assert(sw_build::kTitleId != sw_build::kShieldTitleId);
    static_assert(kActive.aoc_title_ids[0] == UINT64_C(0x0100ABF008969001));
    static_assert(kActive.aoc_title_ids[1] == UINT64_C(0x0100ABF008969002));
    static_assert(kActive.save_game_version == 44); // PKHeX GameVersion.SW
    static_assert(kActive.save_game_version - 44 == 0); // the League Card's TrainerCard8 +0x24: Sword
    assert(std::string_view(kActive.name) == "Pokemon Sword");
    assert(std::string_view(SW_VARIANT_NAME) == kActive.name);
    assert(std::string_view(kActive.module_name) == "Pokemon Sword DSMod");
    assert(std::string_view(kActive.dex_comment_file) == "zukan_comment_A.dat");
    assert(sw_dex_text::ZukanCommentDatPath() == sw_lang::MessagePath("zukan_comment_A.dat"));
    assert(std::string_view(SW_VARIANT_ROUTE_ENCOUNTERS) == "sw_route_encounters.h");
    // The route tables the variant selected are Sword's (encount_k): Route 2's Seedot (273).
    bool seedot = false;
    for (const auto& slot : sw_route_encounters::kSlots) seedot |= slot.species == 273;
    assert(seedot);
}

void TestBuild() {
    static constexpr unsigned char kId[20]{0xA3, 0xB7, 0x5B, 0xCD, 0x33, 0x11, 0x38, 0x5A, 0xEE, 0xD6,
                                           0x7F, 0xBE, 0xEB, 0x79, 0xCB, 0xB7, 0xBF, 0x02, 0xF4, 0x71};
    static_assert(sw_build::kKnownBuildIdBytes == 20);
    assert(std::memcmp(kBuild.id, kId, sizeof kId) == 0);
    assert(&sw_build::kBuildId == &kBuild.id);
    assert(std::string_view(kBuild.id16) == "A3B75BCD3311385A");
    assert(std::string_view(sw_build::kBuildId16) == "A3B75BCD3311385A");
    assert(std::string_view(kBuild.version) == "1.3.2");
    static_assert(kBuild.image_span == 0x2A00000);

    // The gate reads the record: the supported build, the recognised-only 1.3.1, anything else.
    unsigned char id[32]{};
    std::memcpy(id, kId, sizeof kId);
    assert(sw_build::BuildLabel(id) && *sw_build::BuildLabel(id) == '\0');
    assert(sw_build::SupportsBuildHex("A3B75BCD3311385AEED67FBEEB79CBB7BF02F471000000000000000000000000"));
    assert(sw_build::SupportsBuildHex("a3b75bcd3311385aeed67fbeeb79cbb7bf02f471"));
    assert(!sw_build::SupportsBuildHex("A3B75BCD3311385AEED67FBEEB79CBB7BF02F4"));  // short
    assert(kActive.other_build_count == 1);
    static constexpr unsigned char k131[8]{0x46, 0x28, 0xA5, 0x12, 0xB1, 0xD9, 0x3D, 0x37};
    unsigned char old[32]{};
    std::memcpy(old, k131, sizeof k131);
    assert(sw_build::BuildLabel(old) && std::string_view(sw_build::BuildLabel(old)) == "1.3.1");
    assert(sw_build::SupportsBuildHex("4628A512B1D93D370000000000000000000000000000000000000000000000"));
    unsigned char other[32]{};
    other[0] = 0x01;
    assert(!sw_build::BuildLabel(other));
    assert(!sw_build::SupportsBuildHex("0100000000000000000000000000000000000000"));
}

// Every main offset the module used as a literal before the seam (Sword 1.3.2, main A3B75BCD).
void TestMainOffsetsUnchanged() {
    static_assert(kMain.battle_command_wait_fn == 0x8021D0);
    static_assert(kMain.battle_fight_fn == 0x803590);
    static_assert(kMain.battle_bag_fn == 0x8039C0);
    static_assert(kMain.battle_pokemon_fn == 0x8032C0);
    static_assert(kMain.battle_forced_party_fn == 0x7FF830);
    static_assert(kMain.vt_ui_root == 0x2568630);
    static_assert(kMain.vt_command_view == 0x252DDB8);
    static_assert(kMain.vt_move_view == 0x252DC10);
    static_assert(kMain.vt_grid_list == 0x25BC8F0);
    static_assert(kMain.vt_grid_cursor == 0x25BD1F8);
    static_assert(kMain.vt_ball_view == 0x252E320);
    static_assert(kMain.vt_target_view == 0x252F138);
    static_assert(kMain.vt_layout == 0x260D1F0);
    static_assert(kMain.vt_layout_holder == 0x25B9D20);
    static_assert(kMain.vt_layout_work == 0x250F478);
    static_assert(kMain.game_data_slot == 0x2610798);
    static_assert(kMain.field_slot == 0x2610848);
    static_assert(kMain.world_slot == 0x26194D8);
    static_assert(kMain.zone_registry_slot == 0x2617C48);
    static_assert(kMain.player_type_getters[0] == 0xDA2300);
    static_assert(kMain.player_type_getters[1] == 0xDA2390);
    static_assert(kMain.player_type_getters[2] == 0xDA2490);
    static_assert(kMain.portrait_task_head_fn == 0x15306F0);
    static_assert(kMain.portrait_app_fn == 0x15360C0);
    static_assert(kMain.portrait_wrapper_fn == 0x159D610);
    static_assert(kMain.portrait_target_fn == 0x5FE5F0);

    // ... and the names the readers use resolve to them.
    static_assert(sw_battle_cursor::kCommandWaitFn == 0x8021D0);
    static_assert(sw_battle_cursor::kFightFn == 0x803590);
    static_assert(sw_battle_cursor::kBagFn == 0x8039C0);
    static_assert(sw_battle_cursor::kUiRootVtable == 0x2568630);
    static_assert(sw_battle_cursor::kCommandViewVtable == 0x252DDB8);
    static_assert(sw_battle_cursor::kMoveViewVtable == 0x252DC10);
    static_assert(sw_battle_cursor::kListVtable == 0x25BC8F0);
    static_assert(sw_battle_cursor::kCursorVtable == 0x25BD1F8);
    static_assert(sw_battle_cursor::kBallViewVtable == 0x252E320);
    static_assert(sw_battle_cursor::kTargetViewVtable == 0x252F138);
    static_assert(sw_battle_cursor::kLayoutVtable == 0x260D1F0);
    static_assert(sw_battle_cursor::kHolderVtable == 0x25B9D20);
    static_assert(sw_battle_cursor::kLayoutWorkVtable == 0x250F478);
    static_assert(sw_overworld::kPlayerTypeGetters[0] == 0xDA2300);
    static_assert(sw_overworld::kPlayerTypeGetters[1] == 0xDA2390);
    static_assert(sw_overworld::kPlayerTypeGetters[2] == 0xDA2490);
    // The heap literal the League Card owner check used is the anchor table's MyStatus8 TID/SID.
    static_assert(sw_addresses::OffsetOf("mystatus.tid_sid") == 0x45068FB8);
    static_assert(sw_trainer_portrait::kOwnerOffset == 0x45068FB8);
}
#endif

#if defined(SW_VARIANT_SHIELD) && SW_VARIANT_SHIELD
void TestShieldRecord() {
    static_assert(&kActive == &sw_variant::kShield, "SW_VARIANT_SHIELD=1 selects the Shield record");
    static_assert(kActive.title_id == UINT64_C(0x01008DB008C2C000));
    static_assert(sw_build::kTitleId == sw_build::kShieldTitleId);
    static_assert(sw_build::kTitleId != sw_build::kSwordTitleId);
    static_assert(kActive.aoc_title_ids[0] == UINT64_C(0x01008DB008C2D001));
    static_assert(kActive.aoc_title_ids[1] == UINT64_C(0x01008DB008C2D002));
    static_assert(kActive.save_game_version == 45); // PKHeX GameVersion.SH
    static_assert(kActive.save_game_version - 44 == 1); // the League Card's TrainerCard8 +0x24: Shield
    assert(std::string_view(kActive.name) == "Pokemon Shield");
    assert(std::string_view(SW_VARIANT_NAME) == kActive.name);
    assert(std::string_view(kActive.module_name) == "Pokemon Shield DSMod");
    assert(std::string_view(kActive.dex_comment_file) == "zukan_comment_B.dat");
    assert(sw_dex_text::ZukanCommentDatPath() == sw_lang::MessagePath("zukan_comment_B.dat"));
    assert(std::string_view(SW_VARIANT_ROUTE_ENCOUNTERS) == "sw_route_encounters_shield.h");
    // The route tables the variant selected are Shield's (encount_t): Route 2 has Lotad (270),
    // not Sword's Seedot (273).
    bool lotad = false, seedot = false;
    for (const auto& slot : sw_route_encounters::kSlots) {
        lotad |= slot.species == 270;
        seedot |= slot.species == 273;
    }
    assert(lotad && !seedot);
}

void TestShieldBuild() {
    static constexpr unsigned char kId[20]{0xA1, 0x68, 0x02, 0x62, 0x5E, 0x78, 0x26, 0xBF, 0x83, 0xB6,
                                           0xF9, 0x70, 0x8E, 0x47, 0x5B, 0x91, 0x2A, 0x9A, 0xB7, 0xDF};
    assert(std::memcmp(kBuild.id, kId, sizeof kId) == 0);
    assert(std::string_view(kBuild.id16) == "A16802625E7826BF");
    assert(std::string_view(sw_build::kBuildId16) == "A16802625E7826BF");
    assert(std::string_view(kBuild.version) == "1.3.2");
    static_assert(kBuild.image_span == 0x2A00000);
    unsigned char id[32]{};
    std::memcpy(id, kId, sizeof kId);
    assert(sw_build::BuildLabel(id) && *sw_build::BuildLabel(id) == '\0');
    assert(sw_build::SupportsBuildHex("A16802625E7826BF83B6F9708E475B912A9AB7DF000000000000000000000000"));
    // Sword's build (and its 1.3.1 lead) are not Shield builds: no recognised-only build either.
    assert(kActive.other_build_count == 0 && kActive.other_builds == nullptr);
    assert(!sw_build::SupportsBuildHex("A3B75BCD3311385AEED67FBEEB79CBB7BF02F471000000000000000000000000"));
    assert(!sw_build::SupportsBuildHex("4628A512B1D93D370000000000000000000000000000000000000000000000"));
}

// The Shield 1.3.2 offsets (sw_variants.py SHIELD_132, verified by scan + byte identity): same as
// Sword's up to main+0xCB0000, +0x30 / +0x90 for the moved functions.
void TestShieldMainOffsets() {
    static_assert(kMain.battle_command_wait_fn == 0x8021D0);
    static_assert(kMain.battle_fight_fn == 0x803590);
    static_assert(kMain.battle_bag_fn == 0x8039C0);
    static_assert(kMain.battle_pokemon_fn == 0x8032C0);
    static_assert(kMain.battle_forced_party_fn == 0x7FF830);
    static_assert(kMain.vt_ui_root == 0x2568630);
    static_assert(kMain.vt_command_view == 0x252DDB8);
    static_assert(kMain.vt_move_view == 0x252DC10);
    static_assert(kMain.vt_grid_list == 0x25BC8F0);
    static_assert(kMain.vt_grid_cursor == 0x25BD1F8);
    static_assert(kMain.vt_ball_view == 0x252E320);
    static_assert(kMain.vt_target_view == 0x252F138);
    static_assert(kMain.vt_layout == 0x260D1F0);
    static_assert(kMain.vt_layout_holder == 0x25B9D20);
    static_assert(kMain.vt_layout_work == 0x250F478);
    static_assert(kMain.game_data_slot == 0x2610798);
    static_assert(kMain.field_slot == 0x2610848);
    static_assert(kMain.world_slot == 0x26194D8);
    static_assert(kMain.zone_registry_slot == 0x2617C48);
    static_assert(kMain.player_type_getters[0] == 0xDA2330);
    static_assert(kMain.player_type_getters[1] == 0xDA23C0);
    static_assert(kMain.player_type_getters[2] == 0xDA24C0);
    static_assert(kMain.portrait_task_head_fn == 0x1530780);
    static_assert(kMain.portrait_app_fn == 0x1536150);
    static_assert(kMain.portrait_wrapper_fn == 0x159D6A0);
    static_assert(kMain.portrait_target_fn == 0x5FE5F0);
    static_assert(sw_battle_cursor::kCommandWaitFn == 0x8021D0);
    static_assert(sw_battle_cursor::kUiRootVtable == 0x2568630);
    static_assert(sw_overworld::kPlayerTypeGetters[0] == 0xDA2330);
    static_assert(sw_overworld::kPlayerTypeGetters[1] == 0xDA23C0);
    static_assert(sw_overworld::kPlayerTypeGetters[2] == 0xDA24C0);
    // Heap anchors are shared (verified live on Shield 1.3.2, research pokemon-shield STATUS.md).
    static_assert(sw_addresses::OffsetOf("mystatus.tid_sid") == 0x45068FB8);
    static_assert(sw_trainer_portrait::kOwnerOffset == 0x45068FB8);
}
#endif

} // namespace

int main() {
#if defined(SW_VARIANT_SHIELD) && SW_VARIANT_SHIELD
    TestShieldRecord();
    TestShieldBuild();
    TestShieldMainOffsets();
    std::puts("sw_variant: Shield record, build gate and main offsets");
#else
    TestSwordRecord();
    TestBuild();
    TestMainOffsetsUnchanged();
    std::puts("sw_variant: Sword record, build gate and main offsets unchanged");
#endif
    return 0;
}
