// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyward Sword HD art tests. Synthetic checks always run; with SSHD_ROMFS set to one or more
// colon-separated romfs roots (your own dump), builds all maps by default or the keys named
// on the command line. With SSHD_ART_OUT set, writes PNGs there for visual review.

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "sshd_art.h"

namespace {

uint32_t Crc(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

/// Minimal PNG writer (stored deflate blocks), test output only.
void WritePng(const std::string& path, const sshd::Img& img) {
    const auto rgba = img.Rgba8();
    std::vector<uint8_t> raw;
    for (int y = 0; y < img.h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<long>(y) * img.w * 4,
                   rgba.begin() + static_cast<long>(y + 1) * img.w * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t i = 0; i < raw.size(); i += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back(n & 255);
        z.push_back(n >> 8);
        z.push_back(~n & 255);
        z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + static_cast<long>(i), raw.begin() + static_cast<long>(i + n));
    }
    const uint32_t ad = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8)
        z.push_back(ad >> s & 255);
    std::ofstream f(path, std::ios::binary);
    auto be = [&](uint32_t v) {
        const uint8_t q[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        f.write(reinterpret_cast<const char*>(q), 4);
    };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be(static_cast<uint32_t>(data.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        f.write(reinterpret_cast<const char*>(td.data()), static_cast<long>(td.size()));
        be(Crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
    };
    f.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<uint8_t> ihdr(13, 0);
    for (int s = 0; s < 4; ++s) {
        ihdr[s] = img.w >> (24 - 8 * s) & 255;
        ihdr[4 + s] = img.h >> (24 - 8 * s) & 255;
    }
    ihdr[8] = 8;
    ihdr[9] = 6;
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
}

// Small big-endian containers exercise the public parser entry points without game files.
void Put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
    for (int k = 0; k < 4; ++k) b[o + k] = v >> (24 - 8 * k);
}
std::vector<uint8_t> Archive(const std::string& path, const std::vector<uint8_t>& data) {
    const size_t off = 0x40 + path.size();
    std::vector<uint8_t> b(off + data.size());
    Put32(b, 0, 0x55AA382D); Put32(b, 4, 0x20);
    b[0x20] = 1; Put32(b, 0x28, 2);
    Put32(b, 0x2C, 1); Put32(b, 0x30, off); Put32(b, 0x34, data.size());
    std::copy(path.begin(), path.end(), b.begin() + 0x39);
    std::copy(data.begin(), data.end(), b.begin() + off);
    return b;
}
void Malformed() {
    std::vector<uint8_t> tpl(96);
    Put32(tpl, 0, 0x0020AF30); Put32(tpl, 4, 1); Put32(tpl, 8, 12);
    Put32(tpl, 12, 20); tpl[21] = tpl[23] = 8; Put32(tpl, 28, 32);
    assert(sshd::TplDecode(tpl));
    auto no_image = tpl; Put32(no_image, 4, 0);
    assert(!sshd::TplDecode(no_image)); // only image zero is exposed; no caller-controlled index
    for (size_t off : {size_t(8), size_t(12), size_t(16)}) {
        auto bad = tpl; Put32(bad, off, 0xFFFFFFFC);
        assert(!sshd::TplDecode(bad)); // table / image / palette pointer wrap
    }
    auto badroot = Archive("x", {}); Put32(badroot, 4, 0xFFFFFFFC);
    assert(sshd::U8List(badroot).empty());
    // A complete ItemName fixture proves the same entry paths accept valid data.
    std::vector<uint8_t> valid(0x70);
    std::memcpy(valid.data(), "MsgStdBn", 8); valid[8] = 0xFE; valid[15] = 2;
    std::memcpy(valid.data() + 0x20, "LBL1", 4); Put32(valid, 0x24, 32);
    Put32(valid, 0x30, 1); Put32(valid, 0x34, 1); Put32(valid, 0x38, 12);
    valid[0x3C] = 13; std::memcpy(valid.data() + 0x3D, "NAME_ITEM_001", 13);
    std::memcpy(valid.data() + 0x50, "TXT2", 4); Put32(valid, 0x54, 12);
    Put32(valid, 0x60, 1); Put32(valid, 0x64, 8); valid[0x69] = 'A';
    auto name = [](const std::vector<uint8_t>& msbt) {
        const auto arc = Archive("0-Common/003-ItemGet.msbt", msbt);
        sshd::Art art([&](const std::string&) { return std::optional{arc}; });
        return art.ItemName(1);
    };
    assert(name(valid) == "A");
    for (int fault = 0; fault < 7; ++fault) {
        auto bad = valid;
        if (fault == 0) Put32(bad, 0x30, 0xFFFFFFFF); // LBL1 bucket table
        if (fault == 1) Put32(bad, 0x38, 0xFFFFFFFC); // bucket offset
        if (fault == 2) bad[0x3C] = 255; // label text/index cross section end
        if (fault == 3) Put32(bad, 0x24, 29); // truncated label index
        if (fault == 4) Put32(bad, 0x60, 0xFFFFFFFF); // TXT2 offset table
        if (fault == 5) Put32(bad, 0x64, 0xFFFFFFFC); // text offset beyond section
        if (fault == 6) Put32(bad, 0x64, 4); // text overlaps offset table
        assert(!name(bad));
    }
    // LBL1 bucket count, bucket offset, label length/index; TXT2 count and text ranges.
    for (const char* tag : {"LBL1", "TXT2"}) {
        for (int fault = 0; fault < 6; ++fault) {
            std::vector<uint8_t> b(0x60);
            std::memcpy(b.data(), "MsgStdBn", 8); b[8] = 0xFE; b[15] = 1;
            std::memcpy(b.data() + 0x20, tag, 4); Put32(b, 0x24, 32);
            Put32(b, 0x30, 1); Put32(b, 0x34, 1); Put32(b, 0x38, 12);
            if (fault == 0) Put32(b, 0x24, 0); // missing count
            if (fault == 1) Put32(b, 0x30, 0xFFFFFFFF); // oversized table
            if (fault == 2) Put32(b, 0x38, 0xFFFFFFFC); // bad label offset
            if (fault == 3) b[0x3C] = 255; // truncated label/index
            if (fault == 4) Put32(b, 0x34, 0xFFFFFFFC); // bad text offset/count
            if (fault == 5) { Put32(b, 0x34, 8); b[0x38] = 0; b[0x39] = 0x0E; b[0x3E] = b[0x3F] = 255; }
            const auto arc = Archive("0-Common/003-ItemGet.msbt", b);
            sshd::Art art([&](const std::string&) { return std::optional{arc}; });
            assert(!art.ItemName(1));
        }
    }
}

void TriangleCache() {
    const auto u8 = Archive("rarc/D101_r00.arc", Archive("kcl/room.kcl", std::vector<uint8_t>(32)));
    std::vector<uint8_t> lz = {0x11, uint8_t(u8.size()), uint8_t(u8.size() >> 8), uint8_t(u8.size() >> 16)};
    for (size_t i = 0; i < u8.size(); i += 8) {
        lz.push_back(0); // eight literals
        lz.insert(lz.end(), u8.begin() + i, u8.begin() + std::min(i + 8, u8.size()));
    }
    int reads = 0;
    sshd::Art art([&](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        if (path == "Stage/D101/NX/D101_stg_l0.arc.LZ") { ++reads; return lz; }
        return std::nullopt;
    });
    for (uint32_t mask : {1u, 3u, ~0u}) art.Map("D101", mask);
    assert(reads == 1); // each visited mask reuses the stage's parsed rooms
}

void Synthetic() {
    // LZ11: one literal run then a 3-byte back-reference (length 0x11, disp 1)
    const std::vector<uint8_t> lz = {0x11, 0x12, 0x00, 0x00, 0x40, 'A', 0x00, 0x00, 0x00};
    const auto out = sshd::Lz11(lz);
    assert(out && out->size() == 0x12 && (*out)[0] == 'A' && (*out)[0x11] == 'A');
    // I4 8x8 block: nibbles 0xF then 0x0
    std::vector<uint8_t> i4(32, 0xF0);
    const auto im = sshd::GxDecode(i4.data(), i4.size(), 8, 8, 0);
    assert(im && im->at(0, 0)[3] == 1.0f && im->at(1, 0)[3] == 0.0f);
    // resize keeps a flat colour flat
    sshd::Img flat(10, 10);
    for (size_t i = 0; i < flat.p.size(); i += 4) {
        flat.p[i] = 0.5f;
        flat.p[i + 3] = 1.0f;
    }
    const auto r = sshd::Resize(flat, 3, 7);
    assert(std::abs(r.at(1, 3)[0] - 0.5f) < 1e-4f && std::abs(r.at(1, 3)[3] - 1.0f) < 1e-4f);
    // Check every registered picture even without a game dump. Map and SkyMap allocate these
    // dimensions; four bytes per pixel must fit the runtime module image callback.
    for (const auto& area : sshd::AreaMaps()) {
        const int w = static_cast<int>((area.xmax - area.xmin) * area.ppu) + 2 * sshd::kMapPad;
        const int h = static_cast<int>((area.zmax - area.zmin) * area.ppu) + 2 * sshd::kMapPad;
        if (static_cast<size_t>(w) * h * 4 > 16u * 1024 * 1024)
            std::cerr << "oversize map/" << area.stage << " " << w << "x" << h << "\n";
        assert(static_cast<size_t>(w) * h * 4 <= 16u * 1024 * 1024);
    }
    assert(sshd::FindArea("F401") && sshd::FindArea("D100") && !sshd::FindArea("D999"));
}

// dev.32: PLC map classes as dBgS::GetMapCode (map colour analysis): att0 =
// (code3 >> 5) & 63 must be 32+ (else not on the map), base = att0 - 32, att1 = (code1 >> 17) & 7
// picks the subtype classes, and a Silent Realm trial turns target-through faces (code0 0x200000)
// into class 30.
void MapClasses() {
    const auto cls = [](int att0, int att1, bool through = false, bool trial = false) {
        return sshd::Art::MapClass(through ? 0x00200000u : 0u, static_cast<uint32_t>(att1) << 17,
                                   static_cast<uint32_t>(att0) << 5 | 12u /* special: water */, trial);
    };
    for (int a = 0; a < 32; ++a)
        assert(cls(a, 0) == -1 && cls(a, 1) == -1); // raw collision attributes are not map colours
    for (int b = 0; b < 31; ++b)
        assert(cls(32 + b, 0) == b);
    assert(cls(32 + 31, 0) == -1);
    const int remap[][3] = {{4, 1, 19}, {5, 1, 20}, {5, 2, 21}, {8, 1, 22}, {10, 1, 23}, {10, 2, 24},
                            {12, 1, 25}, {13, 1, 26}, {13, 2, 27}, {13, 3, 28}, {15, 1, 29}};
    for (const auto& r : remap)
        assert(cls(32 + r[0], r[1]) == r[2]);
    assert(cls(32 + 4, 2) == 4 && cls(32 + 1, 1) == 1 && cls(32 + 15, 2) == 15); // other subtypes keep the base
    assert(cls(32 + 6, 0, true, true) == 30 && cls(32 + 6, 0, true, false) == 6 && cls(32 + 6, 0, false, true) == 6);
    assert(cls(20, 0, true, true) == -1); // not map-tagged stays off even in a trial
    // the special code (water / lava behaviour) and ground code never pick the class
    assert(sshd::Art::MapClass(0, 0x1F << 20, (34u << 5) | 7u, false) == 2);
}

// Native colours and states on real data (dev.32): Skyloft's lake actor in water, Eldin's lava,
// the Lanayru past backgrounds, the Sealed Grounds by the temple in its own geometry and a
// dungeon's unvisited rooms in the native Map shade.
void CheckNativeColours(sshd::Art& art, const std::string& key, const sshd::Img& img) {
    const auto at = [&](int x, int y) { return art.LastMapClasses()[static_cast<size_t>(y) * img.w + x]; };
    if (key == "map/F000") {
        // world (8000, -4000), the lake east of the Bazaar (StageF000Water, class 7)
        assert(at(1340, 852) == 7);
        const float* px = img.at(1340, 852);
        assert(px[2] > px[0] + 0.2f); // blue
    }
    if (key == "map/F100")
        assert(at(1270, 590) == 7); // world (2200, -3800): the pond of oarc/WaterF100_b
    if (key == "map/D100")
        assert(at(905, 1447) == 7); // world (0, -500): room 2's water (oarc/WaterD100_r02)
    if (key == "map/D300") // world (6000, -5000): room 2's quicksand floor (dzb/StageSand02, subtype 1: class 29)
        assert(at(static_cast<int>((6000.0f + 9100.0f) * 0.075f) + 40, static_cast<int>((-5000.0f + 26100.0f) * 0.075f) + 40) == 29);
    if (key == "map/F300") {
        const auto& c = art.LastMapClasses();
        assert(std::count(c.begin(), c.end(), 4) == 0); // the present desert has no grass
        const auto past = art.Build("map/F300/4294967295/0/4294967295/1");
        const auto& pc = art.LastMapClasses();
        assert(past && std::count(pc.begin(), pc.end(), 4) > 100000); // the past variant is green
    }
    if (key == "map/F402") {
        const auto f401 = art.Build("map/F401");
        assert(f401 && f401->w == img.w && f401->h == img.h && f401->p != img.p);
    }
    if (key == "map/D003_0") {
        // Sky Keep's board from the native map dummy: every occupied cell's centre is drawn (its
        // tile's rock square at least), the empty cell is paper; without the Map only Link's tile
        const auto board = art.Build("skykeep/01234567-/0/1");
        assert(board && board->w == 1655 && board->h == 1655);
        const auto cell = [&](int i) {
            const int px = static_cast<int>((sshd::kSkyKeepPitch * (i % 3) + 3500.0f) * sshd::kSkyKeepPpu) + sshd::kMapPad;
            const int py = static_cast<int>((sshd::kSkyKeepPitch * (i / 3) + 3500.0f) * sshd::kSkyKeepPpu) + sshd::kMapPad;
            return art.LastMapClasses()[static_cast<size_t>(py) * board->w + px];
        };
        for (int i = 0; i < 8; ++i)
            assert(cell(i) != 0);
        assert(cell(8) == 0);
        const auto one = art.Build("skykeep/01234567-/3/0");
        assert(one && cell(3) != 0 && cell(0) == 0 && cell(4) == 0);
    }
    if (key == "map/D100") {
        // room 0 visited, the rest only revealed by the Map: dark olive, not the room's material
        const auto part = art.Build("map/D100/4294967295/0/1");
        assert(part && part->w == img.w);
        const auto& pc = art.LastMapClasses();
        int dark = 0, lit = 0;
        for (size_t i = 0; i < pc.size(); ++i) {
            if (pc[i] == 0)
                continue;
            const float* q = &part->p[i * 4];
            const float* f = &img.p[i * 4];
            if (q[0] < 0.6f && q[0] > q[2] + 0.1f && f[0] > q[0] + 0.15f)
                ++dark;
            else if (q[0] == f[0] && q[1] == f[1] && q[2] == f[2])
                ++lit;
        }
        assert(dark > 100000 && lit > 1000);
    }
}

// The native map class of a picture pixel (Art::KeepMapClasses), 0 = not drawn.
int ClassAt(const sshd::Art& art, const sshd::Img& img, int x, int y) {
    const auto& c = art.LastMapClasses();
    assert(c.size() == static_cast<size_t>(img.w) * img.h);
    return c[static_cast<size_t>(y) * img.w + x];
}

// A world (x, z) point's picture pixel on an area map (Art::Map's transform, generic frames' yaw).
std::pair<int, int> WorldPx(const sshd::AreaMap& a, float x, float z) {
    const float rc = a.rot == 1 ? 0.0f : a.rot == 2 ? -1.0f : a.rot == 3 ? 0.0f : 1.0f;
    const float rs = a.rot == 1 ? 1.0f : a.rot == 3 ? -1.0f : 0.0f;
    const float mx = x * rc - z * rs, mz = x * rs + z * rc;
    return {static_cast<int>((a.flip_h ? mx - a.xmin : a.xmax - mx) * a.ppu) + sshd::kMapPad,
            static_cast<int>((a.flip_v ? mz - a.zmin : a.zmax - mz) * a.ppu) + sshd::kMapPad};
}

// dev.36 (dev.36 analysis): the native interior edge line. Live F001r (Knight
// Academy) draws an opaque line in the stage's edge colour (140, 140, 120) along every height step:
// the spiral stair's 30-unit steps (grey step lines between white treads), the furniture (the wood
// keeps only a core in the native band-3 brown, never dev.35's darkened rim), the silhouette. Fields
// and dungeons draw no line (live D200 / F200 at their native default zoom).
void CheckEdges(sshd::Art& art, const std::string& key, const sshd::Img& img) {
    const auto& e = art.LastMapEdges();
    if ((!key.starts_with("map/") && !key.starts_with("gmapf/")) || key == "map/F020")
        return; // (F020 is the painted sky, not an Art::Map picture)
    assert(e.size() == static_cast<size_t>(img.w) * img.h);
    const auto edge = [&](int x, int y) { return static_cast<int>(e[static_cast<size_t>(y) * img.w + x]); };
    if (key == "map/F000" || key == "map/F200" || key == "map/D000") {
        assert(std::all_of(e.begin(), e.end(), [](uint8_t v) { return v == 0; })); // fields: no line
        return;
    }
    if (key == "map/D100" || key == "map/D200") {
        // a dungeon's zoomed native map (live D100 near zoom): the line in the stage's edge RGBA
        // (D100 110 / 110 / 100, D200 135 / 120 / 100), about 42 + 15 world units
        const float want[3] = {key == "map/D100" ? 110.0f : 135.0f, key == "map/D100" ? 110.0f : 120.0f, 100.0f};
        int full = 0;
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
                if (edge(x, y) == 255) {
                    const float* px = img.at(x, y);
                    for (int k = 0; k < 3; ++k)
                        assert(std::fabs(px[k] * 255 - want[k]) < 1.5f);
                    ++full;
                }
        assert(full > 5000);
        return;
    }
    if (key != "gmapf/F001r/-147/573/0" && key != "gmapf/F001r/445/1643/0")
        return;
    const auto* a = sshd::FindArea("F001r");
    assert(a != nullptr && a->gen);
    int full = 0, wood = 0, wood_line = 0, rim = 0, rim_line = 0;
    for (int y = 1; y + 1 < img.h; ++y)
        for (int x = 1; x + 1 < img.w; ++x) {
            const int c = ClassAt(art, img, x, y);
            const float* px = img.at(x, y);
            if (edge(x, y) == 255) {
                // the stage's MapGradation RGBA, opaque
                assert(std::fabs(px[0] * 255 - 140) < 1.5f && std::fabs(px[1] * 255 - 140) < 1.5f &&
                       std::fabs(px[2] * 255 - 120) < 1.5f);
                ++full;
            }
            if (c == 16) {
                ++wood;
                wood_line += edge(x, y) > 128;
                if (edge(x, y) == 0)
                    assert(px[0] > 0.40f); // the native band-3 wood (126, 76, 48), not dev.35's x 0.73 rim
            }
            if (c != 0 && (ClassAt(art, img, x - 1, y) == 0 || ClassAt(art, img, x + 1, y) == 0 ||
                           ClassAt(art, img, x, y - 1) == 0 || ClassAt(art, img, x, y + 1) == 0)) {
                ++rim;
                rim_line += edge(x, y) > 128;
            }
        }
    assert(full > 20000 && wood > 1000 && wood_line * 10 > wood * 3 && rim_line * 100 > rim * 98);
    if (key == "gmapf/F001r/-147/573/0") {
        // the spiral stair (centre (-61, -1340), treads 137..600 units out, 30 units per step):
        // around an arc 420 units out, the stone treads alternate with step lines
        int changes = 0, last = -1, treads = 0;
        for (int deg = -45; deg >= -265; --deg) {
            const float r = 3.14159265f / 180.0f * static_cast<float>(deg);
            const auto [x, y] = WorldPx(*a, -61.0f + 420.0f * std::cos(r), -1340.0f + 420.0f * std::sin(r));
            if (ClassAt(art, img, x, y) != 8)
                continue;
            const int on = edge(x, y) > 128 ? 1 : edge(x, y) < 64 ? 0 : last;
            treads += on == 0;
            if (last >= 0 && on != last)
                ++changes;
            last = on;
        }
        assert(changes >= 20 && treads > 40);
    }
}

// Real-data regression points, checked against the native Eldin map and the full-map
// contact sheet (dev.32: native +x right / +z down, so the points of the old picture turned
// 180 degrees). These outer mountain samples must stay off the map (their PLC records are not
// map-tagged), while the volcanic paths are drawn. Both stages share the same world bounds.
void CheckEldin(const sshd::Art& art, const std::string& key, const sshd::Img& img) {
    if (key == "map/S200") {
        // dev.34: the native trial's small blob: 0.02 px per unit, no paper (transparent corners)
        assert(img.w == 1280 && img.h == 1440 && img.at(0, 0)[3] == 0.0f);
        return;
    }
    if (key != "map/F200")
        return;
    assert(img.w == 1820 && img.h == 2052);
    for (const auto& [x, y] : {std::pair{364, 1642}, {1274, 718}, {546, 616}, {910, 308}})
        assert(ClassAt(art, img, x, y) == 0);
    // lava, dirt and sand on the native map (dev.32 PLC classes; the Silent Realm's trial draws
    // its target-through faces as class 30)
    for (const auto& [x, y] : {std::pair{1020, 1472}, {680, 1332}, {870, 1052}})
        assert(ClassAt(art, img, x, y) != 0);
    if (key == "map/F200")
        assert(ClassAt(art, img, 1020, 1472) == 6 && ClassAt(art, img, 680, 1332) == 1 && ClassAt(art, img, 870, 1052) == 3);
}

// Native D003_0/D003_1 show their current r00 with only visited bit 0 set. The stage
// suffix is not a room bit: bit 1 alone must not reveal r00, and adding it changes no pixels.
void CheckSkyKeep(sshd::Art& art, const std::string& key, const sshd::Img& full) {
    if (key != "map/D003_0" && key != "map/D003_1")
        return;
    const auto blank = art.Build(key + "/0");
    const auto one = art.Build(key + "/1");
    const auto two = art.Build(key + "/2");
    const auto both = art.Build(key + "/3");
    assert(blank && one && two && both);
    assert(full.p != blank->p && full.p == one->p);
    assert(blank->p == two->p && one->p == both->p);
}

// The Caves approach is a narrow corridor, not the mountain shell projected over it.
void CheckGorge(const sshd::Art& art, const std::string& key, const sshd::Img& img) {
    if (key != "map/F302")
        return;
    assert(img.w == 2398 && img.h == 1585);
    assert(ClassAt(art, img, 1346, 438) != 0); // world (0, -4800), native Caves approach
    for (int x : {1306, 1386}) // 1000 units either side must be outside the corridor
        assert(ClassAt(art, img, x, 438) == 0);
}

void CheckSanctuaryFloor(const sshd::Art& art, const std::string& key, const sshd::Img& img) {
    if (key != "map/D201/576/2" && key != "map/D201/4294966783/1")
        return;
    // Native B1 includes the r06 lava basin at world (0, -6000); 1F has no floor there.
    // This catches both omitting r06 from B1 and drawing its basement on the upper tab.
    const int basin = ClassAt(art, img, 1010, 1201);
    assert(key.ends_with("/2") ? basin != 0 : basin == 0);
    // The bridge and lava share XZ at (0, -10000), but belong to different tabs: B1 the lava
    // (class 6), 1F the stone bridge over it.
    const int bridge = ClassAt(art, img, 1010, 929);
    assert(bridge != 0);
    assert(key.ends_with("/2") ? bridge == 6 : bridge != 6);
}

void CheckSandshipFloor(sshd::Art& art, const std::string& key, const sshd::Img& img) {
    if (key == "map/D301/4096/4") {
        // Native B3 keeps the rising exit stairs with r12 rather than cutting them at a Y band.
        const auto whole_room = art.Build("map/D301/4096");
        assert(whole_room && whole_room->Rgba8() == img.Rgba8());
        return;
    }
    if (key != "map/D301/511/2" && key != "map/D301/3601/3")
        return;
    // Native B2's long right-hand corridor is part of r00, not a separate basement room.
    // At world (6500, 500), it must appear on B2 while B1 remains paper.
    const int corridor = ClassAt(art, img, 1760, 880);
    assert(key.ends_with("/3") ? corridor != 0 : corridor == 0);
}

// Native UI pieces of the 2026-11-11 owner round (compass, area plates, Collection panels).
// dev.46: mean luminance of a picture's opaque pixels in a box (x0, y0)-(x1, y1)
float MeanLum(const sshd::Img& img, int x0, int y0, int x1, int y1) {
    double s = 0, n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float* p = img.at(x, y);
            if (p[3] < 0.5f)
                continue;
            s += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            ++n;
        }
    return n > 0 ? static_cast<float>(s / n) : 0.0f;
}

void CheckDev46(sshd::Art& art, const std::string& key, const sshd::Img& img) {
    if (key == "optrow/394/104/1") {
        // FIT C17: the open Bag tab (the row's focus flash) is lighter than the plain row by >= 8 %
        const auto plain = art.Build("optrow/394/104/0");
        assert(plain && plain->w == img.w && plain->h == img.h);
        const float lit = MeanLum(img, 8, 2, img.w - 8, 96), off = MeanLum(*plain, 8, 2, img.w - 8, 96); // the body
        std::cout << "C17 optrow lit " << lit << " plain " << off << "\n";
        assert(lit >= off * 1.08f);
        // on the page board (#2E3125) against the unselected tabs (state 3, the row at 62 %): >= 8 % too
        const auto dim = art.Build("optrow/394/104/3");
        assert(dim);
        sshd::Img a(img.w, img.h), b(img.w, img.h);
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x) {
                float* pa = a.at(x, y);
                float* pb = b.at(x, y);
                pa[0] = pb[0] = 0x2E / 255.0f, pa[1] = pb[1] = 0x31 / 255.0f, pa[2] = pb[2] = 0x25 / 255.0f, pa[3] = pb[3] = 1;
            }
        sshd::Over(a, img, 0, 0);
        sshd::Over(b, *dim, 0, 0);
        std::cout << "C17 on the board: lit " << MeanLum(a, 8, 2, img.w - 8, 96) << " unselected " << MeanLum(b, 8, 2, img.w - 8, 96) << "\n";
        assert(MeanLum(a, 8, 2, img.w - 8, 96) >= MeanLum(b, 8, 2, img.w - 8, 96) * 1.08f);
        // and the label band itself stays the plain row (the white label keeps 4.5:1)
        assert(MeanLum(img, 60, 38, img.w - 60, 58) <= MeanLum(*plain, 60, 38, img.w - 60, 58) * 1.04f);
        // the row's visible body: rows 2..96, columns 8..386 (gen_manifest OPTROW_VIS)
        assert(img.at(img.w / 2, 1)[3] < 0.5f && img.at(img.w / 2, 3)[3] >= 0.5f);
        assert(img.at(img.w / 2, 95)[3] >= 0.5f && img.at(img.w / 2, 97)[3] < 0.5f);
        return;
    }
    if (key == "choice/250/120/0") {
        // visible body rows 7..112 (gen_manifest CHOICE_VIS); the ends never stretch: both ends
        // mirror each other column for column, the middle repeats one column
        assert(img.at(img.w / 2, 6)[3] < 0.5f && img.at(img.w / 2, 7)[3] >= 0.5f);
        assert(img.at(img.w / 2, 111)[3] >= 0.5f && img.at(img.w / 2, 112)[3] < 0.5f);
        for (int x = 0; x < 40; ++x)
            assert(std::fabs(img.at(x, 30)[3] - img.at(img.w - 1 - x, 30)[3]) < 0.02f);
        // states: selected and pressed light the rim, disabled dims the body
        const auto sel = art.Build("choice/250/120/1"), pr = art.Build("choice/250/120/2"), dis = art.Build("choice/250/120/3");
        assert(sel && pr && dis);
        const float n = MeanLum(img, 0, 0, img.w, img.h);
        assert(MeanLum(*sel, 0, 0, img.w, img.h) > n * 1.08f && MeanLum(*pr, 0, 0, img.w, img.h) > n * 1.08f);
        assert(dis->at(img.w / 2, img.h / 2)[3] < img.at(img.w / 2, img.h / 2)[3] - 0.2f); // the body dimmed
        return;
    }
    if (key == "floortab/0/0") {
        // dev.46: the floor tab body is the clamped 3-slice: its 40-unit left end and 40-unit right
        // end are mirror images (dev.45 stretched the left end over 80 units)
        // (a row 2 units under the top edge: above the scrolls, the label and the offset shadow)
        const int cx = img.w / 2, u = 2, y = img.h / 2 - 32 * u;
        for (int d = 0; d < 36 * u; ++d)
            for (int c = 0; c < 4; ++c)
                assert(std::fabs(img.at(cx - 60 * u + d, y)[c] - img.at(cx + 60 * u - 1 - d, y)[c]) < 0.02f);
        return;
    }
    if (key.starts_with("dopill/")) {
        // opaque navy body in the middle, transparent outside the tips' corners
        const float* mid = img.at(img.w / 2, img.h / 2);
        assert(mid[3] > 0.99f && mid[2] > mid[1] && mid[0] < 0.2f);
        assert(img.at(0, 0)[3] < 0.1f && img.at(img.w - 1, img.h - 1)[3] < 0.1f);
    }
}

void CheckNativeUi(sshd::Art& art, const std::string& key, const sshd::Img& img) {
    if (key == "compass") {
        // 128 layout units at 1.25 px; the north half red above the disc, the south half pale below
        assert(img.w == 160 && img.h == 160);
        const auto* north = img.at(80, 80 - 40);
        const auto* south = img.at(80, 80 + 40);
        assert(north[3] > 0.9f && north[0] > north[1] + 0.25f);
        assert(south[3] > 0.9f && south[0] > 0.7f && south[1] > 0.7f && south[2] > 0.6f);
        assert(img.at(2, 2)[3] == 0.0f); // transparent outside the disc
        return;
    }
    if (key == "cone/0" || key == "cone/90") {
        // centred on the head, widening away from it along the facing: cone/0 points up, cone/90
        // right; nothing behind the head
        const int c = sshd::kConeCanvas / 2;
        assert(img.w == sshd::kConeCanvas && img.h == sshd::kConeCanvas);
        const int far = c - 120;
        const float* ahead = key == "cone/0" ? img.at(c, far) : img.at(2 * c - far, c);
        const float* behind = key == "cone/0" ? img.at(c, 2 * c - far) : img.at(far, c);
        assert(ahead[3] > 0.2f && behind[3] == 0.0f);
        return;
    }
    if (key.starts_with("areaplate/")) {
        // the region colour shows in the middle of the strip: Faron green, Eldin red
        const int region = key.back() - '0';
        const auto* mid = img.at(img.w / 2 - 8, img.h * 51 / 112);
        if (region == 1)
            assert(mid[1] > mid[0] && mid[1] > mid[2]);
        if (region == 2)
            assert(mid[0] > mid[1] + 0.1f);
        return;
    }
    if (key.starts_with("cpanel/")) {
        // the green panel: a mint window with its drop shadow down-left, transparent top-left of it
        if (key.ends_with("/0")) {
            const auto* body = img.at(img.w / 2, img.h / 2);
            assert(body[3] > 0.95f && body[1] > body[0] && body[1] > body[2]);
            assert(img.at(4, 4)[3] == 0.0f);
            assert(img.at(4, img.h - 30)[3] > 0.0f);
        }
        return;
    }
    if (key.starts_with("cnum/")) {
        // every count is one box wide (two digits), right-aligned
        const auto box = art.Build("cnum/" + key.substr(5, key.find('/', 5) - 5) + "/88");
        assert(box && box->w == img.w);
        float right = 0, left = 0;
        for (int y = 0; y < img.h; ++y) {
            right += img.at(img.w - 8, y)[3];
            left += img.at(2, y)[3];
        }
        assert(right > left);
    }
}

} // namespace

int main(int argc, char** argv) {
    Synthetic();
    MapClasses();
    Malformed();
    TriangleCache();
    // dev.47: the map press lights its rim without washing out the map / Link marker.
    sshd::Art frame_art([](const std::string&) -> std::optional<std::vector<uint8_t>> { return std::nullopt; });
    const auto normal = frame_art.MinimapFrame();
    const auto pressed = frame_art.MinimapFrame(true);
    assert(normal && pressed && normal->w == 800 && pressed->h == 800);
    assert(normal->at(400, 400)[3] == 0 && pressed->at(400, 400)[3] == 0);
    assert(pressed->at(759, 400)[0] - normal->at(759, 400)[0] > 0.15f);
    assert(pressed->at(759, 400)[1] - normal->at(759, 400)[1] > 0.15f);
    for (int c = 0; c < 4; ++c)
        assert(pressed->at(0, 0)[c] == normal->at(0, 0)[c]);
    const char* roots = std::getenv("SSHD_ROMFS");
    if (roots == nullptr) {
        std::cout << "sshd art: synthetic ok (set SSHD_ROMFS for real-data keys)\n";
        return 0;
    }
    std::vector<std::string> dirs;
    std::stringstream ss(roots);
    for (std::string d; std::getline(ss, d, ':');)
        dirs.push_back(d);
    sshd::Art art([&](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        for (const auto& d : dirs) {
            std::ifstream f(d + "/" + path, std::ios::binary);
            if (f)
                return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
        }
        return std::nullopt;
    });
    // dev.34: SSHD_STAGE_BOUNDS=<id,id,...> prints each stage's drawn-face extent (the generic
    // sub-stage frames of sshd_stages.inc are generated from it)
    if (const char* list = std::getenv("SSHD_STAGE_BOUNDS")) {
        std::stringstream ls(list);
        for (std::string st; std::getline(ls, st, ',');) {
            const auto b = art.MapBounds(st);
            if (b)
                std::cout << "bounds " << st << " " << (*b)[0] << " " << (*b)[1] << " " << (*b)[2] << " " << (*b)[3] << "\n";
            else
                std::cout << "bounds " << st << " none\n";
        }
        return 0;
    }
    art.KeepMapClasses(true);
    const char* out = std::getenv("SSHD_ART_OUT");
    int fails = 0;
    std::vector<std::string> keys;
    // With no keys, a real-data run covers every registered picture. Explicit keys allow
    // focused art exports; the dimension/cap check above still covers all maps.
    if (argc == 1) {
        for (const auto& area : sshd::AreaMaps())
            keys.push_back(std::string("map/") + area.stage);
        for (const char* k : {"compass", "areaplate/645/120/1", "areaplate/910/120/2", "placeplate/400/60/1",
                              "cpanel/561/422/0", "cpanel/561/155/1", "cpanel/561/561/2", "cslot/122/0",
                              "ccheck/46/33", "cnum/24/7", "sil/sq/MiniGameBugs/tr_bugIcon_00", "cone/0",
                              "cone/90", "openchest", "goddesstag", "statue/0/1", "statue/1/0", "statue/2/0",
                              "door/0", "door/90", "place/2/60/1", "place/12/90/2", "thunderhead/0",
                              "thunderhead/1", "column/0", "column/1", "column/2", "islename/0/66", "islename/2/46",
                              "triforce", "ctitle/plate/34/600/67/MAP_06", "ctitle/title/34/760/67/MAP_04_S",
                              "gmapf/F001r/-100/600/0", "gmapf/F001r/-147/573/0", "gmapf/F001r/445/1643/0",
                              "stamtrack/372/392/1", "gmapf/F100_1/-1100/300/1024",
                              "gmapf/F100_1/-1100/300/512", "gmapf/F100_1/-1100/300/0",
                              // dev.46: the choice planks, the Options rows, the refusal pill, a floor tab
                              "choice/250/120/0", "choice/200/100/0/1", "optrow/394/104/1", "optrow/1208/104/0",
                              "dopill/216/44", "floortab/0/0"})
            keys.emplace_back(k);
        // dev.34: every generic sub-stage frame (sshd_stages.inc)
        for (const auto& g : sshd::GenStages())
            keys.push_back(std::string("map/") + g.stage);
    }
    for (int i = 1; i < argc; ++i)
        if (std::find(keys.begin(), keys.end(), argv[i]) == keys.end())
            keys.emplace_back(argv[i]);
    for (const auto& key : keys) {
        const auto img = art.Build(key);
        if (!img) {
            std::cout << "FAIL " << key << ": " << art.LastError() << "\n";
            ++fails;
            continue;
        }
        if (key.starts_with("map/"))
            assert(img->Rgba8().size() <= 16u * 1024 * 1024); // module image callback limit
        // dev.42: a map name fits its plate at the 18 px secondary cap or more
        if (key.starts_with("ctitle/")) {
            std::cout << "cap " << key << " " << art.LastFitCap() << "\n";
            if (art.LastFitCap() < sshd::Art::kMinPlateCap - 0.01f) {
                std::cout << "FAIL " << key << ": name needs cap " << art.LastFitCap() << " < 18\n";
                ++fails;
            }
        }
        if (out != nullptr) {
            std::string name = key;
            for (auto& c : name)
                if (c == '/' || c == ' ')
                    c = '_';
            WritePng(std::string(out) + "/" + name + ".png", *img);
            const auto& cls = art.LastMapClasses();
            if (key.starts_with("map/") && cls.size() == static_cast<size_t>(img->w) * img->h) {
                // the per-pixel native map classes, for review (binary PGM, value = class)
                std::ofstream f(std::string(out) + "/" + name + ".cls.pgm", std::ios::binary);
                f << "P5\n" << img->w << " " << img->h << "\n255\n";
                f.write(reinterpret_cast<const char*>(cls.data()), static_cast<std::streamsize>(cls.size()));
            }
        }
        CheckEdges(art, key, *img); // first: the checks below build more pictures
        CheckEldin(art, key, *img);
        CheckGorge(art, key, *img);
        CheckSanctuaryFloor(art, key, *img);
        CheckSandshipFloor(art, key, *img);
        CheckSkyKeep(art, key, *img);
        CheckNativeUi(art, key, *img);
        CheckNativeColours(art, key, *img);
        CheckDev46(art, key, *img);
        if (key.starts_with("map/") && sshd::FindArea(key.substr(4)) != nullptr && sshd::FindArea(key.substr(4))->gen)
            assert(img->w == sshd::kGenPx && img->h == sshd::kGenPx); // dev.34: the manifest's GEN frame
        if (key == "map/F301_4") {
            // dev.34: the rail lines (brown) between the platforms, e.g. the long loop's east end
            // (x 19273, z 13.6)
            const auto* a = sshd::FindArea("F301_4");
            const int x = static_cast<int>((19190.0f - a->xmin) * a->ppu) + sshd::kMapPad;
            const int y = static_cast<int>((13.6f - a->zmin) * a->ppu) + sshd::kMapPad;
            const float* px = img->at(x, y);
            assert(px[0] > px[2] + 0.2f && px[3] > 0.99f);
        }
        if (key.starts_with("gmapf/F100_1/-1100/300/")) {
            // dev.35 (item 2): the loaded layer's WaterSf pool (layer 3 y -100, layers 1 / 2 y -400)
            // over the tree's 1F at its centre (0, 0); without a layer the lake bed shows (dev.34)
            const auto* a = sshd::FindArea("F100_1");
            const int x = static_cast<int>(a->flip_h ? (0.0f - a->xmin) * a->ppu : (a->xmax - 0.0f) * a->ppu) + sshd::kMapPad;
            const int y = static_cast<int>(a->flip_v ? (0.0f - a->zmin) * a->ppu : (a->zmax - 0.0f) * a->ppu) + sshd::kMapPad;
            const bool layer = !key.ends_with("/0");
            assert((ClassAt(art, *img, x, y) == 7) == layer);
        }
        if (key == "map/S000" || key == "map/S200") {
            // dev.35 (item 4): the trial palette, solid faces (r, g, b) -> (b, r, r) (green == blue,
            // e.g. S000's lake-bed sand cyan), the scroll lava (S200) keeps its orange; no water in
            // S000 (the native draws the lake bed, not Skyloft's water)
            int solid = 0, lava = 0;
            for (int y = 0; y < img->h; ++y)
                for (int x = 0; x < img->w; ++x) {
                    const int c = ClassAt(art, *img, x, y);
                    const float* px = img->at(x, y);
                    assert(!(key == "map/S000" && c == 7));
                    if (c == 3 || c == 1) {
                        assert(std::fabs(px[1] - px[2]) < 0.02f && px[0] <= px[1] + 0.02f);
                        ++solid;
                    }
                    if (c == 6) {
                        assert(px[0] > px[2] + 0.3f);
                        ++lava;
                    }
                }
            assert(solid > 1000 && (key == "map/S000" || lava > 1000));
        }
        if (key == "ctitle/plate/34/600/67/MAP_06")
            assert(img->w == 600 && img->h == 67);
        if (key == "map/F301") {
            assert(img->w == 1976 && img->h == 2103);
            // The live port spawn remains on land after cropping and uses native +XZ axes.
            const auto* sea = sshd::FindArea("F301");
            const int x = static_cast<int>((12400.0f - sea->xmin) * sea->ppu) + sshd::kMapPad;
            const int y = static_cast<int>((0.0f - sea->zmin) * sea->ppu) + sshd::kMapPad;
            assert(sea->flip_h && sea->flip_v && ClassAt(art, *img, x, y) != 0);
        }
        std::cout << "ok " << key << " " << img->w << "x" << img->h << "\n";
    }
    return fails == 0 ? 0 : 1;
}
