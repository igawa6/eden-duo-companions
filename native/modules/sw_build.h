// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pokémon Sword build identification; the values are the variant record's (sw_variant.h).
// Supported build: 1.3.2 (update v458752, Eden logs "Update (v0.7.0)"), main NSO build id A3B75BCD3311385AEED67FBEEB79CBB7BF02F471 (confirmed
// 2026-10-24; the first 16 hex characters, A3B75BCD3311385A, are also the package's
// <BUILD16> data-file prefix per PACKAGE_FORMAT.md section 1). Other Sword build ids below are
// leads only: 1.3.1's id is source-confirmed (a public cheat-code thread), 1.3.0's was not
// captured. Neither has been dumped or RE'd this cycle, so they are "known, not supported" --
// the module still loads for them and shows an update message, it reads nothing.

#pragma once

#include <cstdint>
#include <cstring>

#include "sw_variant.h"

namespace sw_build {

using u8 = std::uint8_t;
using u64 = std::uint64_t;

// Title ids (package.json / manifest.json "title_id" and the module's own gate). The values live
// in the variant record (sw_variant.h, generated from tools/pokemon-sword/sw_variants.py).
constexpr u64 kTitleId = sw_variant::kActive.title_id;               // the title this module serves
constexpr u64 kSwordTitleId = sw_variant::kSword.title_id;
constexpr u64 kShieldTitleId = sw_variant::kShield.title_id;
constexpr u64 kAocIsleOfArmorTitleId = sw_variant::kActive.aoc_title_ids[0]; // optional, license-stub AOC
constexpr u64 kAocCrownTundraTitleId = sw_variant::kActive.aoc_title_ids[1]; // optional, license-stub AOC

// The supported build's first 20 bytes (40 hex chars), as confirmed this cycle. A real NSO
// build id is 32 bytes; the remaining 12 are not recorded this cycle and are therefore not
// compared -- SupportsBuild below only requires the known 20-byte prefix to match, which is
// stricter than the package manifest's 16-hex (8-byte) build_ids gate but not the full 32-byte
// exact compare MODULE_GUIDE.md recommends ("P5R accepts only the exact 64-character id"). A
// future session with a verified full id should tighten this to all 32 bytes.
constexpr std::size_t kKnownBuildIdBytes = sizeof sw_variant::Build::id;
inline constexpr const u8 (&kBuildId)[kKnownBuildIdBytes] = sw_variant::kBuild.id;

// The <BUILD16> prefix for the per-build data file / package module.build_ids entry.
constexpr const char* kBuildId16 = sw_variant::kBuild.id16;

// Other builds the module recognises by name but does not read (leads, not RE'd): the variant's
// other_builds. Sword 1.3.1 -- UNVERIFIED id (review L10): taken from a public "Pokemon
// Sword/Shield v1.3.1 CFW/EMU Cheat Codes" forum thread's header, never checked against a dump. It
// only names the build on the "update required" page; if it is wrong, a real 1.3.1 gets the host's
// generic "module failed to load" path instead. Nothing is read or patched for it either way.
using KnownBuild = sw_variant::KnownBuild;

inline bool BytesEqual(const u8* a, const u8* b, std::size_t n) {
    return std::memcmp(a, b, n) == 0;
}

/// "" = the supported build; a non-null, non-empty string = a known older build's label;
/// null = an id this module has never seen.
inline const char* BuildLabel(const u8* build_id_32) {
    if (BytesEqual(build_id_32, kBuildId, kKnownBuildIdBytes))
        return "";
    for (std::size_t i = 0; i < sw_variant::kActive.other_build_count; ++i) {
        const auto& b = sw_variant::kActive.other_builds[i];
        if (BytesEqual(build_id_32, b.id, sizeof b.id))
            return b.label;
    }
    return nullptr;
}

/// Parses the ABI's upper-case hex `supports_build` string the same way (module-side gate,
/// independent of and stricter than the package's own build_ids list -- MODULE_GUIDE.md
/// section 3: "the module's own check ... should be stricter").
inline bool ParseHexByte(const char* s, u8& out) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    const int hi = nibble(s[0]);
    const int lo = nibble(s[1]);
    if (hi < 0 || lo < 0) return false;
    out = static_cast<u8>((hi << 4) | lo);
    return true;
}

/// true for the supported build AND every known-but-unsupported one (the module loads either
/// way; Create() decides which mode by re-deriving the label from host->build_id). false for an
/// id this module has never catalogued at all -- GameModule::Load then shows the host's generic
/// "module failed to load" diagnostics, nothing this module authors.
inline bool SupportsBuildHex(const char* build_id_hex) {
    if (!build_id_hex || std::strlen(build_id_hex) < kKnownBuildIdBytes * 2)
        return false;
    u8 id[kKnownBuildIdBytes]{};
    for (std::size_t i = 0; i < kKnownBuildIdBytes; ++i)
        if (!ParseHexByte(build_id_hex + i * 2, id[i]))
            return false;
    return BuildLabel(id) != nullptr;
}

} // namespace sw_build
