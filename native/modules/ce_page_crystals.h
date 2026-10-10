// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes companion: the Crystals page (planner, transfer checklist, smith mirror, pause
// mirror). Layouts: research design/redesign-20261009/render/crystals.png, crystals-drag.png,
// crystals-transfer.png, crystals-smith.png, state-pause.png (renderer tools/redesign2/
// render-manage.py). Only values a verified reader produced (research contracts/
// crystal-readers.md) are in the model. The page plans and tracks; it never writes the game.
#pragma once

#include "ce_draw.h"

#include <array>
#include <string>
#include <vector>

namespace ce_page_crystals {

/// One crystal (Crystal object fields + table 117 / BGDatabase text).
struct Crystal {
    int key{-1};       ///< page-local index into CrystalsView::bag (-1 = not in the list)
    int prop{-1};      ///< propertyId (table 117 id)
    int cat{};         ///< crystalType 0 Status, 1 Offensive, 2 Defensive, 3 Utility
    int rank{};        ///< crystalQuality (I..)
    int purity{};      ///< crystalPurity 0..5
    int size{1};       ///< slots it takes (crystalSize + 1)
    bool art{};        ///< artifical
    bool known{true};  ///< false: a pre-1.2 save's inserted crystal (only property + level known)
    std::string name;  ///< tr_PASSIVES_NAME
    std::string effect; ///< tr_PASSIVES_DESCR with #va / #vb filled for the rank
    std::string owner; ///< "" bag, else the sprite of the member whose gear holds it
    std::string owner_name;
};

bool SameKind(const Crystal& a, const Crystal& b); ///< prop, rank, purity, size, artificial

struct SlotRow {
    int state{};   ///< 0 empty, 1 filled (crystal), 2 locked, 3 covered by the crystal above
    Crystal crystal;
    bool planned{};       ///< a plan entry targets this slot
    Crystal plan;         ///< the planned crystal
    bool valid{};         ///< tap-tap / drag: the selected crystal fits here
};

struct Piece {
    bool present{};
    int kind{};          ///< 0 weapon, 1 armor, 2 accessory, 3 class emblem
    int item{-1};        ///< EquipItem.id
    int equip{-1};       ///< EquipItem.equipID (table 120 id)
    std::string name, icon;
    std::string fixed;   ///< accessory: its fixed property's name
    std::vector<SlotRow> rows; ///< weapon / armor: unlocked slots + at most one locked row
    bool selected{};     ///< tapped (Transfer becomes available)
};

struct Member {
    int id{};
    std::string name, sprite;
};

struct Step {
    std::string label, sub;
    bool done{}, stale{}, next{};
    bool insert{};
    Crystal crystal; ///< insert steps
};

struct Target {
    int equip{};
    std::string name, icon;
    int count{};
    std::string owner; ///< sprite of a member wearing one ("" none)
};

struct Pair {
    Crystal base, fuse;
    bool current{}; ///< the pair in the smith's Base / Fuse boxes
};

enum Mode : int { Browse = 0, Filter = 1, Fusible = 2, TransferPick = 3 };

struct CrystalsView {
    std::vector<Member> members;
    int sel{};                     ///< selected member
    std::array<Piece, 4> pieces;   ///< weapon, armor, accessory, emblem of the selected member
    // inventory (the visible window of the filtered, sorted list)
    std::vector<Crystal> grid;     ///< up to GridRows x GridCols cells
    int total{};                   ///< all crystals (bag + inserted)
    int shown{};                   ///< after the filters
    int first_row{}, rows{};       ///< scroll position / row count
    int chip{-1};                  ///< category chip (-1 = All)
    int sort{};                    ///< 0 Rank, 1 Purity, 2 Size, 3 Property
    int filter{};                  ///< filter picker choice (0 = none)
    int selected{-1};              ///< grid cell selected (-1 none)
    Crystal detail;                ///< the selected crystal (detail pane)
    bool has_detail{};
    int mode{Browse};
    bool read_only{};              ///< native Equipment menu open: mirror, no plan editing
    // plan / transfer
    bool transfer{};               ///< a transfer plan exists for this member
    Piece transfer_old;            ///< the piece the plan transfers from
    std::string transfer_new, transfer_new_icon;
    std::vector<Step> steps;       ///< plan steps for this member (transfer or slot plan)
    bool plan_stale{};
    std::vector<Target> targets;   ///< TransferPick
    std::vector<Pair> pairs;       ///< Fusible / smith
    // smith mirror (native Craft menu crystal mode)
    bool smith{};
    bool has_base{}, has_fuse{}, has_result{};
    Crystal base, fuse, result;
};

std::string Signature(const CrystalsView& v);
ce_draw::Image ComposeCrystals(ce_draw::Art& art, const CrystalsView& v);
/// A grid cell (borderOff / borderOn) as the drag widget shows it; size Cell x Cell.
ce_draw::Image ComposeCell(ce_draw::Art& art, const Crystal& c, bool selected);
/// The lifted cell's drop shadow (drag underlay).
ce_draw::Image ComposeCellShadow();
/// Equipment window geometry (tap targets): one box per slot row of the weapon then the armor,
/// plus the accessory / emblem / weapon+armor item-line tops.
std::vector<ce_draw::Canvas::Box> SlotBoxes(const CrystalsView& v, int& acc_y, int& emblem_y,
                                            std::array<int, 2>& item_y);
/// Picker cell geometry (shared with the Skills page convention).
struct Cell {
    int x{}, w{};
};
Cell PickerCell(int n, int i);

/// Canvas geometry shared with tools/chained-echoes/gen_manifest.py ("Name = value" lines).
namespace geo {
inline constexpr int CrPickY = 30, CrPickH = 90, CrMaxMembers = 8;
inline constexpr int CrEquipX = 24, CrEquipY = 132, CrEquipW = 588, CrEquipH = 804;
inline constexpr int CrRowX = 54, CrRowW = 528, CrRowH = 90, CrLockH = 42, CrMaxRows = 4;
inline constexpr int CrItemH = 48;
inline constexpr int CrGridX = 639, CrGridY = 249, CrPitch = 108, CrCell = 102;
inline constexpr int CrGridCols = 5, CrGridRows = 4;
inline constexpr int CrChipX = 636, CrChipY = 144, CrChipW = 90, CrChipDx = 96;
inline constexpr int CrScrollX = 1176, CrScrollY = 249, CrScrollW = 36, CrScrollH = 426;
inline constexpr int CrBtnY = 948, CrBtnH = 96, CrBtnW = 264, CrBackX = 24;
inline constexpr int CrBtn0 = 400, CrBtn1 = 676, CrBtn2 = 952;
inline constexpr int CrPickCellX = 42, CrPickCellY = 150, CrPickCell = 168, CrPickCellDx = 192,
                     CrPickCols = 6, CrPickCellDy = 192, CrPickRows = 3;
inline constexpr int CrBodyY = 132, CrBodyH = 804;
} // namespace geo

} // namespace ce_page_crystals
