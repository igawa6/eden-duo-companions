// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes 1.41 Sky Armor battle reader (build 17B6A110CC529FBE). Contract: research
// chained-echoes/contracts/skyarmor-readers.md. Read-only; the gear shift itself is the game's own
// R press (manifest button action) gated by ShiftReady().
#pragma once

#include "ce_page_skyarmor.h"
#include "core/mods/dsmod_module_abi.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ce_mech {

using u8 = std::uint8_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

struct Result {
    bool valid{};
    ce_sky::SkyView view;
    /// One R press now shifts the acting armour to view.next_gear (the game's own gate:
    /// mech battle, command phase, PlayerAction.alreadySwitch == 0, Battle.cUser == acting slot).
    bool shift_ready{};
    int acting_slot{-1};
    bool names_from_ui{}; ///< skill names taken from the command window (localized, costs matched)
};

/// Compact verification log line of a valid result (built only when it is logged).
std::string LogLine(const Result& res);

class Reader {
public:
    /// sf_battle / sf_funcs: the resolved static-field blocks (Battle, BattleFunctions). The
    /// selected enemy is the first one alive (the Sky Armor page has no target selection).
    Result Read(const EdenDsmodHostApi& host, u64 sf_battle, u64 sf_funcs);

private:
    bool R(u64 at, void* out, std::size_t n) const;
    u8 U8(u64 at) const;
    s32 I32(u64 at, bool* ok = nullptr) const;
    float F32(u64 at, bool* ok = nullptr) const;
    u64 Ptr(u64 at) const;
    static bool Ok(u64 p);
    std::string CStr(u64 at) const;
    std::string ClassName(u64 obj);
    std::string String(u64 str) const;
    std::vector<u64> ListItems(u64 list, std::size_t limit) const;
    std::vector<u64> NativeComponents(u64 go) const;
    std::map<std::string, u64> Components(u64 go);
    u64 UsageStatics(u64 slot, const char* name);
    /// TextMeshProUGUI texts (GameObject name -> m_text) of a managed GameObject's descendants.
    std::map<std::string, std::string> ChildTexts(u64 go);

    const EdenDsmodHostApi* host{};
    std::unordered_map<u64, std::string> class_names;
};

} // namespace ce_mech
