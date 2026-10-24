// SPDX-License-Identifier: GPL-3.0-or-later
// Optional real-resource smoke tests. Native artwork is read from caller-supplied romfs only;
// no images are saved and no private paths are compiled into this target.
#include "smo_map.h"
#include "smo_romfs_tables.h"
#include "smo_test_romfs_host.h"
#include "smo_ui.h"
#include "smo_world.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
using Span = std::span<const uint8_t>;
Bytes Read(const std::string& path) {
    std::ifstream f(path, std::ios::binary); assert(f);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
Bytes Expanded(Span in) {
    if (in.size() < 4 || std::memcmp(in.data(), "Yaz0", 4)) return {in.begin(), in.end()};
    assert(in.size() >= 16);
    const size_t n = size_t(in[4]) << 24 | size_t(in[5]) << 16 | size_t(in[6]) << 8 | in[7];
    assert(n && n <= 64 * 1024 * 1024);
    Bytes out(n);
    size_t src = 16, dst = 0; unsigned mask = 0, code = 0;
    while (dst < n) {
        if (!mask) { assert(src < in.size()); code = in[src++]; mask = 128; }
        if (code & mask) { assert(src < in.size()); out[dst++] = in[src++]; }
        else {
            assert(src + 2 <= in.size()); const unsigned a = in[src++], b = in[src++];
            const size_t distance = ((a & 15) << 8) | b;
            assert(distance + 1 <= dst);
            size_t count = a >> 4;
            if (count) count += 2;
            else { assert(src < in.size()); count = size_t(in[src++]) + 18; }
            assert(count <= n - dst); size_t at = dst - distance - 1;
            while (count--) out[dst++] = out[at++];
        }
        mask >>= 1;
    }
    return out;
}
Span Member(Span sarc, std::string_view name) {
    std::optional<Span> found;
    for (const auto& f : fe3h_ui2d::Sarc(sarc)) {
        if (f.name != name) continue;
        assert(!found && f.offset <= sarc.size() && f.size <= sarc.size() - f.offset);
        found = sarc.subspan(f.offset, f.size);
    }
    assert(found); return *found;
}
/// Native-table joins are built from the player's own romfs (SmoRomfsTables::Build(), steward
/// ruling 2026-10-24), not read from shipped package TSVs. nullopt means SMO_BASE_ROMFS is unset
/// or the build failed; callers skip cleanly.
std::optional<SmoRomfsTables::Tables> BuildNativeTables() {
    const char* base = std::getenv("SMO_BASE_ROMFS");
    if (!base || !*base) return std::nullopt;
    SmoTestHost::FakeHost host(base);
    auto api = host.Api();
    auto built = SmoRomfsTables::Build(api);
    if (!built.system_data_ok) return std::nullopt;
    return built;
}
std::string Text(const std::string& path) {
    const auto bytes = Read(path); return {bytes.begin(), bytes.end()};
}

// ---- 20261023 map audit: bounds-check every checkpoint/icon/objective/moon marker through the
// REAL decoded map image + view_proj matrix (SmoMap::Project, production code, no re-derivation),
// for every kingdom map variant. Optional SMO_MAP_AUDIT_DIR additionally writes a downsampled
// contact-sheet PNG per variant with the actual decoded terrain under coloured marker dots, for
// human visual review (land/ocean/void, offset/rotation/flip). Minimal store-only PNG writer,
// ported from mk8d_assets_test.cpp's WritePng (same project, no shared header; kept self-contained
// so this test has no extra link dependency).
using Bytes2 = std::vector<std::uint8_t>;
std::uint32_t Crc32(const std::uint8_t* d, size_t n) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = ~0u;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
bool WritePng(const std::filesystem::path& path, std::uint32_t w, std::uint32_t h, const Bytes2& rgba) {
    Bytes2 raw; raw.reserve((size_t(w) * 4 + 1) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + std::ptrdiff_t(y) * w * 4,
                   rgba.begin() + std::ptrdiff_t(y + 1) * w * 4);
    }
    Bytes2 z{0x78, 0x01};
    for (size_t i = 0; i < raw.size(); i += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back(n & 0xff); z.push_back(n >> 8);
        z.push_back(~n & 0xff); z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(i), raw.begin() + std::ptrdiff_t(i + n));
    }
    std::uint32_t a = 1, b = 0;
    for (const auto v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    const std::uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; --k) z.push_back(std::uint8_t(adler >> (8 * k)));
    Bytes2 out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto chunk = [&out](const char* type, const Bytes2& data) {
        for (int k = 3; k >= 0; --k) out.push_back(std::uint8_t(data.size() >> (8 * k)));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        const auto c = Crc32(out.data() + start, out.size() - start);
        for (int k = 3; k >= 0; --k) out.push_back(std::uint8_t(c >> (8 * k)));
    };
    Bytes2 ihdr(13);
    for (int k = 0; k < 4; ++k) { ihdr[k] = std::uint8_t(w >> (8 * (3 - k))); ihdr[4 + k] = std::uint8_t(h >> (8 * (3 - k))); }
    ihdr[8] = 8; ihdr[9] = 6;
    chunk("IHDR", ihdr); chunk("IDAT", z); chunk("IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}
struct MarkerPt { double x, y, z; std::string kind, label, match_id; };
void PutDot(Bytes2& canvas, std::uint32_t cw, std::uint32_t ch, double fx, double fy,
            std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const auto cx = long(std::lround(fx)), cy = long(std::lround(fy));
    for (long dy = -3; dy <= 3; ++dy) for (long dx = -3; dx <= 3; ++dx) {
        const long x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= long(cw) || y >= long(ch)) continue;
        const auto p = (size_t(y) * cw + size_t(x)) * 4;
        canvas[p] = r; canvas[p + 1] = g; canvas[p + 2] = b; canvas[p + 3] = 255;
    }
}
void MarkerAudit(const SmoWorld::Catalog& c,
                  const std::unordered_map<std::string, SmoMap::Resources>& maps,
                  const std::unordered_map<std::string, std::set<std::string>>& world_variants) {
    const char* contact_env = std::getenv("SMO_MAP_AUDIT_DIR");
    const std::filesystem::path contact = contact_env ? contact_env : "";
    if (!contact.empty()) std::filesystem::create_directories(contact);
    // Known, already-isolated finding (20261023-map-audit/AUDIT.md): Lava Kingdom ShineList
    // unique_id 257 ("Climb Up the Cascading Magma", also its scenario-0 objective) and 259 both
    // carry a native Trans position (~-3550, image-space) far outside LavaWorldHomeStage's own
    // 2048x2048 map texture, in all 3 Lava variants -- consistently with each other, so it reads
    // as a genuine distant/vertical sub-area rather than extraction noise. The runtime module
    // already drops any moon marker whose projection misses the map's own bounds (so it is never
    // drawn), which this test re-verifies below for the *catalog* objective/moon points. Any
    // OTHER out-of-bounds marker is a real regression and fails this test.
    const auto known_exception = [](const std::string& world, const std::string& id) {
        return world == "Lava" && (id == "257" || id == "259");
    };
    unsigned total = 0, oob = 0, unprojectable = 0, documented_oob = 0;
    std::printf("AUDIT_HEADER\tworld\ttexture\tkind\tid\tlabel\tpx\tpy\tin_bounds\n");
    for (const auto& world : c.worlds) {
        const auto wv = world_variants.find(world.key);
        if (wv == world_variants.end()) continue;
        std::vector<MarkerPt> pts;
        std::set<std::pair<std::string, std::string>> seen_cp;
        for (uint32_t scenario = 1; scenario <= world.scenario_count; ++scenario) {
            for (const auto& p : SmoWorld::Checkpoints(c, world.key, scenario))
                if (seen_cp.emplace(p.stage, p.id).second)
                    pts.push_back({p.position[0], p.position[1], p.position[2], "checkpoint", p.name, p.id});
            for (const auto& ic : SmoWorld::Icons(c, world.key, scenario))
                pts.push_back({ic.position[0], ic.position[1], ic.position[2], ic.kind, ic.id, ic.id});
        }
        for (const auto& o : c.objectives)
            if (o.world == world.key)
                pts.push_back({o.position[0], o.position[1], o.position[2], "objective", o.name,
                              std::to_string(o.unique_id)});
        for (const auto& m : SmoWorld::Moons(c, world.key))
            pts.push_back({m.position[0], m.position[1], m.position[2],
                          m.is_moon_rock ? "moon_rock" : "moon", std::to_string(m.unique_id),
                          std::to_string(m.unique_id)});
        for (const auto& texture : wv->second) {
            const auto mit = maps.find(texture);
            if (mit == maps.end()) continue;
            const auto& map = mit->second;
            constexpr std::uint32_t cw = 640, ch = 640;
            Bytes2 canvas;
            if (!contact.empty()) {
                canvas.assign(size_t(cw) * ch * 4, 0);
                for (std::uint32_t y = 0; y < ch; ++y) for (std::uint32_t x = 0; x < cw; ++x) {
                    const auto sx = x * map.image.width / cw, sy = y * map.image.height / ch;
                    const auto sp = (size_t(sy) * map.image.width + sx) * 4, dp = (size_t(y) * cw + x) * 4;
                    std::memcpy(&canvas[dp], &map.image.rgba[sp], 4);
                }
            }
            for (const auto& pt : pts) {
                ++total;
                const auto proj = SmoMap::Project(map, {float(pt.x), float(pt.y), float(pt.z)});
                const bool in_bounds = proj && proj->x >= 0 && proj->x <= map.image.width &&
                                       proj->y >= -double(map.image.height) && proj->y <= 0;
                if (!proj) ++unprojectable;
                else if (!in_bounds) { ++oob; if (known_exception(world.key, pt.match_id)) ++documented_oob; }
                std::printf("AUDIT\t%s\t%s\t%s\t%s\t%s\t%.1f\t%.1f\t%d\n", world.key.c_str(), texture.c_str(),
                            pt.kind.c_str(), pt.match_id.c_str(),
                            pt.label.c_str(), proj ? proj->x : -1, proj ? proj->y : -1, proj && in_bounds);
                if (!canvas.empty() && proj) {
                    const double cx = proj->x * cw / map.image.width, cy = (-proj->y) * ch / map.image.height;
                    const auto [r, g, bcol] = pt.kind == "checkpoint" ? std::tuple{255, 40, 40}
                        : pt.kind == "MapIconShop" ? std::tuple{40, 120, 255}
                        : pt.kind == "MapIconMiniGame" ? std::tuple{40, 220, 90}
                        : pt.kind == "MapIconPoetter" ? std::tuple{200, 60, 220}
                        : pt.kind == "objective" ? std::tuple{255, 255, 255}
                        : pt.kind == "moon_rock" ? std::tuple{255, 120, 0}
                        : std::tuple{255, 215, 0}; // moon
                    PutDot(canvas, cw, ch, cx, cy, std::uint8_t(r), std::uint8_t(g), std::uint8_t(bcol));
                }
            }
            if (!canvas.empty()) {
                const auto out = contact / (world.key + "_" + texture + ".png");
                assert(WritePng(out, cw, ch, canvas));
            }
        }
    }
    std::printf("AUDIT_SUMMARY\ttotal=%u\toob=%u\tunprojectable=%u\tdocumented_oob=%u\n",
                total, oob, unprojectable, documented_oob);
    std::fflush(stdout);
    // Every marker is a real, already-placed native world object (shops/checkpoints verified
    // live since 637dd27; moons are home-stage ShineList entries). A sound local affine map
    // projection must place every one of them inside its own kingdom's image bounds, except the
    // two documented Lava exceptions above (which the runtime module also excludes by the same
    // bounds check, so they are never drawn -- this is not a free pass, it is the audit catching
    // exactly what the runtime guard is for). Any UNDOCUMENTED out-of-bounds marker fails.
    assert(unprojectable == 0);
    assert(oob == documented_oob);
    assert(documented_oob == 9); // {objective, moon 257, moon 259} x 3 Lava map variants = 9
}

void Maps() {
    const char* path = std::getenv("SMO_MAP_ARCHIVE");
    if (!path || !*path) {
        std::puts("smo_world_assets: real maps skipped (SMO_MAP_ARCHIVE unset)"); return;
    }
    const auto built = BuildNativeTables();
    if (!built) {
        std::puts("smo_world_assets: SMO_BASE_ROMFS not set (or build failed), skipping real maps test");
        return;
    }
    const SmoWorld::Tables world_tables{built->world_maps, built->world_stages, built->world_map_invalid,
                                        built->world_map_anchors, built->world_checkpoints, built->world_map_icons,
                                        built->world_objectives, built->world_map_variants, built->world_map_moons};
    const auto catalog = SmoWorld::ParseTables(world_tables);
    assert(catalog && catalog->worlds.size() == 17 && catalog->variants.size() == 22);
    const auto original = Read(path), archive = Expanded(original);
    std::set<std::string> decoded;
    std::unordered_map<std::string, SmoMap::Resources> maps;
    for (const auto& variant : catalog->variants) {
        const auto native_byml = Member(archive, variant.projection_member);
        assert(variant.projection_member == variant.texture + ".byml");
        const auto independent = SmoMap::DecodeProjection(native_byml, variant.width, variant.height);
        assert(independent);
        const auto map = SmoMap::DecodeArchive(archive, variant.texture); assert(map);
        assert(map->image.name == variant.texture);
        assert(map->image.width == variant.width && map->image.height == variant.height);
        assert(map->image.width == 2048 && map->image.height == 2048);
        assert(map->image.rgba.size() == size_t(variant.width) * variant.height * 4);
        assert(map->view_proj == independent->view_proj);
        for (const auto v : map->view_proj) assert(std::isfinite(v));
        const auto& m = map->view_proj;
        assert(std::abs(m[0] * m[6] - m[2] * m[4]) > 1e-15);
        assert(SmoMap::Project(*map, {0, 0, 0}));
        // All actual maps have terrain/grid variation; a decoded all-solid placeholder fails.
        bool variation = false;
        for (size_t p = 4; p < map->image.rgba.size(); p += 4)
            if (std::memcmp(map->image.rgba.data(), map->image.rgba.data() + p, 4)) { variation = true; break; }
        assert(variation);
        assert(decoded.insert(variant.texture).second);
        maps.emplace(variant.texture, std::move(*map));
        std::printf("smo_world_assets: decoded %s, native projection matched\n", variant.texture.c_str());
    }
    size_t scenarios = 0;
    std::unordered_map<std::string, std::set<std::string>> world_variants;
    for (const auto& world : catalog->worlds) {
        assert(decoded.contains(world.home_stage));
        for (uint32_t scenario = 1; scenario <= world.scenario_count; ++scenario) {
            ++scenarios;
            const auto* selected = SmoWorld::MapVariant(*catalog, world.key, scenario);
            assert(selected && decoded.contains(selected->texture));
            world_variants[world.key].insert(selected->texture);
        }
    }
    assert(scenarios == 100);
    // Exercise the production raw-Yaz0 path once as well; the all-variant loop uses the
    // expanded SARC to avoid decompressing the same large archive 22 times.
    const auto packed = SmoMap::DecodeArchive(original, "SandWorldHomeStage"); assert(packed);
    const auto independent = SmoMap::DecodeProjection(Member(archive, "SandWorldHomeStage.byml"), 2048, 2048);
    assert(independent && packed->view_proj == independent->view_proj);
    std::puts("smo_world_assets: all 22 native map images, 17 worlds and 100 scenario selections passed");
    MarkerAudit(*catalog, maps, world_variants);
}
Bytes LayoutBntx(const std::string& file) {
    const auto archive = Expanded(Read(file));
    const auto layout = Expanded(Member(archive, "layout.lyarc"));
    const auto bntx = Member(layout, "timg/__Combined.bntx");
    return {bntx.begin(), bntx.end()};
}
void Ui() {
    const char* root = std::getenv("SMO_BASE_ROMFS");
    if (!root || !*root) { std::puts("smo_world_assets: real UI skipped (SMO_BASE_ROMFS unset)"); return; }
    const auto map_line = LayoutBntx(std::string(root) + "/LayoutData/MapLine.szs");
    const auto dot = SmoUi::ObjectiveDot(map_line);
    assert(dot && dot->width == 8 && dot->height == 8 && !SmoUi::ObjectiveDot({}));
    const auto right = SmoUi::ObjectivePointer(map_line, 0), down = SmoUi::ObjectivePointer(map_line, 90);
    assert(right && down && right->width == 48 && right->height == 48);
    assert(right->rgba != down->rgba);
    assert(!SmoUi::ObjectivePointer({}, 0));
    const auto life = LayoutBntx(std::string(root) + "/LayoutData/CounterLife.szs");
    const auto full = SmoUi::Life(life, 3, 3), low = SmoUi::Life(life, 1, 3), empty = SmoUi::Life(life, 0, 3),
               expanded = SmoUi::Life(life, 6, 6);
    assert(full && low && empty && expanded);
    for (const auto* image : {&*full, &*low, &*empty, &*expanded}) {
        assert(image->width == 128 && image->height == 128 && image->rgba.size() == 128 * 128 * 4);
        bool visible = false, transparent = false;
        for (size_t p = 3; p < image->rgba.size(); p += 4) {
            visible |= image->rgba[p] > 0; transparent |= image->rgba[p] == 0;
        }
        assert(visible && transparent);
    }
    assert(full->rgba != low->rgba && low->rgba != empty->rgba);
    assert(!SmoUi::Life(life, 4, 3)); assert(!SmoUi::Life(life, 0, 0));
    const auto background = LayoutBntx(std::string(root) + "/LayoutData/CommonBgParts.szs");
    const auto world = SmoUi::WorldBackground(background); assert(world);
    assert(world->width == 1240 && world->height == 1080 && world->rgba.size() == 1240 * 1080 * 4);
    for (size_t p = 3; p < world->rgba.size(); p += 4) assert(world->rgba[p] == 255);
    // 03-list-wash (owner-requested fix, 2026-10-23): the same CmImageWorld03 crop, but
    // neutral grayscale (R==G==B everywhere) so a manifest tint_bind/tint_colors multiply
    // reproduces the tint colour faithfully for ANY kingdom hue, not just red/gold (tinting
    // the already-red WorldBackground would go muddy for e.g. blue/teal/purple kingdoms).
    const auto neutral = SmoUi::WorldBackgroundNeutral(background); assert(neutral);
    assert(neutral->width == 1240 && neutral->height == 1080 && neutral->rgba.size() == 1240 * 1080 * 4);
    bool neutral_variation = false;
    for (size_t p = 0; p < neutral->rgba.size(); p += 4) {
        assert(neutral->rgba[p] == neutral->rgba[p + 1] && neutral->rgba[p + 1] == neutral->rgba[p + 2]);
        assert(neutral->rgba[p + 3] == 255);
        if (p >= 4 && neutral->rgba[p] != neutral->rgba[p - 4]) neutral_variation = true;
    }
    assert(neutral_variation); // not a flat placeholder
    assert(!SmoUi::WorldBackgroundNeutral({}));
    // TitleLogoSmallParts.szs / CursorParts.szs ship ASTC4x4 sRGB (confirmed this session by
    // decoding the real BNTX headers), which fe3h_ui2d::BntxDecode does not support (BC1/3/4/5
    // only) -- SmoUi::TitleLogoSmall/CappyCursor go through the host's ASTC decoder extension
    // instead (DecodeAstcBrti, below). This standalone ctest has no live host decoder to pass,
    // so it only confirms the real BNTX bytes parse far enough to reach that decoder call (fails
    // closed with a null decoder, same path a module build without the extension would take);
    // TestDecodeAstcBrti (further down, synthetic, no romfs needed) exercises the actual decode
    // + channel-selector plumbing, and the live harness capture is the end-to-end check.
    const auto title_logo = LayoutBntx(std::string(root) + "/LayoutData/TitleLogoSmallParts.szs");
    assert(!title_logo.empty());
    assert(!SmoUi::TitleLogoSmall(title_logo, nullptr, nullptr));
    assert(!SmoUi::TitleLogoSmall({}, nullptr, nullptr));
    const auto cursor_parts = LayoutBntx(std::string(root) + "/LayoutData/CursorParts.szs");
    assert(!cursor_parts.empty());
    assert(!SmoUi::CappyCursor(cursor_parts, nullptr, nullptr));
    assert(!SmoUi::CappyCursor({}, nullptr, nullptr));
    std::puts("smo_world_assets: native life gauge states and world background decoded; "
             "title logo / Cappy cursor BNTX bytes read (ASTC decode covered by the synthetic test)");
}
void TestDecodeAstcBrti() {
    // Synthetic BNTX with one 8x8 (2x2 ASTC4x4 block) BRTI texture named "Spot" -- same minimal
    // layout smo_capture_test.cpp's Bntx() helper uses, generalised to a non-256 size so this
    // test actually exercises DecodeAstcBrti's own width/height handling (not just a copy of the
    // capture-specific 256x256 case).
    using Bytes = std::vector<uint8_t>;
    auto put = [](Bytes& b, size_t p, uint64_t v, size_t n = 4) {
        for (size_t i = 0; i < n; ++i) b[p + i] = uint8_t(v >> (8 * i));
    };
    auto text = [](Bytes& b, size_t p, const char* s) {
        std::memcpy(b.data() + p, s, std::strlen(s));
    };
    constexpr uint32_t kW = 8, kH = 8, kWe = 2, kHe = 2, kBlocks = kWe * kHe * 16; // 64 bytes
    Bytes b(0x100 + kBlocks, 0);
    text(b, 0, "BNTX"); put(b, 0x24, 1); put(b, 0x28, 0x30, 8); put(b, 0x30, 0x40, 8);
    // tile=1 (linear): blocks pass straight through, so FakeDecode below can assert a simple
    // identity byte order instead of re-deriving the GOB-swizzle address formula in the test.
    text(b, 0x40, "BRTI"); put(b, 0x52, 1, 2); put(b, 0x56, 1, 2); put(b, 0x5c, 0x2d06);
    put(b, 0x64, kW); put(b, 0x68, kH); put(b, 0x6c, 1); put(b, 0x70, 1);
    put(b, 0x74, 0); put(b, 0x90, kBlocks); put(b, 0x98, 0x05040302); // identity RGBA selector
    put(b, 0xa0, 0xc0, 8); put(b, 0xb0, 0xf0, 8); put(b, 0xc0, 4, 2); text(b, 0xc2, "Spot");
    put(b, 0xf0, 0x100, 8);
    for (size_t i = 0; i < kBlocks; ++i) b[0x100 + i] = uint8_t(i);
    struct State { unsigned calls = 0; };
    State state;
    EdenDsmodAstcDecoder fake = [](void* ud, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh,
                                  const uint8_t* in, size_t n, uint8_t* out, size_t size) -> EdenDsmodBool {
        auto& st = *static_cast<State*>(ud); ++st.calls;
        assert(w == kW && h == kH && bw == 4 && bh == 4 && n == kBlocks && size == kW * kH * 4);
        for (size_t i = 0; i < n; ++i) assert(in[i] == uint8_t(i)); // tile=1 (linear): identity order
        for (size_t p = 0; p < size; p += 4) { out[p] = 10; out[p+1] = 20; out[p+2] = 30; out[p+3] = 240; }
        return EDEN_DSMOD_TRUE;
    };
    const auto spot = SmoUi::DecodeAstcBrti(b, "Spot", &state, fake);
    assert(spot && spot->width == kW && spot->height == kH && spot->rgba.size() == kW * kH * 4);
    assert(state.calls == 1);
    for (size_t p = 0; p < spot->rgba.size(); p += 4) {
        assert(spot->rgba[p] == 10 && spot->rgba[p+1] == 20 && spot->rgba[p+2] == 30 && spot->rgba[p+3] == 240);
    }
    // Swapped selector (B,G,R,A instead of R,G,B,A) must actually reorder channels.
    Bytes swapped = b; put(swapped, 0x98, 0x05020304);
    const auto spot2 = SmoUi::DecodeAstcBrti(swapped, "Spot", &state, fake);
    assert(spot2 && spot2->rgba[0] == 30 && spot2->rgba[1] == 20 && spot2->rgba[2] == 10 && spot2->rgba[3] == 240);
    assert(!SmoUi::DecodeAstcBrti(b, "Spot", &state, nullptr)); // no decoder: fail closed
    assert(!SmoUi::DecodeAstcBrti(b, "Missing", &state, fake)); // wrong name
    assert(!SmoUi::DecodeAstcBrti({}, "Spot", &state, fake));   // not a BNTX
    Bytes bad_format = b; put(bad_format, 0x5c, 0x1a01); // not ASTC4x4 sRGB
    assert(!SmoUi::DecodeAstcBrti(bad_format, "Spot", &state, fake));
    std::puts("smo_world_assets: synthetic ASTC BRTI decode + channel-selector remap passed");
}
} // namespace
int main() { Maps(); Ui(); TestDecodeAstcBrti(); }
