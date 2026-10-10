// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Development CLI for chained_echoes_unity (Linux only, never packaged).
//   survey <romfs> <bundle>...            header/compression/serialized-file/texture-format facts
//   bench <romfs> <manifest.tsv> [pages]  cold + warm decode of the design's asset set: time, RSS
//   families <romfs> <families.tsv>       resolve every ctb_/bestiary_/portrait_ sprite through
//                                         the family search lists, compare with the references
//   page <romfs> <pages.tsv> <page> [pinned]  one page's first visit in a fresh process
//   dump <romfs> <bundle> <Sprite|Texture2D> <name> <out.png>
//   font <romfs>                          startup cost: CE font + host glyph run + atlas
//   pins <romfs> <tsv>...                 pin table for chained_echoes_unity_pins.inc (stdout)
//   text <romfs> <assets path> <name> [path id]      TextAsset m_Script to stdout

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
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
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

// ---- decoded pixel cache (bench only) -----------------------------------------------------------

using ce_unity::Image;

/// Byte-budgeted LRU of decoded images, keyed by any string ("<bundle>|<sprite>"). A miss runs
/// `load` outside the lock; concurrent misses of one key may decode twice (last insert wins).
class ImageCache {
public:
    explicit ImageCache(std::size_t budget_bytes = 16u * 1024u * 1024u);
    std::shared_ptr<const Image> Get(const std::string& key,
                                     const std::function<std::optional<Image>()>& load);
    void Clear();
    std::size_t Bytes() const;
    std::size_t Count() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

struct ImageCache::Impl {
    std::size_t budget{};
    mutable std::mutex mutex;
    std::list<std::pair<std::string, std::shared_ptr<const Image>>> lru;
    std::unordered_map<std::string, decltype(lru)::iterator> map;
    std::size_t bytes{};

    void Evict() {
        while (lru.size() > 1 && bytes > budget) {
            bytes -= lru.back().second->rgba.size();
            map.erase(lru.back().first);
            lru.pop_back();
        }
    }
};

ImageCache::ImageCache(std::size_t budget_bytes) : impl(std::make_shared<Impl>()) {
    impl->budget = budget_bytes;
}

std::shared_ptr<const Image> ImageCache::Get(const std::string& key,
                                             const std::function<std::optional<Image>()>& load) {
    {
        std::lock_guard lock{impl->mutex};
        if (const auto it = impl->map.find(key); it != impl->map.end()) {
            impl->lru.splice(impl->lru.begin(), impl->lru, it->second);
            return it->second->second;
        }
    }
    std::optional<Image> img;
    try {
        img = load ? load() : std::nullopt;
    } catch (...) {
        img.reset();
    }
    if (!img)
        return nullptr;
    auto shared = std::make_shared<const Image>(std::move(*img));
    std::lock_guard lock{impl->mutex};
    if (const auto it = impl->map.find(key); it != impl->map.end()) {
        impl->bytes -= it->second->second->rgba.size();
        impl->lru.erase(it->second);
        impl->map.erase(it);
    }
    impl->lru.emplace_front(key, shared);
    impl->map.emplace(key, impl->lru.begin());
    impl->bytes += shared->rgba.size();
    impl->Evict();
    return shared;
}

void ImageCache::Clear() {
    std::lock_guard lock{impl->mutex};
    impl->lru.clear();
    impl->map.clear();
    impl->bytes = 0;
}
std::size_t ImageCache::Bytes() const {
    std::lock_guard lock{impl->mutex};
    return impl->bytes;
}
std::size_t ImageCache::Count() const {
    std::lock_guard lock{impl->mutex};
    return impl->lru.size();
}


using Clock = std::chrono::steady_clock;

double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

long StatusKb(const char* key) {
    std::ifstream in("/proc/self/status");
    std::string line;
    const std::size_t n = std::strlen(key);
    while (std::getline(in, line))
        if (line.compare(0, n, key) == 0)
            return std::strtol(line.c_str() + n + 1, nullptr, 10);
    return -1;
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

bool Same(const ce_unity::Image& img, const std::string& png) {
    int w = 0, h = 0, n = 0;
    unsigned char* p = stbi_load(png.c_str(), &w, &h, &n, 4);
    if (!p)
        return false;
    const bool same = static_cast<int>(img.width) == w && static_cast<int>(img.height) == h &&
                      std::memcmp(p, img.rgba.data(), img.rgba.size()) == 0;
    stbi_image_free(p);
    return same;
}

void PrintStats(const char* label) {
    const auto s = ce_unity::GetStats();
    std::printf("  %s: bundles opened %llu, romfs reads %llu (%.1f MiB), LZ4 blocks %llu "
                "(%.1f MiB), names indexed %llu\n",
                label, static_cast<unsigned long long>(s.bundles_opened),
                static_cast<unsigned long long>(s.romfs_reads), s.romfs_bytes / 1048576.0,
                static_cast<unsigned long long>(s.blocks_decompressed),
                s.bytes_decompressed / 1048576.0, static_cast<unsigned long long>(s.objects_indexed));
}

int Survey(const std::string& romfs, int argc, char** argv) {
    ce_unity::Reader reader(ce_unity::MakeDirectoryRangeReader(romfs));
    for (int k = 0; k < argc; ++k) {
        const std::string b = argv[k];
        const auto bundle = reader.OpenBundle(b);
        const auto sf = reader.OpenSerialized(b);
        if (!bundle || !sf) {
            std::printf("%s: open failed\n", b.c_str());
            continue;
        }
        std::printf("%s: UnityFS %u %s flags 0x%x, serialized v%u platform %d typetrees %d, "
                    "objects %zu\n",
                    b.c_str(), bundle->format, bundle->engine_version.c_str(), bundle->header_flags,
                    sf->Version(), sf->Platform(), sf->HasTypeTrees() ? 1 : 0, sf->Objects().size());
        for (const auto& s : bundle->BlockSummary())
            std::printf("  blocks: compression %llu x %llu, %.1f MiB (avg %.0f KiB)\n",
                        static_cast<unsigned long long>(s[0]), static_cast<unsigned long long>(s[1]),
                        s[2] / 1048576.0, s[1] ? s[2] / 1024.0 / s[1] : 0.0);
        for (const auto& n : bundle->Nodes())
            std::printf("  node %s %.1f MiB\n", n.path.c_str(), n.size / 1048576.0);
        std::map<std::string, int> fmts;
        for (const auto& o : sf->Objects()) {
            if (o.class_id != 28)
                continue;
            const auto v = sf->ReadObject(o);
            if (!v)
                continue;
            const auto* f = v->Get("m_TextureFormat");
            const auto* blob = v->Get("m_PlatformBlob");
            const auto* sd = v->Path("m_StreamData.size");
            char key[96];
            int gobs = 0;
            if (blob && blob->s.size() >= 12)
                gobs = 1 << static_cast<unsigned char>(blob->s[8]);
            std::snprintf(key, sizeof(key), "format %lld gobs %d %s",
                          static_cast<long long>(f ? f->AsInt() : -1), gobs,
                          sd && sd->AsInt() > 0 ? "streamed" : "inline");
            ++fmts[key];
        }
        for (const auto& [k2, n] : fmts)
            std::printf("  Texture2D %s: %d\n", k2.c_str(), n);
    }
    return 0;
}

int Bench(const std::string& romfs, const std::string& manifest) {
    const auto refs = ReadManifest(manifest);
    if (refs.empty())
        return 2;
    const long rss0 = StatusKb("VmRSS:");
    std::printf("assets %zu, RSS before %ld KiB\n", refs.size(), rss0);
    std::map<std::string, std::vector<const Ref*>> by_bundle;
    for (const Ref& r : refs)
        by_bundle[r.bundle].push_back(&r);
    ce_unity::ResetStats();
    ce_unity::Reader reader(ce_unity::MakeDirectoryRangeReader(romfs), {},
                            std::string{ce_unity::DefaultAssetRoot}, 8);
    ImageCache cache(64u << 20);
    std::size_t pixels = 0;
    int ok = 0;
    double worst = 0;
    std::string worst_name;
    double compare = 0; // reference PNG loading, excluded from the totals
    const auto t0 = Clock::now();
    for (const auto& [b, list] : by_bundle) {
        const auto tb = Clock::now();
        const auto bundle = reader.OpenBundle(b);
        const auto topen = Clock::now();
        reader.HasSprite(b, "\x01"); // forces the name index
        const auto tindex = Clock::now();
        double decode = 0;
        for (const Ref* r : list) {
            const auto ta = Clock::now();
            const auto img = cache.Get(r->bundle + "|" + r->name, [&] {
                return r->kind == "Texture2D" ? reader.LoadTexture(r->bundle, r->name)
                                              : reader.LoadSprite(r->bundle, r->name);
            });
            const double ms = Ms(ta, Clock::now());
            decode += ms;
            if (ms > worst) {
                worst = ms;
                worst_name = r->bundle + "/" + r->name;
            }
            const auto tc = Clock::now();
            if (img && Same(*img, r->png)) {
                ++ok;
                pixels += img->rgba.size();
            }
            compare += Ms(tc, Clock::now());
        }
        std::printf("  %-34s %3zu assets: open %.1f ms, index %.1f ms, decode %.1f ms, RSS %ld KiB\n",
                    b.c_str(), list.size(), Ms(tb, topen), Ms(topen, tindex), decode,
                    StatusKb("VmRSS:"));
        (void)bundle;
    }
    const auto t1 = Clock::now();
    const long rss1 = StatusKb("VmRSS:");
    std::printf("cold: %.1f ms total for %d/%zu exact, worst %.1f ms (%s), decoded %.1f MiB RGBA, "
                "RSS +%ld KiB, peak %ld KiB\n",
                Ms(t0, t1) - compare, ok, refs.size(), worst, worst_name.c_str(), pixels / 1048576.0,
                rss1 - rss0, StatusKb("VmHWM:"));
    PrintStats("cold");

    // Warm, bundle tables cached but pixels not (module pixel cache cleared): what a page switch
    // costs after the host evicted an image.
    cache.Clear();
    ce_unity::ResetStats();
    const auto t2 = Clock::now();
    for (const Ref& r : refs)
        (void)(r.kind == "Texture2D" ? reader.LoadTexture(r.bundle, r.name)
                                     : reader.LoadSprite(r.bundle, r.name));
    const auto t3 = Clock::now();
    std::printf("warm (tables cached, pixels re-decoded): %.1f ms total\n", Ms(t2, t3));
    PrintStats("warm");

    // Pixel-cache hits.
    for (const Ref& r : refs)
        cache.Get(r.bundle + "|" + r.name, [&] {
            return r.kind == "Texture2D" ? reader.LoadTexture(r.bundle, r.name)
                                         : reader.LoadSprite(r.bundle, r.name);
        });
    const auto t4 = Clock::now();
    for (const Ref& r : refs)
        cache.Get(r.bundle + "|" + r.name, {});
    std::printf("pixel cache: %zu images, %.1f MiB, all-hit pass %.3f ms\n", cache.Count(),
                cache.Bytes() / 1048576.0, Ms(t4, Clock::now()));

    // Font.
    const auto tf = Clock::now();
    const auto font = ce_unity::LoadTmpFont(reader);
    std::uint32_t first = 0, lh = 0;
    std::vector<EdenDsmodFontGlyph> glyphs;
    const bool built = font && ce_unity::BuildHostGlyphs(*font, first, glyphs, lh);
    std::printf("font CE (warm bundle): %.2f ms, %zu chars, host run %zu glyphs from U+%04X, "
                "line height %u%s\n",
                Ms(tf, Clock::now()), font ? font->chars.size() : 0, glyphs.size(), first, lh,
                built ? "" : " (FAILED)");
    std::printf("RSS end %ld KiB, peak %ld KiB\n", StatusKb("VmRSS:"), StatusKb("VmHWM:"));
    return ok == static_cast<int>(refs.size()) && built ? 0 : 1;
}

int Families(const std::string& romfs, const std::string& path) {
    const auto refs = ReadManifest(path);
    ce_unity::ResetStats();
    ce_unity::Reader reader(ce_unity::MakeDirectoryRangeReader(romfs), {},
                            std::string{ce_unity::DefaultAssetRoot}, 16);
    int ok = 0, wrong_bundle = 0;
    const auto t0 = Clock::now();
    for (const Ref& r : refs) {
        ce_unity::Family fam = r.name.rfind("ctb_", 0) == 0        ? ce_unity::Family::Ctb
                               : r.name.rfind("bestiary_", 0) == 0 ? ce_unity::Family::Bestiary
                                                                   : ce_unity::Family::Portrait;
        std::string where;
        const auto img = reader.LoadSpriteAny(ce_unity::FamilyBundles(fam), r.name, &where);
        if (where != r.bundle)
            ++wrong_bundle;
        if (img && Same(*img, r.png))
            ++ok;
        else
            std::printf("  mismatch %s (found in %s, ref %s)\n", r.name.c_str(), where.c_str(),
                        r.bundle.c_str());
    }
    std::printf("families: %d / %zu exact, %d resolved to another bundle, %.0f ms\n", ok,
                refs.size(), wrong_bundle, Ms(t0, Clock::now()));
    PrintStats("families");
    std::printf("RSS peak %ld KiB\n", StatusKb("VmHWM:"));
    return ok == static_cast<int>(refs.size()) ? 0 : 1;
}

// One page's first visit in a fresh process: decode every asset the page's renders use, with the
// built-in pins or with the name index only, evicting mapstextures after use like the module will. Prints time, RSS growth and the peak.
int Page(const std::string& romfs, const std::string& pages, const std::string& page, bool pinned) {
    std::vector<Ref> refs;
    {
        std::ifstream in(pages);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string pg;
            Ref r;
            if (std::getline(ss, pg, '\t') && pg == page && std::getline(ss, r.kind, '\t') &&
                std::getline(ss, r.bundle, '\t') && std::getline(ss, r.name, '\t') &&
                std::getline(ss, r.png))
                refs.push_back(r);
        }
    }
    if (refs.empty())
        return 2;
    const long rss0 = StatusKb("VmRSS:");
    ce_unity::ResetStats();
    ce_unity::Reader reader(ce_unity::MakeDirectoryRangeReader(romfs));
    reader.SetUseBuiltinPins(pinned);
    std::vector<ce_unity::Image> keep; // what the host holds after load_image copies
    std::map<std::string, std::vector<const Ref*>> by_bundle;
    for (const Ref& r : refs)
        by_bundle[r.bundle].push_back(&r);
    std::size_t bytes = 0;
    const auto t0 = Clock::now();
    for (const auto& [b, list] : by_bundle) {
        for (const Ref* r : list) {
            std::optional<ce_unity::Image> img;
            img = r->kind == "Texture2D" ? reader.LoadTexture(r->bundle, r->name)
                                         : reader.LoadSprite(r->bundle, r->name);
            if (img) {
                bytes += img->rgba.size();
                keep.push_back(std::move(*img));
            }
        }
        if (b == ce_unity::bundles::MapsTextures)
            reader.Evict(b);
    }
    const double ms = Ms(t0, Clock::now());
    int ok = 0;
    std::size_t idx = 0;
    for (const auto& [b, list] : by_bundle)
        for (const Ref* r : list)
            if (idx < keep.size() && Same(keep[idx++], r->png))
                ++ok;
    std::printf("page %-7s %s: %zu assets (%d exact), %.1f ms, decoded %.2f MiB, RSS +%ld KiB "
                "(peak %ld KiB)\n",
                page.c_str(), pinned ? "pinned  " : "unpinned", refs.size(), ok, ms,
                bytes / 1048576.0, StatusKb("VmRSS:") - rss0, StatusKb("VmHWM:"));
    PrintStats(page.c_str());
    return ok == static_cast<int>(refs.size()) ? 0 : 1;
}

// Writes the pin table (chained_echoes_unity_pins.inc) for every sprite in the given TSVs.
int Pins(const std::string& romfs, int argc, char** argv) {
    std::set<std::pair<std::string, std::string>> want;
    for (int k = 0; k < argc; ++k)
        for (const Ref& r : ReadManifest(argv[k]))
            if (r.kind == "Sprite")
                want.emplace(r.bundle, r.name);
    ce_unity::Reader reader(ce_unity::MakeDirectoryRangeReader(romfs), {},
                            std::string{ce_unity::DefaultAssetRoot}, 16);
    std::printf("// Generated by ce-unity-tool pins (chained_echoes_unity_tool.cpp) from the 1.41 romfs\n"
                "// (update 0100C510166F0800 v1310720). {bundle, sprite, serialized node, path id}.\n"
                "// Metadata only: object locations, no game data. Stale pins fall back to the name index.\n");
    int n = 0;
    for (const auto& [b, name] : want) {
        const auto pin = reader.FindSpritePin(b, name);
        if (!pin)
            continue;
        std::printf("{\"%s\", \"%s\", %u, %lldLL},\n", b.c_str(), name.c_str(), pin->file,
                    static_cast<long long>(pin->path_id));
        ++n;
    }
    std::fprintf(stderr, "%d / %zu pins\n", n, want.size());
    return n == static_cast<int>(want.size()) ? 0 : 1;
}

int Dump(const std::string& romfs, const std::string& bundle, const std::string& kind,
         const std::string& name, const std::string& out) {
    ce_unity::Reader reader(ce_unity::MakeDirectoryRangeReader(romfs));
    const auto img = kind == "Texture2D" ? reader.LoadTexture(bundle, name)
                                         : reader.LoadSprite(bundle, name);
    if (!img)
        return 1;
    return stbi_write_png(out.c_str(), static_cast<int>(img->width), static_cast<int>(img->height),
                          4, img->rgba.data(), static_cast<int>(img->width) * 4)
               ? 0
               : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "survey") == 0)
        return Survey(argv[2], argc - 3, argv + 3);
    if (argc >= 4 && std::strcmp(argv[1], "bench") == 0)
        return Bench(argv[2], argv[3]);
    if (argc >= 4 && std::strcmp(argv[1], "families") == 0)
        return Families(argv[2], argv[3]);
    if (argc >= 3 && std::strcmp(argv[1], "font") == 0) {
        // Startup cost: a fresh reader, the CE font and its host glyph run (what decode_font does).
        const long rss0 = StatusKb("VmRSS:");
        const auto t0 = Clock::now();
        ce_unity::Reader reader(ce_unity::MakeDirectoryRangeReader(argv[2]));
        const auto font = ce_unity::LoadTmpFont(reader);
        std::uint32_t first = 0, lh = 0;
        std::vector<EdenDsmodFontGlyph> glyphs;
        const bool ok = font && ce_unity::BuildHostGlyphs(*font, first, glyphs, lh);
        const auto atlas = font ? ce_unity::HostFontAtlas(*font, 128) : ce_unity::Image{};
        std::printf("font startup: %.2f ms, %zu glyph records, atlas %ux%u, RSS +%ld KiB\n",
                    Ms(t0, Clock::now()), glyphs.size(), atlas.width, atlas.height,
                    StatusKb("VmRSS:") - rss0);
        PrintStats("font");
        return ok ? 0 : 1;
    }
    if (argc >= 4 && std::strcmp(argv[1], "pins") == 0)
        return Pins(argv[2], argc - 3, argv + 3);
    if (argc >= 5 && std::strcmp(argv[1], "text") == 0) {
        const auto t = ce_unity::ReadTextAsset(ce_unity::MakeDirectoryRangeReader(argv[2]), argv[3],
                                               argv[4], argc >= 6 ? std::atoll(argv[5]) : 0);
        if (!t)
            return 1;
        std::fwrite(t->data(), 1, t->size(), stdout);
        return 0;
    }
    if (argc >= 5 && std::strcmp(argv[1], "page") == 0)
        return Page(argv[2], argv[3], argv[4], argc >= 6 && std::strcmp(argv[5], "pinned") == 0);
    if (argc >= 7 && std::strcmp(argv[1], "dump") == 0)
        return Dump(argv[2], argv[3], argv[4], argv[5], argv[6]);
    std::fprintf(stderr, "usage: see the header of chained_echoes_unity_tool.cpp\n");
    return 2;
}
