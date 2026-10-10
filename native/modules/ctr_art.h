// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Crash Team Racing Nitro-Fueled: the bottom screen's pictures, composed at runtime from the
// player's own romfs (nothing extracted ships in the package). Keys ("module:ctr:<kind>:...").
//   bg:<track>                 1240x1080 blue gradient + the track's race-select pattern tile
//   map:<track>                the track minimap texture, cropped to its ribbon, fitted to the
//                              page's map box (MapBoxW x MapBoxH); view published for markers
//   portrait:<Driver>[:<pkg>]  framed HUD portrait (frame bg + portrait + contour), 128 px; <pkg>
//                              the outfit material package selects the equipped skin's picture
//   head:<Driver>[:<pkg>]      minimap head, HeadPx; headp:... the player's, with the ring
//   name:<w|n>:<text>          a racer name in the game's Zoinks_Outline (w white, n navy)
//   digit:<w|g>:<n>            italic rank digit in ZoinksXL_Outline (w white, g gold)
//   rank:<n>                   the HUD's orange-gradient big rank with its ordinal
//   lap:<cur>:<total>          LAP cur/total
//   tab                        the native selected-row white tab
//   flag                       the HUD's finished flag, 48 px
//   tdigit:<0-9|colon>         one race-timer character (orange game font, fixed box)
//   timer                      the HUD stopwatch icon (dial + hand)
//   loading                    "WAITING FOR RACE" (trophy + flags + hint panel), full canvas
//   wrongpatch:<base|old>      "UPDATE REQUIRED" for a release other than 1.0.15, full canvas
//   cog                        the settings button (options cog on a navy disc), CogPx
//   set:bg                     the HUD settings page backdrop (header, hint, BACK footer)
//   set:row:<i>:<0|1>          settings row i (0 rank list .. 4 timer), value SHOW (0) / HIDE (1)
// Track keys are the level's folder name, e.g. "t111_crash_cove"; Driver keys the driver data
// name, e.g. "DriverCrash".

#pragma once

#include "ctr_alchemy.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct EdenDsmodHostApi;

namespace CtrArt {

/// A romfs reader over `host` (borrowed: use it only while `host` is valid).
CtrAlchemy::ReadAt Reader(const EdenDsmodHostApi& host, const std::string& path);

inline constexpr int CanvasW = 1240, CanvasH = 1080;
inline constexpr int MapBoxX = 470, MapBoxY = 125, MapBoxW = 755, MapBoxH = 925;
/// Map heads: other racers, and the player's (its head inside the ring picture of PlayerMarkerPx).
inline constexpr int HeadPx = 78, PlayerHeadPx = 100, PlayerMarkerPx = 128;
/// Fixed picture boxes for text (left/centre, centre/bottom, right/bottom, right/top aligned).
inline constexpr int NameW = 300, NameH = 72, DigitW = 64, DigitH = 80, RankW = 420, RankH = 330,
                     LapW = 300, LapH = 110;
/// The HUD settings page: one row picture (label + option bar).
inline constexpr int SetRowW = 900, SetRowH = 230, CogPx = 112;
/// The ranking column: row 0's top and the row pitch (the cards slide in steps of it).
inline constexpr int RankRowY = 22, RankRowDy = 132;
/// The race timer: per-character boxes and the stopwatch icon.
inline constexpr int TimeDigitW = 38, TimeColonW = 24, TimeH = 78, TimerIconPx = 72;

/// Where the cropped minimap texture landed in the map box: canvas = box + (tex - crop) * scale.
struct MapView {
    CtrAlchemy::MinimapParams params;
    float crop_x{}, crop_y{}, scale{1}, tex{800};
    float off_x{}, off_y{}; ///< canvas position of the crop's top-left
};

class Library {
public:
    friend struct LibraryTestAccess;
    /// Decodes and composes one key ("module:ctr:..."); null when unknown or unavailable.
    std::shared_ptr<const CtrAlchemy::Image> Load(const EdenDsmodHostApi& host, std::string_view key);
    /// The view of a track's map once `map:<track>` was composed (any thread).
    std::optional<MapView> View(const std::string& track);
    /// Data-model minimap parameters for a track (decodes on a miss: call from the loader).
    std::optional<CtrAlchemy::MinimapParams> Params(const EdenDsmodHostApi& host,
                                                    const std::string& track);

private:
    /// `romfs_path` may name a member archive: "archives/update.pak#archives/DriverBAL.pak".
    std::shared_ptr<CtrAlchemy::Archive> ArchiveFor(const EdenDsmodHostApi& host,
                                                    const std::string& romfs_path);
    /// Never empty: a member that cannot be (re)located reads 0 bytes.
    CtrAlchemy::ReadAt ReaderFor(const EdenDsmodHostApi& host, const std::string& romfs_path);
    /// An outfit material package's own HUD textures (short names) and where to look for them;
    /// empty names for a default skin.
    struct SkinPictures {
        std::string portrait, head;
        std::vector<std::string> paks;
    };
    SkinPictures SkinFor(const EdenDsmodHostApi& host, const std::string& package);
    /// The track's archive: archives/<track>.pak, else the copy inside archives/update.pak.
    std::string TrackPak(const EdenDsmodHostApi& host, const std::string& track);
    /// The race-select gradient with a pak's pattern tile (track backgrounds).
    CtrAlchemy::Image PatternBackdrop(const EdenDsmodHostApi& host, const std::string& pak,
                                      std::initializer_list<std::string_view> tile_name, bool calm);
    /// The full-canvas blue backdrop of the waiting / wrong-patch pages.
    CtrAlchemy::Image Backdrop(const EdenDsmodHostApi& host);
    /// Caches an archive lookup (hits and, bounded, misses).
    std::shared_ptr<CtrAlchemy::Archive> Remember(const std::string& path,
                                                  std::shared_ptr<CtrAlchemy::Archive> a);
    struct Nested {
        std::uint64_t offset{}, length{}; ///< a stored member: a window of the outer file
        CtrAlchemy::Bytes bytes;          ///< a packed member: decoded once
    };
    /// A member archive's bytes/window, decoded again after the budget dropped it.
    std::shared_ptr<const Nested> NestedFor(const EdenDsmodHostApi& host, const std::string& path);
    std::optional<CtrAlchemy::Image> Texture(const EdenDsmodHostApi& host, const std::string& pak,
                                             std::initializer_list<std::string_view> fragments);
    const CtrAlchemy::Font* FontFor(const EdenDsmodHostApi& host, const std::string& name);

    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<CtrAlchemy::Archive>> archives;
    std::unordered_map<std::string, std::unique_ptr<CtrAlchemy::Font>> fonts;
    std::unordered_map<std::string, std::shared_ptr<const Nested>> nested;
    std::unordered_map<std::string, SkinPictures> skins;
    std::unordered_map<std::string, MapView> views;
    std::unordered_map<std::string, std::optional<CtrAlchemy::MinimapParams>> params;
    std::unordered_map<std::string, std::shared_ptr<const CtrAlchemy::Image>> ui_textures;
    static constexpr std::size_t NestedBudget = 128u << 20; ///< decoded member archives kept
    std::size_t nested_bytes{};
};

} // namespace CtrArt
