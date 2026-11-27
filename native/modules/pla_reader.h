// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_abi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace pla {
inline constexpr uint64_t TitleId = 0x01001F5010DFA000;
// Private NSO fixture confirms the recorded 20-byte ID followed by twelve zero
// bytes.
inline constexpr uint8_t BuildId[32] = {0xae, 0xe8, 0xf1, 0x50, 0xdd, 0xa1, 0xb5, 0xa8, 0x38, 0x80,
                                        0x6e, 0x1a, 0x5e, 0xa6, 0x82, 0x7a, 0xd9, 0xf3, 0xc5, 0x1e};
inline constexpr char BuildHex[] =
    "AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E000000000000000000000000";
inline bool Supported(const EdenDsmodHostApi &h) {
    return h.title_id == TitleId && std::memcmp(h.build_id, BuildId, sizeof BuildId) == 0;
}
inline float ZoomWidth(int64_t level) {
    // Level 0 fills the frame width with the whole area width (no paper margins).
    constexpr float widths[] = {2048.f, 2048.f / 3, 2048.f / 9};
    return widths[std::clamp<int64_t>(level, 0, 2)];
}
struct Transform {
    float x_scale, z_scale, x_offset, z_offset;
};
// Community lead: image pixels = 2 * (x,z). Keep each area's calibration here.
inline constexpr std::array<Transform, 6> Transforms{
    {{2, 2, 0, 0}, {2, 2, 0, 0}, {2, 2, 0, 0}, {2, 2, 0, 0}, {2, 2, 0, 0}, {2, 2, 0, 0}}};
inline std::array<float, 2> MapPixel(unsigned area, float x, float z) {
    const auto &t = Transforms.at(area);
    return {x * t.x_scale + t.x_offset, z * t.z_scale + t.z_offset};
}
struct Position {
    float x = 0, height = 0, z = 0;
    bool valid = false;
    // The player object resolves (loading or field). At the title / save select it does not.
    bool object = false;
};
inline bool Range(const EdenDsmodHostApi &h, uint64_t address, uint64_t size) {
    // Switch guest virtual range, followed by the host's actual mapping check.
    constexpr uint64_t Max = uint64_t{1} << 39;
    return address >= 0x10000 && address < Max && size <= Max - address && h.is_mapped &&
           h.is_mapped(h.userdata, address, size);
}
inline bool ResolvePositionObject(const EdenDsmodHostApi &h, uint64_t &pointer) {
    pointer = h.main_base;
    constexpr uint64_t offsets[] = {0x42F18E8, 0x88, 0x90, 0x1F0, 0x18, 0x80};
    for (auto offset : offsets) {
        if (pointer > UINT64_MAX - offset || !Range(h, pointer + offset, sizeof pointer) ||
            !h.read_memory(h.userdata, pointer + offset, &pointer, sizeof pointer) || pointer % 8 ||
            !Range(h, pointer, 1))
            return false;
    }
    return pointer <= UINT64_MAX - 0x9c && Range(h, pointer + 0x90, 12);
}
// Player position (LIVE-READERS-111.md: same chain on two boots, movement-tested).
// Torn-read rule (MODULE_GUIDE "Reject torn reads", steward decision 2026-11-18):
// resolve the chain, read, resolve again and read again; publish only when both
// chains land on the same object and the two samples agree.
inline Position ReadPosition(const EdenDsmodHostApi &h) {
    if (!Supported(h) || !h.read_memory || !h.main_base || h.main_size < 0x42F18F0)
        return {};
    uint64_t first = 0, second = 0;
    float a[3]{}, b[3]{};
    if (!ResolvePositionObject(h, first))
        return {};
    Position placed;
    placed.object = true;
    if (!h.read_memory(h.userdata, first + 0x90, a, sizeof a) ||
        !ResolvePositionObject(h, second) || second != first ||
        !h.read_memory(h.userdata, second + 0x90, b, sizeof b))
        return placed;
    for (unsigned j = 0; j < 3; ++j)
        if (!std::isfinite(a[j]) || std::abs(a[j]) > 100000 || std::abs(a[j] - b[j]) > 4.f)
            return placed;
    // An all-zero vector is the object's state before the field is placed.
    if (a[0] == 0 && a[1] == 0 && a[2] == 0)
        return placed;
    return {b[0], b[1], b[2], true, true};
}
} // namespace pla
