// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
namespace re4 {
// Keep in sync with case_layout.py. Square cells fit the r5 frame above the name plate.
struct CaseLayout {
    int cell, x, y;
};
inline CaseLayout CaseGeometry(const std::array<uint8_t,2>& capacity) {
    if(!capacity[0] || !capacity[1])return {0,0,0};
    const int cell=std::min(1200/capacity[0],768/capacity[1])/2*2;
    return {cell,(1240-capacity[0]*cell)/2,104+(768-capacity[1]*cell)/2};
}
inline std::array<int,2> CaseTapPoint(uint64_t payload) {
    // $tap is relative to the fixed Case content button, not the capacity's grid.
    return {20+static_cast<int32_t>(payload>>32),104+static_cast<int32_t>(payload)};
}
}
