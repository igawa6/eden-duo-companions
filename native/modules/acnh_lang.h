// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ACNH text languages: the 14 Message/<Group>_<Lang>.sarc.zs folders of the 3.0.3 romfs.
// The companion's text follows the GAME language (contract). How the live lane learns it is the
// live lane's business (the game's own folder table, acnh_live.h "language"); LangFromFolder maps
// the folder name ("USen") onto the enum.
#pragma once

#include <string_view>

namespace acnh {

enum class Lang : int {
    USen,
    USes,
    USfr,
    EUen,
    EUde,
    EUes,
    EUfr,
    EUit,
    EUnl,
    EUru,
    JPja,
    KRko,
    CNzh,
    TWzh,
    Count
};

std::string_view LangFolder(Lang lang); ///< "USen" ...
Lang LangFromFolder(std::string_view folder, Lang fallback = Lang::USen);
/// CJK languages need the CJK faces / a CJK atlas.
bool LangIsCjk(Lang lang);

} // namespace acnh
