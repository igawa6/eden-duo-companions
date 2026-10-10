// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// chained_echoes_unity checks.
//   1. Synthetic (always): Switch deswizzle is the inverse of UnityPy's swizzle; host glyph mapping.
//   2. Game data (skipped with code 77 when absent): every asset the accepted bottom-screen design
//      uses (manifest.tsv: kind, bundle, name, reference PNG) is decoded from the merged 1.41 romfs
//      and compared byte for byte with the UnityPy 1.25 reference export; the CE TextMesh Pro font
//      glyph table and every glyph's atlas tile are compared with glyphs.bin.
// Environment: CE_ROMFS (merged romfs root), CE_REFERENCE (directory with manifest.tsv, extras.tsv,
// glyphs.bin and ../game-tables). CTest passes the CMake cache paths CE_UNITY_ROMFS and
// CE_UNITY_REFERENCE; both are private fixtures that never enter this repository.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "chained_echoes_unity.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

std::string Env(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::string{v} : std::string{fallback};
}

// UnityPy TextureSwizzler.swizzle (linear -> block-linear), used to invert our deswizzle.
std::vector<std::uint8_t> Swizzle(const std::vector<std::uint8_t>& data, std::uint32_t w,
                                  std::uint32_t h, std::uint32_t bw, std::uint32_t bh,
                                  std::uint32_t gobs) {
    const std::uint32_t bcx = (w + bw - 1) / bw, bcy = (h + bh - 1) / bh;
    const std::uint32_t gcx = bcx / 4, gcy = bcy / 8;
    std::vector<std::uint8_t> out(data.size());
    std::size_t dst = 0;
    for (std::uint32_t i = 0; i < gcy / gobs; ++i)
        for (std::uint32_t j = 0; j < gcx; ++j)
            for (std::uint32_t k = 0; k < gobs; ++k)
                for (std::uint32_t v = 0; v < 32; ++v) {
                    const std::uint32_t gx = ((v >> 3) & 2) | ((v >> 1) & 1);
                    const std::uint32_t gy = ((v >> 1) & 6) | (v & 1);
                    const std::size_t src =
                        ((static_cast<std::size_t>((i * gobs + k) * 8 + gy) * bcx) + j * 4 + gx) * 16;
                    std::memcpy(out.data() + dst, data.data() + src, 16);
                    dst += 16;
                }
    return out;
}

void SyntheticChecks() {
    // 48 x 64 RGBA32 (4 texels per 16 bytes), gobsPerBlock 2: exactly whole GOB blocks.
    const std::uint32_t w = 48, h = 64;
    std::vector<std::uint8_t> linear(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t k = 0; k < linear.size(); ++k)
        linear[k] = static_cast<std::uint8_t>(k * 131 + (k >> 8));
    const auto sw = Swizzle(linear, w, h, 4, 1, 2);
    CHECK(sw != linear);
    CHECK(ce_unity::SwitchDeswizzle(sw, w, h, 4, 1, 2) == linear);

    // Texture decode orientation: Unity rows are bottom-up; output is top-left.
    ce_unity::TextureInfo tex;
    tex.width = 2;
    tex.height = 2;
    tex.format = 4;
    const std::vector<std::uint8_t> px{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const auto img = ce_unity::DecodeTextureData(px, tex, {});
    CHECK(img && img->width == 2 && img->height == 2);
    if (img)
        CHECK(img->rgba[0] == 9 && img->rgba[8] == 1);

    // Host glyph mapping from a hand-made TMP font.
    ce_unity::TmpFont f;
    f.atlas_w = f.atlas_h = 256;
    f.cap_line = 21.0f;
    ce_unity::TmpGlyph g;
    g.rect_x = 158;
    g.rect_y = 98;
    g.rect_w = 4;
    g.rect_h = 21;
    g.bearing_x = 0;
    g.bearing_y = 21;
    g.advance = 6;
    f.glyphs.push_back(g);
    f.chars[33] = 0;
    f.chars[0x2026] = 0;
    f.chars[0x25A1] = 0;
    std::uint32_t first = 0, lh = 0;
    std::vector<EdenDsmodFontGlyph> hg;
    CHECK(ce_unity::BuildHostGlyphs(f, first, hg, lh));
    CHECK(first == 33 && lh == 21 && hg.size() == 0x2026 - 33 + 1);
    CHECK(hg[0].x == 158 && hg[0].y == 256 - 98 - 21 && hg[0].w == 4 && hg[0].h == 21 &&
          hg[0].bearing_y == 21 && hg[0].advance == 6);
}

struct Ref {
    std::string kind, bundle, name, png;
};

std::vector<Ref> ReadManifest(const std::string& path) {
    std::vector<Ref> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        Ref r;
        if (std::getline(ss, r.kind, '\t') && std::getline(ss, r.bundle, '\t') &&
            std::getline(ss, r.name, '\t') && std::getline(ss, r.png))
            out.push_back(r);
    }
    return out;
}

bool LoadPng(const std::string& path, ce_unity::Image& out) {
    int w = 0, h = 0, n = 0;
    unsigned char* p = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!p)
        return false;
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.rgba.assign(p, p + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(p);
    return true;
}

int GameDataChecks(const std::string& romfs, const std::string& refdir) {
    const auto refs = ReadManifest(refdir + "/manifest.tsv");
    std::FILE* probe = std::fopen((romfs + "/Data/StreamingAssets/aa/Switch/systemgfx_assets_all.bundle").c_str(), "rb");
    if (refs.empty() || !probe) {
        if (probe)
            std::fclose(probe);
        std::printf("game data checks skipped (CE_ROMFS=%s, CE_REFERENCE=%s)\n", romfs.c_str(),
                    refdir.c_str());
        return 77;
    }
    std::fclose(probe);
    ce_unity::Reader reader(ce_unity::MakeDirectoryRangeReader(romfs));
    int exact = 0;
    std::vector<ce_unity::Image> decoded(refs.size());
    std::size_t at_ref = 0;
    for (const Ref& r : refs) {
        const std::size_t slot = at_ref++;
        ce_unity::Image ref;
        if (!LoadPng(r.png, ref)) {
            std::fprintf(stderr, "reference PNG missing: %s\n", r.png.c_str());
            ++failures;
            continue;
        }
        const auto img = r.kind == "Texture2D" ? reader.LoadTexture(r.bundle, r.name)
                                               : reader.LoadSprite(r.bundle, r.name);
        if (!img) {
            std::fprintf(stderr, "decode failed: %s %s/%s\n", r.kind.c_str(), r.bundle.c_str(),
                         r.name.c_str());
            ++failures;
            continue;
        }
        if (img->width != ref.width || img->height != ref.height || img->rgba != ref.rgba) {
            std::size_t diff = 0;
            if (img->rgba.size() == ref.rgba.size())
                for (std::size_t k = 0; k < ref.rgba.size(); k += 4)
                    diff += std::memcmp(&ref.rgba[k], &img->rgba[k], 4) != 0;
            std::fprintf(stderr, "mismatch: %s/%s %ux%u vs ref %ux%u, %zu pixels differ\n",
                         r.bundle.c_str(), r.name.c_str(), img->width, img->height, ref.width,
                         ref.height, diff);
            ++failures;
            continue;
        }
        decoded[slot] = *img;
        ++exact;
    }
    std::printf("assets: %d / %zu byte-exact vs UnityPy reference\n", exact, refs.size());

    CHECK(ce_unity::BuiltinPinCount() >= 764);
    // Concurrency: a second reader (cold caches, small LRU so bundles are evicted and reopened)
    // decoding from four threads must give the same pixels.
    {
        ce_unity::Reader shared(ce_unity::MakeDirectoryRangeReader(romfs), {},
                                std::string{ce_unity::DefaultAssetRoot}, 2);
        shared.SetUseBuiltinPins(false); // the name-index path (the first pass used the pins)
        std::vector<int> bad(4, 0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&, t] {
                for (std::size_t k = static_cast<std::size_t>(t); k < refs.size(); k += 4) {
                    const Ref& r = refs[k];
                    const auto img = r.kind == "Texture2D" ? shared.LoadTexture(r.bundle, r.name)
                                                           : shared.LoadSprite(r.bundle, r.name);
                    if (!decoded[k].rgba.empty() && (!img || img->rgba != decoded[k].rgba))
                        ++bad[static_cast<std::size_t>(t)];
                }
            });
        for (auto& th : threads)
            th.join();
        CHECK(bad[0] + bad[1] + bad[2] + bad[3] == 0);
    }

    // CE font: glyph table + every glyph tile.
    const auto font = ce_unity::LoadTmpFont(reader);
    CHECK(font.has_value());
    std::ifstream gin(refdir + "/glyphs.bin", std::ios::binary);
    std::vector<char> gb((std::istreambuf_iterator<char>(gin)), std::istreambuf_iterator<char>());
    CHECK(gb.size() >= 4);
    if (font && gb.size() >= 4) {
        CHECK(font->family == "CE" && font->cap_line == 21.0f && font->atlas_w == 256);
        std::uint32_t n;
        std::memcpy(&n, gb.data(), 4);
        std::size_t at = 4;
        int glyph_ok = 0;
        for (std::uint32_t k = 0; k < n && at + 32 <= gb.size(); ++k) {
            std::uint32_t cp;
            std::int32_t rect[4];
            float m[3];
            std::memcpy(&cp, gb.data() + at, 4);
            std::memcpy(rect, gb.data() + at + 4, 16);
            std::memcpy(m, gb.data() + at + 20, 12);
            at += 32;
            const std::size_t tile = static_cast<std::size_t>(rect[2]) * rect[3];
            const ce_unity::TmpGlyph* g = font->Find(cp);
            bool ok = g && g->rect_x == rect[0] && g->rect_y == rect[1] && g->rect_w == rect[2] &&
                      g->rect_h == rect[3] && g->bearing_x == m[0] && g->bearing_y == m[1] &&
                      g->advance == m[2];
            if (ok && tile) {
                const auto t = ce_unity::GlyphTile(*font, cp);
                ok = t && t->rgba.size() == tile * 4;
                for (std::size_t p = 0; ok && p < tile; ++p)
                    ok = t->rgba[p * 4 + 3] == static_cast<std::uint8_t>(gb[at + p]);
            }
            at += tile;
            if (!ok) {
                std::fprintf(stderr, "glyph U+%04X mismatch\n", cp);
                ++failures;
            } else {
                ++glyph_ok;
            }
        }
        std::printf("font: %d / %u glyphs exact (metrics + atlas tile)\n", glyph_ok, n);
        CHECK(static_cast<std::uint32_t>(glyph_ok) == n && n == font->chars.size());
    }

    // Pins: a correct pin skips the name index; a stale pin falls back to it.
    {
        ce_unity::Reader probe(ce_unity::MakeDirectoryRangeReader(romfs));
        const auto pin = probe.FindSpritePin(ce_unity::bundles::MapsTextures, "kortara_map");
        CHECK(pin.has_value());
        ce_unity::Reader cold(ce_unity::MakeDirectoryRangeReader(romfs));
        const auto ref_img = reader.LoadSprite(ce_unity::bundles::MapsTextures, "kortara_map");
        ce_unity::ResetStats();
        const auto pinned = pin ? cold.LoadSpritePinned(ce_unity::bundles::MapsTextures, "kortara_map", *pin)
                                : std::nullopt;
        CHECK(pinned && ref_img && pinned->rgba == ref_img->rgba);
        CHECK(ce_unity::GetStats().objects_indexed == 0);
        const auto stale = cold.LoadSpritePinned(ce_unity::bundles::MapsTextures, "kortara_map",
                                                 ce_unity::SpritePin{0, 12345});
        CHECK(stale && ref_img && stale->rgba == ref_img->rgba);
        CHECK(ce_unity::GetStats().objects_indexed > 0);
    }

    // Spec members beyond the renders (area maps, Overdrive slots, ...): extras.tsv, same rule.
    {
        int ok = 0;
        const auto extras = ReadManifest(refdir + "/extras.tsv");
        for (const Ref& r : extras) {
            ce_unity::Image ref;
            const auto img = reader.LoadSprite(r.bundle, r.name);
            if (LoadPng(r.png, ref) && img && img->width == ref.width && img->rgba == ref.rgba)
                ++ok;
            else
                std::fprintf(stderr, "extra mismatch: %s/%s\n", r.bundle.c_str(), r.name.c_str());
        }
        std::printf("extras: %d / %zu byte-exact\n", ok, extras.size());
        CHECK(ok == static_cast<int>(extras.size()));
    }

    // TextAsset tables in Data/resources.assets (game-tables were exported by UnityPy).
    {
        const auto range = ce_unity::MakeDirectoryRangeReader(romfs);
        const std::string tables = refdir + "/../game-tables/";
        int ok = 0, n = 0;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(tables, ec)) {
            if (e.path().extension() != ".txt")
                continue;
            const long long id = std::atoll(e.path().stem().string().c_str());
            std::ifstream in(e.path(), std::ios::binary);
            if (!in || id <= 0)
                continue;
            ++n;
            const std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto got = ce_unity::ReadTextAsset(range, ce_unity::ResourcesAssets, "", id);
            ok += got && *got == want;
        }
        std::printf("text assets: %d / %d exact\n", ok, n);
        CHECK(ok == n);
        CHECK(ce_unity::ReadTextAsset(range, ce_unity::ResourcesAssets, "rewardBoard").has_value());
    }

    // A family lookup across bundles (ctb icons live in gfx2 and systemgfx among others).
    std::string where;
    const auto ctb = reader.LoadSpriteAny(ce_unity::FamilyBundles(ce_unity::Family::Ctb),
                                          ce_unity::FamilySprite(ce_unity::Family::Ctb, "9"), &where);
    CHECK(ctb && where == ce_unity::bundles::SystemGfx);
    return 0;
}

} // namespace

int main() {
    SyntheticChecks();
    const int game = GameDataChecks(Env("CE_ROMFS", ""), Env("CE_REFERENCE", ""));
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return game == 77 ? 77 : 0;
}
