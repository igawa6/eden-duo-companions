// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Bounded reads over romfs file bytes for every ACNH parser (SARC, BNTX, BFLYT/BFLAN, BFRES,
// MSBT, BFFNT). Offsets come from the files themselves, so a range check is never `o + n <= size`
// (a 64-bit offset near 2^64 wraps and passes): Fits() compares without adding. Out of range
// reads return 0 (callers that must tell "absent" from 0 check Fits first).
#pragma once

#include <cstdint>
#include <cstring>
#include <span>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace acnh::bytes {

/// `n` bytes at offset `o` lie inside `s`.
inline bool Fits(std::span<const uint8_t> s, uint64_t o, uint64_t n) {
    return o <= s.size() && n <= s.size() - o;
}
inline uint8_t U8(std::span<const uint8_t> s, uint64_t o) {
    return o < s.size() ? s[o] : 0;
}
inline uint16_t Le16(std::span<const uint8_t> s, uint64_t o) {
    return Fits(s, o, 2) ? dsmod_sdk::Le16(s.data() + o) : 0;
}
inline uint32_t Le32(std::span<const uint8_t> s, uint64_t o) {
    return Fits(s, o, 4) ? dsmod_sdk::Le32(s.data() + o) : 0;
}
inline uint64_t Le64(std::span<const uint8_t> s, uint64_t o) {
    return Fits(s, o, 8) ? dsmod_sdk::Le64(s.data() + o) : 0;
}
inline uint16_t Be16(std::span<const uint8_t> s, uint64_t o) {
    return Fits(s, o, 2) ? dsmod_sdk::Be16(s.data() + o) : 0;
}
inline uint32_t Be32(std::span<const uint8_t> s, uint64_t o) {
    return Fits(s, o, 4) ? dsmod_sdk::Be32(s.data() + o) : 0;
}
/// Little-endian IEEE float.
inline float F32(std::span<const uint8_t> s, uint64_t o) {
    const uint32_t u = Le32(s, o);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
/// u16/u32 in the byte order of the file (BOM-dependent formats: SARC, MSBT).
inline uint16_t U16(std::span<const uint8_t> s, uint64_t o, bool big_endian) {
    return big_endian ? Be16(s, o) : Le16(s, o);
}
inline uint32_t U32(std::span<const uint8_t> s, uint64_t o, bool big_endian) {
    return big_endian ? Be32(s, o) : Le32(s, o);
}

} // namespace acnh::bytes
