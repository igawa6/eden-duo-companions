// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <charconv>
#include <climits>
#include <cstddef>
#include <optional>
#include <string_view>

namespace ce_parse {
// Image keys and action suffixes contain unsigned decimal indices, never table numbers.
// Reject overflow and trailing text instead of silently mapping malformed input to a cell.
inline std::optional<int> Index(std::string_view text) {
    if (text.empty() || text.front() < '0' || text.front() > '9')
        return std::nullopt;
    int value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return std::nullopt;
    return value;
}

// A decimal number at the start of a game table cell or text, read the way strtol reads it:
// leading whitespace, an optional sign, then digits; what follows is left alone ("5 chests" is 5)
// and its offset is stored in `end`. nullopt when no digit follows or the value does not fit an
// int, where atoi was undefined and strtol clamped. Allocation free, locale independent.
inline std::optional<int> Leading(std::string_view text, std::size_t* end = nullptr) {
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || (text[i] >= '\t' && text[i] <= '\r')))
        ++i;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
        negative = text[i++] == '-';
    if (i >= text.size() || text[i] < '0' || text[i] > '9')
        return std::nullopt;
    long long value{};
    const auto result = std::from_chars(text.data() + i, text.data() + text.size(), value);
    if (result.ec != std::errc{})
        return std::nullopt;
    if (negative)
        value = -value;
    if (value < INT_MIN || value > INT_MAX)
        return std::nullopt;
    if (end)
        *end = static_cast<std::size_t>(result.ptr - text.data());
    return static_cast<int>(value);
}

// A whole cell as a number (Leading with nothing after the digits); nullopt otherwise.
inline std::optional<int> Whole(std::string_view text) {
    std::size_t end{};
    const auto value = Leading(text, &end);
    return value && end == text.size() ? value : std::nullopt;
}
} // namespace ce_parse
