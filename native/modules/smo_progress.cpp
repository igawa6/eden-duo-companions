// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_progress.h"
#include <algorithm>
#include <array>
#include <limits>

namespace SmoProgress {
namespace {
template <class T>
bool Get(const EdenDsmodHostApi& h, std::uint64_t at, T& out) {
    return at && at <= std::numeric_limits<std::uint64_t>::max() - sizeof(T) &&
           h.is_mapped && h.read_memory && h.is_mapped(h.userdata, at, sizeof(T)) &&
           h.read_memory(h.userdata, at, &out, sizeof(T));
}
template <class T>
bool ArrayRead(const EdenDsmodHostApi& h, std::uint64_t owner, std::uint64_t offset,
               unsigned index, T& out) {
    std::uint64_t a = 0, b = 0;
    T first{}, second{};
    const auto delta = std::uint64_t(index) * sizeof(T);
    if (!Get(h, owner + offset, a) || !a || a > UINT64_MAX - delta ||
        !Get(h, a + delta, first) || !Get(h, a + delta, second) || first != second ||
        !Get(h, owner + offset, b) || a != b) return false;
    out = first;
    return true;
}
template <class T>
bool Stable(const EdenDsmodHostApi& h, std::uint64_t at, T& out) {
    T check{};
    return Get(h, at, out) && Get(h, at, check) && out == check;
}
bool Text(const EdenDsmodHostApi& h, std::uint64_t at, std::string_view text) {
    if (!at || text.empty() || text.size() > 127 || at > UINT64_MAX - 128) return false;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        std::uint8_t c = 0;
        if (!Stable(h, at + i, c) || c != (i == text.size() ? 0 : std::uint8_t(text[i]))) return false;
    }
    return true;
}
// Native ShineCounter::setText (1dfa10) subtracts deposited and pending moons
// from WorldInfo's grouped travel requirement (4d11e0 -> 4dbe90). The dotted
// row is unrelated to the map's collection of optional moon placements.
void ReadMissing(const EdenDsmodHostApi& h, std::uint64_t game, int world, Snapshot& out) {
    if (!out.moon_row_valid || !out.global_valid) return;
    std::uint64_t progress = 0, info = 0, groups = 0;
    std::array<std::uint8_t, 17> unlocked{}, first{};
    if (!Stable(h, game + 0x6A0, progress) || !progress ||
        !ArrayRead(h, progress, 0x20, 0, unlocked) ||
        !ArrayRead(h, progress, 0x48, 0, first)) return;
    for (unsigned i = 0; i < 17; ++i) if (unlocked[i] > 1 || first[i] > 1) return;
    if (unlocked[16] || (unlocked[14] && !first[14])) {
        out.moon_requirement_valid = true;
        return;
    }
    std::int32_t count = 0, deposited = 0, requirement = -1;
    if (!Stable(h, game + 0x898, info) || !info ||
        !Stable(h, info + 0xF0, count) || count < 1 || count > 32 ||
        !Stable(h, info + 0xF8, groups) || !groups ||
        !ArrayRead(h, game, BankedArray, world, deposited)) return;
    int index = world;
    for (int i = 0; i < count; ++i) {
        std::uint64_t group = 0, values = 0;
        std::int32_t length = 0;
        std::uint8_t no_dots = 0;
        if (!Stable(h, groups + i * 8, group) || !group ||
            !Stable(h, group + 8, length) || length < 0 || length > 17) return;
        if (index >= length) { index -= length; continue; }
        if (!Stable(h, group + 0xC, no_dots) || no_dots > 1 ||
            !Stable(h, group, values) || !values ||
            !Stable(h, values + index * 4, requirement) || requirement < -1 || requirement > 999) return;
        out.moon_missing = no_dots ? 0 : std::max(0, requirement - deposited - out.moon_row);
        std::uint64_t check = 0;
        std::int32_t current = -1;
        if (!Get(h, game + 0x898, check) || check != info ||
            !Get(h, game + CurrentWorld, current) || current != world) return;
        out.moon_requirement_valid = true;
        return;
    }
}
} // namespace

std::optional<bool> CheckpointTouched(const EdenDsmodHostApi& h, std::uint64_t game,
                                     int world, std::string_view stage, std::string_view id) {
    if (!game || game > UINT64_MAX - 0x2000 || world < 0 || world > 16) return std::nullopt;
    std::uint64_t worlds = 0, entries = 0, check = 0;
    std::int32_t current = -1;
    if (!Stable(h, game + CurrentWorld, current) || current != world ||
        !Stable(h, game + 0xA30, worlds) || !worlds || worlds > UINT64_MAX - 17 * 8 ||
        !Stable(h, worlds + world * 8, entries) || !entries || entries > UINT64_MAX - 17 * 0x148)
        return std::nullopt;
    std::optional<bool> result;
    for (unsigned i = 0; i < 17; ++i) {
        const auto entry = entries + i * 0x148;
        std::uint64_t stage_ptr = 0, id_ptr = 0;
        if (!Stable(h, entry + 8, stage_ptr) || !Stable(h, entry + 0xA0, id_ptr)) return std::nullopt;
        if (!stage_ptr || !id_ptr || !Text(h, stage_ptr, stage) || !Text(h, id_ptr, id)) continue;
        std::uint8_t touched = 0;
        if (result.has_value() || !Stable(h, entry + 0x140, touched) || touched > 1) return std::nullopt;
        result = touched == 1;
    }
    if (!Get(h, game + 0xA30, check) || check != worlds ||
        !Get(h, worlds + world * 8, check) || check != entries ||
        !Get(h, game + CurrentWorld, current) || current != world) return std::nullopt;
    return result;
}

Snapshot Read(const EdenDsmodHostApi& host, std::uint64_t game_data, int world) {
    Snapshot out;
    if (!game_data || game_data > UINT64_MAX - 0xA50 || world < 0 || world > 16)
        return out;
    std::int32_t regional_a = 0, regional_b = 0, spent = 0, current_before = -1, current_after = -1;
    // CoinCounter's native purple branch:4d4df0 ->4cad60 reads collected A48
    // minus spent5E8 (save key UseCoinCollectNum).4d4e00 ->4cad90 gives lifetime
    // regional collection count, used for the native counter's completed color.
    if (Get(host, game_data + CurrentWorld, current_before) && current_before == world &&
        ArrayRead(host, game_data, 0xA48, world, regional_a) &&
        ArrayRead(host, game_data, 0x5E8, world, spent) &&
        ArrayRead(host, game_data, 0xA48, world, regional_b) && regional_a == regional_b &&
        Get(host, game_data + CurrentWorld, current_after) && current_before == current_after &&
        regional_a >= 0 && regional_a <= 100 && spent >= 0 && spent <= regional_a) {
        out.regional_valid = true;
        out.regional_wallet = regional_a - spent;
        out.regional_collected = regional_a;
    }
    // Native 4ca4c0 unrolls precisely 20 signed32-bit slots; callers 4d44a0 and
    // 2c80ec/2c8188 clamp 999 before setting TxtTotalShine. ABC is a cached tally
    // refreshed at stage boundaries; use the authoritative array as native does.
    std::array<std::int32_t, BankedSlots> banked{};
    if (ArrayRead(host, game_data, BankedArray, 0, banked)) {
        std::int64_t total = 0;
        bool valid = true;
        for (const auto value : banked) {
            // Repeat shop moons can exceed 999. Reject negative/implausible values,
            // never clamp corrupt individual records into plausible progress.
            if (value < 0 || value > 1000000) { valid = false; break; }
            total += value;
        }
        if (valid) {
            out.global_valid = true;
            out.banked_total = static_cast<std::int32_t>(std::min<std::int64_t>(999, total));
            std::array<std::int32_t, BankedSlots> acquired{}, banked_after{};
            std::int32_t current_a = -1, current_b = -1;
            if (Get(host, game_data + CurrentWorld, current_a) && current_a == world &&
                ArrayRead(host, game_data, AcquiredArray, 0, acquired) &&
                ArrayRead(host, game_data, BankedArray, 0, banked_after) && banked_after == banked &&
                Get(host, game_data + CurrentWorld, current_b) && current_a == current_b) {
                std::int64_t units = 0, pending = 0;
                bool acquired_valid = true;
                for (unsigned i = 0; i < BankedSlots; ++i) {
                    if (acquired[i] < 0 || acquired[i] > 1000000) { acquired_valid = false; break; }
                    units += acquired[i];
                    if (i < 17) pending += std::max(0, acquired[i] - banked[i]);
                }
                if (acquired_valid) {
                    out.unbanked_valid = true;
                    out.world_unbanked = std::max(0, acquired[world] - banked[world]);
                    out.acquired_units = static_cast<std::int32_t>(units);
                    out.all_worlds_unbanked = static_cast<std::int32_t>(pending);
                    // Native 4c7a60 checks Mushroom (world 14) unlocked and no longer
                    // first-time. 4d3c60 then sums differences across WorldList's
                    // 17 worlds, rather than only the current world's difference.
                    std::uint64_t progress_a = 0, progress_b = 0;
                    std::uint8_t unlocked = 0, first_time = 0;
                    if (Get(host, game_data + 0x6A0, progress_a) && progress_a &&
                        progress_a <= UINT64_MAX - 0x50 &&
                        ArrayRead(host, progress_a, 0x20, 14, unlocked) && unlocked <= 1 &&
                        ArrayRead(host, progress_a, 0x48, 14, first_time) && first_time <= 1 &&
                        Get(host, game_data + 0x6A0, progress_b) && progress_a == progress_b) {
                        out.moon_row_valid = true;
                        out.moon_row_global = unlocked && !first_time;
                        out.moon_row = out.moon_row_global ? out.all_worlds_unbanked : out.world_unbanked;
                    }
                }
            }
        }
    }
    std::int32_t value = 0;
    // Native load clamps ScenarioNo upward to1; bounded positive values only.
    if (ArrayRead(host, game_data, ScenarioArray, world, value) && value >= 1 && value <= 255) {
        out.scenario_valid = true;
        out.scenario = value;
    }
    if (ArrayRead(host, game_data, MainShineArray, world, value) && value >= -1 && value <= 9999) {
        out.main_shine_valid = true;
        out.main_shine = value;
    }
    ReadMissing(host, game_data, world, out);
    return out;
}

unsigned ActiveSaveFile(const EdenDsmodHostApi& host, std::uint64_t holder, std::uint64_t game) {
    if (!holder || !game || holder > UINT64_MAX - 0x38) return 0;
    std::int32_t before = -1, after = -1;
    std::uint64_t active_before = 0, active_after = 0;
    std::array<std::uint64_t, 5> files{};
    if (!Get(host, holder + 0x30, before) || before < 0 || before >= 5 ||
        !Get(host, holder + 0x20, active_before) || active_before != game ||
        !ArrayRead(host, holder, 0x18, 0, files) || files[before] != game ||
        !Get(host, holder + 0x20, active_after) || active_before != active_after ||
        !Get(host, holder + 0x30, after) || before != after) return 0;
    unsigned matches = 0;
    for (auto file : files) if (file == game) ++matches;
    return matches == 1 ? static_cast<unsigned>(before + 1) : 0;
}
} // namespace SmoProgress
