// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fire Emblem Engage assets read from the player's own romfs at runtime: the game's message
// tables (localized unit/class/Emblem names, fee_text) and unit face thumbnails. Nothing decoded
// is persisted or shipped.
//
//   Ride-along: Data/StreamingAssets/aa/Switch/fe_assets_message/<region>/<locale>/
//     person.bytes.bundle (TextAsset "Person": MPID_/MGID_ names) and job.bytes.bundle
//     (TextAsset "Job": MJID_ class names). Loaded once per game language on a worker thread.
//   Faces: Data/StreamingAssets/aa/Switch/fe_assets_ui/common/unitlist/facethumb/facethumb.bundle
//     (SpriteAtlas v2 "FaceThumb", ASTC 5x5). Decoded on the host's asset worker through the
//     module image callback and cached per sprite name.
//
// Thread-safe: the text snapshot is swapped under a mutex (the reader samples it on the tick
// thread while the worker finishes loading), and lp_unity::Reader serialises its own cache.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/mods/dsmod_module_abi.h"

#include "fee_sprites.h"
#include "fee_text.h"
#include "lp_unity.h"

namespace fee {

/// Romfs prefix of the Addressables tree; bundle paths passed to lp_unity are relative to it.
inline constexpr std::string_view FeeAssetRoot = "/Data/StreamingAssets/aa/Switch/";

/// read_romfs as a random-access source (paths without a prefix are romfs paths).
lp_unity::RangeReader MakeRomfsReader(const EdenDsmodHostApi& api);

class Assets {
public:
    Assets(lp_unity::RangeReader read, std::string asset_root = std::string{FeeAssetRoot});
    ~Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    void SetAstcDecoder(lp_unity::AstcDecoder astc) {
        unity_.SetAstcDecoder(std::move(astc));
    }

    /// Starts loading the text tables for `lang` (App.Language.Langs). Cheap when the same
    /// language is already loaded or loading; a new language starts a fresh load.
    void EnsureLanguage(int lang);
    void Stop();

    int Language() const {
        return loaded_lang_.load(std::memory_order_relaxed);
    }
    /// Localized text for a game key ("" when the table is not ready or the label is missing).
    /// The player-name tag and the avatar's own name key (MPID_Lueur) become the player-given
    /// name once SetHeroName() has one (else the table's default, "Alear").
    std::string Text(std::string_view key) const;
    /// The avatar's player-given name (Unit.m_Edit), "" = unknown / not named.
    void SetHeroName(std::string name);
    std::string HeroName() const;

    /// A face sprite from the player's facethumb atlas; nullptr when absent or undecodable.
    std::shared_ptr<const lp_unity::Image> Face(std::string_view sprite);

    /// Whether the player's facethumb atlas has a sprite with this name (listed once, lazily).
    bool HasFace(std::string_view sprite);

    /// Synchronous table load (the worker's body; also the test entry point).
    bool LoadText(int lang);
    /// Test hook: use this decoded table as the loaded language.
    void InstallTable(fee_text::Table table);

    /// Native UI art (P0-3): "fee:sprite/..." keys, see fee_sprites.h. Built with this object
    /// over the same romfs reader (no second cache budget); never created lazily, because the
    /// sample thread and the host's asset worker both reach it.
    Sprites& SpritesLib() {
        return *sprites_;
    }

private:
    void Work(int lang);

    lp_unity::Reader unity_;
    mutable std::mutex text_mutex_;
    std::shared_ptr<const fee_text::Table> text_;
    std::string hero_name_; // text_mutex_
    std::atomic<bool> text_ready_{false};
    std::atomic<int> loaded_lang_{-1};
    std::atomic<int> wanted_lang_{-1};
    std::atomic<int> failed_lang_{-1}; // last language whose tables could not be read
    std::atomic<bool> loading_{false};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::mutex worker_mutex_;

    std::mutex face_mutex_;
    std::unordered_map<std::string, std::shared_ptr<const lp_unity::Image>> faces_;
    std::vector<std::string> face_lru_; // oldest first
    bool names_ready_{};
    std::unordered_set<std::string> names_;
    std::unique_ptr<Sprites> sprites_;
};

} // namespace fee
