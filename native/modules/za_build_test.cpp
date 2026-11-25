// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
#include "za_build.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

int main() {
    // The hex form and the byte form must describe the same build.
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string hex;
    for (const auto byte : za::kBuildId) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    assert(hex == za::kBuildHex);
    assert(za::BuildMatches(za::kBuildId));
    assert(za::SupportsBuildHex(hex.c_str()));
    assert(za::SupportsBuildHex(hex.substr(0, 40).c_str()));

    // A build that shares the first 8 bytes (the old, weaker check) must be refused.
    std::uint8_t other[32];
    std::memcpy(other, za::kBuildId, sizeof(other));
    other[19] ^= 1;
    assert(!za::BuildMatches(other));
    other[19] = za::kBuildId[19];
    other[31] = 1;
    assert(!za::BuildMatches(other));
    std::string wrong = hex;
    wrong[39] = '0';
    assert(!za::SupportsBuildHex(wrong.c_str()));
    assert(!za::SupportsBuildHex(hex.substr(0, 16).c_str()));
    assert(!za::SupportsBuildHex("") && !za::SupportsBuildHex(nullptr));
    std::puts("za-build: ok");
}
