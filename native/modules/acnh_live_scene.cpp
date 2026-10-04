// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH 3.0.3 stage names -> the building a stage is the inside of (r9 indoor lane;
// research/acnh/impl/fix/r9-indoor.md).

#include "acnh_live.h"

namespace acnh::live {
namespace {

/// "<prefix><digit>" or "<prefix><digit>_<k>": the digit, else -1.
int Indexed(std::string_view name, std::string_view prefix) {
    if (!name.starts_with(prefix) || name.size() <= prefix.size())
        return -1;
    const char c = name[prefix.size()];
    if (c < '0' || c > '9')
        return -1;
    const std::string_view rest = name.substr(prefix.size() + 1);
    if (!rest.empty() && rest[0] != '_')
        return -1;
    return c - '0';
}

} // namespace

int SceneStructure(std::string_view name) {
    // the stage and structure enums share the names (structure kind table 0x480c950: Invalid,
    // PlayerHouse0..7, NpcHouse0..9, Market, Office, Museum, Airport, TanukichiTent, Tailor,
    // CampSite, ...; StructureInfoParam UniqueID = that index, the hotel 42)
    if (const int i = Indexed(name, "PlayerHouse"); i >= 0 && i <= 7)
        return 1 + i; // PlayerHouse<i> and its rooms PlayerHouse<i>_1.._5
    if (const int i = Indexed(name, "NpcHouse"); i >= 0 && i <= 9 && name.size() == 9)
        return 9 + i;
    if (name.starts_with("IdrMarket"))
        return 19;
    if (name == "IdrOffice01")
        return 20;
    if (name.starts_with("IdrMuseum"))
        return 21; // entrance hall, the three wings, art gallery, cafe
    if (name.starts_with("IdrAirPort"))
        return 22;
    if (name == "IdrTanukichi")
        return 23; // Resident Services tent
    if (name == "IdrTailor")
        return 24;
    if (name == "IdrCampSiteTent")
        return 25;
    if (name.starts_with("IdrHotel"))
        return 42;
    return 0; // dream, Redd's ship, other islands, HHP, photo studio, demos: no building here
}

} // namespace acnh::live
