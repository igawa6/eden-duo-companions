// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace za {

// The only supported executable: Pokemon Legends Z-A 2.0.2. The 20-byte build id is followed by
// zero padding in the host's 32-byte (64 hex digit) form, exactly as in the load plan name.
inline constexpr std::string_view kBuildHex =
    "B1F12FD919EAE86AB8A978317677E64BCE443D1F000000000000000000000000";
inline constexpr std::uint8_t kBuildId[32] = {
    0xB1, 0xF1, 0x2F, 0xD9, 0x19, 0xEA, 0xE8, 0x6A, 0xB8, 0xA9, 0x78, 0x31, 0x76, 0x77, 0xE6, 0x4B,
    0xCE, 0x44, 0x3D, 0x1F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// Compares all 32 bytes of the host's build id.
inline bool BuildMatches(const std::uint8_t* build_id) {
    return build_id && std::memcmp(build_id, kBuildId, sizeof(kBuildId)) == 0;
}

// The host passes the running build id as upper-case hex. Accept the exact 64 digit id, or the
// bare 40 digit id when the host trimmed the zero padding. Nothing shorter, nothing else.
inline bool SupportsBuildHex(const char* hex) {
    if (!hex) return false;
    const std::string_view given{hex};
    return given == kBuildHex || given == kBuildHex.substr(0, 40);
}

}  // namespace za
