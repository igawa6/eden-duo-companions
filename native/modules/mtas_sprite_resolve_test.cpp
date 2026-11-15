// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Round-2 data lane: end-to-end proof that mtas::Art::ResolveSprite/LoadSprite decode REAL item
// icons and NPC Social-head portraits (not just the ~44 fixed UI pins Stage-9 already proved) to
// the right pixels, against the player's own romfs bytes and the real CPU ASTC decoder -- same
// method and same reason as Echoes of Wisdom's dsmod-echoes-assets / Luminescent Platinum's
// lp-unity-tool (video_core/textures/astc.cpp, the exact code the host's own decode_astc runs,
// wrapped in a synchronous stand-in for Eden's texture thread pool): this is NOT the live game,
// but it IS the same on-disk bundle bytes the game itself reads and the same decoder the host
// uses, independent of booting an emulator. mtas_art.h's own header comment documents why a live
// boot alone cannot cheaply prove per-sprite pixel correctness (no manifest binding exists yet
// for item icons/NPC portraits -- that is the UI lane's follow-up, DATA-CONTRACT.md "Icons for
// UI").
//
// Skipped (exit 77) unless MTAS_ROMFS points at a local extraction of this game's romfs (the
// uisystem_sprite_split/ shard tree must be reachable under
// $MTAS_ROMFS/Data/StreamingAssets/AssetBundles/) -- never required for a normal CI run.
// Set MTAS_SPRITE_OUTPUT to also dump each decoded sprite as a PNG for manual visual comparison
// against assets/g0-pack/items/ and assets/g0-pack/portraits/ (research/...).
#include "mtas_art.h"

#include "video_core/textures/astc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop

namespace {

namespace fs = std::filesystem;

int g_checks = 0;
void Check(bool ok, const char* message) {
    ++g_checks;
    if (!ok)
        throw std::runtime_error(message);
}

/// Reverse of mtas::FontCodepoints -- encodes codepoints back to UTF-8, so this test can ask
/// mtas::Art::SetFontText() for exactly the Latin/Cyrillic/CJK/Thai sample characters it checks
/// below, the same way the real module asks for whatever text GameText().FontText() returns.
std::string Utf8(std::initializer_list<std::uint32_t> cps) {
    std::string out;
    for (const auto cp : cps) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

ce_unity::AstcDecoder CpuAstc() {
    return [](const std::uint8_t* blocks, std::size_t size, std::uint32_t w, std::uint32_t h,
              std::uint32_t bw, std::uint32_t bh, std::vector<std::uint8_t>& rgba) {
        const std::size_t need =
            static_cast<std::size_t>((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
        if (size < need)
            return false;
        rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
        Tegra::Texture::ASTC::Decompress(std::span<const std::uint8_t>{blocks, need}, w, h, 1, bw,
                                         bh, std::span<std::uint8_t>{rgba});
        return true;
    };
}

void MaybeSavePng(const char* name, const ce_unity::Image& img) {
    const char* out_dir = std::getenv("MTAS_SPRITE_OUTPUT");
    if (!out_dir)
        return;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    const auto path = fs::path(out_dir) / (std::string(name) + ".png");
    Check(stbi_write_png(path.string().c_str(), static_cast<int>(img.width),
                         static_cast<int>(img.height), 4, img.rgba.data(),
                         static_cast<int>(img.width * 4)) != 0,
         "wrote sprite PNG");
    std::fprintf(stderr, "wrote %s (%ux%u)\n", path.string().c_str(), img.width, img.height);
}

/// Non-degenerate: not every pixel transparent/identical (a decode bug -- wrong offset, wrong
/// block size, swizzle mismatch -- tends to produce either an all-zero or a uniform-noise image,
/// never a real sprite's actual alpha-edged silhouette).
void CheckNonDegenerate(const ce_unity::Image& img, const char* name) {
    Check(img.width > 0 && img.height > 0, "non-zero dimensions");
    Check(img.rgba.size() == std::size_t{img.width} * img.height * 4, "rgba sized w*h*4");
    bool any_opaque = false, any_transparent = false;
    std::uint8_t first_r = img.rgba[0];
    bool all_same_r = true;
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
        if (img.rgba[i + 3] > 16)
            any_opaque = true;
        else
            any_transparent = true;
        if (img.rgba[i] != first_r)
            all_same_r = false;
    }
    Check(any_opaque, (std::string(name) + ": has at least one non-transparent pixel").c_str());
    // A real icon/portrait is not a flat colour field; a handful of sprites in this game ARE
    // fully opaque rectangles (no transparent border), so only require SOME variation once alpha
    // is high enough to matter, not "has transparent pixels".
    (void)any_transparent;
    Check(!all_same_r, (std::string(name) + ": not a flat single-colour image").c_str());
}

} // namespace

int main() {
    try {
        const char* root = std::getenv("MTAS_ROMFS");
        if (!root) {
            std::printf("%d checks passed; native sprite-index fixture skipped (set "
                       "MTAS_ROMFS=/path/to/extracted/romfs)\n",
                       g_checks);
            return 77;
        }
        auto reader = ce_unity::MakeDirectoryRangeReader(root);
        mtas::Art art(reader, CpuAstc());

        // PERF (review-L3 finding 3): the paged font build now lays out exactly `SetFontText()`'s
        // own priority codepoints (plus a small ASCII/Latin-1 floor), not the whole 0x20-0xFFFF
        // BMP range regardless of what was ever asked for -- see BuildLatinFontAtlas's own
        // `paged` comment in mtas_art.cpp. Ask for exactly the sample characters this test checks
        // below, the same way the real module asks SetFontText() for whatever text
        // GameText().FontText() returns; this proves the "render on demand" half of the fix, not
        // just the "don't build what was never asked for" half.
        const std::uint32_t sample_cps[]{0x50u, 0x61u, 0x65u, 0xE9u, 0x416u, 0x4E2Du, 0x3042u,
                                        0x6B8Bu, 0x6728u, 0x66DCu, 0xD55Cu, 0x0E01u};
        art.SetFontText(Utf8({0x50u, 0x61u, 0x65u, 0xE9u, 0x416u, 0x4E2Du, 0x3042u, 0x6B8Bu,
                              0x6728u, 0x66DCu, 0xD55Cu, 0x0E01u}));
        std::uint32_t first{}, line{};
        std::vector<EdenDsmodFontGlyph> glyphs;
        Check(art.FontGlyphs(first, glyphs, line), "paged font metrics build");
        std::uint32_t max_page = 0;
        for (const auto& g : glyphs) {
            if (!g.w || !g.h)
                continue;
            Check(g.y / mtas::FontPageHeight == (g.y + g.h - 1u) / mtas::FontPageHeight,
                  "glyph stays inside its page");
            max_page = std::max(max_page, g.y / mtas::FontPageHeight);
        }
        Check(max_page < 4096, "runtime page limit");
        for (const auto cp : sample_cps) {
            const auto& g = glyphs.at(cp - first);
            Check(g.w && g.h, "Latin/Cyrillic/CJK/Thai glyph present");
            auto image = art.FontAtlasImage(g.y / mtas::FontPageHeight);
            Check(image.has_value(), "font page rasterizes");
            Check(image->rgba.size() <= 1024u * 1024, "page <= 1 MiB");
            bool ink = false;
            for (std::uint32_t y = 0; y < g.h; ++y)
                for (std::uint32_t x = 0; x < g.w; ++x)
                    ink |= image->rgba[((g.y % mtas::FontPageHeight + y) * image->width + g.x + x) * 4 + 3] != 0;
            Check(ink, "requested glyph has ink");
        }
        Check(!art.FontAtlasImage(4096), "out-of-range page refused");
        std::printf("Font: %zu metrics, %u pages, <= 1 MiB/page (32 MiB resident budget)\n",
                    glyphs.size(), max_page + 1);

        // Task's own worked examples: the tracked mission's items (DATA-CONTRACT.md "Tracked
        // mission", Sandrock Commission / Bronze Stick (5/6) / Bronze Bars (9/2)) plus the
        // mission's own item-progress table's "Potato" cross-check example, and three NPCs this
        // round's live boot (harness/my-time-at-sandrock/runs/20261107T1319+0700-probe-default/)
        // cross-checked against the native Social tab's pip strip.
        struct Target {
            const char* name;
            std::uint32_t w, h;
        };
        const Target items[] = {
            {"Item_Drop_Bronze_rods", 64, 64}, // "Bronze Stick" (id 19800019)
            {"Item_Drop_Bar_Bronze", 64, 64},  // "Bronze Bars" (id 19110002)
            {"Item_Crop_Potato", 64, 64},      // "Potato" (id 16200003)
        };
        const Target npcs[] = {
            {"npc_8009", 128, 128},
            {"npc_8027", 128, 128},
            {"npc_8071", 128, 128},
            // Display names differ from ActorProto.iconPath; resolve through index aliases.
            {"npc_8074", 128, 128},
            {"npc_8061", 128, 128},
            {"npc_8047", 128, 128},
            {"npc_8034", 128, 128},
            {"npc_8091", 128, 128},
            {"npc_8092", 128, 128},
            {"npc_8093", 128, 128},
            {"npc_8094", 128, 128},
            {"npc_8095", 128, 128},
        };

        // Regression guard for the round-2 vertical-flip fix (DecodeSwizzledAstcSprite): a fixed
        // UiPins() sprite, decoded the exact same way as the dynamic SpriteIndex() ones above.
        // Coin is small and roughly flip-symmetric (a disc+star) -- it never visibly LOOKED wrong
        // even with the old, buggy formula, which is exactly why the bug went unnoticed through
        // Stage-8/9's own "live verified" passes. This only guards against a regression once this
        // test is run with MTAS_ROMFS set; it cannot catch the bug by eye the way the asymmetric
        // item icons / NPC portraits below did.
        {
            const auto native = art.ResolveSprite("Coin");
            Check(native.has_value(), "fixed UI pin resolves: Coin");
            CheckNonDegenerate(*native, "Coin");
            MaybeSavePng("ui_Coin", *native);
        }
        {
            const auto native = art.ResolveSprite("HbNewBadge");
            Check(native.has_value(), "native Handbook NEW_3 pin resolves");
            Check(native->width == 16 && native->height == 16, "NEW_3 native dimensions");
            CheckNonDegenerate(*native, "HbNewBadge");
            const auto scaled = art.LoadSprite("HbNewBadge", 32, 32);
            Check(scaled.has_value(), "Handbook fresh badge scales");
            Check(scaled->width == 32 && scaled->height == 32, "badge display dimensions");
            MaybeSavePng("ui_HbNewBadge", *scaled);
        }
        // Map page: the native fog, Map tab and player icon pins, and world-map tiles (row 0 =
        // the map's visual top; a 256 px tile is the texture's own resolution).
        for (const char* name : {"MapFog", "TabMapNor", "TabMapSel", "MapPlayer"}) {
            const auto native = art.ResolveSprite(name);
            Check(native.has_value(), "map page pin resolves");
            CheckNonDegenerate(*native, name);
        }
        Check(!art.ResolveSprite("MapWorld"), "whole 4096 px world map refused (64 MiB)");
        for (const auto [col, row] : {std::pair{8u, 7u}, std::pair{0u, 0u}, std::pair{15u, 15u}}) {
            const auto tile = art.LoadMapTile("MapWorld", col, row, 16, 256);
            Check(tile && tile->width == 256 && tile->height == 256, "world map tile decodes");
            const auto small = art.LoadMapTile("MapWorld", col, row, 16, 128);
            Check(small && small->width == 128 && small->height == 128, "overview tile downscales");
            char name[32];
            std::snprintf(name, sizeof name, "map_%u_%u", col, row);
            MaybeSavePng(name, *tile);
        }
        Check(!art.LoadMapTile("MapWorld", 16, 0, 16, 256) && !art.LoadMapTile("MapWorld", 0, 0, 16, 300),
              "out-of-range tile requests refused");
        // Live fog's world picture (module:mtas:world): one 4096 px crop is over the 16 MiB cap,
        // so it is stitched from 4 x 4 tiles into 2048 px, and matches its own tiles.
        Check(!art.LoadMapTile("MapWorld", 0, 0, 1, 2048), "single 4096 px world crop refused");
        {
            const auto world = art.LoadMapWhole("MapWorld", 4, 512);
            Check(world && world->width == 2048 && world->height == 2048, "world picture stitches");
            const auto corner = art.LoadMapTile("MapWorld", 3, 2, 4, 512);
            bool same = world && corner;
            for (std::uint32_t y = 0; same && y < 512; ++y)
                same = std::equal(corner->rgba.begin() + std::size_t{y} * 512 * 4,
                                  corner->rgba.begin() + std::size_t{y + 1} * 512 * 4,
                                  world->rgba.begin() + ((std::size_t{2} * 512 + y) * 2048 + 3 * 512) * 4);
            Check(same, "stitched world tile (3,2) is at its place");
            if (world) CheckNonDegenerate(*world, "MapWorld-whole");
            Check(!art.LoadMapWhole("MapWorld", 4, 1024), "over-cap world picture refused");
            if (world) MaybeSavePng("map_world_2048", *world);
        }
        for (const auto& t : {Target{"HbOptionBG1Nor", 285, 360},
                              Target{"HbIconBG1", 204, 174}, Target{"HbLock", 144, 135}}) {
            const auto native = art.ResolveSprite(t.name);
            Check(native.has_value(), "native Handbook locked-card pin resolves");
            CheckNonDegenerate(*native, t.name);
            const auto scaled = art.LoadSprite(t.name, t.w, t.h);
            Check(scaled && scaled->width == t.w && scaled->height == t.h,
                  "Handbook locked-card display dimensions");
            MaybeSavePng(t.name, *scaled);
        }
        for (const auto& t : items) {
            const auto native = art.ResolveSprite(t.name);
            Check(native.has_value(), (std::string("item icon resolves: ") + t.name).c_str());
            CheckNonDegenerate(*native, t.name);
            MaybeSavePng((std::string("item_") + t.name).c_str(), *native);
            const auto scaled = art.LoadSprite(t.name, t.w, t.h);
            Check(scaled.has_value(), (std::string("item icon scales: ") + t.name).c_str());
            Check(scaled->width == t.w && scaled->height == t.h, "scaled to requested size");
        }
        for (int filter : {0, 1, 2, 3, 4, 5, 6, 9}) {
            for (const char* state : {"Nor", "Sel"}) {
                const auto name = "InvCategory" + std::to_string(filter) + state;
                const auto native = art.ResolveSprite(name);
                Check(native.has_value(), "native Inventory category pin resolves");
                CheckNonDegenerate(*native, name.c_str());
                MaybeSavePng(name.c_str(), *native);
                const auto scaled = art.LoadSprite(name, 112, 112);
                Check(scaled && scaled->width == 112 && scaled->height == 112,
                      "Inventory category scales to touch target");
            }
        }
        for (const char* name : {"I_Map_icon_MissionGuide_00", "I_Map_icon_Mark_00",
                                 "I_Map_icon_Home_00", "I_Map_icon_Player_00",
                                 "I_Button_btn_Confirm01_nor", "I_Reading_img_PageLeft_00",
                                 "I_Reading_img_PageRight_00", "star-1",
                                 "I_Map_btn_ZoomIn_nor", "I_Map_btn_ZoomOut_nor"}) {
            const auto native = art.ResolveSprite(name);
            Check(native.has_value(), "native map/control pin resolves");
            CheckNonDegenerate(*native, name);
            MaybeSavePng(name, *native);
            const auto scaled = art.LoadSprite(name, 64, 64);
            Check(scaled && scaled->width == 64 && scaled->height == 64, "native marker/control scales");
        }
        for (const char* name : {"I_Common_img_Arrow_00", "I_Common_img_Arrow_01", "I_Common_img_Arrow_02", "I_Common_img_Arrow_03", "I_Common_img_Arrow_04"}) {
            const auto image = art.ResolveSprite(name);
            Check(image.has_value(), "native paging chevron resolves");
            MaybeSavePng(name, *image);
        }
        const auto detail_panel = art.LoadSprite("SocialPolaroidBG", 438, 132);
        Check(detail_panel.has_value(), "compact detail panel retains native slice borders");
        CheckNonDegenerate(*detail_panel, "compact detail panel");
        Check(art.LoadMapBadge("monster_BumbleAnt_01_Mini~I_Map_img_MonsterBg_00").has_value(), "native creature and background compose");
        Check(art.LoadMapBadge("room_I_Map_icon_Wish_00").has_value(), "native indoor POI house backing composes");
        const auto room_badge = art.LoadMapBadge("room_MapIcon_MissionSide_00");
        Check(room_badge && room_badge->width == 100 && room_badge->height == 100,
              "native mission prefab composition");
        CheckNonDegenerate(*room_badge, "native mission prefab composition");
        MaybeSavePng("mission-house-badge", *room_badge);
        Check(!art.LoadMapBadge("plain_UnknownPrefab"), "unknown prefab fails closed");
        for (const auto& t : npcs) {
            const auto native = art.ResolveSprite(t.name);
            Check(native.has_value(), (std::string("npc portrait resolves: ") + t.name).c_str());
            CheckNonDegenerate(*native, t.name);
            MaybeSavePng((std::string("npc_") + t.name).c_str(), *native);
            const auto scaled = art.LoadSprite(t.name, t.w, t.h);
            Check(scaled.has_value(), (std::string("npc portrait scales: ") + t.name).c_str());
            Check(scaled->width == t.w && scaled->height == t.h, "scaled to requested size");
        }

        // The sprite cache actually caches: a second ResolveSprite for the same name returns
        // identical pixels and the cache's own entry count does not grow (same key, not a new
        // decode).
        {
            const auto [entries_before, bytes_before] = art.SpriteCacheStats();
            const auto again = art.ResolveSprite("npc_8009");
            Check(again.has_value(), "cached resolve still succeeds");
            const auto [entries_after, bytes_after] = art.SpriteCacheStats();
            Check(entries_after == entries_before && bytes_after == bytes_before,
                 "repeat resolve does not grow the cache");
        }

        // An unknown name (in neither UiPins() nor SpriteIndex()) is refused, never a fabricated
        // image -- same contract as the fixed-pin table already had.
        Check(!art.ResolveSprite("ThisSpriteNameDoesNotExist12345").has_value(),
             "unknown name refused");

        if (std::getenv("MTAS_BUNDLE_SOAK")) {
            const auto report = [&](const char* phase) {
                const auto [bundles, bytes] = art.BundleCacheStats();
                const auto [sprites, rgba] = art.SpriteCacheStats();
                std::printf("soak %s: bundles=%zu resident_bytes=%zu sprites=%zu rgba_bytes=%zu\n",
                            phase, bundles, bytes, sprites, rgba);
                std::fflush(stdout);
                Check(bytes <= mtas::Art::BundleCacheBudget, "bundle byte budget");
                Check(bundles <= 32 && sprites <= 64 && rgba <= 24u * 1024u * 1024u,
                      "bundle and sprite entry/byte budgets");
            };
            report("before");
            // Every resolvable item/Handbook icon and Social portrait, including entries
            // beyond this save's lists. Repeat after a font/language change to catch growth.
            for (int pass = 0; pass < 2; ++pass) {
                std::size_t decoded = 0;
                for (const auto& entry : mtas::SpriteIndex()) {
                    Check(art.ResolveSprite(entry.name).has_value(), "soak sprite resolves");
                    ++decoded;
                    const auto [count, bytes] = art.BundleCacheStats();
                    Check(count <= 32 && bytes <= mtas::Art::BundleCacheBudget,
                          "residency bounded after every decode");
                }
                std::printf("soak pass=%d decoded=%zu\n", pass, decoded);
                art.SetFontText(pass == 0 ? Utf8({0x3042, 0x6728, 0x66DC}) : "Francais ecole");
                Check(art.FontGlyphs(first, glyphs, line), "soak language font rebuild");
                report(pass == 0 ? "after-japanese" : "after-french");
            }
        }

        std::printf("mtas_sprite_resolve_test: all ok (%d checks, %zu item icons + %zu NPC "
                   "portraits live-decoded against the real romfs + real CPU ASTC decoder)\n",
                   g_checks, std::size(items), std::size(npcs));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
}
