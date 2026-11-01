// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Item id -> native icon romfs source. Unlike species box icons (sw_species_icons.h), item icons
// need no remap table: `romfs:/bin/appli/icon_item/item_%04d.bntx` is a standalone (non-SARC)
// .bntx holding exactly one texture, named identically to its own filename stem (confirmed this
// cycle with tools/bntx_decode.py against the real extracted romfs: item_0017.bntx's internal
// texture name decodes as literally "item_0017", matching the "x.bntx#tex" 2-level convention
// sw_species_icons.h already documents and PACKAGE_FORMAT.md section 3.11 specifies for a
// standalone-bntx romfs source). ASSET-SURVEY.md's folder inventory counts 907 files
// `icon_item/item_0017.bntx`..`item_1606.bntx`-ish; not every item id present in
// ItemStorage8SWSH's pouch tables necessarily has a shipped icon file (the asset-free-package
// convention already in place elsewhere in this module means a missing file just fails the
// page's `need_bind` gate at runtime, never a guess), so this header only builds the source
// string -- it does not claim the file exists.

#pragma once

#include <cstdint>
#include <cstdio>

namespace sw_item_icons {

/// Highest item id Sword/Shield 1.3.2 defines (tools/pokemon-sword/item_ids_full.txt).
constexpr std::uint16_t kMaxItemId = 1607;

/// Builds the standalone-bntx romfs source for an item's icon. Fails closed (returns false, `out`
/// untouched) for item id 0 or past kMaxItemId (not a real item) or a too-small `out` buffer.
/// Callers also require the item's name to resolve (review L8: a garbage id must not claim an
/// icon).
inline bool IconSource(std::uint16_t item_id, char* out, std::size_t out_size) {
    if (item_id == 0 || item_id > kMaxItemId || !out || out_size == 0) return false;
    const int n = std::snprintf(out, out_size,
        "romfs:/bin/appli/icon_item/item_%04u.bntx#item_%04u",
        static_cast<unsigned>(item_id), static_cast<unsigned>(item_id));
    return n > 0 && static_cast<std::size_t>(n) < out_size;
}

} // namespace sw_item_icons
