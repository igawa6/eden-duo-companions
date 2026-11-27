// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// The Party page profile (owner feedback 2026-11-27 item 2, contracts/PARTY-PROFILE-1.md): a member's
// level, class, rank and ground gear from the saved character record, and its stats from the status
// component's block 2. Item, effect and class names come from the player's own common.bdat /
// common_ms.bdat (no names are shipped).
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace xcx::profile {
// The saved character record C = *(main+0x1dc1408) + 0xf8 + id * 0x7f8 (contracts/PROFILE-DATA.md).
inline constexpr std::uint64_t RecordOffset = 0xf8, RecordStride = 0x7f8;
inline constexpr std::size_t RecordBytes = 0x300; // name .. rank (+0x2f6)
// Ground gear in the record: 0x34-byte slots from +0x80, record order 0 ranged, 1 melee, 2 torso,
// 3 head, 4 arm R, 5 arm L, 6 legs (7..11 fashion, not read). Slot word +4 low 6 bits = category.
inline constexpr std::size_t GearSlots = 7, GearOffset = 0x80, GearStride = 0x34;
inline constexpr std::array<std::uint8_t, GearSlots> GearCategory{6, 7, 2, 1, 3, 4, 5};
struct Gear {
    std::uint16_t item{};                  // 0 = empty
    std::array<std::uint16_t, 3> effects{}; // Additional Effects (effect table ids, 0 = none)
    bool operator==(const Gear &) const = default;
};
struct Record {
    bool valid{};
    std::uint8_t level{}, class_id{};
    std::uint16_t rank{};
    std::array<Gear, GearSlots> gear{};
    bool operator==(const Record &) const = default;
};
// The record's profile part, or !valid when anything is off: empty or unterminated name, a name
// other than `expect_name` (when given), level outside 1..99, class outside 1..63, rank outside 1..30,
// or a non-empty gear slot whose category is not the slot's own.
Record DecodeRecord(std::span<const std::uint8_t> record, std::string_view expect_name = {});
// Status block 2 (status + 0x250), 22 int32 (BATTLE-LIVE-2, PROFILE-DATA s2).
struct Stats {
    bool valid{};
    std::int32_t melee{}, ranged{}, potential{}, defense{}, melee_acc{}, ranged_acc{}, evasion{};
    std::array<std::int32_t, 6> res{}; // physical, beam, ether, thermal, electric, gravity (order: static)
    bool operator==(const Stats &) const = default;
};
inline constexpr std::size_t StatsBytes = 0x58;
Stats DecodeStats(std::span<const std::uint8_t> block2);

// Native display order of the ground gear (Ground Gear page: Melee, Ranged, Head, Torso, Left Arm,
// Right Arm, Legs) -> record slot, and the icon cell in icon_equipposition (60 x 60 cells, row 0).
struct SlotInfo {
    std::string_view label;
    int record_slot;
    int icon;
};
inline constexpr std::array<SlotInfo, GearSlots> DisplaySlots{{{"Melee", 1, 0},
                                                              {"Ranged", 0, 1},
                                                              {"Head", 3, 2},
                                                              {"Torso", 2, 3},
                                                              {"Left Arm", 5, 5},
                                                              {"Right Arm", 4, 4},
                                                              {"Legs", 6, 6}}};

struct ItemInfo {
    bool known{};
    std::string name;
    int level{}, rarity{}, attack{}, accuracy{};
};
// Names and the few item columns the page shows, extracted once from the player's BDAT files and
// kept small (the 11 MB files are not held).
class Names {
    struct Row {
        std::uint16_t name{}, level{};
        std::uint8_t rarity{};
        std::int16_t attack{}, accuracy{};
    };
    std::vector<Row> weapons, armor;
    std::uint32_t weapon_base{}, armor_base{}, effect_base{}, class_base{};
    std::vector<std::uint16_t> effects;
    struct ClassRow {
        std::uint16_t name{};
        std::uint8_t max_rank{};
    };
    std::vector<ClassRow> classes;
    struct Strings {
        std::uint32_t base{};
        std::vector<std::string> text;
        std::string_view At(unsigned id) const {
            return id >= base && id - base < text.size() ? std::string_view(text[id - base]) : std::string_view{};
        }
    } weapon_text, armor_text, effect_text, class_text;
    bool ready{};

  public:
    bool Load(std::span<const std::uint8_t> common, std::span<const std::uint8_t> common_ms);
    bool Ready() const { return ready; }
    ItemInfo Weapon(unsigned id) const;
    ItemInfo Armor(unsigned id) const;
    std::string Effect(unsigned id) const;
    std::string Class(unsigned id) const;
    int MaxRank(unsigned class_id) const; // 0 when unknown
};
// The text the Overview shows for a rank: "MAX" at the class's maximum, else the number.
std::string RankText(int rank, int max_rank);
} // namespace xcx::profile
