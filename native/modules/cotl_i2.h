// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// English texts from the game's I2 Localization (MonoBehaviour "I2Languages" in
// romfs:/Data/resources.assets). Nothing is shipped, everything comes from the game files.
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "chained_echoes_unity.h"

namespace cotl {

class I2Table {
public:
    // Reads the English column from `path` (default Data/resources.assets).
    static std::optional<I2Table> Load(const ce_unity::RangeReader& romfs,
                                       std::string_view path = "Data/resources.assets");
    // For tests: ready-made term to text mapping.
    static I2Table FromMap(std::map<std::string, std::string> terms);

    std::optional<std::string> English(std::string_view term) const;
    size_t Size() const { return terms_.size(); }

private:
    std::map<std::string, std::string, std::less<>> terms_;
};

} // namespace cotl
