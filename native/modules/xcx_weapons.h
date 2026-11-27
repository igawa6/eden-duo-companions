// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Equipped Skell weapons, resolved from the saved owned record and the player's own archive:
// record blocks (0x34 bytes at +0x24, blocks 0..9) -> WPN_DlList (common.bdat table 695) ->
// RSC_DlWpnList (table 598) -> /chr/wd model, and the Skell-side mount bone from the frame's
// .wimtp locator table. See contracts/SKELL-STILL-MODULE.md (weapons section).
#include "xcx_model.h"
#include <array>
#include <map>
namespace xcx {
struct WeaponMount {
    std::string resource;          // "wd0345012l" (side letter included)
    std::string skell_bone;        // mount bone in the frame model ("EX_slmtA_L")
    std::string weapon_bone;       // weapon-side locator bone ("" = the model origin)
    std::string motion;            // "wd034501l": the weapon's rig, locators and animations
    unsigned block{};              // record block 0..9 (sidearm L/R, shoulder, back, arm, spare)
    // The mount bone's bind-pose world transform from the frame's rig (_rig.skl):
    // world = rotation * local + position; fold_side 1/2 when it hangs under JU_backD_L/R.
    std::array<float, 9> mount_rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 3> mount_position{};
    int fold_side{};
};
struct RigBone {
    std::string name;
    int parent{-1};
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1}; // world, row-major
    std::array<float, 3> position{};                           // world
};
// The skeleton of a frame (_rig.skl, a BC/SKEL file): names, parents and local transforms, composed
// into bind-pose world transforms. Empty on any malformed or inconsistent input.
std::vector<RigBone> ParseRig(std::span<const std::uint8_t> rig);
// Local bone transforms of one animation at one frame, keyed by the murmur3 hash of the bone name.
// Source: a SAR1 archive (/hkc/<dir>/<name>.sar) of BC/ANIM entries; the first entry whose name starts
// with `entry_prefix` is sampled (packed cubic tracks only). Empty when absent or malformed.
struct BoneTRS {
    std::array<float, 3> t{};
    std::array<float, 4> q{0, 0, 0, 1};
    std::array<float, 3> s{1, 1, 1};
};
using BonePose = std::map<std::uint32_t, BoneTRS>;
BonePose SampleAnimation(std::span<const std::uint8_t> sar, std::string_view entry_prefix, float frame);
std::uint32_t BoneHash(std::string_view name);
// The rig with `pose` applied (bones without a track keep their rest transform), as world transforms;
// `rotation` then holds the whole linear part (rotation * scale). Empty on malformed input.
std::vector<RigBone> PoseRig(std::span<const std::uint8_t> rig, const BonePose &pose);
// The rig of /chr/<dir>/<name>_rig.skl posed by frame 0 of the animation whose entry name starts with
// `entry_prefix` in /hkc/<dir>/<name>.sar: the field idle of a Skell ("000000_idle"), the stowed
// state of a Skell weapon ("0000"). Empty when either file is missing or unreadable.
std::vector<RigBone> RestPoseRig(Archive &, const EdenDsmodHostApi &, std::string_view dir,
                                 std::string_view name, std::string_view entry_prefix);
// Item ids of the ten weapon blocks of an owned record (0 = empty).
using WeaponItems = std::array<std::uint16_t, 10>;
WeaponItems WeaponItemsFromRecord(std::span<const std::uint8_t> owned_record);
// Resolve the mounts for these items on this frame. Items that cannot be resolved
// (unknown id, missing model, missing locator) are left out; nothing is guessed.
std::vector<WeaponMount> ResolveWeapons(Archive &, const EdenDsmodHostApi &, const FrameModel &,
                                        const WeaponItems &);
// "LF_WD_SL_L" -> "EX_slmtA_L" from a CSVB locator table (.wimtp). Empty on a malformed table.
std::map<std::string, std::string> ParseLocators(std::span<const std::uint8_t> wimtp);
} // namespace xcx
