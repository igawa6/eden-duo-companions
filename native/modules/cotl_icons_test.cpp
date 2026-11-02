// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <cstring>

#include "cotl_icons.h"
#include "cotl_test_util.h"

namespace {

struct Bytes {
    std::vector<uint8_t> b;
    template <class T>
    void put(T v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(T));
    }
    void str(const std::string& s) {
        put<int32_t>(static_cast<int32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
        align();
    }
    void align() {
        while (b.size() % 4) b.push_back(0);
    }
};

// MonoBehaviour-Kopf: m_GameObject PPtr, m_Enabled (ausgerichtet), m_Script PPtr = 28 Byte
Bytes MonoHeader(const std::string& name) {
    Bytes x;
    x.b.assign(28, 0);
    x.str(name);
    return x;
}

} // namespace

TEST(icons_parse_mapping) {
    auto x = MonoHeader("Inventory Icon Mapping");
    x.put<int32_t>(2);
    x.put<int32_t>(1);     // ITEM_TYPE
    x.put<int32_t>(0);     // m_FileID
    x.put<int64_t>(2407);  // m_PathID
    x.put<int32_t>(9);
    x.put<int32_t>(1);     // external file: skipped
    x.put<int64_t>(55);
    const auto m = cotl::ParseIconMapping(x.b);
    REQUIRE(m.has_value());
    CHECK_EQ(m->size(), size_t{1});
    CHECK_EQ(m->at(1), int64_t{2407});
}

TEST(icons_parse_mapping_rejects_truncated) {
    auto x = MonoHeader("Inventory Icon Mapping");
    x.put<int32_t>(5); // 5 entries announced, none present
    CHECK(!cotl::ParseIconMapping(x.b).has_value());
}

TEST(icons_parse_sprite_ref) {
    Bytes x;
    x.str("Wood");
    for (int i = 0; i < 4 + 2 + 4 + 1 + 2; ++i) x.put<float>(1.0f); // rect, offset, border, ppu, pivot
    x.put<uint32_t>(1);                                            // m_Extrude
    x.put<uint8_t>(0);                                             // m_IsPolygon
    x.align();
    for (uint32_t g : {0x03f08af7u, 0x81409650u, 0xe550d0b2u, 0xb3d19f0bu}) x.put<uint32_t>(g);
    x.put<int64_t>(21300000);
    x.put<int32_t>(1);
    x.str("Resources_Atlas");
    const auto s = cotl::ParseSpriteRef(x.b);
    REQUIRE(s.has_value());
    CHECK_EQ(s->guid[0], 0x03f08af7u);
    CHECK_EQ(s->guid[3], 0xb3d19f0bu);
    CHECK_EQ(s->second, int64_t{21300000});
    CHECK_EQ(s->atlas_tag, std::string("Resources_Atlas"));
}

TEST(icons_parses_sprite_with_own_texture) {
    Bytes x;
    x.str("Seed Pumpkin");
    x.put<float>(4.0f);
    x.put<float>(8.0f);
    x.put<float>(91.0f);
    x.put<float>(104.0f);                                     // m_Rect
    for (int i = 0; i < 2 + 4 + 1 + 2; ++i) x.put<float>(1.0f); // offset, border, ppu, pivot
    x.put<uint32_t>(1);                                       // m_Extrude
    x.put<uint8_t>(0);                                        // m_IsPolygon
    x.align();
    for (uint32_t g : {1u, 2u, 3u, 4u}) x.put<uint32_t>(g);
    x.put<int64_t>(21300000);
    x.put<int32_t>(0);  // m_AtlasTags
    x.put<int32_t>(0);  // m_SpriteAtlas
    x.put<int64_t>(0);
    x.put<int32_t>(0);  // m_RD.texture
    x.put<int64_t>(777);
    const auto s = cotl::ParseSpriteRef(x.b);
    REQUIRE(s.has_value());
    CHECK(s->atlas_tag.empty());
    CHECK_EQ(s->texture, int64_t{777});
    CHECK_EQ(s->x, 4.0f);
    CHECK_EQ(s->y, 8.0f);
    CHECK_EQ(s->w, 91.0f);
    CHECK_EQ(s->h, 104.0f);
}

TEST(icons_parses_raw_texture_with_stream) {
    Bytes x;
    x.str("Seed Pumpkin");
    x.put<int32_t>(4);    // m_ForcedFallbackFormat
    x.put<int32_t>(0);    // m_DownscaleFallback
    x.put<int32_t>(91);   // m_Width
    x.put<int32_t>(104);  // m_Height
    x.put<int32_t>(2048); // m_CompleteImageSize
    x.put<int32_t>(0);    // m_MipsStripped
    x.put<int32_t>(53);   // m_TextureFormat
    for (int i = 0; i < 15; ++i) x.put<int32_t>(0); // up to m_PlatformBlob
    x.put<int32_t>(4);
    x.put<uint32_t>(0xAABBCCDD); // Blob
    x.put<int32_t>(0);           // image data
    x.put<uint64_t>(0x13AC8C0);  // m_StreamData
    x.put<uint32_t>(2048);
    x.str("resources.assets.resS");
    const auto t = cotl::ParseRawTexture(x.b);
    REQUIRE(t.has_value());
    CHECK_EQ(t->width, 91);
    CHECK_EQ(t->height, 104);
    CHECK_EQ(t->format, 53);
    CHECK_EQ(t->platform_blob.size(), size_t{4});
    CHECK_EQ(t->stream_offset, uint64_t{0x13AC8C0});
    CHECK_EQ(t->stream_size, 2048u);
    CHECK_EQ(t->stream_path, std::string("resources.assets.resS"));
}

TEST(icons_rejects_truncated_raw_texture) {
    Bytes x;
    x.str("Short");
    x.put<int32_t>(0);
    CHECK(!cotl::ParseRawTexture(x.b).has_value());
}

namespace {
// Each block becomes one colour from its first byte; data: 1 GOB per block, 2 x 2 blocks in the image
ce_unity::AstcDecoder FakeAstc() {
    return [](const uint8_t* blocks, size_t, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh,
              std::vector<uint8_t>& rgba) {
        rgba.assign(size_t{w} * h * 4, 0);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                rgba[(size_t{y} * w + x) * 4] = blocks[(size_t{y / bh} * (w / bw) + x / bw) * 16];
        return true;
    };
}
ce_unity::TextureInfo SwizzledTexture(int32_t w, int32_t h) {
    ce_unity::TextureInfo t;
    t.width = w;
    t.height = h;
    t.format = 53;
    t.platform_blob.assign(12, 0);
    return t;
}
} // namespace

TEST(icons_decodes_swizzled_sprite_at_one_gob) {
    std::vector<uint8_t> data(512, 0); // 4 x 8 blocks = one GOB
    data[0] = 7;                       // block (0, 0), Unity bottom left
    const auto tex = SwizzledTexture(24, 24);
    const auto bottom_left = cotl::DecodeSwitchSprite(data, tex, FakeAstc(), 0, 0, 12, 12);
    REQUIRE(bottom_left.has_value());
    CHECK_EQ(bottom_left->width, 12u);
    CHECK_EQ(bottom_left->height, 12u);
    CHECK_EQ(int{bottom_left->rgba[0]}, 7);
    const auto above = cotl::DecodeSwitchSprite(data, tex, FakeAstc(), 0, 12, 12, 12);
    REQUIRE(above.has_value());
    CHECK_EQ(int{above->rgba[0]}, 0);
    // Top image row = top Unity row: in the whole image, block (0, 0) is at the bottom
    const auto whole = cotl::DecodeSwitchSprite(data, tex, FakeAstc(), 0, 0, 24, 24);
    REQUIRE(whole.has_value());
    CHECK_EQ(int{whole->rgba[0]}, 0);
    CHECK_EQ(int{whole->rgba[23 * 24 * 4]}, 7);
}

TEST(icons_rejects_bad_swizzled_sprites) {
    const std::vector<uint8_t> small(100, 0);
    const auto tex = SwizzledTexture(24, 24);
    CHECK(!cotl::DecodeSwitchSprite(small, tex, FakeAstc(), 0, 0, 12, 12).has_value()); // too little data
    const std::vector<uint8_t> data(512, 0);
    CHECK(!cotl::DecodeSwitchSprite(data, tex, FakeAstc(), 20, 0, 12, 12).has_value()); // outside
    auto other = tex;
    other.format = 4;
    CHECK(!cotl::DecodeSwitchSprite(data, other, FakeAstc(), 0, 0, 12, 12).has_value());
    CHECK(!cotl::DecodeSwitchSprite(data, tex, {}, 0, 0, 12, 12).has_value());
}

TEST(icons_crop_flips_unity_y) {
    ce_unity::Image tex;
    tex.width = 4;
    tex.height = 4;
    tex.rgba.assign(4 * 4 * 4, 0);
    // Pixel (1, 0) in Unity coordinates (bottom) = row 3 in the image
    tex.rgba[(3 * 4 + 1) * 4] = 200;
    const auto c = cotl::CropUnityRect(tex, 1, 0, 2, 2);
    REQUIRE(c.has_value());
    CHECK_EQ(c->width, 2u);
    CHECK_EQ(c->height, 2u);
    CHECK_EQ(int{c->rgba[(1 * 2 + 0) * 4]}, 200); // bottom row of the crop
}

TEST(icons_crop_rejects_out_of_bounds) {
    ce_unity::Image tex;
    tex.width = 4;
    tex.height = 4;
    tex.rgba.assign(64, 0);
    CHECK(!cotl::CropUnityRect(tex, 3, 3, 4, 4).has_value());
}

// Large atlases (padded to 2052x2052) exceed two limits: ce_unity::MakeHostAstcDecoder
// (> 16 MiB output) and Eden's decode_astc (width/height > 2048). Our own adapter decodes in
// tiles of whole blocks.
namespace {
int g_tile_calls = 0;
uint32_t g_max_w = 0;
EdenDsmodBool FakeAstc(void*, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, const uint8_t* in,
                       size_t in_size, uint8_t* out, size_t out_size) {
    ++g_tile_calls;
    g_max_w = std::max(g_max_w, w);
    const size_t bx = (w + bw - 1) / bw;
    const size_t blocks = bx * ((h + bh - 1) / bh);
    if (in_size != blocks * 16 || out_size != static_cast<size_t>(w) * h * 4) return EDEN_DSMOD_FALSE;
    // Each pixel gets the first byte of its block as its value
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            out[(static_cast<size_t>(y) * w + x) * 4] = in[((y / bh) * bx + x / bw) * 16];
    return EDEN_DSMOD_TRUE;
}
} // namespace

TEST(icons_tiled_astc_decoder_assembles_tiles) {
    const uint32_t w = 24, h = 48, bw = 12, bh = 12; // 2x4 blocks
    std::vector<uint8_t> blocks(8 * 16, 0);
    for (int b = 0; b < 8; ++b) blocks[static_cast<size_t>(b) * 16] = static_cast<uint8_t>(10 + b);
    g_tile_calls = 0;
    g_max_w = 0;
    // Tiles at most 12 px wide (1 block) and 12 px high (1 block row)
    const auto dec = cotl::MakeTiledAstcDecoder(nullptr, FakeAstc, /*max_tile_bytes=*/12 * 12 * 4,
                                                /*max_width=*/12);
    REQUIRE(static_cast<bool>(dec));
    std::vector<uint8_t> rgba;
    REQUIRE(dec(blocks.data(), blocks.size(), w, h, bw, bh, rgba));
    CHECK_EQ(rgba.size(), size_t{24 * 48 * 4});
    CHECK_EQ(g_tile_calls, 8);
    CHECK_EQ(g_max_w, 12u);
    CHECK_EQ(int{rgba[(0 * w + 0) * 4]}, 10);   // Block (0,0)
    CHECK_EQ(int{rgba[(0 * w + 13) * 4]}, 11);  // Block (1,0)
    CHECK_EQ(int{rgba[(25 * w + 5) * 4]}, 14);  // Block (0,2)
    CHECK_EQ(int{rgba[(47 * w + 23) * 4]}, 17); // Block (1,3)
}

// Real game files: location of the log icon (ITEM_TYPE 1 = LOG) in Resources_Atlas
TEST(icons_locate_real_wood_icon) {
    const char* romfs = std::getenv("COTL_ROMFS");
    if (romfs == nullptr) {
        return;
    }
    cotl::IconLoader loader(ce_unity::MakeDirectoryRangeReader(romfs), {});
    const auto loc = loader.Locate(1);
    REQUIRE(loc.has_value());
    CHECK_EQ(loc->bundle, std::string("f70416a55415bfa877b16a7e48f9c324.bundle"));
    CHECK(loc->texture.find("Resources_Atlas") != std::string::npos);
    CHECK(loc->w > 10 && loc->h > 10);
}

namespace {

// MonoBehaviour header with m_Script PathID (in globalgamemanagers.assets)
Bytes ScriptHeader(int64_t script, const std::string& name) {
    Bytes x;
    x.b.assign(16, 0);
    x.put<int32_t>(1);
    x.put<int64_t>(script);
    x.str(name);
    return x;
}

} // namespace

TEST(icons_parse_weapon_data_sprite) {
    auto x = ScriptHeader(cotl::kScriptWeaponData, "Sword_Default");
    x.put<int32_t>(0);    // EquipmentType
    x.put<int32_t>(0);    // PrimaryEquipmentType
    x.put<uint8_t>(1);    // CanBreakDodge
    x.align();
    x.put<int32_t>(0);    // UISprite.m_FileID
    x.put<int64_t>(2055); // UISprite.m_PathID
    const auto r = cotl::ParseDataSprite(x.b);
    REQUIRE(r.has_value());
    CHECK(r->kind == cotl::DataKind::Equipment);
    CHECK_EQ(r->type, 0);
    CHECK_EQ(r->sprite, int64_t{2055});
}

TEST(icons_parse_relic_data_sprite) {
    auto x = ScriptHeader(cotl::kScriptRelicData, "Enlarge_Relic");
    x.put<int32_t>(29); // RelicType
    x.put<int32_t>(0);  // InteractionType
    x.put<int32_t>(0);  // RelicSubType
    x.put<int32_t>(0);  // DamageType
    x.put<float>(1.0f); // DamageRequiredToCharge
    x.put<int32_t>(0);
    x.put<int64_t>(2889);
    const auto r = cotl::ParseDataSprite(x.b);
    REQUIRE(r.has_value());
    CHECK(r->kind == cotl::DataKind::Relic);
    CHECK_EQ(r->type, 29);
    CHECK_EQ(r->sprite, int64_t{2889});
}

TEST(icons_parse_data_sprite_ignores_other_scripts) {
    auto x = ScriptHeader(12345, "Something");
    x.put<int32_t>(1);
    CHECK(!cotl::ParseDataSprite(x.b).has_value());
}
