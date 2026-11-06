// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Item icons from the player's game files (nothing is shipped).
//
// Path: "Inventory Icon Mapping" (MonoBehaviour in resources.assets: ITEM_TYPE → Sprite) →
// Sprite (RenderDataKey + atlas tag, raw layout Unity 2022.3) to SpriteAtlas in the Addressables bundle
// (atlas_bundles_1_5_27.inc, with TypeTree) to a rectangle in the atlas texture, then cropped.
// Details in tools/cotl/README.md (inventory icons).
//
// Likewise weapons/curses (WeaponData/CurseData to UISprite) and relics (RelicData to UISprite) from
// resources.assets; HUD hearts and tarot cards from Spine atlases (spine_atlas.h).
#pragma once

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "chained_echoes_unity.h"

namespace cotl {

// "Inventory Icon Mapping" raw: ITEM_TYPE to sprite PathID (only references into the same file)
std::optional<std::map<int32_t, int64_t>> ParseIconMapping(const std::vector<uint8_t>& raw);

struct SpriteRef {
    std::array<uint32_t, 4> guid{}; // m_RenderDataKey.first
    int64_t second = 0;             // m_RenderDataKey.second
    std::string atlas_tag;          // first m_AtlasTags entry, empty without an atlas
    float x = 0, y = 0, w = 0, h = 0; // m_Rect
    int64_t texture = 0;            // m_RD.texture: own texture in the same file, 0 in an atlas
};

// m_Script PathIDs in globalgamemanagers.assets (1.5.27)
inline constexpr int64_t kScriptCurseData = 372;
inline constexpr int64_t kScriptEquipmentData = 1116;
inline constexpr int64_t kScriptRelicData = 2731;
inline constexpr int64_t kScriptWeaponData = 3987;

enum class DataKind { Equipment, Relic };

struct DataSprite {
    DataKind kind = DataKind::Equipment;
    int32_t type = 0;   // EquipmentType or RelicType
    int64_t sprite = 0; // UISprite PathID in the same file
};

// EquipmentData/RelicData MonoBehaviour raw up to UISprite; nullopt for other scripts
std::optional<DataSprite> ParseDataSprite(const std::vector<uint8_t>& raw);

// Sprite object raw (Unity 2022.3, no TypeTree) up to and including m_RD.texture
std::optional<SpriteRef> ParseSpriteRef(const std::vector<uint8_t>& raw);

// Texture2D object raw (Unity 2022.3, no TypeTree); image data in m_StreamData
std::optional<ce_unity::TextureInfo> ParseRawTexture(const std::vector<uint8_t>& raw);

// Crops a rectangle in Unity coordinates (origin bottom left) from an image with
// its origin at the top left.
std::optional<ce_unity::Image> CropUnityRect(const ce_unity::Image& tex, float x, float y, float w,
                                              float h);

// Section of an ASTC 12x12 texture in Tegra block layout (m_PlatformBlob[8]: log2 GOBs per block);
// rectangle in Unity coordinates, image with origin top left.
std::optional<ce_unity::Image> DecodeSwitchSprite(std::span<const uint8_t> data,
                                                  const ce_unity::TextureInfo& tex,
                                                  const ce_unity::AstcDecoder& astc, float x, float y,
                                                  float w, float h);

// ASTC texture (8×8 or 12×12) from the Tegra block layout into rows of whole blocks; the
// follower page (4096², 8×8) is unscrambled once this way and then decoded per region.
struct LinearAstc {
    std::vector<uint8_t> blocks;
    uint32_t row_blocks = 0, col_blocks = 0, block = 0;
    int32_t width = 0, height = 0;
};
std::optional<LinearAstc> DeswizzleSwitchAstc(std::span<const uint8_t> data, const ce_unity::TextureInfo& tex);
// Rectangle in Unity coordinates (origin bottom left), image with origin top left
std::optional<ce_unity::Image> DecodeLinearRect(const LinearAstc& lin, const ce_unity::AstcDecoder& astc,
                                                float x, float y, float w, float h);

// ASTC decoder via the host that decodes large textures in tiles of whole blocks:
// ce_unity::MakeHostAstcDecoder rejects outputs > 16 MiB, Eden's decode_astc widths/heights
// > 2048 (2048 atlases are padded to 2052). max_tile_bytes limits the output per
// host call, max_width the tile width (and height).
ce_unity::AstcDecoder MakeTiledAstcDecoder(void* userdata, EdenDsmodAstcDecoder decode,
                                           size_t max_tile_bytes = 8u << 20,
                                           uint32_t max_width = 2048);

struct IconLocation {
    std::string bundle;   // Addressables bundle relative to Data/StreamingAssets/aa/Switch/
    std::string texture;  // name of the atlas texture
    float x = 0, y = 0, w = 0, h = 0;
    uint32_t settings = 0; // settingsRaw (bit 0 packed, bits 2-5 rotation)
};

class IconLoader {
public:
    IconLoader(ce_unity::RangeReader romfs, ce_unity::AstcDecoder astc);
    ~IconLoader();

    // Location of the icon without decoding (testable offline); nullopt without an atlas entry
    std::optional<IconLocation> Locate(int32_t item_type);
    // RGBA icon; decoded atlas textures are cached
    std::optional<ce_unity::Image> LoadItem(int32_t item_type);
    // Weapon or curse (EquipmentType), relic (RelicType)
    std::optional<ce_unity::Image> LoadEquipment(int32_t type);
    std::optional<ce_unity::Image> LoadRelic(int32_t type);
    // Heart as on the HUD: kind red/spirit/blue/black/fire/ice, state full/half/empty
    std::optional<ce_unity::Image> LoadHeart(const std::string& kind, const std::string& state);
    // Front of a tarot card (TarotCards.Card)
    std::optional<ce_unity::Image> LoadTarot(int32_t card);
    // In-game map sprite from the MapIcons atlas (room icons "Icon_Boss" ..., "Map_You")
    std::optional<ce_unity::Image> LoadMapIcon(const std::string& sprite);
    // Head of a follower (module:cotl:follower/<arg>, follower_portrait.h): skin and slot colours
    std::optional<ce_unity::Image> LoadFollower(const std::string& arg);
    // Reason for the last failure (for the log)
    std::string LastError() {
        std::lock_guard lock(mutex_);
        return last_error_;
    }

private:
    bool EnsureMapping();
    bool EnsureData();
    std::optional<IconLocation> LocateSprite(int64_t sprite_path_id); // mutex_ held
    std::optional<IconLocation> LocateRef(const SpriteRef& ref);       // mutex_ held
    std::optional<ce_unity::Image> LoadLocated(const std::optional<IconLocation>& loc,
                                               const std::string& what);
    // Icon with its own texture in resources.assets (no TypeTree, image data in .resS)
    std::optional<ce_unity::Image> LoadOwnTexture(int32_t item_type);
    // Spine atlas "<name>_Atlas" in the Spine bundle with its decoded page (mutex_ held)
    struct SpinePage;
    const SpinePage* EnsureSpine(const std::string& atlas_asset);
    // Follower.atlas and the unscrambled page; regions in the original frame (mutex_ held)
    struct FollowerPage;
    FollowerPage* EnsureFollowerPage();
    const ce_unity::Image* FollowerRegion(FollowerPage& page, std::string_view region);

    ce_unity::RangeReader romfs_;
    ce_unity::AstcDecoder astc_;
    std::unique_ptr<ce_unity::Reader> bundles_;
    std::shared_ptr<ce_unity::SerializedFile> resources_;
    std::optional<std::map<int32_t, int64_t>> mapping_;
    std::optional<std::map<int32_t, int64_t>> equipment_, relics_; // type → sprite PathID
    std::map<std::string, std::unique_ptr<SpinePage>> spine_;
    std::unique_ptr<FollowerPage> follower_;
    std::string follower_error_; // page could not be loaded: do not read again for every image
    using AtlasKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, int64_t>;
    std::map<std::string, std::map<AtlasKey, IconLocation>> atlases_; // atlas tag to its sprites
    std::list<std::pair<std::string, ce_unity::Image>> textures_;     // most recently used first
    std::string last_error_;
    std::mutex mutex_;
};

} // namespace cotl
