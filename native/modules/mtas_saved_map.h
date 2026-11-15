// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "chained_echoes_unity.h"

#include <functional>
#include <optional>
#include <span>
#include <string>

namespace mtas {

// The save container and image have independent bounds. No image-magic scanning.
constexpr std::size_t SavedMapMaxSave = 32u * 1024u * 1024u;
std::optional<ce_unity::Image> DecodeSavedFog(std::span<const std::uint8_t> save);

// Read-only host callback adapter, including null-output size queries.
using SaveMapRead = std::function<std::size_t(const char*, std::uint64_t, void*, std::size_t)>;
class SavedMapCache {
public:
    // Call at a low rate. A new load/slot must pass reset=true, even if the name
    // is unchanged. Unreadable, unstable and malformed files clear the image.
    bool Poll(const std::string& path, bool reset, const SaveMapRead& read);
    void Clear();
    const std::optional<ce_unity::Image>& Fog() const { return fog_; }
    std::uint64_t Revision() const { return revision_; }
private:
    std::string path_;
    std::vector<std::uint8_t> bytes_;
    std::optional<ce_unity::Image> fog_;
    std::uint64_t revision_{};
};
} // namespace mtas
