// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Binding of Isaac: short map image keys. The runtime refuses module image keys longer than
// 256 characters, and the map encoding (CONTRACT "isaac:map/<hex>", 6 bytes per room) passes that on
// floors with ~20+ rooms. The reader registers each encoding here and publishes
// "isaac:map/h<16 hex FNV-1a>" instead; the glue's load_image expands it back before the asset side
// renders it. 'h' never occurs in the hex encoding, so both key forms stay unambiguous.
// One registry per loaded module (.so); thread-safe (tick thread registers, asset worker looks up).

#include "core/mods/modules/dsmod_module_sdk.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace isaac_mapreg {

namespace detail {
struct Registry {
    std::mutex mu;
    std::unordered_map<std::string, std::string> by_id;
    std::deque<std::string> order; // oldest first; bounded
};
inline Registry& Get() {
    static Registry r;
    return r;
}
inline constexpr std::size_t Keep = 64;
} // namespace detail

/// Registers `hex` and returns its id ("h" + 16 lower-case hex digits).
inline std::string Register(const std::string& hex) {
    static constexpr char Digits[] = "0123456789abcdef";
    std::uint64_t f = dsmod_sdk::Fnv1a64(reinterpret_cast<const std::uint8_t*>(hex.data()), hex.size());
    std::string id(17, 'h');
    for (int i = 16; i >= 1; --i, f >>= 4)
        id[static_cast<std::size_t>(i)] = Digits[f & 15];
    auto& r = detail::Get();
    std::scoped_lock lk{r.mu};
    if (r.by_id.emplace(id, hex).second) {
        r.order.push_back(id);
        while (r.order.size() > detail::Keep) {
            r.by_id.erase(r.order.front());
            r.order.pop_front();
        }
    }
    return id;
}

/// The encoding registered under `id` (with its leading 'h'); false if unknown or evicted.
inline bool Lookup(std::string_view id, std::string& hex) {
    auto& r = detail::Get();
    std::scoped_lock lk{r.mu};
    const auto it = r.by_id.find(std::string{id});
    if (it == r.by_id.end())
        return false;
    hex = it->second;
    return true;
}

} // namespace isaac_mapreg
