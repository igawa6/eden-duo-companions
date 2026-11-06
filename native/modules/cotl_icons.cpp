// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_icons.h"

#include "cotl_follower_portrait.h"
#include "cotl_spine_atlas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace cotl {

namespace {

constexpr std::string_view kResources = "Data/resources.assets";
constexpr std::string_view kMappingName = "Inventory Icon Mapping";
constexpr std::string_view kBundleRoot = "Data/StreamingAssets/aa/Switch/";
constexpr int32_t kClassMonoBehaviour = 114;
constexpr int32_t kClassTexture2D = 28;
constexpr int32_t kClassSpriteAtlas = 687078895;
constexpr size_t kMonoBehaviourNameAt = 28; // m_GameObject PPtr, m_Enabled (aligned), m_Script
constexpr int32_t kMaxEntries = 4096;
constexpr uint32_t kMaxDataObjectBytes = 64 * 1024;
constexpr size_t kCachedTextures = 3;
constexpr uint32_t kMaxOwnTextureBytes = 32u << 20;
constexpr uint64_t kMaxFollowerTextureBytes = 16u << 20; // 4096² ASTC 8×8: 4 MiB
// Spine atlases (MonoBehaviour "<name>_Atlas", TextAsset and texture in the same file)
constexpr std::string_view kSpineBundle = "4cccbc033ad47e0c563830268671f0cd.bundle";
// Map icons (atlas MapIcons, sprites with TypeTree): room icons, player figure
constexpr std::string_view kMapIconsBundle = "f2a159f385d211425a4c13127a7d127b.bundle";
constexpr int32_t kClassSprite = 213;
constexpr int32_t kAstc8x8 = 51, kAstc12x12 = 53; // Unity TextureFormat

// TarotCards.Card to the card front region in WeaponCard.atlas
const std::map<int32_t, std::string_view>& TarotRegions() {
    static const std::map<int32_t, std::string_view> table = {
#define COTL_TAROT(card, region) {card, region},
#include "cotl_tarot_regions_1_5_27.inc"
#undef COTL_TAROT
    };
    return table;
}

// Little-endian reader with Unity alignment (strings to 4 bytes)
class Cursor {
public:
    Cursor(const std::vector<uint8_t>& b, size_t pos) : b_(b), pos_(pos) {}
    bool ok() const { return ok_; }
    template <class T>
    T Get() {
        T v{};
        if (!ok_ || sizeof(T) > b_.size() - std::min(pos_, b_.size())) {
            ok_ = false;
            return v;
        }
        std::memcpy(&v, b_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    void Skip(size_t n) {
        if (n > b_.size() - std::min(pos_, b_.size())) {
            ok_ = false;
        } else {
            pos_ += n;
        }
    }
    void Align() { pos_ = (pos_ + 3) & ~size_t{3}; }
    std::string Str() {
        const auto n = Get<int32_t>();
        if (!ok_ || n < 0 || static_cast<size_t>(n) > b_.size() - pos_) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(b_.data() + pos_), static_cast<size_t>(n));
        pos_ += static_cast<size_t>(n);
        Align();
        return s;
    }

private:
    const std::vector<uint8_t>& b_;
    size_t pos_;
    bool ok_ = true;
};

const std::map<std::string_view, std::string_view>& AtlasBundles() {
    static const std::map<std::string_view, std::string_view> table = {
#define COTL_ATLAS(atlas, bundle) {atlas, bundle},
#include "cotl_atlas_bundles_1_5_27.inc"
#undef COTL_ATLAS
    };
    return table;
}

uint32_t U32(const ce_unity::Value* v) {
    return v ? static_cast<uint32_t>(v->AsInt()) : 0;
}

} // namespace

std::optional<std::map<int32_t, int64_t>> ParseIconMapping(const std::vector<uint8_t>& raw) {
    Cursor c(raw, kMonoBehaviourNameAt);
    c.Str(); // m_Name
    const auto count = c.Get<int32_t>();
    if (!c.ok() || count < 0 || count > kMaxEntries) {
        return std::nullopt;
    }
    std::map<int32_t, int64_t> out;
    for (int32_t i = 0; i < count; ++i) { // ItemSpritePair: ItemType, Sprite PPtr
        const auto type = c.Get<int32_t>();
        const auto file = c.Get<int32_t>();
        const auto path = c.Get<int64_t>();
        if (!c.ok()) {
            return std::nullopt;
        }
        if (file == 0 && path != 0) {
            out[type] = path;
        }
    }
    return out;
}

std::optional<DataSprite> ParseDataSprite(const std::vector<uint8_t>& raw) {
    Cursor c(raw, kMonoBehaviourNameAt - 8);
    const auto script = c.Get<int64_t>(); // m_Script.m_PathID
    c.Str();                              // m_Name
    if (!c.ok()) {
        return std::nullopt;
    }
    DataSprite d;
    if (script == kScriptWeaponData || script == kScriptCurseData || script == kScriptEquipmentData) {
        d.kind = DataKind::Equipment;
        d.type = c.Get<int32_t>(); // EquipmentType
        c.Skip(4);                 // PrimaryEquipmentType
        c.Get<uint8_t>();          // CanBreakDodge
        c.Align();
    } else if (script == kScriptRelicData) {
        d.kind = DataKind::Relic;
        d.type = c.Get<int32_t>(); // RelicType
        c.Skip(4 * 4);             // InteractionType, RelicSubType, DamageType, DamageRequiredToCharge
    } else {
        return std::nullopt;
    }
    const auto file = c.Get<int32_t>(); // UISprite
    d.sprite = c.Get<int64_t>();
    if (!c.ok() || file != 0 || d.sprite == 0) {
        return std::nullopt;
    }
    return d;
}

std::optional<SpriteRef> ParseSpriteRef(const std::vector<uint8_t>& raw) {
    Cursor c(raw, 0);
    c.Str();                         // m_Name
    const float rx = c.Get<float>(), ry = c.Get<float>(), rw = c.Get<float>(), rh = c.Get<float>(); // m_Rect
    c.Skip(2 * 4 + 4 * 4);           // m_Offset, m_Border
    c.Skip(4 + 2 * 4);               // m_PixelsToUnits, m_Pivot
    c.Skip(4);                       // m_Extrude
    c.Get<uint8_t>();                // m_IsPolygon
    c.Align();
    SpriteRef s;
    s.x = rx;
    s.y = ry;
    s.w = rw;
    s.h = rh;
    for (auto& g : s.guid) {
        g = c.Get<uint32_t>();       // m_RenderDataKey.first (GUID)
    }
    s.second = c.Get<int64_t>();     // m_RenderDataKey.second
    const auto tags = c.Get<int32_t>();
    if (!c.ok() || tags < 0 || tags > 64) {
        return std::nullopt;
    }
    for (int32_t i = 0; i < tags; ++i) {
        std::string tag = c.Str();
        if (i == 0) {
            s.atlas_tag = std::move(tag);
        }
    }
    if (!c.ok()) {
        return std::nullopt;
    }
    c.Skip(4 + 8);                   // m_SpriteAtlas
    const auto file = c.Get<int32_t>();
    const auto texture = c.Get<int64_t>();
    if (c.ok() && file == 0) {
        s.texture = texture;
    }
    return s;
}

std::optional<ce_unity::TextureInfo> ParseRawTexture(const std::vector<uint8_t>& raw) {
    constexpr size_t kFormatToBlob = 88 - 28; // m_MipCount to m_ColorSpace
    constexpr int32_t kMaxBlob = 4096;
    Cursor c(raw, 0);
    ce_unity::TextureInfo t;
    t.name = c.Str();
    c.Skip(4 + 4);                   // m_ForcedFallbackFormat, m_DownscaleFallback
    t.width = c.Get<int32_t>();
    t.height = c.Get<int32_t>();
    c.Skip(4 + 4);                   // m_CompleteImageSize, m_MipsStripped
    t.format = c.Get<int32_t>();
    c.Skip(kFormatToBlob);
    const auto blob = c.Get<int32_t>();
    if (!c.ok() || blob < 0 || blob > kMaxBlob || t.width <= 0 || t.height <= 0) {
        return std::nullopt;
    }
    t.platform_blob.resize(static_cast<size_t>(blob));
    for (auto& b : t.platform_blob) {
        b = c.Get<uint8_t>();
    }
    c.Align();
    const auto image = c.Get<int32_t>();
    if (!c.ok() || image < 0 || static_cast<size_t>(image) > raw.size()) {
        return std::nullopt;
    }
    t.image_data.resize(static_cast<size_t>(image));
    for (auto& b : t.image_data) {
        b = c.Get<uint8_t>();
    }
    c.Align();
    t.stream_offset = c.Get<uint64_t>();
    t.stream_size = c.Get<uint32_t>();
    t.stream_path = c.Str();
    if (!c.ok()) {
        return std::nullopt;
    }
    return t;
}

std::optional<ce_unity::Image> CropUnityRect(const ce_unity::Image& tex, float x, float y, float w,
                                              float h) {
    const auto ix = static_cast<int64_t>(std::lround(x));
    const auto iy = static_cast<int64_t>(std::lround(y));
    const auto iw = static_cast<int64_t>(std::lround(w));
    const auto ih = static_cast<int64_t>(std::lround(h));
    if (ix < 0 || iy < 0 || iw <= 0 || ih <= 0 || ix + iw > tex.width || iy + ih > tex.height ||
        tex.rgba.size() < static_cast<size_t>(tex.width) * tex.height * 4) {
        return std::nullopt;
    }
    ce_unity::Image out;
    out.width = static_cast<uint32_t>(iw);
    out.height = static_cast<uint32_t>(ih);
    out.rgba.resize(static_cast<size_t>(iw * ih * 4));
    for (int64_t row = 0; row < ih; ++row) {
        // Unity row iy + (ih - 1 - row) is at height - 1 - ... in the image (origin at the top)
        const int64_t src_row = static_cast<int64_t>(tex.height) - 1 - (iy + ih - 1 - row);
        std::memcpy(out.rgba.data() + row * iw * 4,
                    tex.rgba.data() + (src_row * tex.width + ix) * 4, static_cast<size_t>(iw * 4));
    }
    return out;
}

std::optional<LinearAstc> DeswizzleSwitchAstc(std::span<const uint8_t> data, const ce_unity::TextureInfo& tex) {
    constexpr uint32_t kBlockBytes = 16;
    uint32_t block = 0;
    if (tex.format == kAstc12x12) {
        block = 12;
    } else if (tex.format == kAstc8x8) {
        block = 8;
    }
    if (block == 0 || tex.platform_blob.size() < 12 || tex.width <= 0 || tex.height <= 0) {
        return std::nullopt;
    }
    const uint32_t shift = tex.platform_blob[8];
    if (shift >= 8) {
        return std::nullopt;
    }
    // Tegra blocks: GOBs 4 ASTC blocks wide, blocks 8 x GOBs-per-block high. Even 1 GOB per block
    // is swizzled here (ce_unity::DecodeTextureData treats it as linear and limits the size).
    const uint32_t gobs = 1u << shift;
    LinearAstc out;
    out.block = block;
    out.width = tex.width;
    out.height = tex.height;
    out.row_blocks = ((static_cast<uint32_t>(tex.width) + block * 4 - 1) / (block * 4)) * 4;
    out.col_blocks = ((static_cast<uint32_t>(tex.height) + block * 8 * gobs - 1) / (block * 8 * gobs)) * 8 * gobs;
    const uint64_t bytes = uint64_t{out.row_blocks} * out.col_blocks * kBlockBytes;
    if (bytes > data.size()) {
        return std::nullopt;
    }
    out.blocks = ce_unity::SwitchDeswizzle(data.first(static_cast<size_t>(bytes)), out.row_blocks * block,
                                           out.col_blocks * block, block, block, gobs);
    if (out.blocks.size() != bytes) {
        return std::nullopt;
    }
    return out;
}

std::optional<ce_unity::Image> DecodeLinearRect(const LinearAstc& lin, const ce_unity::AstcDecoder& astc,
                                                float x, float y, float w, float h) {
    constexpr uint32_t kBlockBytes = 16;
    const uint32_t kBlock = lin.block;
    const auto ix = static_cast<int64_t>(std::lround(x));
    const auto iy = static_cast<int64_t>(std::lround(y));
    const auto iw = static_cast<int64_t>(std::lround(w));
    const auto ih = static_cast<int64_t>(std::lround(h));
    if (!astc || kBlock == 0 || ix < 0 || iy < 0 || iw <= 0 || ih <= 0 || ix + iw > lin.width ||
        iy + ih > lin.height) {
        return std::nullopt;
    }
    // Decode only the blocks of the section (large textures would exceed memory otherwise)
    const auto bx0 = static_cast<uint32_t>(ix / kBlock);
    const auto by0 = static_cast<uint32_t>(iy / kBlock);
    const auto bx1 = static_cast<uint32_t>((ix + iw + kBlock - 1) / kBlock);
    const auto by1 = static_cast<uint32_t>((iy + ih + kBlock - 1) / kBlock);
    const uint32_t nx = bx1 - bx0, ny = by1 - by0;
    std::vector<uint8_t> blocks(size_t{nx} * ny * kBlockBytes);
    for (uint32_t r = 0; r < ny; ++r) {
        std::memcpy(blocks.data() + size_t{r} * nx * kBlockBytes,
                    lin.blocks.data() + ((size_t{by0} + r) * lin.row_blocks + bx0) * kBlockBytes,
                    size_t{nx} * kBlockBytes);
    }
    std::vector<uint8_t> rgba;
    if (!astc(blocks.data(), blocks.size(), nx * kBlock, ny * kBlock, kBlock, kBlock, rgba) ||
        rgba.size() < size_t{nx} * ny * kBlock * kBlock * 4) {
        return std::nullopt;
    }
    // The data starts at the bottom edge (Unity), the image at the top edge
    ce_unity::Image out;
    out.width = static_cast<uint32_t>(iw);
    out.height = static_cast<uint32_t>(ih);
    out.rgba.resize(static_cast<size_t>(iw * ih * 4));
    const int64_t left = ix - int64_t{bx0} * kBlock, bottom = iy - int64_t{by0} * kBlock;
    for (int64_t row = 0; row < ih; ++row) {
        const int64_t src_row = bottom + ih - 1 - row;
        std::memcpy(out.rgba.data() + row * iw * 4,
                    rgba.data() + (src_row * nx * kBlock + left) * 4, static_cast<size_t>(iw * 4));
    }
    return out;
}

std::optional<ce_unity::Image> DecodeSwitchSprite(std::span<const uint8_t> data,
                                                  const ce_unity::TextureInfo& tex,
                                                  const ce_unity::AstcDecoder& astc, float x, float y,
                                                  float w, float h) {
    if (tex.format != kAstc12x12 || !astc) {
        return std::nullopt;
    }
    const auto lin = DeswizzleSwitchAstc(data, tex);
    return lin ? DecodeLinearRect(*lin, astc, x, y, w, h) : std::nullopt;
}

ce_unity::AstcDecoder MakeTiledAstcDecoder(void* userdata, EdenDsmodAstcDecoder decode,
                                           size_t max_tile_bytes, uint32_t max_width) {
    if (decode == nullptr) {
        return {};
    }
    return [userdata, decode, max_tile_bytes, max_width](const uint8_t* blocks, size_t size,
                                                         uint32_t w, uint32_t h, uint32_t bw,
                                                         uint32_t bh, std::vector<uint8_t>& rgba) {
        if (w == 0 || h == 0 || bw == 0 || bh == 0 || bw > max_width || bh > max_width) {
            return false;
        }
        const size_t bx = (w + bw - 1) / bw; // blocks per row
        const size_t by = (h + bh - 1) / bh; // block rows
        if (size < bx * by * 16) {
            return false;
        }
        // Tile size in blocks: width and height <= max_width, output <= max_tile_bytes
        const size_t cols = std::min(bx, static_cast<size_t>(max_width / bw));
        const size_t rows = std::max<size_t>(
            1, std::min({by, static_cast<size_t>(max_width / bh),
                         max_tile_bytes / std::max<size_t>(1, cols * bw * bh * 4)}));
        rgba.assign(static_cast<size_t>(w) * h * 4, 0);
        std::vector<uint8_t> in, out;
        for (size_t r0 = 0; r0 < by; r0 += rows) {
            const size_t nr = std::min(rows, by - r0);
            const auto th = static_cast<uint32_t>(std::min<size_t>(nr * bh, h - r0 * bh));
            for (size_t c0 = 0; c0 < bx; c0 += cols) {
                const size_t nc = std::min(cols, bx - c0);
                const auto tw = static_cast<uint32_t>(std::min<size_t>(nc * bw, w - c0 * bw));
                // Collect the tile's blocks contiguously (the source is row by row across the
                // whole width)
                in.resize(nr * nc * 16);
                for (size_t r = 0; r < nr; ++r) {
                    std::memcpy(in.data() + r * nc * 16, blocks + ((r0 + r) * bx + c0) * 16, nc * 16);
                }
                out.assign(static_cast<size_t>(tw) * th * 4, 0);
                if (decode(userdata, tw, th, bw, bh, in.data(), in.size(), out.data(), out.size()) ==
                    0) {
                    return false;
                }
                for (uint32_t y = 0; y < th; ++y) {
                    std::memcpy(rgba.data() + ((r0 * bh + y) * w + c0 * bw) * 4,
                                out.data() + static_cast<size_t>(y) * tw * 4,
                                static_cast<size_t>(tw) * 4);
                }
            }
        }
        return true;
    };
}

struct IconLoader::FollowerPage {
    SpineAtlas atlas;
    LinearAstc lin;
    float scale = 1; // texture pixels per atlas pixel (Switch: 4096 instead of 8192)
    std::map<std::string, ce_unity::Image, std::less<>> regions;
    std::map<std::string, bool, std::less<>> missing;
};

IconLoader::IconLoader(ce_unity::RangeReader romfs, ce_unity::AstcDecoder astc)
    : romfs_(std::move(romfs)), astc_(astc),
      bundles_(std::make_unique<ce_unity::Reader>(romfs_, std::move(astc), std::string(kBundleRoot))) {}

IconLoader::~IconLoader() = default;

bool IconLoader::EnsureMapping() {
    if (mapping_) {
        return true;
    }
    if (!romfs_) {
        return false;
    }
    const auto size = romfs_.size(kResources);
    if (!size) {
        return false;
    }
    ce_unity::ByteSource src;
    src.size = *size;
    src.read = [r = romfs_](uint64_t off, uint8_t* out, size_t n) {
        return r.read(kResources, off, out, n);
    };
    resources_ = ce_unity::SerializedFile::Open(std::move(src));
    if (!resources_) {
        return false;
    }
    for (const auto& obj : resources_->Objects()) {
        if (obj.class_id != kClassMonoBehaviour || resources_->PeekName(obj) != kMappingName) {
            continue;
        }
        if (const auto raw = resources_->ReadRaw(obj)) {
            mapping_ = ParseIconMapping(*raw);
        }
        break;
    }
    return mapping_.has_value();
}

struct IconLoader::SpinePage {
    SpineAtlas atlas;
    ce_unity::Image page;
};

bool IconLoader::EnsureData() {
    if (equipment_ && relics_) {
        return true;
    }
    if (!EnsureMapping()) { // opens resources.assets
        return false;
    }
    equipment_.emplace();
    relics_.emplace();
    for (const auto& obj : resources_->Objects()) {
        // Large objects (I2Languages: 12 MB) are not equipment data
        if (obj.class_id != kClassMonoBehaviour || obj.byte_size > kMaxDataObjectBytes) {
            continue;
        }
        const auto raw = resources_->ReadRaw(obj);
        const auto d = raw ? ParseDataSprite(*raw) : std::nullopt;
        if (d) {
            // First entry wins (several data objects can carry the same type)
            (d->kind == DataKind::Relic ? *relics_ : *equipment_).emplace(d->type, d->sprite);
        }
    }
    return true;
}

std::optional<IconLocation> IconLoader::Locate(int32_t item_type) {
    std::lock_guard lock(mutex_);
    if (!EnsureMapping()) {
        return std::nullopt;
    }
    const auto entry = mapping_->find(item_type);
    if (entry == mapping_->end()) {
        return std::nullopt;
    }
    return LocateSprite(entry->second);
}

std::optional<IconLocation> IconLoader::LocateSprite(int64_t sprite_path_id) {
    const auto* sprite_obj = resources_->Find(sprite_path_id);
    const auto raw = sprite_obj ? resources_->ReadRaw(*sprite_obj) : std::nullopt;
    const auto sprite = raw ? ParseSpriteRef(*raw) : std::nullopt;
    return sprite ? LocateRef(*sprite) : std::nullopt;
}

std::optional<IconLocation> IconLoader::LocateRef(const SpriteRef& ref) {
    const SpriteRef* sprite = &ref;
    if (sprite->atlas_tag.empty()) {
        return std::nullopt; // sprite without an atlas: handled by LoadOwnTexture
    }
    const auto bundle = AtlasBundles().find(sprite->atlas_tag);
    if (bundle == AtlasBundles().end()) {
        return std::nullopt;
    }
    // Read the atlas once and remember all its sprites: the object is large, parsing and searching
    // it per icon would be noticeable when a page first opens
    auto index = atlases_.find(sprite->atlas_tag);
    if (index == atlases_.end()) {
        const std::string bundle_name(bundle->second);
        const auto atlas = bundles_->ReadObjectByName(bundle_name, sprite->atlas_tag, kClassSpriteAtlas);
        const auto* map = atlas ? atlas->Get("m_RenderDataMap") : nullptr;
        if (map == nullptr) {
            return std::nullopt; // not remembered: another try on the next icon
        }
        // Atlas whose texture is in another bundle (texture.m_FileID != 0)
        static const std::map<std::string_view, std::string_view> kExternalTextures = {
            {"Coop_Indicator", "a74de6afa4daf31e5ef7756c56cc52bb.bundle"},
        };
        std::map<int64_t, std::pair<std::string, std::string>> textures; // PathID to bundle, name
        for (const auto& [name, pid] : bundles_->ListObjects(bundle_name, kClassTexture2D)) {
            textures[pid] = {bundle_name, name};
        }
        if (const auto ext = kExternalTextures.find(sprite->atlas_tag); ext != kExternalTextures.end()) {
            const std::string ext_bundle(ext->second);
            for (const auto& [name, pid] : bundles_->ListObjects(ext_bundle, kClassTexture2D)) {
                textures[pid] = {ext_bundle, name};
            }
        }
        std::map<AtlasKey, IconLocation> sprites;
        // m_RenderDataMap: [[GUID, second], SpriteAtlasData]
        for (const auto& pair : map->items) {
            if (pair.items.size() != 2 || pair.items[0].items.size() != 2) {
                continue;
            }
            const auto& key = pair.items[0];
            const auto& guid = key.items[0];
            const auto& data = pair.items[1];
            const auto* tex = data.Get("texture");
            const auto* rect = data.Get("textureRect");
            if (tex == nullptr || rect == nullptr) {
                continue;
            }
            const auto texture = textures.find(tex->Get("m_PathID") ? tex->Get("m_PathID")->AsInt() : 0);
            if (texture == textures.end()) {
                continue;
            }
            IconLocation loc;
            loc.bundle = texture->second.first;
            loc.texture = texture->second.second;
            loc.x = static_cast<float>(rect->Get("x")->AsFloat());
            loc.y = static_cast<float>(rect->Get("y")->AsFloat());
            loc.w = static_cast<float>(rect->Get("width")->AsFloat());
            loc.h = static_cast<float>(rect->Get("height")->AsFloat());
            loc.settings = U32(data.Get("settingsRaw"));
            sprites[{U32(guid.Get("data[0]")), U32(guid.Get("data[1]")), U32(guid.Get("data[2]")),
                     U32(guid.Get("data[3]")), key.items[1].AsInt()}] = std::move(loc);
        }
        index = atlases_.emplace(sprite->atlas_tag, std::move(sprites)).first;
    }
    const auto found = index->second.find(
        {sprite->guid[0], sprite->guid[1], sprite->guid[2], sprite->guid[3], sprite->second});
    if (found == index->second.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<ce_unity::Image> IconLoader::LoadItem(int32_t item_type) {
    auto loc = Locate(item_type);
    if (!loc) {
        // No atlas given: sprite with its own texture (e.g. pumpkin seeds)
        auto own = LoadOwnTexture(item_type);
        if (own) {
            return own;
        }
    }
    return LoadLocated(loc, "item " + std::to_string(item_type));
}

std::optional<ce_unity::Image> IconLoader::LoadOwnTexture(int32_t item_type) {
    std::lock_guard lock(mutex_);
    if (!EnsureMapping()) {
        return std::nullopt;
    }
    const auto entry = mapping_->find(item_type);
    const auto* sprite_obj = entry != mapping_->end() ? resources_->Find(entry->second) : nullptr;
    const auto sprite_raw = sprite_obj ? resources_->ReadRaw(*sprite_obj) : std::nullopt;
    const auto sprite = sprite_raw ? ParseSpriteRef(*sprite_raw) : std::nullopt;
    const auto* tex_obj = sprite && sprite->texture != 0 ? resources_->Find(sprite->texture) : nullptr;
    const auto tex_raw = tex_obj && tex_obj->class_id == kClassTexture2D ? resources_->ReadRaw(*tex_obj)
                                                                         : std::nullopt;
    const auto tex = tex_raw ? ParseRawTexture(*tex_raw) : std::nullopt;
    if (!tex || tex->stream_size == 0 || tex->stream_size > kMaxOwnTextureBytes) {
        return std::nullopt;
    }
    std::vector<uint8_t> data(tex->stream_size);
    if (!romfs_.read("Data/" + tex->stream_path, tex->stream_offset, data.data(), data.size())) {
        return std::nullopt;
    }
    return DecodeSwitchSprite(data, *tex, astc_, sprite->x, sprite->y, sprite->w, sprite->h);
}

std::optional<ce_unity::Image> IconLoader::LoadEquipment(int32_t type) {
    std::optional<IconLocation> loc;
    {
        std::lock_guard lock(mutex_);
        if (EnsureData()) {
            const auto it = equipment_->find(type);
            if (it != equipment_->end()) {
                loc = LocateSprite(it->second);
            }
        }
    }
    return LoadLocated(loc, "equipment " + std::to_string(type));
}

std::optional<ce_unity::Image> IconLoader::LoadRelic(int32_t type) {
    std::optional<IconLocation> loc;
    {
        std::lock_guard lock(mutex_);
        if (EnsureData()) {
            const auto it = relics_->find(type);
            if (it != relics_->end()) {
                loc = LocateSprite(it->second);
            }
        }
    }
    return LoadLocated(loc, "relic " + std::to_string(type));
}

const IconLoader::SpinePage* IconLoader::EnsureSpine(const std::string& atlas_asset) {
    if (const auto it = spine_.find(atlas_asset); it != spine_.end()) {
        return it->second.get();
    }
    const std::string bundle(kSpineBundle);
    const auto asset = bundles_->ReadObjectByName(bundle, atlas_asset, kClassMonoBehaviour);
    const auto* text_id = asset ? asset->Path("atlasFile.m_PathID") : nullptr;
    const auto* mats = asset ? asset->Get("materials") : nullptr;
    if (text_id == nullptr || mats == nullptr || mats->items.empty()) {
        last_error_ = "spine atlas asset missing: " + atlas_asset;
        return nullptr;
    }
    const auto text = bundles_->ReadObject(bundle, text_id->AsInt());
    const auto* mat_id = mats->items[0].Get("m_PathID");
    const auto mat = mat_id ? bundles_->ReadObject(bundle, mat_id->AsInt()) : std::nullopt;
    const auto* envs = mat ? mat->Path("m_SavedProperties.m_TexEnvs") : nullptr;
    if (!text || !text->Get("m_Script") || envs == nullptr) {
        last_error_ = "spine atlas text/material missing: " + atlas_asset;
        return nullptr;
    }
    int64_t tex_id = 0;
    for (const auto& env : envs->items) { // [name, UnityTexEnv]
        if (env.items.size() == 2 && env.items[0].AsString() == "_MainTex") {
            if (const auto* t = env.items[1].Path("m_Texture.m_PathID")) {
                tex_id = t->AsInt();
            }
        }
    }
    std::string tex_name;
    for (const auto& [name, pid] : bundles_->ListObjects(bundle, kClassTexture2D)) {
        if (pid == tex_id) {
            tex_name = name;
        }
    }
    auto img = tex_name.empty() ? std::nullopt : bundles_->LoadTexture(bundle, tex_name);
    if (!img) {
        last_error_ = "spine page decode failed: " + atlas_asset; // not remembered: try again
        return nullptr;
    }
    auto page = std::make_unique<SpinePage>();
    page->atlas = ParseSpineAtlas(text->Get("m_Script")->AsString());
    page->page = std::move(*img);
    return spine_.emplace(atlas_asset, std::move(page)).first->second.get();
}

std::optional<ce_unity::Image> IconLoader::LoadMapIcon(const std::string& sprite) {
    std::optional<IconLocation> loc;
    {
        std::lock_guard lock(mutex_);
        // Sprite with TypeTree in the MapIcons bundle: RenderDataKey and atlas tag as in the raw layout
        const auto v = bundles_->ReadObjectByName(std::string(kMapIconsBundle), sprite, kClassSprite);
        const auto* key = v ? v->Get("m_RenderDataKey") : nullptr;
        const auto* tags = v ? v->Get("m_AtlasTags") : nullptr;
        if (key != nullptr && key->items.size() == 2 && tags != nullptr && !tags->items.empty()) {
            SpriteRef ref;
            for (size_t k = 0; k < 4; ++k) {
                ref.guid[k] = U32(key->items[0].Get("data[" + std::to_string(k) + "]"));
            }
            ref.second = key->items[1].AsInt();
            ref.atlas_tag = std::string(tags->items[0].AsString());
            loc = LocateRef(ref);
        } else if (EnsureMapping()) {
            // otherwise in resources.assets (e.g. "Coop_Indicator_Lamb")
            for (const auto& obj : resources_->Objects()) {
                if (obj.class_id == kClassSprite && resources_->PeekName(obj) == sprite) {
                    loc = LocateSprite(obj.path_id);
                    break;
                }
            }
        }
    }
    return LoadLocated(loc, "map icon " + sprite);
}


IconLoader::FollowerPage* IconLoader::EnsureFollowerPage() {
    if (follower_) {
        return follower_.get();
    }
    const std::string bundle(kSpineBundle);
    const auto asset = bundles_->ReadObjectByName(bundle, "Follower_Atlas", kClassMonoBehaviour);
    const auto* text_id = asset ? asset->Path("atlasFile.m_PathID") : nullptr;
    const auto* mats = asset ? asset->Get("materials") : nullptr;
    const auto text = text_id ? bundles_->ReadObject(bundle, text_id->AsInt()) : std::nullopt;
    const auto* mat_id = mats && !mats->items.empty() ? mats->items[0].Get("m_PathID") : nullptr;
    const auto mat = mat_id ? bundles_->ReadObject(bundle, mat_id->AsInt()) : std::nullopt;
    const auto* envs = mat ? mat->Path("m_SavedProperties.m_TexEnvs") : nullptr;
    if (!text || !text->Get("m_Script") || envs == nullptr) {
        last_error_ = "follower atlas missing";
        return nullptr;
    }
    int64_t tex_id = 0;
    for (const auto& env : envs->items) { // [Name, UnityTexEnv]
        if (env.items.size() == 2 && env.items[0].AsString() == "_MainTex") {
            if (const auto* t = env.items[1].Path("m_Texture.m_PathID")) {
                tex_id = t->AsInt();
            }
        }
    }
    const auto tv = tex_id != 0 ? bundles_->ReadObject(bundle, tex_id) : std::nullopt;
    if (!tv) {
        last_error_ = "follower texture missing";
        return nullptr;
    }
    ce_unity::TextureInfo tex;
    const auto geti = [&](std::string_view k) {
        const auto* x = tv->Get(k);
        return x ? static_cast<int32_t>(x->AsInt()) : 0;
    };
    tex.width = geti("m_Width");
    tex.height = geti("m_Height");
    tex.format = geti("m_TextureFormat");
    if (const auto* blob = tv->Get("m_PlatformBlob")) {
        if (blob->kind == ce_unity::Value::Kind::Bytes) {
            tex.platform_blob.assign(blob->s.begin(), blob->s.end());
        } else {
            for (const auto& b : blob->items) {
                tex.platform_blob.push_back(static_cast<uint8_t>(b.AsInt()));
            }
        }
    }
    const auto* sd = tv->Get("m_StreamData");
    const auto* s_off = sd ? sd->Get("offset") : nullptr;
    const auto* s_size = sd ? sd->Get("size") : nullptr;
    const auto* s_path = sd ? sd->Get("path") : nullptr;
    const auto b = bundles_->OpenBundle(bundle);
    const auto* node = b && s_path ? b->FindNode(s_path->AsString()) : nullptr;
    const uint64_t off = s_off ? static_cast<uint64_t>(s_off->AsInt()) : 0;
    const uint64_t size = s_size ? static_cast<uint64_t>(s_size->AsInt()) : 0;
    if (node == nullptr || size == 0 || size > kMaxFollowerTextureBytes || off > node->size ||
        size > node->size - off) {
        last_error_ = "follower texture data missing";
        return nullptr;
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!b->Read(node->offset + off, data.data(), data.size())) {
        last_error_ = "follower texture read failed";
        return nullptr;
    }
    auto lin = DeswizzleSwitchAstc(data, tex);
    auto page = std::make_unique<FollowerPage>();
    page->atlas = ParseSpineAtlas(text->Get("m_Script")->AsString());
    if (!lin || page->atlas.page_w <= 0) {
        last_error_ = "follower texture format " + std::to_string(tex.format);
        return nullptr;
    }
    page->scale = static_cast<float>(lin->width) / static_cast<float>(page->atlas.page_w);
    page->lin = std::move(*lin);
    follower_ = std::move(page);
    return follower_.get();
}

const ce_unity::Image* IconLoader::FollowerRegion(FollowerPage& page, std::string_view region) {
    if (const auto it = page.regions.find(region); it != page.regions.end()) {
        return &it->second;
    }
    if (page.missing.find(region) != page.missing.end()) {
        return nullptr;
    }
    const auto r = page.atlas.regions.find(std::string(region));
    std::optional<ce_unity::Image> framed;
    if (r != page.atlas.regions.end()) {
        // Round the occupied area like CropRegion, decode it as its own image, then rotate back
        const float s = page.scale;
        const auto sc = [s](int v) { return static_cast<int>(static_cast<float>(v) * s + 0.5f); };
        const SpineRegion& sr = r->second;
        // Values from the atlas file: only within the page (protects frame size and sums)
        const int pw = page.atlas.page_w, ph = page.atlas.page_h;
        const bool sane = sr.x >= 0 && sr.y >= 0 && sr.w > 0 && sr.h > 0 && sr.w <= pw && sr.h <= ph &&
                          sr.x <= pw && sr.y <= ph && sr.orig_w >= 0 && sr.orig_h >= 0 &&
                          sr.orig_w <= pw && sr.orig_h <= ph;
        const int rx = sane ? sc(sr.x) : 0, ry = sane ? sc(sr.y) : 0, rw = sane ? sc(sr.w) : 0,
                  rh = sane ? sc(sr.h) : 0;
        const int aw = sr.rotate ? rh : rw, ah = sr.rotate ? rw : rh;
        const bool inside = sane && rx + aw <= static_cast<int>(page.lin.width) &&
                            ry + ah <= static_cast<int>(page.lin.height);
        const auto patch = !inside ? std::nullopt : DecodeLinearRect(page.lin, astc_, static_cast<float>(rx),
                                            static_cast<float>(page.lin.height - (ry + ah)),
                                            static_cast<float>(aw), static_cast<float>(ah));
        SpineRegion local = sr;
        local.x = local.y = 0;
        const auto crop = patch ? CropRegion(*patch, local, s) : std::nullopt;
        if (crop) {
            // Into the original frame (Spine: offset from the bottom left)
            ce_unity::Image f;
            f.width = static_cast<uint32_t>(std::max(sc(sr.orig_w), rw));
            f.height = static_cast<uint32_t>(std::max(sc(sr.orig_h), rh));
            f.rgba.assign(size_t{f.width} * f.height * 4, 0);
            BlitOver(f, *crop, sc(sr.off_x), sc(sr.orig_h - sr.off_y - sr.h));
            framed = std::move(f);
        }
    }
    if (!framed) {
        page.missing.emplace(std::string(region), true);
        return nullptr;
    }
    return &page.regions.emplace(std::string(region), std::move(*framed)).first->second;
}

std::optional<ce_unity::Image> IconLoader::LoadFollower(const std::string& arg) {
    const auto spec = ParsePortraitArg(arg);
    const auto parts = spec ? PortraitParts(spec->skin) : std::vector<const PortraitPart*>{};
    std::lock_guard lock(mutex_);
    if (parts.empty()) {
        last_error_ = "no portrait parts for skin";
        return std::nullopt;
    }
    if (!follower_error_.empty()) {
        last_error_ = follower_error_;
        return std::nullopt;
    }
    FollowerPage* page = EnsureFollowerPage();
    if (page == nullptr) {
        follower_error_ = last_error_.empty() ? "follower page unavailable" : last_error_;
        return std::nullopt;
    }
    auto img = ComposePortrait(
        parts, [&](std::string_view region) { return FollowerRegion(*page, region); }, page->scale,
        spec->tints);
    if (!img) {
        last_error_ = "follower portrait regions missing";
    }
    return img;
}

std::optional<ce_unity::Image> IconLoader::LoadHeart(const std::string& kind, const std::string& state) {
    std::lock_guard lock(mutex_);
    const SpinePage* p = EnsureSpine("heart_Atlas");
    auto img = p ? ComposeHeart(p->atlas, p->page, kind, state) : std::nullopt;
    if (!img) {
        last_error_ = "heart " + kind + "_" + state + ": " + last_error_;
    }
    return img;
}

std::optional<ce_unity::Image> IconLoader::LoadTarot(int32_t card) {
    std::lock_guard lock(mutex_);
    const auto it = TarotRegions().find(card);
    if (it == TarotRegions().end()) {
        last_error_ = "no tarot region for card " + std::to_string(card);
        return std::nullopt;
    }
    const SpinePage* p = EnsureSpine("WeaponCard_Atlas");
    if (p == nullptr) {
        return std::nullopt;
    }
    const auto r = p->atlas.regions.find(std::string(it->second));
    if (r == p->atlas.regions.end()) {
        last_error_ = "tarot region missing for card " + std::to_string(card);
        return std::nullopt;
    }
    const float scale = p->atlas.page_w > 0 ? static_cast<float>(p->page.width) / p->atlas.page_w : 1.0f;
    return CropRegion(p->page, r->second, scale);
}

std::optional<ce_unity::Image> IconLoader::LoadLocated(const std::optional<IconLocation>& loc,
                                                       const std::string& what) {
    if (!loc) {
        std::lock_guard lock(mutex_);
        last_error_ = "no atlas entry for " + what;
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    const std::string key = loc->bundle + "/" + loc->texture;
    // Keep a few decoded atlases: pages mix icons from several, decoding is expensive
    auto cached = std::find_if(textures_.begin(), textures_.end(),
                               [&](const auto& entry) { return entry.first == key; });
    if (cached == textures_.end()) {
        // A failure is not remembered: the next call tries again
        auto image = bundles_->LoadTexture(loc->bundle, loc->texture);
        if (!image) {
            last_error_ = "texture decode failed: " + key;
            return std::nullopt;
        }
        if (textures_.size() >= kCachedTextures) {
            textures_.pop_back();
        }
        textures_.emplace_front(key, std::move(*image));
        cached = textures_.begin();
    } else if (cached != textures_.begin()) {
        textures_.splice(textures_.begin(), textures_, cached);
        cached = textures_.begin();
    }
    // Rotated packing (settingsRaw bits 2-5) does not occur for inventory icons; if it did,
    // the section would be returned unrotated.
    return CropUnityRect(cached->second, loc->x, loc->y, loc->w, loc->h);
}

} // namespace cotl
