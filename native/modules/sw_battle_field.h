// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pokémon Sword 1.3.2 battle field, from native accessors in main:
// - client constructor 0x7FC390 calls container+0x28 getter 0x831930 and stores the field
//   pointer at client+0xE0 (corroborated by battle adapter getter 0x8C6320).
// - weather 0x832760/0x832780: id u8+0, duration u32+4, elapsed u32+0xC, 255 indefinite.
// - terrain 0x833080/0x8330A0: id u8+0x11, source/elapsed in effect slot 8.
// - field effect accessor 0x832AE0: active u32+0x180+4*slot; source u64+0x18+8*slot,
//   elapsed u32+0x68+4*slot. Duration getter 0x7ED270 reads source bits8..13 when low3
//   bits are 2 or 4, otherwise no finite duration is known.
// - effect end-message switch 0x847510: slot1 Trick Room,2 Gravity,4 Wonder Room,5 Magic Room.
// Native evidence is maintained privately in games/pokemon-sword/analysis/20261028-field-features.
// Verified2026-10-28 on two independent wild-battle boots (differentASLR), with native
// RainDance,ElectricTerrain,TrickRoom,Reflect and subsequenttimerexpiry in boot1. Rule0,
// participant0 maps to ownside0; Reflect appeared in that real side0 condition record.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/mods/dsmod_module_abi.h"
#include "sw_lang.h"
#include "sw_text.h"

namespace sw_battle_field {

inline constexpr bool kRuntimeVerified = true;
inline constexpr bool kSideIdentityVerified = true;
inline constexpr std::uint64_t kClientFieldOffset = 0xE0;
inline constexpr std::size_t kBytes = 0x1A8;
// The current companion language's file (sw_lang.h MessagePath).
inline std::string StateTextPath() { return sw_lang::MessagePath("btl_state.dat"); }

inline std::uint32_t U32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
           std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
inline std::uint64_t U64(const std::uint8_t* p) {
    return std::uint64_t(U32(p)) | std::uint64_t(U32(p + 4)) << 32;
}
struct Timer {
    bool known = false;
    bool indefinite = false;
    std::uint32_t remaining = 0;
};
struct EffectSpec {
    int slot;
    int text_state;
};
inline constexpr std::array<EffectSpec, 6> kEffects{{
    {1, 8}, {2, 29}, {3, 46}, {4, 10}, {5, 9}, {7, 65},
}};

inline Timer SourceTimer(std::uint64_t source, std::uint32_t elapsed) {
    const unsigned kind = source & 7;
    if (kind != 2 && kind != 4) return {};
    const unsigned total = (source >> 8) & 0x3F;
    if (total == 0) return {};
    return {true, false, elapsed < total ? total - elapsed : 0};
}
struct Field {
    bool valid = false;
    std::uint8_t weather = 0;
    std::uint8_t terrain = 0;
    Timer weather_timer;
    Timer terrain_timer;
    std::array<bool, kEffects.size()> effects{};
    std::array<Timer, kEffects.size()> effect_timers{};
};
inline Field Decode(const std::uint8_t* p, std::size_t bytes) {
    Field out;
    if (!p || bytes < kBytes || p[0] > 7 || p[0x11] > 4) return out;
    out.valid = true;
    out.weather = p[0];
    out.terrain = p[0x11];
    if (out.weather != 0) {
        const auto total = U32(p + 4), elapsed = U32(p + 0xC);
        if (total == 0xFF) out.weather_timer = {true, true, 0};
        else if (total > 0 && total < 0xFF)
            out.weather_timer = {true, false, elapsed < total ? total - elapsed : 0};
    }
    if (out.terrain != 0) out.terrain_timer = SourceTimer(U64(p + 0x58), U32(p + 0x88));
    for (std::size_t i = 0; i < kEffects.size(); ++i) {
        const int slot = kEffects[i].slot;
        out.effects[i] = U32(p + 0x180 + 4 * slot) != 0;
        if (out.effects[i] && slot != 3 && slot != 7)
            out.effect_timers[i] = SourceTimer(U64(p + 0x18 + 8 * slot), U32(p + 0x68 + 4 * slot));
    }
    return out;
}

struct SideEffectSpec {
    int key;
    int text_state;
    int mode; // native descriptor mode: 4 turns, 3 layers, 1 presence only
};
inline constexpr std::array<SideEffectSpec, 19> kSideEffects{{
    {0, 45, 4},
    {1, 44, 4},
    {2, 31, 4},
    {3, 30, 4},
    {4, 19, 4},
    {5, 20, 4},
    {6, 48, 3},
    {7, 37, 3},
    {8, 32, 1},
    {11, 66, 4},
    {12, 68, 4},
    {13, 67, 4},
    {14, 40, 1},
    {17, 74, 4},
    {19, 80, 1},
    {20, 78, 4},
    {21, 79, 4},
    {22, 81, 4},
    {23, 82, 4},
}};
inline constexpr std::size_t kSideTableBytes = 0x600; // two sides, 24 shared-pointer slots each
struct SideEffect {
    bool active = false;
    std::uint32_t layers = 0;
    Timer timer;
};
inline SideEffect DecodeSideEffect(const std::uint8_t* p, std::size_t bytes, int mode) {
    SideEffect out;
    if (!p || bytes < 0x20) return out;
    out.layers = U32(p + 0x1C);
    out.active = out.layers != 0;
    if (out.active && mode == 4) {
        const auto duration = (U32(p + 0x10) >> 8) & 0x3F;
        const auto elapsed = U32(p + 0x18);
        if (duration != 0) out.timer = {true, false, elapsed < duration ? duration - elapsed : 0};
    }
    return out;
}

// Exact native descriptor table main+0x2053878 (77 entries, stride0x20), mapped through
// the btl_state.tbl hashes. Weather 3 is Hail and 4 Sandstorm; other games' enum order differs.
inline int WeatherTextState(unsigned id) {
    static constexpr int states[8]{0, 1, 2, 4, 3, 70, 69, 71};
    return id < 8 ? states[id] : 0;
}
// Native terrain end-message table 0x84756C: Grass, Mist, Electric, Psychic.
inline int TerrainTextState(unsigned id) {
    static constexpr int states[5]{0, 6, 7, 5, 72};
    return id < 5 ? states[id] : 0;
}

// Native 0x7FB3C0 -> 0x7EE7A0 -> 0x7EE550 maps local participant -> battler
// position -> side. Rule0 is singles,1 doubles,2 raids. Position5 and side2 are sentinels.
// Use the client's real participant id; assuming side0 is wrong for a remote participant.
inline int SideForParticipant(unsigned rule, unsigned mode, unsigned participant) {
    if (participant >= 5) return -1;
    std::array<int, 5> positions{};
    if (rule == 0) positions = {0, 1, 5, 5, 5};
    else if (rule == 1) {
        if (mode >= 1 && mode <= 3) positions = {0, 1, 2, 3, 5};
        else if (mode == 4) positions = {0, 1, 0, 3, 5};
        else if (mode == 5) positions = {0, 1, 2, 5, 5};
        else positions = {0, 1, 0, 1, 5};
    } else if (rule == 2) {
        if (mode == 1) positions = {0, 1, 2, 5, 4};
        else if (mode == 2 || mode == 4) positions = {0, 1, 5, 5, 4};
        else if (mode == 3) positions = {0, 5, 5, 5, 4};
        else if (mode == 0 || (mode >= 5 && mode <= 7)) positions = {0, 1, 2, 3, 4};
        else return -1;
    } else return -1;
    const int position = positions[participant];
    if (position >= 5) return -1;
    static constexpr int sides[3][5]{{0, 1, 2, 2, 2}, {0, 1, 0, 1, 2}, {0, 0, 0, 0, 1}};
    const int side = sides[rule][position];
    return side < 2 ? side : -1;
}

// btl_state.dat has BTR_STATE_NN_01 name / BTR_STATE_NN_02 description pairs, at
// lines 2*NN and 2*NN+1 (checked against the corresponding native .tbl labels).
inline bool LoadStrings(const EdenDsmodHostApi& host, std::vector<std::string>& out) {
    if (!host.read_romfs) return false;
    const std::string path = StateTextPath();
    const auto size = host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0);
    if (size == 0 || size > 256 * 1024) return false;
    std::vector<std::uint8_t> bytes(size);
    if (host.read_romfs(host.userdata, path.c_str(), 0, bytes.data(), bytes.size()) != size) return false;
    return sw_text::ReadDat(bytes.data(), bytes.size(), out);
}
inline bool StateStrings(const std::vector<std::string>& strings, int state,
                         std::string& name, std::string& description) {
    const std::size_t index = static_cast<std::size_t>(state) * 2;
    if (state <= 0 || index + 1 >= strings.size() || strings[index].empty()) return false;
    name = strings[index];
    description = strings[index + 1];
    // The decoder retains native line breaks as literal \n escapes. Let the companion's
    // wider row wrap the same words to its own width, rather than displaying escape glyphs.
    for (std::size_t at = 0; (at = description.find("\\n", at)) != std::string::npos; )
        description.replace(at, 2, " ");
    return true;
}

} // namespace sw_battle_field
