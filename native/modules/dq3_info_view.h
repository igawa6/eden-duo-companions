// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Party, Bag and Journal tab values of the Dragon Quest III HD-2D Remake companion (owner-chosen
// option B in the revision 2 "current-compact" layout, research design/redesign-2026-10/current-compact), built from the
// field snapshot, the live readers' results (dq3_info.h) and the game tables. Pure: no guest access,
// unit-testable (like BuildMapView / BuildBattleView).
//
// Party tab: dq3.pp.* status scrap (portrait, name, job . Lv, personality, Next lv, ten stats) and the
// six equipped slots dq3.pp.e<k>.* (k = equip slot 1..6 - 1, the native Equipment names); dq3.pr.* the
// member's dark window (title, count, Carried / Spells sub-tabs): Carried = grid rows qg<field><row> (n name,
// y = 13 for a one-line name, i item icon, x selected; icon ints are the cell + 1, count dq3.pr.n); Spells =
// list rows qs<field><row> (owner 2026-10-10: n name, s / z spell icon / greyed, m "N MP", x selected, l separator;
// count dq3.ps.n, gate dq3.ps.on); the selection's detail dq3.dt.* below either (spells: MP cost / the member's
// MP, where it can be cast, description). Bag tab (revision 2: a list with a description pane):
// dq3.bg.* (bag name, "N kinds", Items / Equipment / Important, subx = the sub-tabs' offset), list rows
// qb<field><row> (n name, i icon, c "xN", x selected, l row separator) and the pane dq3.bd.* (icon, name,
// Number Held, Carried by, stat effect, description). Journal tab: dq3.jn / jo (Next step), jt + qtn / qtw
// (Traveller's Tips), jm (Mini medals), jr + qc / qcs (Records . Monsters grid and the selected monster).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dq3_font.h"
#include "dq3_info.h"
#include "dq3_map.h"
#include "dq3_tables.h"

namespace dq3 {

struct InfoView {
    std::map<std::string, std::int64_t> ints;
    std::map<std::string, std::string> texts;
    int sel{};      ///< grid selection after clamping (Party pane / Bag)
    int slot{-1};   ///< Party: selected equipped slot 0..5 (-1 = the grid selection shows)
    int mon_sel{-1}; ///< Journal monster selection after clamping
};

struct PartyViewInput {
    const PartyMember* member{};      ///< the shown member (field snapshot); nullptr = nothing to show
    std::u32string job;               ///< the job name ("" = unknown)
    std::string sprite{"-"};          ///< looks sprite index ("-" = none): portrait/ and head/ keys
    const MemberDetail* detail{};     ///< ReadMemberDetail of that member
    std::optional<std::uint32_t> gold;
    int sub{};                        ///< 0 Carried, 1 Spells
    int sel{}, slot{-1};
    std::int64_t key{}; ///< dq3.pp.key (bumped on companion-side member / sub-tab changes)
    const InfoData* info{};
    const BattleData* battle{};
    const FontAtlas* font{};
};
InfoView BuildPartyView(const PartyViewInput& in);

struct BagViewInput {
    const std::vector<OwnedItem>* rows{}; ///< the shown bag (nullptr = not read yet)
    int page{};                           ///< 0 Item Bag, 1 Equipment Bag, 2 Important Items
    int sel{};
    std::int64_t key{}; ///< dq3.bg.key (bumped on page changes)
    const InfoData* info{};
    const FontAtlas* font{};
};
InfoView BuildBagView(const BagViewInput& in);

struct JournalViewInput {
    const JournalState* state{};
    const std::map<std::string, s32>* record{}; ///< monster record (nullptr = unavailable)
    int mon_sel{-1};                            ///< -1 = the first defeated row
    const InfoData* info{};
    const MapData* map{};
    const FontAtlas* font{};
};
InfoView BuildJournalView(const JournalViewInput& in);

} // namespace dq3
