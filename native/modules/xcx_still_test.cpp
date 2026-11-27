// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Skell still: synthetic checks always run; the real-archive checks run only when XCX_ROMFS names
// the player's merged RomFS directory (exit 77 = skipped when nothing could run at all).
//   XCX_ROMFS                 merged RomFS (sts.arh / sts.ard)
//   XCX_STILL_REFERENCE_A8    optional alpha mask of the offline Python still (tools/xcx/png_alpha.py)
//   XCX_OWNED_RECORD          optional recorded 0x400-byte owned record (opt-in paint mapping)
//   XCX_STILL_ALL=1           optional: every frame row of table 130 must render
#include "xcx_still_cache.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>
namespace fs = std::filesystem;
namespace {
int failures = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            std::fprintf(stderr, "  " __VA_ARGS__);                                                \
            std::fprintf(stderr, "\n");                                                            \
        }                                                                                          \
    } while (0)
bool Is(const std::array<std::uint8_t, 3> &c, int r, int g, int b) {
    return c[0] == r && c[1] == g && c[2] == b;
}
template <class F> bool Throws(F &&f) {
    try {
        f();
    } catch (const std::exception &) {
        return true;
    }
    return false;
}
struct Rom {
    fs::path root;
    std::atomic<int> reads{0};
};
EdenDsmodHostApi MakeHost(Rom &rom) {
    EdenDsmodHostApi host{};
    host.userdata = &rom;
    host.read_romfs = [](void *u, const char *path, std::uint64_t offset, void *buffer,
                         std::size_t size) -> std::uint64_t {
        auto &r = *static_cast<Rom *>(u);
        ++r.reads;
        fs::path rel = fs::path(path).relative_path();
        for (const auto &part : rel)
            if (part == "..")
                return 0;
        std::ifstream f(r.root / rel, std::ios::binary);
        if (!f)
            return 0;
        f.seekg(std::streamoff(offset));
        f.read(static_cast<char *>(buffer), std::streamsize(size));
        return std::uint64_t(f.gcount());
    };
    return host;
}
EdenDsmodHostApi EmptyHost() {
    EdenDsmodHostApi host{};
    host.read_romfs = [](void *, const char *, std::uint64_t, void *, std::size_t) -> std::uint64_t {
        return 0;
    };
    return host;
}
struct Metrics {
    std::uint64_t hash{};
    std::size_t covered{};
    int x0{1 << 30}, y0{1 << 30}, x1{-1}, y1{-1};
    double r{}, g{}, b{};
};
Metrics Measure(const xcx::Image &image) {
    Metrics m;
    m.hash = 14695981039346656037ULL;
    for (auto v : image.rgba) {
        m.hash ^= v;
        m.hash *= 1099511628211ULL;
    }
    for (int y = 0; y < image.h; ++y)
        for (int x = 0; x < image.w; ++x) {
            const auto *p = image.rgba.data() + (std::size_t(y) * image.w + x) * 4;
            if (p[3] <= 127)
                continue;
            ++m.covered;
            m.x0 = std::min(m.x0, x);
            m.x1 = std::max(m.x1, x);
            m.y0 = std::min(m.y0, y);
            m.y1 = std::max(m.y1, y);
            m.r += p[0];
            m.g += p[1];
            m.b += p[2];
        }
    if (m.covered) {
        m.r /= m.covered;
        m.g /= m.covered;
        m.b /= m.covered;
    }
    return m;
}
long StatusKb(const char *key) {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind(key, 0) == 0)
            return std::atol(line.c_str() + std::strlen(key));
    return -1;
}
// ---------------------------------------------------------------------------- synthetic
void Synthetic() {
    // HSV (hue 0..1) -> sRGB, the opt-in player-paint mapping.
    std::array<float, 24> f{};
    const float hsv[6][3] = {{0, 1, 1}, {1.f / 3, 1, 1}, {2.f / 3, 1, 1}, {0, 0, 0.5f},
                             {0.5f, 1, 1}, {1.f / 6, 1, 1}};
    for (int g = 0; g < 6; ++g) {
        f[g * 4] = hsv[g][0];
        f[g * 4 + 1] = hsv[g][1];
        f[g * 4 + 2] = hsv[g][2];
        f[g * 4 + 3] = 1;
    }
    const auto p = xcx::PlayerPaintFromFloats(f);
    CHECK(p.has_value(), "valid HSV groups refused");
    if (p) {
        CHECK(Is(p->rgb[0], 255, 0, 0), "red");
        CHECK(Is(p->rgb[1], 0, 255, 0), "green");
        CHECK(Is(p->rgb[2], 0, 0, 255), "blue");
        CHECK(Is(p->rgb[3], 128, 128, 128), "grey");
        CHECK(Is(p->rgb[4], 0, 255, 255), "cyan");
        CHECK(Is(p->rgb[5], 255, 255, 0), "yellow");
    }
    auto bad = f;
    bad[5] = std::nanf("");
    CHECK(!xcx::PlayerPaintFromFloats(bad), "NaN accepted");
    bad = f;
    bad[2] = 7.f;
    CHECK(!xcx::PlayerPaintFromFloats(bad), "out-of-range value accepted");
    CHECK(!xcx::PlayerPaintFromFloats(std::span<const float>(f).first(23)), "short group list accepted");
    CHECK(!xcx::PlayerPaintFromRecord({}), "empty record accepted");
    std::vector<std::uint8_t> record(0x400, 0);
    CHECK(!xcx::PlayerPaintFromRecord(std::span<const std::uint8_t>(record).first(0x3ff)),
          "short record accepted");
    std::memcpy(record.data() + 0x364, f.data(), sizeof f);
    CHECK(xcx::PlayerPaintFromRecord(record).has_value(), "record at +0x364 refused");
    // Weapon plumbing refuses malformed input.
    CHECK(xcx::ParseLocators(std::vector<std::uint8_t>(64, 0x41)).empty(), "junk locators");
    CHECK(xcx::ParseLocators({}).empty(), "empty locators");
    CHECK(xcx::ParseRig(std::vector<std::uint8_t>(0x200, 0x41)).empty(), "junk rig");
    CHECK(xcx::ParseRig({}).empty(), "empty rig");
    const auto none = xcx::WeaponItemsFromRecord({});
    CHECK(std::all_of(none.begin(), none.end(), [](auto v) { return v == 0; }),
          "short record gave weapons");
    CHECK(Throws([&] {
              xcx::Archive a;
              xcx::ReadWeaponGeometry(a, EmptyHost(), "../x");
          }),
          "unsafe weapon path");
    // FitStill: crop to the alpha box, scale to fit, centre; empty stays empty.
    xcx::Image src(100, 80);
    for (int y = 20; y < 60; ++y)
        for (int x = 40; x < 60; ++x) {
            auto *q = src.rgba.data() + (std::size_t(y) * 100 + x) * 4;
            q[0] = 200;
            q[1] = 100;
            q[2] = 50;
            q[3] = 255;
        }
    const auto fit = xcx::FitStill(src, 40, 40);
    int minx = 99, maxx = -1, miny = 99, maxy = -1;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x)
            if (fit.rgba[(std::size_t(y) * 40 + x) * 4 + 3] > 127) {
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            }
    CHECK(miny == 0 && maxy == 39 && maxx - minx == 19 && minx == 10, "fit box %d..%d x %d..%d",
          minx, maxx, miny, maxy);
    const auto blank = xcx::FitStill(xcx::Image(10, 10), 5, 5);
    CHECK(std::all_of(blank.rgba.begin(), blank.rgba.end(), [](auto v) { return v == 0; }),
          "blank fit not transparent");
    // Malformed model input is refused, never read past.
    xcx::Bytes junk(256, 0x41);
    CHECK(Throws([&] { xcx::DecodeFrameGeometry(junk, junk); }), "junk model accepted");
    CHECK(Throws([&] { xcx::DecodeFrameGeometry({}, {}); }), "empty model accepted");
    // No archive: every entry point refuses cleanly.
    const auto empty = EmptyHost();
    xcx::Archive archive;
    CHECK(Throws([&] { xcx::FrameModelForRow(archive, empty, 220); }), "empty archive accepted");
    CHECK(Throws([&] { xcx::FactoryPalette(archive, empty, 224); }), "empty palette archive");
    CHECK(Throws([&] {
              xcx::FrameModel m;
              m.frame = 1;
              m.resource = "../escape";
              xcx::StillOptions o;
              xcx::RenderStill(archive, empty, m, o);
          }),
          "unsafe resource rendered");
    xcx::StillOptions tiny;
    tiny.width = 4;
    CHECK(Throws([&] {
              xcx::FrameModel m;
              m.resource = "dl069110";
              xcx::RenderStill(archive, empty, m, tiny);
          }),
          "bad options accepted");
    // The cache with a host that has no archive: the job fails, is counted, retried a bounded
    // number of times, never crashes, and the destructor joins promptly.
    {
        xcx::StillCache cache;
        cache.Want(empty, {{220, std::nullopt}});
        cache.WaitIdle();
        CHECK(!cache.Get({220, std::nullopt}), "picture from nothing");
        CHECK(cache.GetStats().failures >= 1, "failure not counted");
        cache.Clear();
        CHECK(!cache.Get({220, std::nullopt}), "cleared cache still serves");
    }
    {
        xcx::StillCache cache; // never asked: no thread at all
        CHECK(!cache.Get({1, std::nullopt}), "empty cache serves");
    }
}
// ---------------------------------------------------------------------------- real archive
// Metrics of this build's own output for the three fixture frames (700 x 760, factory paint,
// bind pose with the pods folded 60 degrees: StillOptions::pose_idle off): coverage, silhouette box, mean colour. The FNV-1a hash is recorded for
// the build that produced it (x86-64 GNU/Linux Release); other targets may differ in the last bit
// of a float and are only held to the metrics.
struct Golden {
    unsigned frame;
    const char *resource;
    std::uint64_t hash;
    std::size_t covered;
    int x0, y0, x1, y1;
    double r, g, b;
};
const Golden Fixtures[] = {
    {220, "dl069110", 0xb3f66b00a6e37f4aULL, 158410, 147, 15, 552, 744, 179.0, 154.3, 122.3},
    {189, "dl059110", 0x35be7215fd5eb1ebULL, 141964, 153, 15, 546, 744, 160.7, 165.1, 125.9},
    {156, "dl049110", 0x2b66e7213b2ec349ULL, 138927, 145, 15, 554, 744, 179.0, 169.3, 112.1},
};
void RealArchive(const fs::path &root) {
    Rom rom{root};
    const auto host = MakeHost(rom);
    xcx::Archive archive;
    const long rss_before = StatusKb("VmRSS:");
    bool first = true;
    for (const auto &g : Fixtures) {
        const auto frame = xcx::FrameModelForRow(archive, host, g.frame);
        CHECK(frame.resource == g.resource, "frame %u -> %s", g.frame, frame.resource.c_str());
        xcx::StillStats stats;
        xcx::StillOptions bind;
        bind.pose_idle = false;
        const auto image = xcx::RenderStill(archive, host, frame, bind, nullptr, &stats);
        if (first) {
            // Whole-process high-water mark growth over the first render (index, geometry,
            // textures, buffers). The job's budget is 64 MiB.
            const auto growth = (StatusKb("VmHWM:") - rss_before) / 1024.0;
            std::printf("first render RSS growth %.1f MiB\n", growth);
#if !defined(__SANITIZE_ADDRESS__) // sanitizer shadow memory and quarantine inflate the RSS
            CHECK(growth < 64.0, "job memory %.1f MiB", growth);
#endif
            first = false;
        }
        CHECK(image.w == 700 && image.h == 760, "size %dx%d", image.w, image.h);
        CHECK(stats.folded && stats.glass && !stats.posed, "fold %d glass %d posed %d", stats.folded,
              stats.glass, stats.posed);
        CHECK(stats.tracked_peak_bytes() < 64u * 1024 * 1024, "tracked %zu", stats.tracked_peak_bytes());
        const auto m = Measure(image);
        std::printf("%s frame %u: hash 0x%016llx covered %zu box %d,%d..%d,%d mean %.1f %.1f %.1f "
                    "ms %.0f (decode %.0f raster %.0f shade %.0f) tracked %.1f MiB\n",
                    g.resource, g.frame, (unsigned long long)m.hash, m.covered, m.x0, m.y0, m.x1,
                    m.y1, m.r, m.g, m.b, stats.ms_total, stats.ms_geometry + stats.ms_textures,
                    stats.ms_raster, stats.ms_shade, stats.tracked_peak_bytes() / 1048576.0);
        // Plausible silhouette: filled most of the box height, transparent corners, opaque body.
        CHECK(m.covered > 100000 && m.covered < 350000, "coverage %zu", m.covered);
        CHECK(m.y1 - m.y0 > 700 && m.x1 - m.x0 > 250, "box %d,%d..%d,%d", m.x0, m.y0, m.x1, m.y1);
        CHECK(image.rgba[3] == 0 && image.rgba[(700 * 760 - 1) * 4 + 3] == 0, "corners not clear");
        // Deterministic: the same inputs give the same pixels.
        const auto again = xcx::RenderStill(archive, host, frame, bind);
        CHECK(Measure(again).hash == m.hash, "second render differs");
        if (g.covered) {
            const double tol = 0.01 * g.covered;
            CHECK(std::abs(double(m.covered) - double(g.covered)) <= tol, "coverage %zu vs %zu",
                  m.covered, g.covered);
            CHECK(std::abs(m.x0 - g.x0) <= 2 && std::abs(m.x1 - g.x1) <= 2 &&
                      std::abs(m.y0 - g.y0) <= 2 && std::abs(m.y1 - g.y1) <= 2,
                  "box moved");
            CHECK(std::abs(m.r - g.r) < 3 && std::abs(m.g - g.g) < 3 && std::abs(m.b - g.b) < 3,
                  "mean colour moved");
            if (m.hash != g.hash)
                std::printf("NOTE: hash differs from the recorded one (metrics hold)\n");
        }
    }
    // The pods: folding changes the silhouette; the unfolded bind pose is wider and flatter.
    {
        const auto frame = xcx::FrameModelForRow(archive, host, 220);
        xcx::StillOptions flat;
        flat.fold_pods = false;
        flat.pose_idle = false;
        xcx::StillStats stats;
        const auto a = xcx::RenderStill(archive, host, frame, flat, nullptr, &stats);
        CHECK(!stats.folded, "fold reported with fold_pods off");
        xcx::StillOptions bind;
        bind.pose_idle = false;
        const auto b = xcx::RenderStill(archive, host, frame, bind);
        CHECK(Measure(a).hash != Measure(b).hash, "fold changed nothing");
    }
    // The default pose: frame 0 of the game's field idle ("000000_idle" in /hkc/dl/<motion>.sar,
    // motion = table 130 column 10, dl069100 for the dl069110 model), skinned; no pod fold.
    {
        const auto frame = xcx::FrameModelForRow(archive, host, 220);
        CHECK(frame.motion == "dl069100", "motion %s", frame.motion.c_str());
        const auto sar = archive.Read(host, "/hkc/dl/dl069100.sar");
        const auto pose = xcx::SampleAnimation(sar, "000000_idle", 0.f);
        // values checked against xc3_model_py's sampling of the same track (frames 0 and 37)
        const auto hip = pose.find(xcx::BoneHash("JD_hip"));
        CHECK(pose.size() == 99 && hip != pose.end(), "idle tracks %zu", pose.size());
        if (hip != pose.end())
            CHECK(std::abs(hip->second.t[1] - 4.117f) < 0.002f && std::abs(hip->second.q[0] - 0.456f) < 0.002f &&
                      std::abs(hip->second.q[3] - 0.540f) < 0.002f,
                  "JD_hip %.3f %.3f %.3f", hip->second.t[1], hip->second.q[0], hip->second.q[3]);
        const auto later = xcx::SampleAnimation(sar, "000000_idle", 37.f);
        const auto hip37 = later.find(xcx::BoneHash("JD_hip"));
        CHECK(hip37 != later.end() && hip37->second.t[0] > 0.007f && hip37->second.t[0] < 0.011f,
              "JD_hip x at frame 37");
        CHECK(xcx::BoneHash("JD_hip") == 0xb9d4424fu, "murmur3");
        CHECK(xcx::SampleAnimation(sar, "no_such_entry", 0.f).empty(), "missing entry sampled");
        xcx::StillStats stats;
        const auto posed = xcx::RenderStill(archive, host, frame, {}, nullptr, &stats);
        CHECK(stats.posed && !stats.folded && stats.glass, "posed %d folded %d", stats.posed, stats.folded);
        const auto m = Measure(posed);
        CHECK(m.covered > 100000 && m.covered < 400000 && m.y1 - m.y0 > 700, "posed coverage %zu", m.covered);
        CHECK(Measure(xcx::RenderStill(archive, host, frame, {})).hash == m.hash, "posed not deterministic");
        xcx::StillOptions bind;
        bind.pose_idle = false;
        CHECK(Measure(xcx::RenderStill(archive, host, frame, bind)).hash != m.hash, "pose changed nothing");
        std::printf("posed dl069110 frame 220: hash 0x%016llx covered %zu box %d,%d..%d,%d ms %.0f\n",
                    (unsigned long long)m.hash, m.covered, m.x0, m.y0, m.x1, m.y1, stats.ms_total);
    }
    // Camera: the rear view is a different, non-empty picture (used for native comparisons).
    {
        const auto frame = xcx::FrameModelForRow(archive, host, 220);
        xcx::StillOptions rear;
        rear.yaw_degrees = 150.f;
        rear.pitch_degrees = 5.f;
        const auto r = Measure(xcx::RenderStill(archive, host, frame, rear));
        CHECK(r.covered > 100000 && r.hash != Measure(xcx::RenderStill(archive, host, frame, {})).hash,
              "rear view %zu", r.covered);
    }
    // Paint: a different palette changes the picture, the same palette does not.
    {
        const auto frame = xcx::FrameModelForRow(archive, host, 220);
        xcx::StillOptions debug;
        debug.palette = xcx::StillPalette{{{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0},
                                            {255, 0, 255}, {0, 255, 255}}}};
        const auto a = xcx::RenderStill(archive, host, frame, debug);
        const auto factory = xcx::FactoryPalette(archive, host, frame.name_id);
        xcx::StillOptions same;
        same.palette = factory;
        const auto b = xcx::RenderStill(archive, host, frame, same);
        const auto c = xcx::RenderStill(archive, host, frame, {});
        CHECK(Measure(a).hash != Measure(c).hash, "palette ignored");
        CHECK(Measure(b).hash == Measure(c).hash, "explicit factory palette differs from default");
        CHECK(Is(factory.rgb[0], 252, 127, 45),
              "table 150 row for name 224");
    }
    if (const char *rec = std::getenv("XCX_OWNED_RECORD")) {
        std::ifstream f(rec, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
        CHECK(bytes.size() == 0x400, "record size");
        const auto paint = xcx::PlayerPaintFromRecord(bytes);
        CHECK(paint.has_value(), "recorded paint groups refused");
        if (paint) {
            const auto frame = xcx::OwnedFrameModel(archive, host, bytes);
            xcx::StillOptions o;
            o.palette = paint;
            const auto img = xcx::RenderStill(archive, host, frame, o);
            const auto fac = xcx::RenderStill(archive, host, frame, {});
            CHECK(Measure(img).hash != Measure(fac).hash, "player paint ignored");
            // Equipped weapons: the record's ten blocks resolve through WPN_DlList/RSC_DlWpnList
            // and the frame's locator table and rig; drawn stowed on the posed frame.
            const auto items = xcx::WeaponItemsFromRecord(bytes);
            CHECK(items[0] == 318 && items[2] == 1112 && items[4] == 0 && items[5] == 1867 &&
                      items[6] == 2392 && items[8] == 2417,
                  "weapon items %u %u %u", items[0], items[2], items[5]);
            const auto mounts = xcx::ResolveWeapons(archive, host, frame, items);
            CHECK(mounts.size() == 9, "mounts %zu", mounts.size());
            if (mounts.size() == 9) {
                CHECK(mounts[0].resource == "wd0122073l" && mounts[0].skell_bone == "EX_wtmt_L",
                      "sidearm L %s %s", mounts[0].resource.c_str(), mounts[0].skell_bone.c_str());
                CHECK(mounts[2].resource == "wd0345012l" && mounts[2].skell_bone == "EX_slmtA_L" &&
                          mounts[2].fold_side == 1,
                      "shoulder L");
                CHECK(mounts[4].resource == "wd0335013r" && mounts[4].skell_bone == "EX_bcmtA_R" &&
                          mounts[4].weapon_bone == "EX_bodyB" && mounts[4].motion == "wd033501r",
                      "back R");
                // EX_slmtA_L world position from the rig (checked against the offline skeleton)
                CHECK(std::abs(mounts[2].mount_position[0] - 2.141f) < 0.01f &&
                          std::abs(mounts[2].mount_position[1] - 7.484f) < 0.01f &&
                          std::abs(mounts[2].mount_position[2] + 1.495f) < 0.01f,
                      "slmt position %.3f %.3f %.3f", mounts[2].mount_position[0],
                      mounts[2].mount_position[1], mounts[2].mount_position[2]);
                xcx::StillOptions wo;
                wo.palette = paint;
                wo.weapons = mounts;
                xcx::StillStats ws;
                const auto armed = xcx::RenderStill(archive, host, frame, wo, nullptr, &ws);
                CHECK(ws.weapons == 9 && ws.posed, "weapons drawn %zu posed %d (%s)", ws.weapons, ws.posed,
                      ws.weapon_notes.c_str());
                CHECK(Measure(armed).hash != Measure(img).hash, "weapons changed nothing");
                CHECK(ws.tracked_peak_bytes() < 64u * 1024 * 1024, "armed tracked %zu", ws.tracked_peak_bytes());
            }
            // Items the frame cannot mount are left out, never guessed.
            xcx::WeaponItems odd{};
            odd[0] = 65000;
            CHECK(xcx::ResolveWeapons(archive, host, frame, odd).empty(), "unknown item mounted");
            // The module's default path: the cache applies the record on slots 0-3 and keeps
            // the factory glow colours on slots 4-5; a request without paint is factory.
            auto hybrid = *paint;
            const auto factory = xcx::FactoryPalette(archive, host, frame.name_id);
            hybrid.rgb[4] = factory.rgb[4];
            hybrid.rgb[5] = factory.rgb[5];
            xcx::StillOptions ho;
            ho.palette = hybrid;
            const auto expect = Measure(xcx::RenderStill(archive, host, frame, ho)).hash;
            xcx::StillCache cache;
            const xcx::StillRequest with{frame.frame, paint}, without{frame.frame, std::nullopt};
            cache.Want(host, {with, without});
            cache.WaitIdle();
            const auto a = cache.Get(with), b = cache.Get(without);
            CHECK(a && b, "paint/factory requests not both cached");
            if (a && b) {
                CHECK(Measure(*a).hash == expect, "cache did not apply record paint + factory glow");
                CHECK(Measure(*b).hash == Measure(fac).hash, "no-paint request is not factory");
            }
        }
    }
    // Reference similarity: silhouette IoU against the offline Python still (dl069100, 1240x1080,
    // fit 0.90). Opaque pixels only: the Python file also carries a soft ground shadow.
    if (const char *ref = std::getenv("XCX_STILL_REFERENCE_A8")) {
        std::ifstream f(ref, std::ios::binary);
        std::uint32_t w{}, h{};
        f.read(reinterpret_cast<char *>(&w), 4);
        f.read(reinterpret_cast<char *>(&h), 4);
        std::vector<std::uint8_t> a(std::size_t(w) * h);
        f.read(reinterpret_cast<char *>(a.data()), std::streamsize(a.size()));
        CHECK(w == 1240 && h == 1080 && f.good(), "reference file");
        const auto frame = xcx::FrameModelForRow(archive, host, 218); // dl069100
        xcx::StillOptions o;
        o.width = 1240;
        o.height = 1080;
        o.fit = 0.90f;
        const auto img = xcx::RenderStill(archive, host, frame, o);
        std::size_t inter = 0, uni = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const bool x = img.rgba[i * 4 + 3] > 200, y = a[i] > 200;
            inter += x && y;
            uni += x || y;
        }
        const double iou = double(inter) / double(uni);
        std::printf("silhouette IoU vs Python reference: %.4f\n", iou);
        CHECK(iou >= 0.90, "IoU %.4f", iou);
    }
    // Malformed model data from the real archive: each mutation must be refused.
    {
        const auto frame = xcx::FrameModelForRow(archive, host, 220);
        const auto stem = "/chr/dl/" + frame.resource;
        const auto md = archive.Read(host, stem + ".wimdo"), mt = archive.Read(host, stem + ".wismt");
        CHECK(!md.empty() && !mt.empty(), "model files");
        const auto u32 = [&](std::size_t p) {
            std::uint32_t v;
            std::memcpy(&v, md.data() + p, 4);
            return v;
        };
        const auto put = [](xcx::Bytes &b, std::size_t p, std::uint32_t v) {
            std::memcpy(b.data() + p, &v, 4);
        };
        const auto models = u32(8);
        CHECK(!Throws([&] { xcx::DecodeFrameGeometry(md, mt); }), "unmodified model refused");
        auto bad = md;
        put(bad, models + 84, 0xffffffffu); // bone count
        CHECK(Throws([&] { xcx::DecodeFrameGeometry(bad, mt); }), "huge bone count accepted");
        bad = md;
        put(bad, models + 80, 0x7fffffffu); // bone table offset
        CHECK(Throws([&] { xcx::DecodeFrameGeometry(bad, mt); }), "bone table outside the file");
        bad = md;
        put(bad, models + 104, 100000); // skin name count
        CHECK(Throws([&] { xcx::DecodeFrameGeometry(bad, mt); }), "huge skin name count accepted");
        bad = md;
        put(bad, models + 100, 0x7ffffff0u); // skin name table
        CHECK(Throws([&] { xcx::DecodeFrameGeometry(bad, mt); }), "skin names outside the file");
        // a bone whose parent is itself / out of range
        const auto bones = std::size_t(models) + u32(models + 80);
        bad = md;
        put(bad, bones + 184 + 4, 1); // bone 1's parent = 1
        CHECK(Throws([&] { xcx::DecodeFrameGeometry(bad, mt); }), "self-parent bone accepted");
        bad = md;
        put(bad, bones + 184 + 4, 900); // out of range parent
        CHECK(Throws([&] { xcx::DecodeFrameGeometry(bad, mt); }), "out-of-range parent accepted");
        bad = md;
        put(bad, bones + 4, 1); // root bone 0's parent = 1, bone 1's parent = 0: a cycle
        CHECK(Throws([&] { xcx::DecodeFrameGeometry(bad, mt); }), "bone cycle accepted");
        bad = md;
        put(bad, bones + 56, 0x7f800000u); // an infinite inverse bind entry
        CHECK(Throws([&] { xcx::DecodeFrameGeometry(bad, mt); }), "non-finite bind matrix accepted");
        CHECK(Throws([&] { xcx::DecodeFrameGeometry(std::span(md).first(models + 100), mt); }),
              "truncated skeleton accepted");
        // A model that decodes but whose material has no base colour texture cannot be drawn.
        auto geo = xcx::DecodeFrameGeometry(md, mt);
        CHECK(geo.skin.bones.size() == 90 && geo.skin.skin_names.size() == 90 &&
                  geo.skin.weight_buffers.size() == 2,
              "skin %zu/%zu/%zu", geo.skin.bones.size(), geo.skin.skin_names.size(),
              geo.skin.weight_buffers.size());
    }
    // The cache: stale jobs are cancelled and never published, frames are cached by id.
    {
        xcx::StillCache cache;
        const xcx::StillRequest a{220, std::nullopt}, b{189, std::nullopt};
        cache.Want(host, {a});
        cache.Want(host, {b}); // the frame changed at once: the first job is stale
        cache.WaitIdle();
        CHECK(!cache.Get(a), "stale frame published");
        CHECK(cache.Get(b) != nullptr, "wanted frame missing");
        const auto renders = cache.GetStats().renders;
        cache.Want(host, {b}); // cached: no new render
        cache.WaitIdle();
        CHECK(cache.GetStats().renders == renders, "cached frame rendered again");
        cache.Want(host, {a, b}); // both wanted: a renders, b stays
        cache.WaitIdle();
        CHECK(cache.Get(a) && cache.Get(b), "both frames not cached");
        cache.Clear();
        CHECK(!cache.Get(a) && !cache.Get(b), "Clear kept pictures");
        // cancellation inside a running job
        cache.Want(host, {a});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cache.Want(host, {});
        cache.WaitIdle();
        CHECK(!cache.Get(a), "cancelled job published");
    }
    if (const char *all = std::getenv("XCX_STILL_ALL"); all && std::string_view(all) == "1") {
        // Every Skell frame row of table 130 that names a model: all must render.
        const unsigned rows[] = {20,  21,  32,  34,  57,  59,  61,  84,  86,  88,  118, 120, 122,
                                 152, 154, 156, 186, 187, 189, 217, 218, 220, 248, 250, 251, 252,
                                 253, 254, 255, 256, 257, 259, 260, 261, 262, 263, 264};
        // These model families use another vertex layout (Ares, the police frame, Ares Prime and
        // the DLC frames); the decoder refuses them and the page simply shows no picture.
        const auto unsupported = [](unsigned row) {
            return row == 250 || row == 251 || row == 252 || row == 255 || row >= 260;
        };
        for (const auto row : rows) {
            try {
                const auto frame = xcx::FrameModelForRow(archive, host, row);
                xcx::StillStats stats;
                const auto img = xcx::RenderStill(archive, host, frame, {}, nullptr, &stats);
                const auto m = Measure(img);
                std::printf("row %u %s name %u: covered %zu ms %.0f tracked %.1f MiB%s\n", row,
                            frame.resource.c_str(), frame.name_id, m.covered, stats.ms_total,
                            stats.tracked_peak_bytes() / 1048576.0,
                            m.covered < 20000 ? " (thin)" : "");
                CHECK(m.covered > 5000, "row %u nearly empty", row);
                CHECK(!unsupported(row), "row %u rendered but is listed unsupported", row);
            } catch (const std::exception &e) {
                std::printf("row %u refused: %s\n", row, e.what());
                CHECK(unsupported(row) && std::string_view(e.what()) == "XCX missing vertex inputs",
                      "row %u: %s", row, e.what());
            }
        }
    }
}
} // namespace
int main() {
    Synthetic();
    const char *root = std::getenv("XCX_ROMFS");
    if (root && *root && fs::exists(fs::path(root) / "sts.arh"))
        RealArchive(root);
    else
        std::printf("XCX_ROMFS not set: real-archive checks skipped\n");
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("xcx still tests passed\n");
    return 0;
}
