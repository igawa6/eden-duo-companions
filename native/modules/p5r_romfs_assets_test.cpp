// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// p5r_romfs_assets tests.
// Part 1 (always): synthetic data only -- random-block DDS files in every supported format (every
//   BC7 mode incl. the reserved one) against Pillow hashes, region/crop semantics, @UTF, PAK, FTD,
//   NAME.TBL layouts. No game data.
// Part 2 (when the local romfs is present; skipped otherwise): the real CPK (dir in P5R_ROMFS)
//   is read through the same Romfs class the module uses and compared byte-for-byte with a local
//   unpack (dir in P5R_UNPACK), and every
//   decoded image the package needs is compared with Pillow-produced hashes
//   (p5r_romfs_assets_fixtures.inc: hashes only, never pixels). Timings/memory are printed.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "p5r_bgm_reader.h"
#include "p5r_bgm_titles_ref.h"
#include "p5r_game_text.h"
#include "p5r_romfs_assets.h"
#include "p5r_romfs_local.h"

namespace {
#include "p5r_romfs_assets_fixtures.inc"

using namespace p5r_assets;
using Clock = std::chrono::steady_clock;
int failures = 0;
// FNV-1a-64 of a title's UTF-8 text (p5r_bgm_titles_ref.h keeps hashes, not the titles).
uint64_t TextFnv(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s)
        h = (h ^ c) * 0x100000001b3ULL;
    return h;
}
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL " << what << '\n';
        ++failures;
    }
}
double Ms(Clock::time_point a) {
    return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
}

struct Fnv {
    uint64_t h = 0xcbf29ce484222325ull;
    void Add(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ b[i]) * 0x100000001b3ull;
    }
    void U32(uint32_t v) {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        Add(b, 4);
    }
    void Img(const Image& im) {
        U32(im.w);
        U32(im.h);
        Add(im.rgba.data(), im.rgba.size());
    }
};
uint64_t ImgHash(const Image& im) {
    Fnv f;
    f.Img(im);
    return f.h;
}

// ------------------------------------------------------------------ synthetic DDS
std::vector<uint8_t> RandBytes(uint64_t seed, size_t n) {
    std::vector<uint8_t> out;
    uint64_t s = seed;
    while (out.size() < n) {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        for (int i = 0; i < 8; ++i)
            out.push_back(uint8_t(z >> (8 * i)));
    }
    out.resize(n);
    return out;
}
void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i)
        v[at + i] = uint8_t(x >> (8 * i));
}
std::vector<uint8_t> DdsHeader(uint32_t w, uint32_t h, uint32_t pf, const char* fourcc,
                               uint32_t bits, const uint32_t masks[4], int dxgi) {
    std::vector<uint8_t> d(128, 0);
    std::memcpy(d.data(), "DDS ", 4);
    Put32(d, 4, 124);
    Put32(d, 8, 0x1007);
    Put32(d, 12, h);
    Put32(d, 16, w);
    Put32(d, 76, 32);
    Put32(d, 80, pf);
    if (fourcc)
        std::memcpy(&d[84], fourcc, 4);
    Put32(d, 88, bits);
    for (int i = 0; i < 4; ++i)
        Put32(d, 92 + 4 * i, masks ? masks[i] : 0);
    Put32(d, 108, 0x1000);
    if (dxgi >= 0) {
        d.resize(148, 0);
        Put32(d, 128, uint32_t(dxgi));
        Put32(d, 132, 3);
        Put32(d, 140, 1);
    }
    return d;
}
std::vector<uint8_t> SynthDds(unsigned i) {
    constexpr uint32_t W = 64, H = 64;
    const uint64_t seed = 0x5035520000ull + i;
    static const uint32_t rgba[4] = {0xff, 0xff00, 0xff0000, 0xff000000};
    static const uint32_t bgra[4] = {0xff0000, 0xff00, 0xff, 0xff000000};
    static const uint32_t la[4] = {0xff, 0, 0, 0xff00};
    struct S {
        const char* cc;
        int dxgi;
        size_t bs;
        uint32_t bits;
        const uint32_t* m;
        uint32_t pf;
    };
    const S table[] = {{"DXT1", -1, 8, 0, nullptr, 4},   {"DXT3", -1, 16, 0, nullptr, 4},
                       {"DXT5", -1, 16, 0, nullptr, 4},  {"ATI1", -1, 8, 0, nullptr, 4},
                       {"ATI2", -1, 16, 0, nullptr, 4},  {"DX10", 98, 16, 0, nullptr, 4},
                       {nullptr, -1, 0, 32, rgba, 0x41}, {nullptr, -1, 0, 32, bgra, 0x41},
                       {nullptr, -1, 0, 16, la, 0x20001}};
    const S& s = table[i];
    auto d = DdsHeader(W, H, s.pf, s.cc, s.bits, s.m, s.dxgi);
    std::vector<uint8_t> body =
        s.bs ? RandBytes(seed, (W / 4) * (H / 4) * s.bs) : RandBytes(seed, W * H * s.bits / 8);
    if (s.dxgi == 98)
        for (size_t k = 0; k < body.size() / 16; ++k) {
            const unsigned m = k % 9;
            uint8_t& b = body[16 * k];
            b = m == 8 ? 0 : uint8_t((b & ~((1u << (m + 1)) - 1)) | (1u << m));
        }
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

void TestSyntheticDds() {
    for (const auto& f : kSynth) {
        const auto dds = SynthDds(f.index);
        Image im;
        check(DecodeDDS(dds, im), std::string("decode synthetic ") + f.label);
        check(ImgHash(im) == f.hash, std::string("Pillow hash synthetic ") + f.label);
        // Region = Pillow crop semantics, incl. out-of-texture padding.
        Image reg;
        check(DecodeDDSRegion(dds, -5, 3, 30, 70, reg), std::string("region ") + f.label);
        bool same = reg.w == 30 && reg.h == 70;
        for (uint32_t y = 0; same && y < 70; ++y)
            for (uint32_t x = 0; same && x < 30; ++x) {
                const int sx = int(x) - 5, sy = int(y) + 3;
                const bool in = sx >= 0 && sy < 64;
                for (int c = 0; c < 4; ++c) {
                    const uint8_t want = in ? im.rgba[(size_t(sy) * 64 + sx) * 4 + c] : 0;
                    same = same && reg.rgba[(size_t(y) * 30 + x) * 4 + c] == want;
                }
            }
        check(same, std::string("region == padded crop ") + f.label);
    }
    // Truncated data fails closed.
    auto dds = SynthDds(2);
    dds.resize(dds.size() - 1);
    Image im;
    check(!DecodeDDS(dds, im), "truncated DDS rejected");
}

// ------------------------------------------------------------------ synthetic tables
void Be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i)
        v.push_back(uint8_t(x >> (8 * i)));
}
void Be16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void TestUtf() {
    // strings: "<NULL>\0T\0A\0B\0S\0Z\0x\0yy\0"
    std::vector<uint8_t> strings;
    auto add = [&](const char* s) {
        const uint32_t off = uint32_t(strings.size());
        strings.insert(strings.end(), s, s + std::strlen(s) + 1);
        return off;
    };
    const uint32_t tn = add("Tbl"), ca = add("A"), cb = add("B"), cs = add("S"), cz = add("Z"),
                   sx = add("x"), sy = add("yy");
    const std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    std::vector<uint8_t> schema;
    schema.push_back(0x34);
    Be32(schema, ca);
    Be32(schema, 0xDEADBEEF); // constant u32
    schema.push_back(0x53);
    Be32(schema, cb); // per-row s16
    schema.push_back(0x5A);
    Be32(schema, cs); // per-row string
    schema.push_back(0x12);
    Be32(schema, cz); // zero u16
    schema.push_back(0x5B);
    Be32(schema, add("D")); // per-row data
    std::vector<uint8_t> rows;
    Be16(rows, uint16_t(-7));
    Be32(rows, sx);
    Be32(rows, 0);
    Be32(rows, 2);
    Be16(rows, 300);
    Be32(rows, sy);
    Be32(rows, 2);
    Be32(rows, 3);
    const uint32_t row_len = 14;
    const uint32_t rows_off = 0x18 + uint32_t(schema.size());
    const uint32_t str_off = rows_off + uint32_t(rows.size());
    const uint32_t data_off = str_off + uint32_t(strings.size());
    std::vector<uint8_t> p = {'@', 'U', 'T', 'F'};
    Be32(p, data_off + uint32_t(data.size()));
    Be16(p, 1);
    Be16(p, uint16_t(rows_off));
    Be32(p, str_off);
    Be32(p, data_off);
    Be32(p, tn);
    Be16(p, 5);
    Be16(p, uint16_t(row_len));
    Be32(p, 2);
    p.insert(p.end(), schema.begin(), schema.end());
    p.insert(p.end(), rows.begin(), rows.end());
    p.insert(p.end(), strings.begin(), strings.end());
    p.insert(p.end(), data.begin(), data.end());
    UtfTable t;
    check(t.Parse(std::span<const uint8_t>(p)), "utf parse");
    check(t.Name() == "Tbl" && t.Rows() == 2, "utf name/rows");
    check(t.IntOr(1, "A", 0) == 0xDEADBEEF && t.IntOr(0, "B", 0) == -7 && t.IntOr(1, "B", 0) == 300,
          "utf ints");
    check(t.StrOr(0, "S") == "x" && t.StrOr(1, "S") == "yy" && t.IntOr(0, "Z", 9) == 0,
          "utf strings");
    const auto d1 = t.BytesOr(1, "D");
    check(d1.size() == 3 && d1[0] == 3 && d1[2] == 5, "utf data");
    check(t.Column("nope") == -1 && t.IntOr(5, "A", 42) == 42, "utf missing");
    p.resize(p.size() - 3);
    UtfTable bad;
    check(!bad.Parse(std::span<const uint8_t>(p)), "utf truncated rejected");
}

void TestPakFtdNameTbl() {
    // PAK v3: two members.
    std::vector<uint8_t> pak;
    Be32(pak, 2);
    auto member = [&](const char* name, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> n(32, 0);
        std::memcpy(n.data(), name, std::strlen(name));
        pak.insert(pak.end(), n.begin(), n.end());
        Be32(pak, uint32_t(body.size()));
        pak.insert(pak.end(), body.begin(), body.end());
    };
    // FTD0 type 0 with one 3-row x 4-byte table.
    std::vector<uint8_t> ftd = {0, 1, 0, 0, 'F', 'T', 'D', '0'};
    Be32(ftd, 0x20 + 0x10 + 12);
    Be32(ftd, 1);
    Be32(ftd, 0x20);
    ftd.resize(0x20, 0);
    Be32(ftd, 0);
    Be32(ftd, 12);
    Be32(ftd, 3);
    Be32(ftd, 0);
    for (uint32_t i = 0; i < 3; ++i)
        Be32(ftd, 100 + i);
    member("a.ctd", ftd);
    member("second.bin", {9, 9});
    std::vector<PakEntry> entries;
    check(ParsePak(pak, entries) && entries.size() == 2 && entries[1].name == "second.bin",
          "pak v3");
    std::vector<std::span<const uint8_t>> fe;
    FtdTable tab;
    check(ParseFtd(PakFind(entries, "A.CTD"), fe) && fe.size() == 1 && ParseFtdTable(fe[0], tab) &&
              tab.rows == 3 && tab.row_size == 4 && tab.Row(2)[3] == 102,
          "ftd table");
    // NAME.TBL: one section, 2 strings.
    std::vector<uint8_t> tbl;
    Be32(tbl, 4);
    Be16(tbl, 0);
    Be16(tbl, 3);
    tbl.resize(16, 0);
    Be32(tbl, 7);
    for (uint8_t c : {uint8_t('a'), uint8_t('b'), uint8_t(0), uint8_t(0x80), uint8_t(0x86),
                      uint8_t('c'), uint8_t(0)})
        tbl.push_back(c);
    tbl.resize(32, 0);
    std::vector<std::vector<std::string>> sec;
    check(ParseNameTbl(tbl, sec) && sec.size() == 1 && sec[0].size() == 2 && sec[0][0] == "ab",
          "name.tbl layout");
    std::string text;
    check(sec.size() == 1 &&
              DecodeAtlusText(
                  std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(sec[0][1].data()),
                                           sec[0][1].size()),
                  text) &&
              text == "&c",
          "atlus text glyph");
}

// ------------------------------------------------------------------ real data
std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}
bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
long PeakRssKiB() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmHWM:", 0) == 0)
            return std::strtol(line.c_str() + 6, nullptr, 10);
    return -1;
}

void TestReal(const std::string& romfs_dir, const std::string& unpack) {
    Romfs romfs;
    auto t0 = Clock::now();
    const bool opened = romfs.Open(LocalDirReader(romfs_dir));
    check(opened, "open local romfs: " + romfs.Error());
    if (!opened)
        return;
    std::printf("[time] %s (wall %.1f ms), rss %ld KiB\n", romfs.Describe().c_str(), Ms(t0),
                PeakRssKiB());
    check(romfs.Base().Count() == 94631, "ALL_USEU file count");
    // PATCH1 override: its only file is PATCH1/MOVIE/MOV000.USM.
    uint64_t size = 0;
    check(romfs.Patch().Count() == 1 && romfs.Stat("MOVIE/MOV000.USM", size) && size == 32973056,
          "PATCH1 entry reachable as MOVIE/MOV000.USM");
    Cpk::Entry base_mov;
    const bool base_has_mov = romfs.Base().Find("MOVIE/MOV000.USM", base_mov);
    std::printf("[info] ALL_USEU has MOVIE/MOV000.USM: %s (%llu bytes); PATCH1 copy wins\n",
                base_has_mov ? "yes" : "no", (unsigned long long)base_mov.extract);
    // Duplicate paths in the TOC?
    size_t dups = 0;
    std::string prev, path;
    Cpk::Entry e;
    for (size_t i = 0; i < romfs.Base().Count(); ++i) {
        romfs.Base().At(i, path, e);
        std::string up = path;
        for (auto& c : up)
            c = char(std::toupper(static_cast<unsigned char>(c)));
        dups += up == prev;
        prev = up;
    }
    std::printf("[info] duplicate TOC paths: %zu\n", dups);
    check(dups == 0, "no duplicate TOC paths");

    // Byte-exact vs the local unpack: the files the package needs + a stride sample of the TOC.
    std::vector<std::string> paths = {"EN/INIT/P5CAMP_00SPD.SPD",
                                      "EN/FONT/ICON.DDS",
                                      "EN/FONT/FONT0.FNT",
                                      "EN/BATTLE/TABLE/NAME.TBL",
                                      "EN/INIT/MYPTABLE.BIN",
                                      "EN/INIT/CMM.BIN",
                                      "BASE/SOUND/BGM.ACB",
                                      "EN/FIELD/PANEL/ROADMAP/ROADMAP.TBL",
                                      "EN/FIELD/FTD/FLDPLACENAME.FTD",
                                      "BASE/FIELD/FTD/FLDPLACENO.FTD",
                                      "BASE/FIELD/FTD/FLDDNGPLACENO.FTD",
                                      "en/camp/charatex/c_chara_02.dds",
                                      "\\EN\\CAMP\\CARDTEX\\C_CARD00.DDS"};
    for (const auto& f : kFiles)
        if (std::string_view(f.kind) != "dds" ||
            std::string_view(f.path).find("RMAP_001") != std::string_view::npos)
            paths.push_back(f.path);
    size_t sampled = 0, compressed = 0;
    for (size_t i = 0; i < romfs.Base().Count(); i += 211) {
        romfs.Base().At(i, path, e);
        if (e.extract > (size_t(8) << 20) || path.rfind("PATCH1/", 0) == 0)
            continue;
        paths.push_back(path);
        ++sampled;
        compressed += e.size != e.extract;
    }
    size_t exact = 0, bytes = 0;
    t0 = Clock::now();
    for (const auto& p : paths) {
        std::vector<uint8_t> got, want;
        std::string norm = p;
        for (auto& c : norm)
            c = c == '\\' ? '/' : char(std::toupper(static_cast<unsigned char>(c)));
        while (!norm.empty() && norm[0] == '/')
            norm.erase(0, 1);
        const bool ok = romfs.Read(p, got);
        const bool have = ReadFile(unpack + "/" + norm, want);
        check(ok && have && got == want, "byte-exact " + p);
        exact += ok && have && got == want;
        bytes += got.size();
    }
    std::printf(
        "[time] %zu files byte-exact vs unpack (%zu sampled, %zu CRILAYLA), %.1f MiB in %.0f ms\n",
        exact, sampled, compressed, bytes / 1048576.0, Ms(t0));
    std::vector<uint8_t> dummy;
    check(!romfs.Read("EN/NO_SUCH_FILE.BIN", dummy), "missing file fails");

    // Pillow hashes.
    for (const auto& f : kFiles) {
        const std::string kind = f.kind;
        t0 = Clock::now();
        if (kind == "spd") {
            auto spd = romfs.CachedSpd(f.path);
            check(spd != nullptr, std::string("spd parse ") + f.path);
            if (!spd)
                continue;
            Fnv agg;
            uint32_t n = 0;
            for (const auto& [id, idx] : spd->by_id) {
                (void)idx;
                Image im;
                if (!SpriteImage(romfs, f.path, id, im))
                    continue;
                agg.U32(id);
                agg.Img(im);
                ++n;
            }
            check(n == f.count && agg.h == f.hash, std::string("sprites == Pillow ") + f.path);
            std::printf("[time] %-45s %4u sprites %7.1f ms (%zu textures, file %.1f MiB)\n", f.path,
                        n, Ms(t0), spd->textures.size(), spd->bytes->size() / 1048576.0);
        } else if (kind == "icon") {
            Fnv agg;
            for (int c = 0; c < 84; ++c) {
                Image im;
                check(IconCell(romfs, c, im), "icon cell");
                agg.U32(uint32_t(c));
                agg.Img(im);
            }
            check(agg.h == f.hash, "ICON.DDS cells == Pillow");
            std::printf("[time] ICON.DDS 84 cells %.1f ms\n", Ms(t0));
        } else {
            Image im;
            const bool ok = DdsImage(romfs, f.path, im);
            check(ok && ImgHash(im) == f.hash, std::string("dds == Pillow ") + f.path);
        }
    }
    check(romfs.CacheBytes() <= (size_t(48) << 20) + (size_t(16) << 20), "cache within budget");
    std::printf("[mem] cache %.1f MiB after all sheets (budget 48), peak rss %ld KiB\n",
                romfs.CacheBytes() / 1048576.0, PeakRssKiB());

    // Load/decode timing measurements.
    {
        romfs.ClearCache();
        t0 = Clock::now();
        auto spd = romfs.CachedSpd("EN/INIT/P5CAMP_00SPD.SPD");
        const double load = Ms(t0);
        t0 = Clock::now();
        size_t pixels = 0;
        for (const auto& t : spd->textures) {
            Image im;
            check(DecodeDDS(t.dds, im), "P5CAMP texture");
            pixels += im.rgba.size();
        }
        std::printf(
            "[time] P5CAMP_00SPD read+CRILAYLA+parse %.1f ms; all %zu textures decoded %.1f ms "
            "(%.1f MiB RGBA if all kept)\n",
            load, spd->textures.size(), Ms(t0), pixels / 1048576.0);
        t0 = Clock::now();
        Image im;
        check(SpriteImage(romfs, "EN/INIT/P5CAMP_00SPD.SPD", 274, im), "sprite 274");
        std::printf("[time] one cached P5CAMP sprite (274, %ux%u) %.3f ms\n", im.w, im.h, Ms(t0));
        for (const char* p : {"EN/CAMP/CHARATEX/C_CHARA_02.DDS", "EN/CAMP/CHARATEX/C_CHARA_30.DDS",
                              "BASE/FIELD/PANEL/ROADMAP/RMAP_001_1_0.DDS",
                              "BASE/FIELD/PANEL/ROADMAP/RMAP_150_1_0.DDS"}) {
            t0 = Clock::now();
            std::vector<uint8_t> raw;
            romfs.Read(p, raw);
            const double r = Ms(t0);
            t0 = Clock::now();
            Image img;
            check(DecodeDDS(raw, img), std::string("decode ") + p);
            std::printf("[time] %-42s %ux%u read %.1f ms decode %.1f ms\n", p, img.w, img.h, r,
                        Ms(t0));
        }
    }

    // Tables.
    std::vector<std::vector<std::string>> sections;
    check(ReadNameTbl(romfs, sections) && sections.size() >= 2, "NAME.TBL");
    if (!sections.empty() && sections[0].size() > 2) {
        std::string a1, a2;
        DecodeAtlusText(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(sections[0][1].data()),
                                     sections[0][1].size()),
            a1);
        DecodeAtlusText(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(sections[0][2].data()),
                                     sections[0][2].size()),
            a2);
        check(a1 == "Fool" && a2 == "Magician",
              "NAME.TBL arcana 1/2 = Fool/Magician, got " + a1 + "/" + a2);
        std::printf("[info] NAME.TBL %zu sections:", sections.size());
        for (const auto& s : sections)
            std::printf(" %zu", s.size());
        std::printf("\n");
    }
    std::vector<MypSound> sounds;
    check(ReadMypSounds(romfs, sounds) && sounds.size() == p5r_bgm::ThievesDenRows,
          "MYPTABLE rows");
    t0 = Clock::now();
    std::vector<MusicTitle> titles;
    check(BuildMusicTitles(romfs, titles), "BuildMusicTitles");
    const double tt = Ms(t0);
    const size_t n_titles = std::size(p5r_bgm_ref::kTitles);
    check(titles.size() == n_titles, "music titles count " + std::to_string(titles.size()) +
                                         " vs " + std::to_string(n_titles));
    size_t matched = 0, art = 0;
    for (const auto& k : p5r_bgm_ref::kTitles) {
        const auto it = std::find_if(titles.begin(), titles.end(),
                                     [&](const MusicTitle& t) { return t.cue == k.cue; });
        const bool ok = it != titles.end() && it->row == k.row &&
                        it->inherited == (k.inherited != 0) &&
                        (it->art_only ? it->row >= 78 : TextFnv(it->text) == k.text_fnv);
        check(ok, "music title cue " + std::to_string(k.cue));
        matched += ok;
        art += ok && it->art_only;
    }
    std::printf(
        "[info] music titles: %zu/%zu match kTitles (%zu art-only rows >= 78), built in %.1f ms\n",
        matched, n_titles, art, tt);
    // Game-text tables the module reads at run time instead of embedding them (p5r_game_text.h).
    {
        t0 = Clock::now();
        p5r_text::GameText text;
        check(p5r_text::Load(romfs, text) && text.ready, "game text: " + text.error);
        const double tl = Ms(t0);
        // the arcana labels the module embedded up to v0.9.2 (p5r_social_reader.h ArcanaLabels)
        constexpr const char* OldArcana[32] = {
            "",         "Fool",       "Magician",   "High Priestess",
            "Empress",  "Emperor",    "Hierophant", "Lovers",
            "Chariot",  "Justice",    "Hermit",     "Fortune",
            "Strength", "Hanged Man", "Death",      "Temperance",
            "Devil",    "Tower",      "Star",       "Moon",
            "Sun",      "Judgement",  "Aeon",       "",
            "",         "",           "",           "",
            "",         "Faith",      "Councillor", "Faith"};
        check(std::string(text.Arcana(0)).empty(), "arcana 0 = none");
        for (unsigned i = 1; i < 32; ++i)
            if (OldArcana[i][0]) // 23..28 were blank in the old table: never published (ArcanaOfId)
                check(std::string(text.Arcana(i)) == OldArcana[i], "arcana " + std::to_string(i) +
                                                                       " = " + OldArcana[i] +
                                                                       ", got " + text.Arcana(i));
        constexpr const char* OldMembers[11] = {"",       "Protagonist", "Ryuji",  "Morgana",
                                                "Ann",    "Yusuke",      "Makoto", "Haru",
                                                "Futaba", "Akechi",      "Kasumi"};
        for (unsigned i = 1; i < 11; ++i)
            check(std::string(text.Member(i)) == OldMembers[i], "member " + std::to_string(i) +
                                                                    " = " + OldMembers[i] +
                                                                    ", got " + text.Member(i));
        check(std::string(text.Member(p5r_text::MemberSumire)) == "Sumire", "member 0x20 = Sumire");
        constexpr const char* OldStats[5] = {"Knowledge", "Charm", "Proficiency", "Guts",
                                             "Kindness"};
        for (unsigned i = 0; i < 5; ++i)
            check(std::string(text.Stat(i)) == OldStats[i], std::string("stat ") + OldStats[i]);
        size_t same = 0;
        for (const auto& k : p5r_bgm_ref::kTitles) {
            const auto* t = text.Music(k.cue);
            const bool ok =
                t && t->row == k.row && t->inherited == (k.inherited != 0) &&
                (t->art_only ? t->row >= 78 && t->text.empty() : TextFnv(t->text) == k.text_fnv);
            check(ok, "text.Music cue " + std::to_string(k.cue));
            same += ok;
        }
        check(text.music.size() == std::size(p5r_bgm_ref::kTitles), "text.music count");
        check(!text.Music(26) && !text.Music(-1) && !text.Music(70000), "untitled cues");
        std::printf("[info] game text: 31 arcana, %zu member names, 5 stats, %zu/%zu music titles "
                    "from romfs "
                    "in %.1f ms\n",
                    text.members.size(), same, std::size(p5r_bgm_ref::kTitles), tl);
        // fail closed: an unreadable romfs leaves every table empty
        Romfs empty;
        p5r_text::GameText none;
        const bool opened_empty =
            empty.Open([](const char*, uint64_t, void*, size_t) -> size_t { return 0; });
        check(!opened_empty || !p5r_text::Load(empty, none), "empty romfs: no tables");
        check(!none.ready && std::string(none.Arcana(1)).empty() &&
                  std::string(none.Member(2)).empty() && !none.Music(640),
              "fail closed: nothing published");
    }
    FtdTable cmm;
    std::vector<uint8_t> storage;
    check(ReadPakTable(romfs, "EN/INIT/CMM.BIN", "cmmFormat.ctd", cmm, storage) && cmm.rows == 38 &&
              cmm.row_size == 0xBC,
          "cmmFormat.ctd table");
    std::vector<uint8_t> ref;
    if (ReadFile(Env("P5R_CMM_REF"), ref) &&
        ref.size() >= 0x30 + 38 * 0xBC)
        check(std::equal(cmm.data.begin(), cmm.data.end(), ref.begin() + 0x30),
              "cmmFormat == local extract");
    std::vector<uint8_t> rm;
    std::vector<PakEntry> parts;
    check(romfs.Read("EN/FIELD/PANEL/ROADMAP/ROADMAP.TBL", rm) && ParsePak(rm, parts) &&
              !PakFind(parts, "texpack.bin").empty() && !PakFind(parts, "roadmap.bin").empty(),
          "ROADMAP.TBL parts");
    std::printf("[mem] index %.1f MiB, cache %.1f MiB, peak rss %ld KiB\n",
                romfs.IndexBytes() / 1048576.0, romfs.CacheBytes() / 1048576.0, PeakRssKiB());
}
} // namespace

int main() {
    TestSyntheticDds();
    TestUtf();
    TestPakFtdNameTbl();
    const std::string romfs = Env("P5R_ROMFS");
    const std::string unpack = Env("P5R_UNPACK");
    std::ifstream probe(romfs + "/CPK/ALL_USEU.CPK", std::ios::binary);
    if (!romfs.empty() && !unpack.empty() && probe &&
        std::ifstream(unpack + "/EN/FONT/ICON.DDS"))
        TestReal(romfs, unpack);
    else
        std::printf("SKIP real-data tests (no %s/CPK/ALL_USEU.CPK or unpack %s)\n", romfs.c_str(),
                    unpack.c_str());
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
