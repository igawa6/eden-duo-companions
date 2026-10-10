// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Crash Team Racing Nitro-Fueled asset tests over the player's own archives.
// CTR_ROMFS=<dir holding archives/*.pak> runs the romfs checks (skipped without it);
// CTR_OUT=<dir> also writes every composed picture as <key>.rgba (+ .txt with the size).
// The LZMA, parser-robustness and key-validation checks are synthetic and always run.

#include "core/mods/dsmod_module_abi.h"
#include "ctr_alchemy.h"
#include "ctr_art.h"
#include "ctr_lzma.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

std::string g_root;

std::size_t ReadRomfs(void*, const char* path, std::uint64_t offset, void* out, std::size_t size) {
    std::ifstream f(g_root + "/" + path, std::ios::binary);
    if (!f)
        return 0;
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(f.gcount());
}

int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}

void LzmaSynthetic() {
    // raw LZMA1 (lc3 lp0 pb2, dictionary 64 KiB) produced by Python's lzma encoder
    const std::uint8_t props[5]{0x5d, 0x00, 0x00, 0x01, 0x00};
    const std::uint8_t stream[]{0x00, 0x30, 0x98, 0x88, 0xa7, 0xea, 0x75, 0xc5, 0x62, 0xa8, 0xb1, 0x3b, 0x41, 0x38, 0x54, 0x04, 0x07, 0x5a, 0x60, 0x5e, 0xde, 0x66, 0x7c, 0x62, 0x9b, 0x18, 0x18, 0x63, 0x07, 0x48, 0x07, 0x9c, 0xd6, 0xf2, 0xd9, 0x27, 0x77, 0x9c, 0xf7, 0x94, 0xf5, 0x6c, 0x4f, 0xe2, 0x5d, 0x25, 0x2f, 0x72, 0xa9, 0xe4, 0xf3, 0xf7, 0x76, 0x7b, 0xf5, 0xe9, 0xc2, 0x57, 0xb7, 0x58, 0xdb, 0x4e, 0x85, 0xa9, 0x40, 0xd7, 0x50, 0x52, 0xa2, 0xeb, 0x15, 0x3e, 0xae, 0xec, 0xd2, 0x59, 0x86, 0xc3, 0x11, 0xeb, 0xe0, 0x91, 0xaf, 0x7d, 0xd2, 0x14, 0x15, 0x7a, 0xee, 0x7c, 0x2d, 0x63, 0x46, 0x03, 0x94, 0xd9, 0xa0, 0x13, 0x40, 0x09, 0x7d, 0x66, 0xf8, 0x03, 0xff, 0xff, 0x51, 0x10, 0x00, 0x00};
    std::vector<std::uint8_t> out;
    const bool ok = CtrLzma::Decode(props, stream, sizeof stream, 268, out);
    const std::vector<std::uint8_t> full = out;
    const std::string want = std::string("abcabcabcabc") + [] {
        std::string s;
        for (int i = 0; i < 6; ++i)
            s += "Crash Team Racing Nitro-Fueled! ";
        for (int i = 0; i < 64; ++i)
            s += static_cast<char>(i);
        return s;
    }();
    Check(ok && std::string(out.begin(), out.end()) == want, "lzma synthetic");

    // malformed input fails cleanly and never writes past out_size
    Check(!CtrLzma::Decode(props, stream, sizeof stream - 20, 268, out), "lzma truncated input fails");
    const std::uint8_t bad_props[5]{225, 0, 0, 1, 0};
    Check(!CtrLzma::Decode(bad_props, stream, sizeof stream, 268, out), "lzma invalid props fail");
    Check(CtrLzma::Decode(props, stream, sizeof stream, 100, out) && out.size() == 100 &&
              std::equal(out.begin(), out.end(), full.begin()),
          "lzma partial output");
    std::mt19937 rng{1234};
    bool bounded = true;
    for (int i = 0; i < 1000; ++i) {
        std::vector<std::uint8_t> s(stream, stream + sizeof stream);
        s[rng() % s.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
        CtrLzma::Decode(props, s.data(), s.size(), 268, out);
        bounded = bounded && out.size() <= 268;
    }
    Check(bounded, "lzma 1000 bit flips stay bounded");
}

void ParsersSurviveGarbage() {
    // the IGZ readers take romfs bytes as-is (a LayeredFS mod may replace them): random, empty,
    // truncated and header-only buffers must not crash (results do not matter)
    std::mt19937 rng{99};
    for (int i = 0; i < 300; ++i) {
        CtrAlchemy::Bytes b(rng() % 4096);
        for (auto& x : b)
            x = static_cast<std::uint8_t>(rng());
        if (i % 3 == 0 && b.size() >= 0x40) { // a plausible IGZ v10 header, garbage after it
            b[0] = 0x01, b[1] = 0x5A, b[2] = 0x47, b[3] = 0x49;
            b[4] = 10, b[5] = b[6] = b[7] = 0;
            b[0x0C] = static_cast<std::uint8_t>(1 + rng() % 3), b[0x0D] = b[0x0E] = b[0x0F] = 0;
        }
        (void)CtrAlchemy::DecodeImage(b);
        (void)CtrAlchemy::DecodeFont(b);
        (void)CtrAlchemy::ParseMinimap(b);
    }
    (void)CtrAlchemy::DecodeImage({});
    Check(true, "parsers survive 300 garbage buffers");
}

void KeysValidated() {
    // module keys carry guest text: malformed ones are refused without touching the romfs
    EdenDsmodHostApi host{};
    CtrArt::Library lib;
    bool all_null = true;
    for (const char* key : {"module:ctr:", "module:ctr:name:w:", "module:ctr:set:row:9:1", "module:ctr:set:row:0",
                            "module:ctr:portrait:../x", "module:ctr:rank:ab", "module:ctr:tdigit:12",
                            "module:ctr:bg:../../etc", "module:ctr:wrongpatch:new", "other:ctr:tab",
                            "module:ctr:map:t111_crash_cove"})
        all_null = all_null && !lib.Load(host, key);
    Check(all_null, "malformed keys / no romfs refused");
}

} // namespace

namespace CtrArt {
/// Member archives inside another archive (update.pak's driver paks): a reader asked for after
/// the decode budget dropped the member decodes it again, and a member that cannot be found reads
/// nothing instead of handing out an empty std::function (which would throw on the first read).
struct LibraryTestAccess {
    static inline std::unordered_map<std::string, CtrAlchemy::Bytes> files;
    static std::size_t Read(void*, const char* path, std::uint64_t offset, void* out, std::size_t size) {
        const auto it = files.find(path);
        if (it == files.end() || offset >= it->second.size())
            return 0;
        const std::size_t n = std::min<std::size_t>(size, it->second.size() - offset);
        std::memcpy(out, it->second.data() + offset, n);
        return n;
    }
    /// A version-13 igArchive: header, TOC, name table, then the members.
    static CtrAlchemy::Bytes Iga(const std::vector<std::tuple<std::string, CtrAlchemy::Bytes, std::uint32_t>>& members) {
        const std::uint32_t count = static_cast<std::uint32_t>(members.size());
        const std::uint32_t toc = 0x38 + 4 * count, names = toc + 16 * count;
        CtrAlchemy::Bytes out(names + 4 * count);
        const auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
        std::memcpy(out.data(), "IGA\x1a", 4);
        put32(4, 13);
        put32(0x0C, count);
        put32(0x10, 0x10); // sector
        const std::uint64_t names64 = names;
        std::memcpy(out.data() + 0x28, &names64, 8);
        for (std::uint32_t i = 0; i < count; ++i) {
            put32(names + 4 * i, static_cast<std::uint32_t>(out.size() - names));
            const std::string& name = std::get<0>(members[i]);
            const std::string record = "full/" + name + '\0' + name + '\0';
            out.insert(out.end(), record.begin(), record.end());
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            out.resize((out.size() + 0xF) & ~std::size_t{0xF});
            const auto& data = std::get<1>(members[i]);
            put32(toc + 16 * i, static_cast<std::uint32_t>(out.size()));
            put32(toc + 16 * i + 8, static_cast<std::uint32_t>(data.size()));
            put32(toc + 16 * i + 12, std::get<2>(members[i]));
            out.insert(out.end(), data.begin(), data.end());
        }
        return out;
    }
    static std::string Text(Library& lib, const EdenDsmodHostApi& host, const std::string& pak) {
        const auto a = lib.ArchiveFor(host, pak);
        const CtrAlchemy::Entry* e = a ? a->FindExact("hello.txt") : nullptr;
        CtrAlchemy::Bytes b;
        return e && a->Read(*e, lib.ReaderFor(host, pak), b) ? std::string(b.begin(), b.end()) : std::string{};
    }
    static void Run() {
        const std::string hello = "hello";
        const CtrAlchemy::Bytes inner = Iga({{"hello.txt", {hello.begin(), hello.end()}, 0xFFFFFFFFu}});
        files["archives/update.pak"] = Iga({{"archives/stored.pak", inner, 0xFFFFFFFFu},
                                            {"archives/packed.pak", inner, 0x20000000u}});
        EdenDsmodHostApi host{};
        host.read_romfs = &Read;
        Library lib;
        const std::string stored = "archives/update.pak#archives/stored.pak",
                          packed = "archives/update.pak#archives/packed.pak";
        Check(Text(lib, host, stored) == hello && Text(lib, host, packed) == hello, "member archives read");
        // the budget drops decoded members (and their archives); a reader handed out before
        // keeps its bytes, a new one decodes the member again
        const CtrAlchemy::ReadAt before = lib.ReaderFor(host, packed);
        lib.nested.clear();
        lib.archives.clear();
        char head[4]{};
        Check(before(0, head, 4) == 4 && std::memcmp(head, "IGA\x1a", 4) == 0, "member reader outlives eviction");
        const auto kept = lib.ArchiveFor(host, packed);
        lib.nested.clear(); // dropped again while `kept` is still in use
        const CtrAlchemy::Entry* e = kept ? kept->FindExact("hello.txt") : nullptr;
        CtrAlchemy::Bytes b;
        Check(e && kept->Read(*e, lib.ReaderFor(host, packed), b) && std::string(b.begin(), b.end()) == hello,
              "evicted member archive re-opened");
        bool clean = true;
        for (const std::string missing : {"archives/update.pak#archives/none.pak", "archives/none.pak#archives/x.pak"}) {
            const CtrAlchemy::ReadAt r = lib.ReaderFor(host, missing);
            clean = clean && r && r(0, head, 4) == 0 && !lib.ArchiveFor(host, missing);
            if (e)
                clean = clean && !kept->Read(*e, r, b);
        }
        Check(clean, "missing member archive reads fail cleanly");
    }
};
} // namespace CtrArt

int main() {
    LzmaSynthetic();
    ParsersSurviveGarbage();
    KeysValidated();
    CtrArt::LibraryTestAccess::Run();
    const char* root = std::getenv("CTR_ROMFS");
    if (!root || !*root) {
        std::printf("SKIP romfs checks (set CTR_ROMFS)\n");
        return failures ? 1 : 0;
    }
    g_root = root;
    EdenDsmodHostApi host{};
    host.read_romfs = &ReadRomfs;

    // minimap parameters (values verified against live trails, contracts/RACE-DATA.md)
    CtrArt::Library lib;
    struct Expect {
        const char* track;
        int dir;
        float w, h;
    };
    for (const Expect e : {Expect{"t111_crash_cove", 3, 300, 300}, Expect{"t112_roos_tubes", 3, 380, 420},
                           Expect{"t131_coco_park", 2, 300, 300}, Expect{"t143_polar_pass", 1, 200, 530},
                           Expect{"t144_tiny_arena", 0, 870, 530}}) {
        const auto p = lib.Params(host, e.track);
        char what[96];
        std::snprintf(what, sizeof what, "minimap params %s", e.track);
        Check(p && p->direction == e.dir && p->w == e.w && p->h == e.h, what);
    }
    const char* out_dir = std::getenv("CTR_OUT");
    for (const char* key :
         {"module:ctr:map:t111_crash_cove", "module:ctr:bg:t111_crash_cove", "module:ctr:map:t144_tiny_arena",
          "module:ctr:portrait:DriverCrash", "module:ctr:head:DriverCoco", "module:ctr:headp:DriverCrash",
          "module:ctr:name:w:DINGODILE", "module:ctr:name:n:CRASH", "module:ctr:digit:w:7",
          "module:ctr:digit:g:4", "module:ctr:rank:4", "module:ctr:rank:1", "module:ctr:lap:2:3",
          "module:ctr:tab", "module:ctr:loading", "module:ctr:wrongpatch:base",
          "module:ctr:wrongpatch:old", "module:ctr:timer", "module:ctr:tdigit:0",
          "module:ctr:tdigit:colon", "module:ctr:set:bg",
          "module:ctr:set:row:0:1", "module:ctr:set:row:2:0", "module:ctr:cog"}) {
        const auto img = lib.Load(host, key);
        Check(img && img->width && img->rgba.size() == std::size_t{img->width} * img->height * 4, key);
        if (img && out_dir) {
            std::string name = key + 11;
            for (char& c : name)
                if (c == ':')
                    c = '_';
            std::ofstream(std::string(out_dir) + "/" + name + ".rgba", std::ios::binary)
                .write(reinterpret_cast<const char*>(img->rgba.data()), static_cast<std::streamsize>(img->rgba.size()));
            std::ofstream(std::string(out_dir) + "/" + name + ".txt") << img->width << " " << img->height << "\n";
        }
    }
    const auto view = lib.View("t111_crash_cove");
    Check(view.has_value() && view->scale > 0, "map view published");
    if (view) {
        // the idle player's start position (6706, 1094) must land on the checkered line
        float px, py;
        CtrAlchemy::ProjectToTexture(view->params, 6706.0f, 1094.0f, view->tex, px, py);
        std::printf("start line -> texture (%.1f, %.1f)\n", px, py);
    }
    return failures ? 1 : 0;
}
