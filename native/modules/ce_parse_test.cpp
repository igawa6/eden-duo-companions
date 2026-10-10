// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ce_parse.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/// strtol's reading of an in-range cell, the reference Leading must reproduce.
std::optional<int> Strtol(const std::string& s) {
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return end == s.c_str() ? std::nullopt : std::optional<int>{static_cast<int>(v)};
}

} // namespace

int main() {
    for (const auto bad : {"", "-1", "+1", " 1", "1 ", "0tail", "2147483648",
                           "999999999999999999999999999999999999"}) {
        if (ce_parse::Index(bad)) {
            std::fprintf(stderr, "accepted malformed index: %s\n", bad);
            return 1;
        }
    }
    if (ce_parse::Index("0") != 0 || ce_parse::Index("1007") != 1007 ||
        ce_parse::Index("2147483647") != 2147483647)
        return 1;

    // Leading: strtol semantics on table cells ("5", "-1", " 12", "5 chests", "+3")
    for (const std::string cell : {"0", "5", "-1", " 12", "\t7", "5 chests", "+3", "007", "-0",
                                   "2147483647", "-2147483648", "12;34", "3.5", "1e3"})
        Expect(ce_parse::Leading(cell) == Strtol(cell), cell.c_str());
    for (const auto none : {"", " ", "-", "+", "abc", "- 1", ".5", "x12"})
        Expect(!ce_parse::Leading(none), none);
    // out of int range: atoi was undefined, strtol clamped; Leading refuses
    for (const auto big : {"2147483648", "-2147483649", "99999999999999999999999"})
        Expect(!ce_parse::Leading(big), big);
    std::size_t end{};
    Expect(ce_parse::Leading("5/9 chests", &end) == 5 && end == 1, "end offset");
    Expect(ce_parse::Leading("  -42x", &end) == -42 && end == 5, "end offset after sign");

    // Whole: the cell must be the number only
    Expect(ce_parse::Whole("17") == 17 && ce_parse::Whole("-3") == -3 && ce_parse::Whole(" 4") == 4,
           "whole numbers");
    for (const auto bad : {"17 ", "1.5", "4a", "", "-"})
        Expect(!ce_parse::Whole(bad), bad);
    return failures == 0 ? 0 : 1;
}
