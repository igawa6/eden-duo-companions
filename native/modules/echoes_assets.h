// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "acnh_types.h"
#include <functional>
#include <memory>
#include <string_view>

namespace echoes {
using Image = acnh::Image;
// Null output and zero size queries the complete resource size. Other calls return bytes read.
// The reader resolves native archive-member paths in the player's effective RomFS.
using ReadAssetFn = std::function<size_t(std::string_view, uint64_t, void*, size_t)>;

// One instance belongs to the asset worker. Native sources and decoded images are bounded;
// install the host ASTC decoder with acnh::SetAstcDecoder before requesting images.
class Assets {
public:
    explicit Assets(ReadAssetFn read);
    ~Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;
    // background, divider, cell/<width>/<height>, cursor/<width>/<height>.
    // Failure always clears out. Cell/cursor corners retain their original pixel dimensions.
    bool Load(std::string_view key, Image& out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace echoes
