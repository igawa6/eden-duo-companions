// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared by the publisher's files (acnh_publish.cpp: status, clock, static texts, calendar rules,
// actions; acnh_publish_map.cpp; acnh_publish_pages.cpp: Bag, Critterpedia, DIY;
// acnh_publish_today.cpp; acnh_publish_phone.cpp): cache-key mixing and small joins.
#pragma once

#include <cstdint>
#include <string_view>

#include "acnh_catalog.h"
#include "acnh_live.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace acnh::pub {

inline constexpr std::string_view Key = "module:acnh/";

inline uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

inline uint64_t MixBytes(uint64_t h, const void* p, size_t n) {
    return dsmod_sdk::Fnv1a64(static_cast<const uint8_t*>(p), n, h);
}

/// Field by field: live::Item has 3 padding bytes a copy may fill with stack bytes (a byte hash of
/// the pockets rebuilt the page on samples where nothing changed).
template <class Items>
inline uint64_t MixItems(uint64_t h, const Items& items) {
    for (const live::Item& it : items)
        h = Mix(Mix(h, (uint64_t{it.id} << 24) | (uint64_t{it.sys} << 16) |
                           (uint64_t{it.add} << 8) | static_cast<uint8_t>(it.fav)),
                it.free);
    return h;
}

inline uint64_t MixStr(uint64_t h, std::string_view s) {
    return MixBytes(h, s.data(), s.size());
}

inline const char* const MonthShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

inline int StackCount(Catalog& cat, const live::Item& it) {
    if (it.Empty())
        return 0;
    const auto* d = cat.FindItem(it.id);
    return d && d->stack_max > 1 ? static_cast<int>(it.free & 0xFFFF) + 1 : 1;
}

inline int64_t DaysFromCivil(int y, int m, int d) { // days since 1970-01-01 (proleptic Gregorian)
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

} // namespace acnh::pub
