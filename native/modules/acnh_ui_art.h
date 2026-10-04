// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Every image the ACNH companion serves as module:acnh/<key>, built at runtime from the player's
// romfs (nothing shipped). The recipes port research/acnh/tools/acnh_mock.py (round 3 mocks,
// research/acnh/design/MOCKS.md); colour literals there are material/FLMC colours of the named
// panes (DATA), samples of the game's screen (LIVE) or our composition (LAYOUT), as tagged in
// the comments next to each recipe.
//
// Key grammar: "<path>[?<k>=<v>&...]". Sizes are canvas pixels; colours are "rrggbb[aa]" without
// '#'. Unknown parameters are ignored; a malformed key yields no image. Full list with defaults:
// research/acnh/impl/core/REPORT.md ("Image keys"), and Art::Keys() for tests/tools.
//
//   icon/item/<id>              pocket icon (Model/Layout_MenuIcon_<ItemMenuIcon.ResName>)
//   icon/itemart/<id>           full item art (FtrIcon/ClosetIcon/DIYRecipeIcon/...; else pocket)
//   icon/crit/<kind>/<uid>      critter's pocket icon (kind 0 insect, 1 fish, 2 sea; StatusParam
//   uid) icon/critbook/<kind>/<uid>  Critterpedia illustration icon/npc/<label>            villager
//   icon (Layout_NpcIcon_<label>) icon/app/<frame>            NookPhone app button
//   (LMenuDeviceBtn_Type frame 1..20) icon/quest/<genre>          Nook Miles+ task icon
//   (Layout_DailyQuestIcon_DailyQuest<NN>) num/outline/<digits>        SystemBOutline_00 digits
//   (?h=25&fill=312a1fc0&line=fff1c3ff) font/<spec>                 the font atlas (acnh_font.h
//   spec; also served as load_data bytes) photo/passport              the passport photo (?d=<px>
//   square; ?mask=1 ProfilePhotoMaskView) bg/<theme>/<page>           page background at canvas
//   size (?w=1240&h=1080); theme 0 =
//                               "Original" (round-2 per-page look), 1.. = the round-3 swatches
//   ui/<recipe>                 chrome elements (acnh_ui_pages.cpp)
//
// Display size (the runtime draws images nearest-neighbour, so keys ask for the widget size):
// icon/* and map/icon/* take ?s=<px> (fit, centred on an s x s square) or ?w=/?h= (exact; one
// alone keeps the aspect), via draw::SizeTo (linear-light area average). map/icon/<kind>?rot=90
// rotates counter-clockwise first. num/*/<digits>?w=<px> = fixed-width box, digits right-aligned.
// ui/today/photoframe takes ?c=; ui/wait/loadicon takes ?w= (the 300 x 200
// composition scaled); any ui/ key takes ?sc=<rgba> (its shadow_of silhouette, for a separate
// shadow widget).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "acnh_lang.h"
#include "acnh_types.h"

namespace acnh {

class Romfs;
class Catalog;
class Fonts;
class Layout;

/// Parsed "<path>?k=v&k=v".
struct ArtKey {
    std::string path;
    std::vector<std::string> parts; ///< path split on '/'
    std::map<std::string, std::string, std::less<>> params;
    static bool Parse(std::string_view key, ArtKey& out);
    double Num(std::string_view k, double fallback) const;
    int Int(std::string_view k, int fallback) const;
    Rgba Col(std::string_view k, Rgba fallback) const;
    bool Flag(std::string_view k) const {
        return Int(k, 0) != 0;
    }
    std::string Str(std::string_view k, std::string fallback = {}) const;
};

class Art {
public:
    struct Sources {
        std::function<bool(std::vector<uint8_t>&, uint64_t&)> passport_jpeg;
        std::function<Lang()> language;
    };
    Art(Romfs& romfs, Catalog& catalog, Fonts& fonts, Sources sources);
    ~Art();

    /// Any key above (without "module:acnh/"). Thread-safe; results cached (LRU by bytes).
    bool Load(std::string_view key, Image& out);
    /// Representative keys of every recipe family (tests, the debug page, tools).
    static std::vector<std::string> Keys();

    // ---- shared helpers for the recipe files (acnh_ui_pages.cpp, acnh_ui_bg.cpp) ----
    /// A layout texture (selectors applied) or an empty image.
    Image Tex(std::string_view layout, std::string_view tex);
    std::shared_ptr<Layout> Lyt(std::string_view layout);
    /// First texture of Model/<archive> (icon archives); empty when missing.
    Image ModelIcon(std::string_view archive);
    Romfs& Rom() {
        return romfs;
    }
    Catalog& Cat() {
        return catalog;
    }
    Fonts& Font() {
        return fonts;
    }
    Lang Language();
    /// The passport photo, decoded (cached by revision); false when not available.
    bool Passport(Image& out);

    size_t CachedBytes();

private:
    bool Build(const ArtKey& key, Image& out);

    Romfs& romfs;
    Catalog& catalog;
    Fonts& fonts;
    Sources sources;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const Image>> cache;
    std::vector<std::string> lru;
    size_t cached = 0;
    std::shared_ptr<const Image> photo;
    uint64_t photo_rev = 0;
};

// Recipe families (defined in acnh_ui_pages.cpp / acnh_ui_bg.cpp).
bool BuildUi(Art& art, const ArtKey& key, Image& out);
bool BuildBackground(Art& art, const ArtKey& key, Image& out);
bool BuildAppButton(Art& art, int frame, Image& out);
/// The theme list: index -> BACKGROUNDS.tsv catalog number (0 = Original, not a catalog row).
const std::vector<int>& ThemeCatalogNumbers();

} // namespace acnh
