// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tests only: a host whose read_romfs reads an extracted romfs (SW_ROMFS_DIR, never in the
// repository), for the checks that need the game's own files. Without it those checks are skipped.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

#include "core/mods/dsmod_module_abi.h"

namespace sw_test {

inline const char* RomfsDir() {
    const char* d = std::getenv("SW_ROMFS_DIR");
    return d && *d ? d : nullptr;
}

inline std::size_t ReadRomfs(void*, const char* path, std::uint64_t offset, void* out, std::size_t size) {
    std::string p = path;
    if (p.rfind("romfs:", 0) != 0 || !RomfsDir()) return 0;
    std::ifstream f(std::string(RomfsDir()) + p.substr(6), std::ios::binary | std::ios::ate);
    if (!f) return 0;
    const auto total = static_cast<std::size_t>(f.tellg());
    if (!out) return total;
    if (offset >= total) return 0;
    const std::size_t n = std::min<std::size_t>(size, total - offset);
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
    return n;
}

inline EdenDsmodHostApi RomfsHost() {
    EdenDsmodHostApi host{};
    host.read_romfs = &ReadRomfs;
    return host;
}

inline std::uint64_t Fnv(const std::string& text) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const unsigned char c : text) h = (h ^ c) * 0x100000001B3ull;
    return h;
}

} // namespace sw_test
