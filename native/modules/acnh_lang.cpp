// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "acnh_lang.h"

#include <array>
#include <utility>

namespace acnh {
namespace {
constexpr std::array<std::string_view, static_cast<int>(Lang::Count)> Folders{
    "USen", "USes", "USfr", "EUen", "EUde", "EUes", "EUfr",
    "EUit", "EUnl", "EUru", "JPja", "KRko", "CNzh", "TWzh"};
} // namespace

std::string_view LangFolder(Lang lang) {
    const int i = static_cast<int>(lang);
    return i >= 0 && i < static_cast<int>(Folders.size()) ? Folders[i] : Folders[0];
}

Lang LangFromFolder(std::string_view folder, Lang fallback) {
    for (size_t i = 0; i < Folders.size(); ++i)
        if (Folders[i] == folder)
            return static_cast<Lang>(i);
    return fallback;
}

bool LangIsCjk(Lang lang) {
    return lang == Lang::JPja || lang == Lang::KRko || lang == Lang::CNzh || lang == Lang::TWzh;
}

} // namespace acnh
