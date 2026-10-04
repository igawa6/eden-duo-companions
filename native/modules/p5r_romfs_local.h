// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Tests/tools only: a p5r_assets::ReadFn over a local directory laid out like the game's romfs
// (<root>/CPK/ALL_USEU.CPK, <root>/CPK/PATCH1.CPK). Not used by the
// shipped module (which reads through host.read_romfs).
#include <cstdio>
#include <string>

#include "p5r_romfs_assets.h"

namespace p5r_assets {

inline ReadFn LocalDirReader(std::string root) {
    return [root = std::move(root)](const char* path, uint64_t offset, void* out,
                                    size_t size) -> size_t {
        const std::string full = root + "/" + path;
        std::FILE* f = std::fopen(full.c_str(), "rb");
        if (!f)
            return 0;
        size_t result = 0;
        if (!out) {
            if (fseeko(f, 0, SEEK_END) == 0)
                result = size_t(ftello(f));
        } else if (fseeko(f, off_t(offset), SEEK_SET) == 0) {
            result = std::fread(out, 1, size, f);
        }
        std::fclose(f);
        return result;
    };
}

} // namespace p5r_assets
