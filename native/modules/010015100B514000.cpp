// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone, read-only Super Mario Bros. Wonder companion reader for game version 1.2.1.
// The pointer chains, clamps and hashed game-data keys follow the Eden-DS Wonder companion
// (a third-party build that supports 1.0.0 only); every main-module global was re-found in
// 1.2.1 and is resolved at runtime from a pinned accessor instruction pair (ADRP + LDR).

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "wonder_assets.h"
#include "wonder_catalog.h"
#include "wonder_font.h"
#include "wonder_glyphs.h"
#include "wonder_seed_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <thread>

namespace {
using namespace dsmod_sdk::int_types;
using VAddr = u64;

constexpr u64 TitleId = UINT64_C(0x010015100B514000);
constexpr std::string_view BuildId =
    "FF773E90972D544EB79406EAA65396D53C43EFB9000000000000000000000000";
// Other game versions, accepted only to draw the "wrong pipe" page with the game's own art and
// font: nothing is read from guest memory on these builds (none of the 1.2.1 addresses apply).
// Add a build here (and to the package's module.build_ids) to give it the game-art page.
struct WrongPipeBuild {
    std::string_view build_id;
    std::string_view version;
};
constexpr std::array<WrongPipeBuild, 2> WrongPipeBuilds{{
    {"CD6E42AEE7934F4D8393CD43893AE25EDC83D338000000000000000000000000", "1.0.0"}, // base
    {"F91868B88F60D3D59009DB3389FDE314A6A32FCD000000000000000000000000", "1.0.1"}, // v65536
}};

// One accessor per global: "adrp xN, page; ldr xN, [xN, #lo]" at a 1.2.1 code offset. The
// module checks both opcodes (only the immediates may vary), decodes the global and compares it
// with the expected offset, so a wrong image fails closed instead of reading a wrong slot.
struct Accessor {
    VAddr site;
    VAddr expected;
};
constexpr Accessor SceneRootAccessor{0x4f0, 0x37b4e00};
constexpr Accessor PlayerRootAccessor{0xea8, 0x37b2128};
constexpr Accessor CameraRootAccessor{0xa6c18, 0x37cf780};
constexpr Accessor GameDataAccessor{0x2604b8, 0x37cbc70};
constexpr Accessor OverlayRootAccessor{0xb42fc, 0x37cd300};
constexpr Accessor FlagsRootAccessor{0x2897f4, 0x37cf120};
constexpr Accessor CourseTableAccessor{0x32ff0c, 0x37bdd20};
// The course table's ready byte sits 8 bytes below its pointer (same layout as 1.0.0).
constexpr VAddr CourseTableReadyDelta = 8;

// Instruction words the chains depend on; all four are unchanged from 1.0.0 (only moved).
constexpr std::array<std::pair<VAddr, u32>, 4> CodeSignatures{{
    {0x104738, 0xb9400108}, // GameData getInt: ldr w8,[x8] (entry stride 0x28, value +0x1c)
    {0x20db3c, 0xb9400109}, // property map value read (values = [obj+0xb0])
    {0x33ed40, 0xf900f2c8}, // str x8,[x22,#0x1e0]
    {0x495cb4, 0xb9433aca}, // ldr w10,[x22,#0x338]: course gold coins (+0x33c purple next)
}};

// Opaque 32-bit keys the game itself passes to its game-data getters.
constexpr u32 KeyWorld = 0x9f5ead3c;
constexpr u32 KeyWorldSeeds = 0xeeff353b;
constexpr u32 KeyMapGold = 0x17f0bb21;
constexpr u32 KeyMapPurple = 0xf4ee6827;
constexpr u32 KeyMapLives = 0x7940dc77;
constexpr u32 KeyPaused = 0x35db180e;
constexpr u32 KeyPowerUp = 0xe811e5ef;

// GameData hash tables share one layout at G+<table>: {u32 count; entries*@8; keys*@0x10 ({u32
// key, u32 index} pairs, linear probing); u32 cap@0x1c}.
constexpr VAddr GameDataInts = 0xd0;      // 0x28-byte entries, value at +0x1c
constexpr VAddr GameDataRefs = 0x130;     // 0x40-byte entries, u32 array (count +0x20, data [+0x28])
constexpr VAddr GameDataIntArrays = 0x7f0; // 0x40-byte entries, same array layout

// Saved per-course state (1.2.1). Per world W the game's key table (rodata main+0x2B57BB8 +
// W*0x70, u32 keys) names int arrays in GameData's int-array table indexed by the course's
// world-map key: [4] goal seeds earned (bit = GoalID), [5] Wonder-effect seed (bit0).
constexpr VAddr WorldKeyTable = 0x2B57BB8;
constexpr VAddr WorldKeyStride = 0x70;
constexpr int SavedGoalSeeds = 4, SavedWonderSeed = 5;

struct SavedCourse {
    u32 goal_seeds{}, wonder_seed{};
};

constexpr std::array<int, 12> CharacterMap{0, 1, 2, 3, 4, 5, 6, 11, 7, 8, 9, 10};

enum Status : int {
    StatusUnsupported = 3,
    StatusResolving = 4,
    StatusCourse = 5,
    StatusWorldMap = 7,
    StatusLoading = 8,
    StatusTitle = 9,
    StatusMiss = 10,
    StatusTransition = 11,
};

constexpr bool Sane(u64 p) {
    return p >= UINT64_C(0x100000000) && p < UINT64_C(0x8000000000);
}

std::string BuildHex(const EdenDsmodHostApi& h) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const u8 b : h.build_id) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

// A romfs reader over the host's read_romfs. The function pointer and userdata stay valid for
// the module instance's lifetime, so a copy taken at create() is safe on any thread.
WonderAssets::RomfsReader MakeRomfsReader(const EdenDsmodHostApi& h) {
    return [read_romfs = h.read_romfs,
            userdata = h.userdata](const std::string& path) -> std::optional<std::vector<u8>> {
        if (!read_romfs)
            return std::nullopt;
        const auto size = read_romfs(userdata, path.c_str(), 0, nullptr, 0);
        if (!size || size > 32 * 1024 * 1024)
            return std::nullopt;
        std::vector<u8> out(size);
        if (read_romfs(userdata, path.c_str(), 0, out.data(), size) != size)
            return std::nullopt;
        return out;
    };
}

// A copy of the image centred on a transparent square (side = the longer edge).
WonderAssets::Image PadSquare(const WonderAssets::Image& in) {
    const u32 side = std::max(in.width, in.height);
    WonderAssets::Image out{side, side, std::vector<u8>(static_cast<size_t>(side) * side * 4)};
    const u32 ox = (side - in.width) / 2, oy = (side - in.height) / 2;
    for (u32 y = 0; y < in.height; ++y)
        std::memcpy(out.rgba.data() + ((static_cast<size_t>(y) + oy) * side + ox) * 4,
                    in.rgba.data() + static_cast<size_t>(y) * in.width * 4,
                    static_cast<size_t>(in.width) * 4);
    return out;
}

// Separable box blur, `passes` times (three passes approximate a Gaussian), edges clamped.
WonderAssets::Image Blur(const WonderAssets::Image& in, int radius, int passes) {
    if (in.width == 0 || in.height == 0 || in.rgba.size() < size_t{in.width} * in.height * 4)
        return in;
    WonderAssets::Image a = in, b = in;
    const int w = static_cast<int>(in.width), h = static_cast<int>(in.height);
    const auto px = [&](WonderAssets::Image& im, int x, int y) {
        return im.rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
    };
    const int n = 2 * radius + 1;
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < h; ++y)
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (int k = -radius; k <= radius; ++k)
                    sum += px(a, std::clamp(k, 0, w - 1), y)[c];
                for (int x = 0; x < w; ++x) {
                    px(b, x, y)[c] = static_cast<u8>(sum / n);
                    sum += px(a, std::min(x + radius + 1, w - 1), y)[c] -
                           px(a, std::max(x - radius, 0), y)[c];
                }
            }
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (int k = -radius; k <= radius; ++k)
                    sum += px(b, x, std::clamp(k, 0, h - 1))[c];
                for (int y = 0; y < h; ++y) {
                    px(a, x, y)[c] = static_cast<u8>(sum / n);
                    sum += px(b, x, std::min(y + radius + 1, h - 1))[c] -
                           px(b, x, std::max(y - radius, 0))[c];
                }
            }
    }
    return a;
}

// Package-authored backgrounds generated in code (no game art): "gen/sky" is the diagonal
// blue -> purple -> pink -> yellow wash with soft light discs; "gen/wave" is a cream band whose
// top edge is a gentle wave; "gen/shade" is a vertical fade to transparent (backdrop legibility).
std::optional<WonderAssets::Image> GenerateArt(std::string_view key) {
    struct C {
        float r, g, b;
    };
    const auto mix = [](C a, C b, float t) {
        return C{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    };
    const auto put = [](WonderAssets::Image& im, u32 x, u32 y, C c, float a) {
        u8* p = im.rgba.data() + (static_cast<size_t>(y) * im.width + x) * 4;
        p[0] = static_cast<u8>(std::clamp(c.r, 0.0f, 255.0f));
        p[1] = static_cast<u8>(std::clamp(c.g, 0.0f, 255.0f));
        p[2] = static_cast<u8>(std::clamp(c.b, 0.0f, 255.0f));
        p[3] = static_cast<u8>(std::clamp(a * 255.0f, 0.0f, 255.0f));
    };
    WonderAssets::Image im;
    if (key == "gen/sky") {
        im = {620, 540, std::vector<u8>(620u * 540u * 4)};
        const std::array<C, 4> stops{C{42, 200, 242}, C{132, 71, 229}, C{255, 66, 151},
                                     C{255, 211, 54}};
        struct Disc {
            float x, y, r, a;
        };
        const std::array<Disc, 5> discs{Disc{520, 60, 150, 0.16f}, Disc{80, 470, 190, 0.10f},
                                        Disc{330, 250, 90, 0.08f}, Disc{600, 380, 120, 0.10f},
                                        Disc{140, 90, 60, 0.12f}};
        for (u32 y = 0; y < im.height; ++y)
            for (u32 x = 0; x < im.width; ++x) {
                const float t = std::clamp((x * 0.55f + y * 0.45f) / 620.0f, 0.0f, 1.0f) * 3.0f;
                const int i = std::min(2, static_cast<int>(t));
                C c = mix(stops[i], stops[i + 1], t - static_cast<float>(i));
                for (const auto& d : discs) {
                    const float dist = std::hypot(x - d.x, y - d.y);
                    const float edge = std::clamp((d.r - dist) / 2.0f, 0.0f, 1.0f);
                    c = mix(c, C{255, 255, 255}, edge * d.a);
                }
                put(im, x, y, c, 1.0f);
            }
        return im;
    }
    if (key == "gen/wave") {
        im = {620, 100, std::vector<u8>(620u * 100u * 4)};
        for (u32 y = 0; y < im.height; ++y)
            for (u32 x = 0; x < im.width; ++x) {
                const float top = 40.0f + 14.0f * std::sin(x / 620.0f * 6.2831853f * 1.5f + 0.6f);
                const float a = std::clamp(static_cast<float>(y) - top + 0.5f, 0.0f, 1.0f);
                put(im, x, y, C{255, 250, 229}, a);
            }
        return im;
    }
    // "gen/card/<w>x<h>/<radius>/<argb hex>": a rounded rectangle drawn at its final size (the
    // runtime's rect has no corner radius, and stretching a small image would distort corners).
    if (key.starts_with("gen/card/")) {
        unsigned w = 0, h = 0, r = 0, argb = 0;
        const std::string spec{key.substr(9)};
        if (std::sscanf(spec.c_str(), "%ux%u/%u/%x", &w, &h, &r, &argb) != 4 || w == 0 ||
            h == 0 || w > 1240 || h > 1080 || r * 2 > std::min(w, h))
            return std::nullopt;
        im = {w, h, std::vector<u8>(static_cast<size_t>(w) * h * 4)};
        const C col{static_cast<float>(argb >> 16 & 255), static_cast<float>(argb >> 8 & 255),
                    static_cast<float>(argb & 255)};
        const float alpha = static_cast<float>(argb >> 24 & 255) / 255.0f;
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                // Distance outside the rounded corner (pixel centres), 1 px anti-aliased edge.
                const float px = x + 0.5f, py = y + 0.5f;
                const float cx = std::clamp(px, static_cast<float>(r), static_cast<float>(w - r));
                const float cy = std::clamp(py, static_cast<float>(r), static_cast<float>(h - r));
                const float d = std::hypot(px - cx, py - cy) - static_cast<float>(r);
                put(im, x, y, col, alpha * std::clamp(0.5f - d, 0.0f, 1.0f));
            }
        return im;
    }
    if (key == "gen/pipe") { // green warp pipe (rim + body), shaded, dark outline
        im = {240, 260, std::vector<u8>(240u * 260u * 4)};
        const auto shade = [](float u) { // cylinder light from the left
            return 0.55f + 0.45f * std::sin(u * 3.14159265f * 0.9f + 0.35f);
        };
        for (u32 y = 0; y < im.height; ++y)
            for (u32 x = 0; x < im.width; ++x) {
                const bool rim = y < 70;
                const float x0 = rim ? 8.0f : 28.0f, x1 = rim ? 232.0f : 212.0f;
                const float y0 = rim ? 8.0f : 62.0f, y1 = rim ? 70.0f : 256.0f;
                const float px = x + 0.5f, py = y + 0.5f;
                if (px < x0 - 6 || px > x1 + 6 || py < y0 - 6 || py > y1 + 6)
                    continue;
                const bool inside = px >= x0 && px <= x1 && py >= y0 && py <= y1;
                if (!inside) {
                    put(im, x, y, C{20, 44, 26}, 1.0f); // outline
                    continue;
                }
                const float u = (px - x0) / (x1 - x0);
                const float k = shade(u);
                C c{46 * k + 20, 170 * k + 30, 62 * k + 16};
                if (u > 0.12f && u < 0.2f)
                    c = C{180, 245, 170}; // highlight stripe
                if (rim && (py - y0 < 6 || y1 - py < 6))
                    c = C{c.r * 0.7f, c.g * 0.7f, c.b * 0.7f};
                put(im, x, y, c, 1.0f);
            }
        return im;
    }
    if (key == "gen/fade_cream") { // transparent at the top -> cream at the bottom
        im = {4, 256, std::vector<u8>(4u * 256u * 4)};
        for (u32 y = 0; y < im.height; ++y)
            for (u32 x = 0; x < im.width; ++x) {
                const float t = static_cast<float>(y) / 255.0f;
                put(im, x, y, C{255, 250, 229}, t * t * (3.0f - 2.0f * t));
            }
        return im;
    }
    if (key == "gen/shade") {
        im = {4, 256, std::vector<u8>(4u * 256u * 4)};
        for (u32 y = 0; y < im.height; ++y)
            for (u32 x = 0; x < im.width; ++x)
                put(im, x, y, C{20, 16, 40}, 0.62f * (1.0f - static_cast<float>(y) / 255.0f));
        return im;
    }
    return std::nullopt;
}

class Reader {
public:
    explicit Reader(const EdenDsmodHostApi& api) : host{api}, romfs{MakeRomfsReader(api)} {
        const std::string build = BuildHex(api);
        supported = build == BuildId;
        for (const auto& w : WrongPipeBuilds)
            if (build == w.build_id)
                game_version = w.version;
    }
    // Course/world names and map tables come from the player's romfs. The host's romfs chain is
    // not open yet when create() runs, so the build starts from the first sample and runs off the
    // tick thread. A failed build is retried a few seconds later, at most CatalogAttempts times
    // (and not at all without a host clock).
    void StartCatalog() {
        constexpr int CatalogAttempts = 5;
        {
            std::scoped_lock lock{catalog_mutex};
            if (catalog_state == 1 || catalog_running)
                return;
            if (catalog_state == 2 &&
                (!host.get_tick || catalog_attempts >= CatalogAttempts ||
                 host.get_tick(host.userdata) < catalog_retry_tick))
                return;
            catalog_running = true;
            ++catalog_attempts;
            if (host.get_tick)
                catalog_retry_tick = host.get_tick(host.userdata) + 5 * 60;
        }
        if (catalog_thread.joinable())
            catalog_thread.join();
        catalog_thread = std::thread([this] {
            std::shared_ptr<const WonderCatalog::Catalog> built;
            if (auto c = WonderCatalog::BuildCatalog(romfs))
                built = std::make_shared<const WonderCatalog::Catalog>(std::move(*c));
            auto built_routes = std::make_shared<std::map<std::string, WonderCatalog::Route>>();
            if (built) {
                std::set<int> courses;
                for (const auto& [key, resource] : built->area_resource) {
                    if (stopping)
                        break;
                    if (courses.insert(key.first).second)
                        built_routes->merge(WonderCatalog::BuildCourseRoutes(romfs, key.first));
                }
            }
            std::scoped_lock lock{catalog_mutex};
            catalog_state = built ? 1 : 2;
            catalog = std::move(built);
            routes = std::move(built_routes);
            catalog_running = false;
        });
    }
    ~Reader() {
        stopping = true;
        if (catalog_thread.joinable())
            catalog_thread.join();
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    // decode_font: the package's font file names a game font and cap height ("name:px").
    EdenDsmodBool DecodeFont(const u8* bytes, size_t size, void* receiver,
                             EdenDsmodFontSink sink) const {
        if (!bytes || !sink)
            return EDEN_DSMOD_FALSE;
        const auto spec = WonderFont::ParseFontSpec({bytes, size});
        const auto font = spec ? WonderFont::BuildFontAtlas(romfs, *spec) : nullptr;
        if (!font || font->glyphs.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, font->line_height, font->first_codepoint, font->glyphs.data(),
             static_cast<u32>(font->glyphs.size()));
        return EDEN_DSMOD_TRUE;
    }
    void UpdateHost(const EdenDsmodHostApi& api) {
        host = api;
    }
    void Sample();
    void SampleValues();
    void Status(int status) {
        last_status = status;
        I64("wonder.status", status);
    }
    void Configure(const EdenDsmodHostExtensions* ext) {
        if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
            ext->struct_size >= sizeof(*ext))
        {
            astc = {ext->userdata, ext->decode_astc};
            assets.SetAstcDecoder(astc);
        }
    }
    // Asset worker thread: touches only `assets` (internally locked) and the borrowed host.
    EdenDsmodBool LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                            EdenDsmodImageSink sink) {
        constexpr std::string_view prefix{"module:wonder:"};
        if (!image_host || !image_host->read_romfs || !key || !sink)
            return EDEN_DSMOD_FALSE;
        const std::string_view k{key};
        if (!k.starts_with(prefix))
            return EDEN_DSMOD_FALSE;
        const auto logical = k.substr(prefix.size());
        if (constexpr std::string_view pict{"pict/"}; logical.starts_with(pict)) {
            const auto glyph =
                WonderAssets::LoadPictureGlyph(romfs, logical.substr(pict.size()), astc);
            if (!glyph || glyph->rgba.empty())
                return EDEN_DSMOD_FALSE;
            // Glyph cells are narrower than tall (~45x53): centre them on a square canvas so a
            // square widget keeps their proportions.
            const auto sq = PadSquare(*glyph);
            sink(receiver, sq.width, sq.height, sq.rgba.data(), sq.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        // "blur/<key>": any image this module serves, softened (the course thumbnails are 352x198
        // and shown far larger, so a deliberate blur reads better than a magnified one).
        if (constexpr std::string_view blur{"blur/"}; logical.starts_with(blur)) {
            const auto image = assets.Load(MakeRomfsReader(*image_host), logical.substr(blur.size()));
            if (!image)
                return EDEN_DSMOD_FALSE;
            const auto soft = Blur(*image, 5, 3);
            sink(receiver, soft.width, soft.height, soft.rgba.data(), soft.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (logical.starts_with("gen/")) {
            const auto art = GenerateArt(logical);
            if (!art)
                return EDEN_DSMOD_FALSE;
            sink(receiver, art->width, art->height, art->rgba.data(), art->rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (constexpr std::string_view font_prefix{"font/"}; logical.starts_with(font_prefix)) {
            const auto font =
                WonderFont::BuildFontAtlas(romfs, logical.substr(font_prefix.size()));
            if (!font || font->atlas.rgba.empty())
                return EDEN_DSMOD_FALSE;
            sink(receiver, font->atlas.width, font->atlas.height, font->atlas.rgba.data(),
                 font->atlas.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        const auto image = assets.Load(MakeRomfsReader(*image_host), logical);
        if (!image)
            return EDEN_DSMOD_FALSE;
        sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
        return EDEN_DSMOD_TRUE;
    }

private:
    std::shared_ptr<const std::map<std::string, WonderCatalog::Route>> Routes() const {
        std::scoped_lock lock{catalog_mutex};
        return routes;
    }
    void PublishRail(const WonderCatalog::Route& route, float x, float y, int flowers, int world,
                     int course);
    bool MarkerObtained(const WonderCatalog::RouteMarker& m, int flowers) const;
    std::shared_ptr<const WonderCatalog::Catalog> Catalog() const {
        std::scoped_lock lock{catalog_mutex};
        return catalog;
    }
    void Text(const char* name, const std::string& value) {
        if (host.publish_text)
            host.publish_text(host.userdata, name, value.c_str());
        frame_text[name] = value;
    }

    WonderAssets::Decoder assets;
    WonderAssets::RomfsReader romfs;
    bool supported{}; ///< false on older builds: wrong-pipe page only, no memory reads
    std::string_view game_version{"?"}; ///< version shown on the wrong-pipe page
    WonderAssets::AstcDecoder astc{};
    mutable std::mutex catalog_mutex;
    std::shared_ptr<const WonderCatalog::Catalog> catalog;
    std::shared_ptr<const std::map<std::string, WonderCatalog::Route>> routes;
    std::optional<WonderState::RunSeeds> run_seeds; ///< live flags, sampled once per tick
    std::optional<SavedCourse> saved; ///< this tick's saved state for the current course
    std::string reached_key;            ///< course + area of the current visit
    float reached_x{-1e9f};             ///< furthest player x in that area this visit
    int catalog_state{}; // 0 building, 1 ready, 2 failed (guarded by catalog_mutex)
    bool catalog_running{};
    int catalog_attempts{};
    u64 catalog_retry_tick{};
    std::atomic<bool> stopping{}; ///< destructor: the catalog thread skips the remaining routes
    std::thread catalog_thread;
    template <typename T>
    std::optional<T> Read(VAddr address) const {
        T value{};
        if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, address, &value,
                                                                       sizeof(T)))
            return std::nullopt;
        return value;
    }
    std::optional<VAddr> Ptr(VAddr address) const {
        const auto p = Read<u64>(address);
        if (!p || !Sane(*p))
            return std::nullopt;
        return *p;
    }
    std::optional<VAddr> Deref(std::optional<VAddr> base, VAddr offset) const {
        if (!base)
            return std::nullopt;
        return Ptr(*base + offset);
    }
    bool Resolve();
    std::optional<VAddr> DecodeAccessor(const Accessor& a) const;
    std::optional<VAddr> Root(VAddr global) const {
        return Ptr(host.main_base + global);
    }

    std::optional<VAddr> GameDataEntry(VAddr table, u32 stride, u32 key) const;
    std::optional<u32> GameDataArray(VAddr table, u32 key, u32 index) const;
    std::optional<u32> GameDataInt(u32 key) const;
    std::optional<SavedCourse> SavedCourseState(int world, int course) const;
    std::optional<u32> GameDataRef(u32 key) const;
    std::optional<bool> PausedFlag() const;
    std::optional<std::pair<u32, u32>> SceneState() const;
    bool MissFlag() const;
    bool OverlayPresent() const;
    std::optional<int> CourseId() const;
    bool CourseCoins(int& lives, int& gold, int& purple) const;
    std::optional<int> Character() const;
    std::optional<int> PowerUp() const;
    std::optional<int> PropertyValue(VAddr object, u32 key) const;
    std::optional<int> ReserveItem() const;
    std::optional<std::pair<float, float>> Position() const;
    // The current area's stage name ("Course852_Main") from the game's own area list.
    std::optional<std::string> AreaName() const;
    std::optional<int> FlowerCoinMask() const;
    std::optional<WonderState::RunSeeds> RunSeeds() const;
    bool WorldMap(int& selected_course_key);
    std::optional<VAddr> CourseProgress() const;

    void I64(const char* name, std::int64_t value) {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, value);
        frame_i64[name] = value;
    }
    void F64(const char* name, double value) {
        if (host.publish_f64)
            host.publish_f64(host.userdata, name, value);
        frame_f64[name] = value;
    }
    // Loading screens (badge select, course intro, area pipes) publish almost nothing; replay the
    // last good game frame so the companion holds its page instead of flashing empty values.
    std::map<std::string, std::int64_t> frame_i64, held_i64;
    std::map<std::string, double> frame_f64, held_f64;
    std::map<std::string, std::string> frame_text, held_text;
    void Log(const char* text) const {
        if (host.log)
            host.log(host.userdata, EDEN_DSMOD_LOG_INFO, text);
    }

    EdenDsmodHostApi host{};
    int last_status{-1};
    int last_page{0};
    bool resolved{};
    bool resolve_failed{};
    VAddr scene_root{}, player_root{}, camera_root{}, game_data{}, overlay_root{}, flags_root{},
        course_table{};
};

std::optional<VAddr> Reader::DecodeAccessor(const Accessor& a) const {
    const auto adrp = Read<u32>(host.main_base + a.site);
    const auto ldr = Read<u32>(host.main_base + a.site + 4);
    if (!adrp || !ldr)
        return std::nullopt;
    const u32 rd = *adrp & 31;
    if ((*adrp & 0x9f000000) != 0x90000000 || (*ldr & 0xffc00000) != 0xf9400000 ||
        ((*ldr >> 5) & 31) != rd || (*ldr & 31) != rd)
        return std::nullopt;
    s64 imm = static_cast<s64>(((*adrp >> 5) & 0x7ffff) << 2 | ((*adrp >> 29) & 3));
    if (imm & (s64{1} << 20))
        imm -= s64{1} << 21;
    const s64 page = static_cast<s64>(a.site & ~VAddr{0xfff}) + imm * 0x1000;
    const s64 global = page + static_cast<s64>(((*ldr >> 10) & 0xfff) * 8);
    if (global <= 0 || static_cast<VAddr>(global) != a.expected ||
        !dsmod_sdk::InMain(host.main_base, host.main_size, host.main_base + a.expected, 8))
        return std::nullopt;
    return static_cast<VAddr>(global);
}

bool Reader::Resolve() {
    if (resolved)
        return true;
    if (resolve_failed)
        return false;
    for (const auto& [offset, word] : CodeSignatures) {
        const auto w = Read<u32>(host.main_base + offset);
        if (!w || *w != word) {
            resolve_failed = true;
            Log("Wonder DSMod: code signature mismatch; companion disabled");
            return false;
        }
    }
    const auto s = DecodeAccessor(SceneRootAccessor);
    const auto p = DecodeAccessor(PlayerRootAccessor);
    const auto c = DecodeAccessor(CameraRootAccessor);
    const auto g = DecodeAccessor(GameDataAccessor);
    const auto o = DecodeAccessor(OverlayRootAccessor);
    const auto f = DecodeAccessor(FlagsRootAccessor);
    const auto t = DecodeAccessor(CourseTableAccessor);
    if (!s || !p || !c || !g || !o || !f || !t) {
        resolve_failed = true;
        Log("Wonder DSMod: accessor decode failed; companion disabled");
        return false;
    }
    scene_root = *s;
    player_root = *p;
    camera_root = *c;
    game_data = *g;
    overlay_root = *o;
    flags_root = *f;
    course_table = *t;
    resolved = true;
    Log("Wonder DSMod: 1.2.1 globals resolved from accessor code");
    return true;
}

// Entry address for `key` in the GameData hash table at G+table, mirroring the game's own getters
// (linear probing, an empty key ends the chain).
std::optional<VAddr> Reader::GameDataEntry(VAddr table, u32 stride, u32 key) const {
    const auto g = Root(game_data);
    if (!g)
        return std::nullopt;
    const VAddr t = *g + table;
    const auto count = Read<u32>(t);
    const auto entries = Ptr(t + 0x8);
    const auto keys = Ptr(t + 0x10);
    const auto cap = Read<u32>(t + 0x1c);
    if (!count || !entries || !keys || !cap || *cap == 0 || *cap > 0x10000 || *count == 0 ||
        *count > 0x10000)
        return std::nullopt;
    const u32 probes = std::min<u32>(*cap, 256);
    for (u32 i = 0; i < probes; ++i) {
        const u32 slot = static_cast<u32>((u64{key % *cap} + i) % *cap);
        const auto k = Read<u32>(*keys + u64{slot} * 8);
        if (!k || *k == 0)
            return std::nullopt;
        if (*k == key) {
            const auto idx = Read<u32>(*keys + u64{slot} * 8 + 4);
            if (!idx || *idx >= *count)
                return std::nullopt;
            return *entries + u64{*idx} * stride;
        }
    }
    return std::nullopt;
}

// Element `index` of a u32-array entry (0x40-byte entries: count +0x20, data [+0x28]).
std::optional<u32> Reader::GameDataArray(VAddr table, u32 key, u32 index) const {
    const auto e = GameDataEntry(table, 0x40, key);
    if (!e)
        return std::nullopt;
    const auto len = Read<u32>(*e + 0x20);
    const auto vals = Ptr(*e + 0x28);
    if (!len || !vals || *len > 0x1000 || index >= *len)
        return std::nullopt;
    return Read<u32>(*vals + u64{index} * 4);
}

std::optional<u32> Reader::GameDataInt(u32 key) const {
    const auto e = GameDataEntry(GameDataInts, 0x28, key);
    return e ? Read<u32>(*e + 0x1c) : std::nullopt;
}

// The array holds 4 values in 1.0.0 and 12 in 1.2.1 (lives, verified live); element 0 is the value
// the HUD shows.
std::optional<u32> Reader::GameDataRef(u32 key) const {
    return GameDataArray(GameDataRefs, key, 0);
}

std::optional<SavedCourse> Reader::SavedCourseState(int world, int course) const {
    if (world < 1 || world > 9)
        return std::nullopt;
    // The save slot is the course's key on its world map (array index = Key, verified: W1's
    // non-zero slots are exactly its keys 1..13, 20..22, 30, 40, 50..52, 60, 79), taken from the
    // catalog's world tables. (The GameData "current course" string is not reliable: it can still
    // name the previously selected course after travelling through the Courses menu.)
    const auto cat = Catalog();
    if (!cat)
        return std::nullopt;
    int slot = -1;
    for (auto it = cat->world_course.lower_bound({world, 0});
         it != cat->world_course.end() && it->first.first == world; ++it)
        if (it->second == course && it->first.second >= 0 && it->first.second <= 80) {
            slot = it->first.second;
            break;
        }
    if (slot < 0)
        return std::nullopt;
    const VAddr keys = host.main_base + WorldKeyTable + u64(world) * WorldKeyStride;
    const auto value = [&](int field) -> std::optional<u32> {
        const auto key = Read<u32>(keys + u64(field) * 4);
        if (!key || *key == 0)
            return std::nullopt;
        return GameDataArray(GameDataIntArrays, *key, static_cast<u32>(slot));
    };
    const auto seeds = value(SavedGoalSeeds), wonder = value(SavedWonderSeed);
    if (!seeds || !wonder)
        return std::nullopt;
    return SavedCourse{*seeds, *wonder};
}

std::optional<bool> Reader::PausedFlag() const {
    const auto a = Root(flags_root);
    const auto b = Deref(a, 0x38);
    if (!b)
        return std::nullopt;
    const auto keys = Ptr(*b);
    const auto bit_index = Ptr(*b + 0x8);
    const auto n = Read<u32>(*b + 0x14);
    const auto bits = Read<u64>(*a + 0x30);
    if (!keys || !bit_index || !n || !bits || *n == 0 || *n > 0x400)
        return std::nullopt;
    for (u32 i = 0; i < *n; ++i) {
        const u32 slot = (KeyPaused % *n + i) % *n;
        const auto k = Read<u32>(*keys + u64{slot} * 4);
        if (!k || *k == 0)
            return false;
        if (*k == KeyPaused) {
            const auto b_idx = Read<u8>(*bit_index + slot);
            if (!b_idx || *b_idx > 63 || Root(flags_root) != a)
                return std::nullopt;
            return ((*bits >> *b_idx) & 1) != 0;
        }
    }
    return false;
}

std::optional<std::pair<u32, u32>> Reader::SceneState() const {
    const auto q = Deref(Root(scene_root), 0x30);
    if (!q)
        return std::nullopt;
    const auto s1 = Read<u32>(*q + 0x2a9c);
    const auto s2 = Read<u32>(*q + 0x2a70);
    if (!s1 || !s2 || *s1 > 8 || *s2 > 16 || Deref(Root(scene_root), 0x30) != q)
        return std::nullopt;
    return std::pair{*s1, *s2};
}

bool Reader::MissFlag() const {
    const auto q = Deref(Root(player_root), 0x40);
    const auto v = q ? Read<u8>(*q + 0x530) : std::nullopt;
    return v && *v == 1;
}

bool Reader::OverlayPresent() const {
    return Deref(Root(overlay_root), 0x58).has_value();
}

std::optional<int> Reader::CourseId() const {
    const auto q = Deref(Root(scene_root), 0x30);
    const auto c = Deref(q, 0x2aa8);
    if (!c)
        return std::nullopt;
    const auto flags = Read<u8>(*c + 0x10c);
    const auto id = Read<u32>(*c + 0xec);
    if (!flags || !(*flags & 1) || !id || *id < 1 || *id > 999 || Deref(q, 0x2aa8) != c)
        return std::nullopt;
    return static_cast<int>(*id);
}

bool Reader::CourseCoins(int& lives, int& gold, int& purple) const {
    const auto p3 = Deref(Deref(Deref(Root(scene_root), 0x30), 0x8), 0x30);
    const auto l = Deref(p3, 0x168);
    const auto p5 = Deref(p3, 0x188);
    if (!l || !p5)
        return false;
    const auto lv = Read<u8>(*l + 0x20);
    const auto gv = Read<u8>(*p5 + 0x338);
    const auto pv = Read<u16>(*p5 + 0x33c);
    if (!lv || !gv || !pv || *lv > 99 || *gv > 99 || *pv > 999)
        return false;
    lives = *lv;
    gold = *gv;
    purple = *pv;
    return true;
}

std::optional<int> Reader::Character() const {
    const auto q = Deref(Root(player_root), 0x40);
    if (!q)
        return std::nullopt;
    for (const VAddr off : {VAddr{0x1c0}, VAddr{0x100}}) {
        const auto pl = Ptr(*q + off + 8);
        const auto idx = Read<s32>(*q + off + 0x10);
        if (!pl || !idx || *idx < 0)
            continue;
        const auto check = Read<u32>(*pl + 0x48);
        if (!check || *check != static_cast<u32>(*idx))
            continue;
        const auto a = Ptr(*pl + 0x40);
        if (!a)
            continue;
        const auto size = Read<u32>(*a + 0x200);
        if (!size || *size < 0x1a || *size > 0x1000)
            continue;
        const auto d = Deref(Deref(a, 0x208), 0xc8);
        const auto raw = d ? Read<u32>(*d + 0xa8) : std::nullopt;
        if (!raw || *raw >= CharacterMap.size())
            continue;
        return CharacterMap[*raw];
    }
    return std::nullopt;
}

std::optional<int> Reader::PropertyValue(VAddr object, u32 key) const {
    const auto keys = Ptr(object + 0xd8);
    const auto cap = Read<u32>(object + 0xe4);
    if (!keys || !cap || *cap == 0 || *cap > 0x4000)
        return std::nullopt;
    for (u32 i = 0; i < *cap; ++i) {
        const u32 slot = (key % *cap + i) % *cap;
        const auto k = Read<u32>(*keys + u64{slot} * 8);
        if (!k || *k == 0)
            return std::nullopt;
        if (*k != key)
            continue;
        const auto idx = Read<s32>(*keys + u64{slot} * 8 + 4);
        const auto count = Read<u32>(object + 0xa8);
        const auto values = Ptr(object + 0xb0);
        if (!idx || *idx < 0 || !count || *count > 0x4000 || static_cast<u32>(*idx) >= *count ||
            !values)
            return std::nullopt;
        const auto v = Read<u32>(*values + u64(*idx) * 4);
        if (!v || !(*v == 0 || *v == 1 || *v == 2 || *v == 3 || *v == 6 || *v == 9))
            return std::nullopt;
        return static_cast<int>(*v);
    }
    return std::nullopt;
}

std::optional<int> Reader::PowerUp() const {
    const auto q = Deref(Root(player_root), 0x40);
    if (!q)
        return std::nullopt;
    for (u32 i = 0; i < 4; ++i) {
        const auto active = Read<u32>(*q + 0x570 + 4 * i);
        if (!active || *active == 0)
            continue;
        const VAddr h = *q + 0x108 + 0x18 * i;
        const auto pl = Ptr(h);
        const auto idx = Read<s32>(h + 8);
        if (!pl || !idx || *idx < 0)
            continue;
        const auto check = Read<u32>(*pl + 0x48);
        if (!check || *check != static_cast<u32>(*idx))
            continue;
        const auto a = Ptr(*pl + 0x40);
        const auto d = Deref(Deref(Deref(a, 0x208), 0x0), 0x10);
        if (!d)
            continue;
        const auto v = PropertyValue(*d, KeyPowerUp);
        if (Read<s32>(h + 8) != idx || Ptr(*pl + 0x40) != a)
            return std::nullopt;
        return v;
    }
    return std::nullopt;
}

std::optional<VAddr> Reader::CourseProgress() const {
    return Deref(Deref(Deref(Deref(Deref(Root(scene_root), 0x30), 0x8), 0x8), 0x0), 0x118);
}

std::optional<int> Reader::ReserveItem() const {
    const auto r = Deref(CourseProgress(), 0x70);
    if (!r)
        return std::nullopt;
    const auto buf = Ptr(*r + 0x3d8);
    const auto cap = Read<u32>(*r + 0x3e0);
    const auto head = Read<u32>(*r + 0x3e4);
    const auto cnt = Read<u32>(*r + 0x3e8);
    if (!buf || !cap || !head || !cnt || *cap == 0 || *cap > 64 || *head >= *cap ||
        *cnt > *cap || *cnt > 4)
        return std::nullopt;
    s32 raw = 20;
    if (*cnt) {
        const auto v = Read<s32>(*buf + u64{*head} * 4);
        if (!v)
            return std::nullopt;
        raw = *v;
    }
    if (Read<u32>(*r + 0x3e4) != head || Read<u32>(*r + 0x3e8) != cnt)
        return std::nullopt;
    switch (raw) {
    case 0:
    case 20:
        return -2;
    case 1:
        return 1;
    case 2:
        return 2;
    case 5:
        return 3;
    case 12:
        return 6;
    case 18:
        return 9;
    default:
        return std::nullopt;
    }
}

std::optional<std::pair<float, float>> Reader::Position() const {
    const auto q = Deref(Root(camera_root), 0x0);
    if (!q)
        return std::nullopt;
    const auto f = Read<std::array<float, 6>>(*q + 0x10);
    if (!f || !std::all_of(f->begin(), f->end(),
                           [](float v) { return std::isfinite(v) && std::fabs(v) < 100000.0f; }))
        return std::nullopt;
    const auto& v = *f;
    if (std::fabs(v[0] - v[3]) > 64.0f || std::fabs(v[1] - v[4]) > 64.0f)
        return std::nullopt;
    return std::pair{v[0], v[1]};
}

// PLAYER_ROOT->+0x50 is the loaded stage list: entry 0 is the whole-course stage
// ("CourseNNN_Course"), entries 1.. are the areas in RefStages order; each entry holds a pointer
// to its C-string name ("Course852_Main.mumap") at +0x0. The camera's selected slot names the
// entry the player is in (verified live on 1.2.1: Main = 1 in courses 1, 852 and 900).
std::optional<std::string> Reader::AreaName() const {
    const auto q = Deref(Root(camera_root), 0x0);
    if (!q)
        return std::nullopt;
    const auto sel = Read<u32>(*q + 0x50);
    if (!sel)
        return std::nullopt;
    auto a = Read<s8>(*q + (*sel < 2 ? *sel * 16 : 0) + 0x3c);
    if (a && *a == -1)
        a = Read<s8>(*q + 0x120);
    if (!a || *a < 1 || Read<u32>(*q + 0x50) != sel)
        return std::nullopt;
    const auto s = Deref(Root(player_root), 0x50);
    const auto n = s ? Read<u32>(*s) : std::nullopt;
    if (!n || *n > 0x80 || static_cast<u32>(*a) >= *n)
        return std::nullopt;
    const auto entry = Ptr(Ptr(*s + 0x8).value_or(0) + u64(*a) * 8);
    const auto text = entry ? Ptr(*entry) : std::nullopt;
    if (!text)
        return std::nullopt;
    std::array<char, 64> buf{};
    if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *text, buf.data(),
                                                                   buf.size() - 1))
        return std::nullopt;
    std::string name{buf.data()};
    if (const auto dot = name.find('.'); dot != std::string::npos)
        name.resize(dot);
    if (name.empty() || name.size() > 48 ||
        !std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }))
        return std::nullopt;
    return name;
}

std::optional<int> Reader::FlowerCoinMask() const {
    const auto o = Deref(CourseProgress(), 0x38);
    if (!o)
        return std::nullopt;
    const auto c = Read<std::array<u8, 3>>(*o + 0x3ca);
    if (!c || (*c)[0] > 1 || (*c)[1] > 1 || (*c)[2] > 1)
        return std::nullopt;
    return (*c)[0] | (*c)[1] << 1 | (*c)[2] << 2;
}

std::optional<WonderState::RunSeeds> Reader::RunSeeds() const {
    const auto o = Deref(CourseProgress(), 0x48);
    if (!o)
        return std::nullopt;
    std::array<u8, 60> bytes{};
    if (!dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, *o + 0x5b8, bytes.data(),
                                                                   bytes.size()))
        return std::nullopt;
    return WonderState::DecodeRunSeeds(bytes);
}

bool Reader::WorldMap(int& selected_course_key) {
    selected_course_key = -1;
    const auto p1 = Deref(Root(scene_root), 0x30);
    const auto p2 = Deref(p1, 0x8);
    if (!p2)
        return false;
    const auto n = Read<u32>(*p2);
    if (!n || *n == 0 || *n > 0x10000)
        return false;
    const auto arr = Ptr(*p2 + 0x8);
    const auto p3 = arr ? Ptr(*arr + (*n > 1 ? 8 : 0)) : std::nullopt;
    if (!p3)
        return false;
    // The selected course is best-effort: the map is detected even when it cannot be read.
    const auto p5 = Deref(Deref(p3, 0x118), 0x38);
    if (!p5)
        return true;
    const auto key = Read<u64>(*p5 + 0x760);
    const auto cnt = Read<u32>(*p5 + 0x38);
    const auto list = Ptr(*p5 + 0x40);
    if (!key || *key == 0 || !cnt || *cnt > 0x400 || !list)
        return true;
    u32 lo = 0, hi = *cnt;
    std::optional<VAddr> hit;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        const auto e = Ptr(*list + u64{mid} * 8);
        const auto ek = e ? Read<u64>(*e) : std::nullopt;
        if (!ek)
            return true;
        if (*ek == *key) {
            hit = e;
            break;
        }
        if (*ek < *key)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!hit)
        return true;
    const auto k = Read<s32>(*hit + 0x48);
    const auto ready = Read<u8>(host.main_base + course_table - CourseTableReadyDelta);
    // In 1.2.1 the table pointer targets the main image's own .bss (0x83B7xxxx under Dynarmic),
    // below the heap range Sane() accepts, so accept either range here.
    std::optional<VAddr> table;
    if (const auto t = Read<u64>(host.main_base + course_table);
        t && (Sane(*t) || dsmod_sdk::InMain(host.main_base, host.main_size, *t, 0x51 * 4)))
        table = *t;
    if (!k || *k < 0 || *k > 0x50 || !ready || !(*ready & 1) || !table)
        return true;
    const auto v = Read<s32>(*table + u64(*k) * 4);
    if (v && *v >= 1 && *v <= 0x50 && Read<u64>(*p5 + 0x760) == key)
        selected_course_key = *v;
    return true;
}

// Page selector for the manifest's page_binds: 0 setup/title, 1 course, 2 world map.
constexpr int PageSetup = 0, PageCourse = 1, PageMap = 2;

void Reader::Sample() {
    if (!supported) {
        I64("wonder.status", StatusUnsupported);
        I64("wonder.page", PageSetup);
        I64("wonder.wrong_build", 1);
        Text("wonder.game_version_line", "Your game is Ver. " + std::string{game_version} + ".");
        return;
    }
    frame_i64.clear();
    frame_f64.clear();
    frame_text.clear();
    SampleValues();
    if (last_status == StatusCourse || last_status == StatusWorldMap ||
        last_status == StatusTransition) {
        // This frame becomes the held one (the old held maps are cleared next tick).
        held_i64.swap(frame_i64);
        held_f64.swap(frame_f64);
        held_text.swap(frame_text);
    } else if ((last_status == StatusLoading || last_status == StatusResolving ||
                last_status == StatusMiss) &&
               (last_page == PageCourse || last_page == PageMap)) {
        for (const auto& [k, v] : held_i64)
            if (k != "wonder.status" && !frame_i64.contains(k) && host.publish_i64)
                host.publish_i64(host.userdata, k.c_str(), v);
        for (const auto& [k, v] : held_f64)
            if (!frame_f64.contains(k) && host.publish_f64)
                host.publish_f64(host.userdata, k.c_str(), v);
        for (const auto& [k, v] : held_text)
            if (!frame_text.contains(k) && host.publish_text)
                host.publish_text(host.userdata, k.c_str(), v.c_str());
    }
    // Held pages: transitions (11) and a miss (10) keep the course page; resolving (4) and
    // loading (8) keep whichever game page was showing instead of flashing the setup page.
    int page = PageSetup;
    switch (last_status) {
    case StatusCourse:
    case StatusTransition:
    case StatusMiss:
        page = PageCourse;
        break;
    case StatusResolving:
    case StatusLoading:
        page = last_page == PageCourse || last_page == PageMap ? last_page : PageSetup;
        break;
    case StatusWorldMap:
        page = PageMap;
        break;
    default:
        page = PageSetup;
        break;
    }
    last_page = page;
    I64("wonder.page", page);
}

void Reader::SampleValues() {
    StartCatalog();
    if (!Resolve()) {
        Status(StatusUnsupported);
        return;
    }
    const auto scene = SceneState();
    int status = StatusLoading;
    // A miss only counts in normal play (s2 == 3): the flag is stale on the pre-course badge screen
    // (s2 == 5), which must read as loading, not as a death.
    const bool miss = scene && scene->second == 3 && scene->first != 5 && scene->first != 6 &&
                      MissFlag();
    if (!scene) {
        status = StatusLoading;
    } else if (miss) {
        status = StatusMiss;
    } else if (scene->second != 3 || scene->first == 6) {
        status = StatusLoading;
    } else if (scene->first == 5) {
        status = StatusTitle;
    } else {
        status = -1; // decided below
    }
    if (status != -1) {
        Status(status);
        return;
    }

    const auto paused = PausedFlag();
    I64("wonder.paused", paused && *paused ? 1 : 0);
    const auto world = GameDataInt(KeyWorld);
    const auto world_seeds = GameDataInt(KeyWorldSeeds);
    int w = world && *world >= 1 && *world <= 9 ? static_cast<int>(*world) : -1;
    I64("wonder.world_seeds", world_seeds && *world_seeds <= 999 ? s64{*world_seeds} : -1);

    int lives = -1, gold = -1, purple = -1;
    if (!CourseCoins(lives, gold, purple))
        lives = gold = purple = -1;
    const auto character = Character();
    I64("wonder.character", character ? *character : -1);

    int selected = -1;
    if (WorldMap(selected)) {
        const auto l = GameDataRef(KeyMapLives);
        const auto g = GameDataInt(KeyMapGold);
        const auto p = GameDataInt(KeyMapPurple);
        I64("wonder.world", w);
        I64("wonder.lives", l && *l <= 99 ? s64{*l} : -1);
        I64("wonder.gold", g && *g <= 99 ? s64{*g} : -1);
        I64("wonder.purple", p && *p <= 999 ? s64{*p} : -1);
        reached_key.clear();
        const auto cat = Catalog();
        const auto course =
            cat && w > 0 && selected > 0 ? cat->CourseAt(w, selected) : std::nullopt;
        const std::string* name = nullptr;
        if (course) {
            if (const auto it = cat->course_name.find(*course);
                it != cat->course_name.end() && !it->second.empty())
                name = &it->second;
        }
        I64("wonder.map.course", course ? *course : -1);
        I64("wonder.map.has_course", name ? 1 : 0);
        if (name)
            Text("wonder.map.course_name", *name);
        Status(StatusWorldMap);
        return;
    }

    const auto course = CourseId();
    const auto reserve = ReserveItem();
    const auto pos = Position();
    const auto power = PowerUp();
    I64("wonder.lives", lives);
    I64("wonder.gold", gold);
    I64("wonder.purple", purple);
    I64("wonder.reserve_item", reserve ? *reserve : -1);
    I64("wonder.power_up", power ? *power : -1);
    // Gates for icons: a stocked item (1..9) and a non-Small form (the Small form shows the
    // character icon instead).
    I64("wonder.has_item", reserve && *reserve > 0 ? 1 : 0);
    I64("wonder.item_empty", reserve && *reserve == -2 ? 1 : 0);
    I64("wonder.has_power", power && *power > 0 ? 1 : 0);
    if (!pos || !course) {
        I64("wonder.world", w);
        Status(StatusResolving);
        return;
    }
    if ((*course == 1 || *course == 2) && w < 0)
        w = 1;
    I64("wonder.world", w);
    I64("wonder.course", *course);
    // Named courses have the full shelf (flower coins, seeds, form, balloon); the unnamed
    // prologue/tutorial courses (900-series) get the plain pattern instead.
    bool named = false;
    if (const auto cat = Catalog()) {
        if (const auto it = cat->course_name.find(*course);
            it != cat->course_name.end() && !it->second.empty()) {
            Text("wonder.course_name", it->second);
            named = true;
        }
    }
    I64("wonder.shelf", named ? 1 : 0);
    I64("wonder.no_shelf", named ? 0 : 1);
    const auto flowers = FlowerCoinMask();
    for (int i = 0; i < 3; ++i) {
        static constexpr std::array<const char*, 3> names{"wonder.flower.0", "wonder.flower.1",
                                                           "wonder.flower.2"};
        I64(names[i], flowers && (*flowers >> i & 1) ? 1 : 0);
    }
    const auto area = AreaName();
    // One line, laid out by the text renderer: "COURSE 005 \u2022 MAIN AREA" / "... SUB-AREA 2".
    char course_line[64];
    std::snprintf(course_line, sizeof(course_line), "COURSE %03d", *course);
    std::string line{course_line};
    if (area) {
        const auto us = area->rfind('_');
        const std::string part = us == std::string::npos ? *area : area->substr(us + 1);
        if (part == "Main")
            line += " \u2022 MAIN AREA";
        else if (part.starts_with("Sub") && part.size() > 3)
            line += " \u2022 SUB-AREA " + std::to_string(std::atoi(part.c_str() + 3));
    }
    Text("wonder.course_line", line);
    run_seeds = RunSeeds();
    if (area) {
        if (const auto all = Routes()) {
            const std::string key = std::to_string(*course) + "/" + *area;
            if (key != reached_key) {
                reached_key = key;
                reached_x = pos->first;
            }
            reached_x = std::max(reached_x, pos->first);
            if (const auto r = all->find(*area); r != all->end())
                PublishRail(r->second, pos->first, pos->second, flowers ? *flowers : 0, w, *course);
        }
    }
    I64("wonder.run_seeds", run_seeds ? run_seeds->count : -1);
    Status(OverlayPresent() ? StatusTransition : StatusCourse);
}

// Whether a rail marker counts as obtained. 10-flower coins: the game's own per-course bytes
// (including coins from earlier runs, as the game shows them). Wonder seed: its saved bit or its live collection flag.
// Checkpoints trigger when Mario reaches them and stay for the rest of the visit, retries
// included: the furthest x reached in this area of this visit (reset on the world map / new area).
bool Reader::MarkerObtained(const WonderCatalog::RouteMarker& m, int flowers) const {
    using K = WonderCatalog::RouteMarker;
    switch (m.kind) {
    case K::BigFlowerCoin:
        return m.id >= 0 && m.id < 3 && (flowers >> m.id & 1);
    case K::WonderSeed:
        return WonderState::SeedObtained(saved ? saved->wonder_seed : 0, run_seeds, m.id);
    case K::Checkpoint:
        return reached_x >= m.x;
    default:
        return false;
    }
}

// Progress rail: positions are published in rail pixels (0..RailWidth) so the manifest can place
// widgets with x_bind directly. Markers are the area's own actors (10-flower coins, Wonder
// Flower, Wonder Seed, checkpoints), measured along the route path like the player.
constexpr int RailWidth = 1000;
constexpr int RailMarkers = 12;

void Reader::PublishRail(const WonderCatalog::Route& route, float x, float y, int flowers,
                         int world, int course) {
    // Per marker: x, on, k1..k4 (kind gates), then got/miss gates for flower coins (k1), the
    // Wonder seed (k3) and checkpoints (k4).
    static const auto names = [] {
        std::array<std::array<std::string, 12>, RailMarkers> n{};
        for (int i = 0; i < RailMarkers; ++i) {
            const std::string b = "wonder.rail.m." + std::to_string(i) + ".";
            n[i] = {b + "x",     b + "on",     b + "k1",    b + "k2",     b + "k3",    b + "k4",
                    b + "k1got", b + "k1miss", b + "k3got", b + "k3miss", b + "k4got", b + "k4miss"};
        }
        return n;
    }();
    const auto rail_px = [](float progress) {
        return static_cast<int>(std::lround(progress * RailWidth));
    };
    const float progress = route.Progress(x, y);
    const int player_px = rail_px(progress);
    I64("wonder.rail.valid", 1);
    I64("wonder.rail.goal_present", route.normal_goal_id >= 0 ? 1 : 0);
    I64("wonder.rail.next_area", route.normal_goal_id < 0 ? 1 : 0);
    I64("wonder.rail.px", player_px);
    I64("wonder.rail.pct", static_cast<s64>(std::floor(progress * 100.0f)));
    // Goal seeds and the Wonder-effect seed, from the save (persist across reruns). Each pole's
    // seed is the save bit of its own GoalID.
    saved = SavedCourseState(world, course);
    const auto seed_bit = [&](int id) {
        return saved && id >= 0 && id < 32 && (saved->goal_seeds >> id & 1) ? 1 : 0;
    };
    const int goal_px = rail_px(route.normal_progress);
    I64("wonder.rail.goal_got", seed_bit(route.normal_goal_id));
    I64("wonder.rail.goal_px", goal_px);
    I64("wonder.rail.secret_got", seed_bit(route.secret_goal_id));
    int player_dy = 0;
    if (route.secret_goal) {
        // Secret branch lane: leaves the rail BranchLead px before the nearer goal and runs up to
        // the secret pole (BranchSeg-px segments right of the 36-px riser, so the manifest needs
        // only a count).
        constexpr int BranchLead = 120, BranchSeg = 20, RiserHalf = 18, BranchDy = -100;
        const int secret_px = rail_px(route.secret_progress);
        const int branch_px = std::max(0, std::min(goal_px, secret_px) - BranchLead);
        I64("wonder.rail.secret", 1);
        I64("wonder.rail.secret_px", secret_px);
        I64("wonder.rail.branch_px", branch_px);
        I64("wonder.rail.branch_n", std::max(1, (secret_px - branch_px - RiserHalf) / BranchSeg));
        // Player on the secret branch: past the branch point and nearer the secret pole than the
        // main goal -> the manifest lifts the player icon onto the lane.
        const auto d2 = [&](const WonderCatalog::RoutePoint& q) {
            return (q.x - x) * (q.x - x) + (q.y - y) * (q.y - y);
        };
        if (player_px > branch_px && d2(*route.secret_goal) < d2(route.normal_goal))
            player_dy = BranchDy;
    }
    I64("wonder.rail.player_dy", player_dy);
    using K = WonderCatalog::RouteMarker;
    const int count = std::min<int>(RailMarkers, static_cast<int>(route.markers.size()));
    for (int i = 0; i < count; ++i) {
        const auto& m = route.markers[i];
        const auto& n = names[i];
        const bool got = MarkerObtained(m, flowers);
        I64(n[0].c_str(), rail_px(m.progress));
        I64(n[1].c_str(), 1);
        for (int k = 1; k <= 4; ++k)
            I64(n[1 + k].c_str(), m.kind == k ? 1 : 0);
        I64(n[6].c_str(), m.kind == K::BigFlowerCoin && got ? 1 : 0);
        I64(n[7].c_str(), m.kind == K::BigFlowerCoin && !got ? 1 : 0);
        I64(n[8].c_str(), m.kind == K::WonderSeed && got ? 1 : 0);
        I64(n[9].c_str(), m.kind == K::WonderSeed && !got ? 1 : 0);
        I64(n[10].c_str(), m.kind == K::Checkpoint && got ? 1 : 0);
        I64(n[11].c_str(), m.kind == K::Checkpoint && !got ? 1 : 0);
    }
}

EdenDsmodBool SupportsBuild(const char* build_id) {
    // 1.2.1 is read; older builds only get the wrong-pipe page (art and font, no memory reads).
    if (!build_id)
        return EDEN_DSMOD_FALSE;
    const std::string_view b{build_id};
    return b == BuildId || std::ranges::any_of(WrongPipeBuilds,
                                               [&](const auto& w) { return w.build_id == b; })
               ? EDEN_DSMOD_TRUE
               : EDEN_DSMOD_FALSE;
}
void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory)
            return nullptr;
        return new Reader{*host};
    } catch (...) {
        return nullptr;
    }
}
void Destroy(void* p) {
    try {
        delete static_cast<Reader*>(p);
    } catch (...) {
    }
}
void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (p && host) {
            auto& r = *static_cast<Reader*>(p);
            r.UpdateHost(*host);
            r.Sample();
        }
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "Wonder DSMod sample callback failed");
    }
}
void TickCallback(void*, const EdenDsmodHostApi*) {}
void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    try {
        if (p)
            static_cast<Reader*>(p)->Configure(ext);
    } catch (...) {
    }
}
EdenDsmodBool ActionCallback(void*, const char*, std::int64_t) {
    return EDEN_DSMOD_FALSE; // touch actions are plain manifest button presses
}
EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key,
                                void* receiver, EdenDsmodImageSink sink) {
    try {
        if (p)
            return static_cast<Reader*>(p)->LoadImage(host, key, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Super Mario Bros. Wonder DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH,    &ConfigureCallback,
    &ActionCallback,        &LoadImageCallback};
EdenDsmodBool DecodeFontCallback(void* p, const std::uint8_t* bytes, size_t size, void* receiver,
                                 EdenDsmodFontSink sink) {
    try {
        if (p)
            return static_cast<Reader*>(p)->DecodeFont(bytes, size, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,
                                             sizeof(EdenDsmodFontExtensions),
                                             EDEN_DSMOD_FONT_EXT_HASH, &DecodeFontCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
