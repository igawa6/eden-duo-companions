// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Animal Crossing: New Horizons 3.0.3 companion: types shared by every acnh_* file, and the seams
// between the module's lanes.
//
//   core   ABI glue (01006F8002326000.cpp), the asset layer (romfs, textures, tables, text, fonts,
//          UI art, backgrounds, passport photo) and the image dispatcher.
//   live   acnh_live* / acnh_publish* / acnh_bag*: resolves and samples the game, publishes values,
//          the Bag page's guarded writes; implements the Live* functions below.
//   map    acnh_map*: the island raster and icons; serves module:acnh/map/... (MapLoadImage).
//
// Threads: sample/tick/on_action run on the host's timing thread; load_image / load_data on one
// asset worker that may overlap them. Everything the worker reaches is internally locked.
#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

struct EdenDsmodHostWriteApi;

namespace acnh {

/// Tightly packed RGBA8, straight (not premultiplied) alpha, row-major, top row first.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;

    Image() = default;
    /// A negative side is an empty image (never a size_t wrap-around allocation).
    Image(int width, int height)
        : w{width > 0 ? width : 0}, h{height > 0 ? height : 0},
          rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0) {}
    bool Empty() const {
        return w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4;
    }
    uint8_t* At(int x, int y) {
        return rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
    }
    const uint8_t* At(int x, int y) const {
        return rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
    }
};

/// An 8-bit RGBA colour ("#rrggbb[aa]" in recipes; six digits mean alpha 0xFF).
struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

class Romfs;
class Catalog;
class Fonts;

/// What the core hands to the other lanes. All pointers stay valid for the module instance.
struct Services {
    Romfs* romfs = nullptr;     ///< the player's romfs (zstd/SARC helpers, archive cache)
    Catalog* catalog = nullptr; ///< item / critter / villager / recipe tables + game text
    Fonts* fonts = nullptr;     ///< the module's faces (one copy of each; null = the user's own)
    /// r9 pop: steady-clock ms (live::NowMs) of the last load_image start or end (asset worker),
    /// null = no picture prewarm (tests): the publisher then never holds the waiting page.
    const std::atomic<uint64_t>* image_activity_ms = nullptr;
    /// r9 pop: load_image calls running now.
    const std::atomic<int>* image_inflight = nullptr;
};

// ---- live lane seam (acnh_live.cpp) ---------------------------------------------------------
class Live; ///< opaque here; the live lane defines it

/// create(): `host` is borrowed (copy what you keep). Never throws; null = no live reader.
Live* LiveCreate(const EdenDsmodHostApi& host, const Services& services);
void LiveDestroy(Live* live);
/// Timing thread, once per sample. Publishes every live value (acnh.ready, clock.*, bag.* ...).
void LiveSample(Live* live, const EdenDsmodHostApi& host);
void LiveTick(Live* live, const EdenDsmodHostApi& host);
/// Module actions from the manifest (ui_open, crit_kind, phone_open, ...). True = accepted.
bool LiveAction(Live* live, const char* action, int64_t argument);
/// configure() of the write extension: the host's write_batch (copied). Without it the Bag page's
/// direct writes stay off (bag.can_write 0, "no write_batch").
void LiveSetWriteApi(Live* live, const EdenDsmodHostWriteApi& api);
/// Asset worker: a copy of the player's passport JPEG (Player.ProfileMain.JPEG) from guest
/// memory, and a change counter for it (0 = none yet). Must be safe against a concurrent sample.
bool LivePassportJpeg(Live* live, std::vector<uint8_t>& jpeg, uint64_t& revision);
/// The game's text language as a Lang value (acnh_lang.h), or -1 while unknown. Any thread.
int LiveLanguage(Live* live);

// ---- map lane seam (acnh_map.h, same declaration) -----------------------------------------
/// Serves module:acnh/map/<...>; the core passes the key without "module:acnh/" ("map/island/7").
/// Asset worker thread (acnh_map_icons.cpp).
bool MapLoadImage(Romfs& romfs, std::string_view key, Image& out);

} // namespace acnh
