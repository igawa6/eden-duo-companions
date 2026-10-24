// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_map.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
void Put(Bytes& b, size_t p, uint64_t v, size_t n = 4) {
    if (b.size() < p + n) b.resize(p + n);
    for (size_t i = 0; i < n; ++i) b[p + i] = uint8_t(v >> (8 * i));
}
void Text(Bytes& b, size_t p, const char* s) {
    if (b.size() < p + std::strlen(s) + 1) b.resize(p + std::strlen(s) + 1);
    std::memcpy(b.data() + p, s, std::strlen(s));
}
SmoMap::Resources Sand() {
    SmoMap::Resources r;
    r.image.width = r.image.height = 2048;
    r.view_proj = {0, 0, -2.3810000129742548e-5, -.2750000059604645,
                  -2.3810000129742548e-5, 0, 0, -.034523818641901016,
                  0, -.00010976000339724123, 0, .6210142970085144,
                  0, 0, 0, 1};
    return r;
}
Bytes Matrix(const std::array<double, 16>& m) {
    Bytes b(0x90);
    Text(b, 0, "YB"); Put(b, 2, 3, 2); Put(b, 4, 0x10); Put(b, 0xc, 0x30);
    Put(b, 0x10, 0x1c2); Put(b, 0x14, 8); Text(b, 0x18, "ViewProjMatrix");
    Put(b, 0x30, 0x1c1); Put(b, 0x34, 0xc0000000); Put(b, 0x38, 0x40);
    Put(b, 0x40, 0x10c0);
    for (size_t i = 0; i < 16; ++i) {
        b[0x44 + i] = 0xd2;
        const float f = float(m[i]); uint32_t bits; std::memcpy(&bits, &f, 4);
        Put(b, 0x54 + 4 * i, bits);
    }
    return b;
}
Bytes Bntx(const char* name) {
    Bytes b(0x110);
    Text(b, 0, "BNTX"); Put(b, 0x24, 1); Put(b, 0x28, 0x30, 8); Put(b, 0x30, 0x40, 8);
    Text(b, 0x40, "BRTI"); Put(b, 0x52, 1, 2); Put(b, 0x5c, 0x1a01);
    Put(b, 0x64, 4); Put(b, 0x68, 4); Put(b, 0x90, 8);
    Put(b, 0xa0, 0xc0, 8); Put(b, 0xb0, 0xf0, 8);
    Put(b, 0xc0, std::strlen(name), 2); Text(b, 0xc2, name); Put(b, 0xf0, 0x100, 8);
    // BC1 black, white, grey, transparent: exercises both palette branches and alpha.
    Put(b, 0x100, 0, 2); Put(b, 0x102, 0xffff, 2); Put(b, 0x104, 0xe4e4e4e4);
    return b;
}
Bytes Bfres() {
    const auto texture = Bntx("SandWorldHomeStage");
    Bytes b(0x100 + texture.size());
    Text(b, 0, "FRES"); Put(b, 8, 0x00080000); Put(b, 0xc, 0xfeff, 2);
    Put(b, 0x1c, b.size()); Put(b, 0x98, 0xd0, 8); Put(b, 0xc8, 1, 2);
    Put(b, 0xd0, 0x100, 8); Put(b, 0xd8, texture.size(), 8);
    std::memcpy(b.data() + 0x100, texture.data(), texture.size());
    return b;
}
Bytes Archive(const Bytes& bfres, const Bytes& matrix) {
    Bytes b(0x100);
    Text(b, 0, "SARC"); Put(b, 4, 0x14, 2); Put(b, 6, 0xfeff, 2);
    Put(b, 0xc, 0x100); Text(b, 0x14, "SFAT"); Put(b, 0x18, 0xc, 2);
    Put(b, 0x1a, 2, 2); Text(b, 0x40, "SFNT"); Put(b, 0x44, 8, 2);
    Text(b, 0x48, "Texture2dMap.bfres"); Text(b, 0x60, "SandWorldHomeStage.byml");
    Put(b, 0x24, 0x1000000); Put(b, 0x28, 0); Put(b, 0x2c, bfres.size());
    Put(b, 0x34, 0x1000006); Put(b, 0x38, bfres.size());
    Put(b, 0x3c, bfres.size() + matrix.size());
    b.insert(b.end(), bfres.begin(), bfres.end()); b.insert(b.end(), matrix.begin(), matrix.end());
    Put(b, 8, b.size());
    return b;
}
void SetYaz0Size(Bytes& b, uint32_t n) {
    for (size_t i = 0; i < 4; ++i) b[4 + i] = uint8_t(n >> (24 - 8 * i));
}
Bytes Yaz0(const Bytes& raw, bool runs) {
    Bytes b(16); Text(b, 0, "Yaz0"); SetYaz0Size(b, uint32_t(raw.size()));
    size_t at = 0;
    while (at < raw.size()) {
        const size_t code_at = b.size(); b.push_back(0);
        for (unsigned bit = 0; bit < 8 && at < raw.size(); ++bit) {
            size_t n = 0;
            if (runs && at) {
                while (at + n < raw.size() && n < 273 && raw[at + n] == raw[at - 1]) ++n;
            }
            if (n >= 3) {
                // Distance one exercises overlapping copies, including long-run byte lengths.
                if (n >= 18) { b.push_back(0); b.push_back(0); b.push_back(uint8_t(n - 18)); }
                else { b.push_back(uint8_t((n - 2) << 4)); b.push_back(0); }
                at += n;
            } else {
                b[code_at] |= uint8_t(0x80 >> bit); b.push_back(raw[at++]);
            }
        }
    }
    return b;
}
void TestYaz0() {
    const auto archive = Archive(Bfres(), Matrix(Sand().view_proj));
    const auto literal = Yaz0(archive, false), runs = Yaz0(archive, true);
    assert(runs.size() < literal.size());
    for (const auto& packed : {literal, runs}) {
        const auto r = SmoMap::DecodeArchive(packed, "SandWorldHomeStage");
        assert(r && r->image.width == 4 && r->image.rgba.size() == 64);
    }
    auto bad = literal; bad.resize(15); // short header
    assert(!SmoMap::DecodeArchive(bad, "SandWorldHomeStage"));
    bad = literal; bad.pop_back(); // truncated final literal
    assert(!SmoMap::DecodeArchive(bad, "SandWorldHomeStage"));
    bad = literal; SetYaz0Size(bad, 0);
    assert(!SmoMap::DecodeArchive(bad, "SandWorldHomeStage"));
    bad = literal; SetYaz0Size(bad, 64 * 1024 * 1024 + 1);
    assert(!SmoMap::DecodeArchive(bad, "SandWorldHomeStage"));
    bad = Bytes(16); Text(bad, 0, "Yaz0"); SetYaz0Size(bad, 3);
    // Backreference before any output, then truncated two-byte and long forms.
    bad.insert(bad.end(), {0, 0x10, 0});
    assert(!SmoMap::DecodeArchive(bad, "SandWorldHomeStage"));
    bad = Bytes(16); Text(bad, 0, "Yaz0"); SetYaz0Size(bad, 20);
    bad.insert(bad.end(), {0x80, 'S', 0});
    assert(!SmoMap::DecodeArchive(bad, "SandWorldHomeStage"));
    bad.push_back(0); // long reference lacks the extra length byte
    assert(!SmoMap::DecodeArchive(bad, "SandWorldHomeStage"));
    bad.push_back(255); // reference length exceeds declared output
    assert(!SmoMap::DecodeArchive(bad, "SandWorldHomeStage"));
}
void Near(double a, double b, double tolerance = .002) { assert(std::abs(a - b) < tolerance); }
void TestProjection() {
    auto r = Sand();
    const auto shop = SmoMap::Project(r, {23055, -300, -11315}); assert(shop);
    Near(shop->x, 1018.276); Near(shop->y, -1621.466);
    const auto mario = SmoMap::Project(r, {32900.0039f, -1149.7222f, -12699.9961f}); assert(mario);
    Near(mario->x, 1052.044); Near(mario->y, -1861.502);
    const auto high = SmoMap::Project(r, {23055, 8000, -11315}); assert(high);
    Near(high->x, shop->x); Near(high->y, shop->y);
    const std::array<float, 3> origin{23055, -300, -11315};
    Near(*SmoMap::Heading(r, origin, {-1, 0, 0}), 0);
    Near(*SmoMap::Heading(r, origin, {0, 0, -1}), 90);
    Near(*SmoMap::Heading(r, origin, {1, 0, 0}), 180);
    Near(*SmoMap::Heading(r, origin, {0, 0, 1}), 270);
    assert(!SmoMap::Heading(r, origin, {0, 1, 0}));
    assert(!SmoMap::Project(r, {std::numeric_limits<float>::infinity(), 0, 0}));
    r.view_proj[15] = 0; assert(!SmoMap::Project(r, origin));
    r = Sand(); r.view_proj[0] = std::numeric_limits<double>::quiet_NaN();
    assert(!SmoMap::Project(r, origin));
}
void TestArchive() {
    const auto matrix = Matrix(Sand().view_proj), bfres = Bfres();
    const auto good = Archive(bfres, matrix);
    const auto decoded = SmoMap::DecodeArchive(good, "SandWorldHomeStage");
    assert(decoded && decoded->image.width == 4 && decoded->image.rgba.size() == 64);
    assert(!SmoMap::DecodeArchive(good, "MissingStage"));
    assert(!SmoMap::DecodeArchive(good, "../SandWorldHomeStage"));
    assert(!SmoMap::DecodeArchive({}, "SandWorldHomeStage"));
    auto bad = matrix; bad[0x44] = 0xd1; // integer masquerading as a matrix float
    assert(!SmoMap::DecodeArchive(Archive(bfres, bad), "SandWorldHomeStage"));
    bad = matrix; Put(bad, 0x40, 0xfc0); // wrong element count
    assert(!SmoMap::DecodeArchive(Archive(bfres, bad), "SandWorldHomeStage"));
    bad = matrix; Put(bad, 0x54, 0x7fc00000); // NaN
    assert(!SmoMap::DecodeArchive(Archive(bfres, bad), "SandWorldHomeStage"));
    auto zero_w = Sand().view_proj; zero_w[15] = 0;
    assert(!SmoMap::DecodeArchive(Archive(bfres, Matrix(zero_w)), "SandWorldHomeStage"));
    bad = bfres; Put(bad, 8, 0x00050003);
    assert(!SmoMap::DecodeArchive(Archive(bad, matrix), "SandWorldHomeStage"));
    bad = bfres; Put(bad, 0xd0, UINT64_MAX, 8);
    assert(!SmoMap::DecodeArchive(Archive(bad, matrix), "SandWorldHomeStage"));
    bad = bfres; Put(bad, 0xd8, 4, 8); // valid magic, undeclared body must not be read
    assert(!SmoMap::DecodeArchive(Archive(bad, matrix), "SandWorldHomeStage"));
    bad = good; Put(bad, 0x3c, UINT32_MAX);
    assert(!SmoMap::DecodeArchive(bad, "SandWorldHomeStage"));
    assert(!SmoMap::DecodeArchive(std::span(good).first(0x100), "SandWorldHomeStage"));
}
void TestFacingArc() {
    auto bytes = Bntx("NyIconView^t");
    bytes.resize(0x100 + 16 * 16 * 8);
    Put(bytes, 0x64, 64); Put(bytes, 0x68, 64); Put(bytes, 0x90, 16 * 16 * 8);
    // Transparent BC1 quadrant with one interior 4x4 white block. Keeping the border clear
    // makes quarter-turn ink preservation exact around the specified pixel origin (64,64).
    for (size_t block = 0; block < 16 * 16; ++block) {
        Put(bytes, 0x100 + block * 8, 0, 2);
        Put(bytes, 0x102 + block * 8, 0xffff, 2);
        Put(bytes, 0x104 + block * 8, 0xffffffff);
    }
    Put(bytes, 0x104 + (9 * 16 + 8) * 8, 0x55555555);
    const auto original = fe3h_ui2d::BntxDecode(bytes, "NyIconView^t"); assert(original);
    const std::array<std::array<unsigned, 2>, 4> locations{{{32,36}, {92,32}, {96,92}, {36,96}}};
    for (unsigned turn = 0; turn < 4; ++turn) {
        const auto arc = SmoMap::DecodeFacingArc(bytes, turn * 90); assert(arc);
        assert(arc->width == 128 && arc->height == 128 && arc->rgba.size() == 128 * 128 * 4);
        size_t ink = 0;
        for (size_t p = 0; p < arc->rgba.size(); p += 4) {
            if (!arc->rgba[p + 3]) continue;
            ++ink;
            for (size_t channel = 0; channel < 4; ++channel) assert(arc->rgba[p + channel] == 255);
        }
        assert(ink == 16);
        const auto& at = locations[turn];
        const size_t p = (size_t(at[1]) * 128 + at[0]) * 4;
        assert(arc->rgba[p + 3] == 255);
        assert(arc->rgba[(64 * 128 + 64) * 4 + 3] == 0);
    }
    const auto zero = SmoMap::DecodeFacingArc(bytes, 0); assert(zero);
    for (size_t y = 0; y < 64; ++y) {
        for (size_t x = 0; x < 64; ++x) {
            const size_t src = (y * 64 + x) * 4, dst = (y * 128 + x) * 4;
            for (size_t c = 0; c < 4; ++c) assert(zero->rgba[dst + c] == original->rgba[src + c]);
        }
    }
    const auto diagonal = SmoMap::DecodeFacingArc(bytes, 45); assert(diagonal);
    bool visible = false;
    for (size_t p = 3; p < diagonal->rgba.size(); p += 4) visible |= diagonal->rgba[p] != 0;
    assert(visible);
    assert(SmoMap::DecodeFacingArc(bytes, 359));
    assert(!SmoMap::DecodeFacingArc(bytes, 360));
    assert(!SmoMap::DecodeFacingArc(bytes, std::numeric_limits<unsigned>::max()));
    assert(!SmoMap::DecodeFacingArc({}, 0));
    assert(!SmoMap::DecodeFacingArc(Bntx("NyIconView^t"), 0)); // wrong dimensions
}
void TestPlayerPin() {
    const auto bytes = Bntx("CmIconPlayer^t");
    const auto before = fe3h_ui2d::BntxDecode(bytes, "CmIconPlayer^t");
    const auto after = SmoMap::DecodePlayerPin(bytes); assert(before && after);
    bool dark = false, light = false, transparent = false;
    for (size_t p = 0; p < before->rgba.size(); p += 4) {
        const bool d = before->rgba[p] < 128;
        dark |= d; light |= !d; transparent |= before->rgba[p + 3] == 0;
        assert(after->rgba[p] == (d ? 255 : 0xe6));
        assert(after->rgba[p + 1] == (d ? 255 : 0x10));
        assert(after->rgba[p + 2] == (d ? 255 : 0x1e));
        assert(after->rgba[p + 3] == before->rgba[p + 3]);
    }
    assert(dark && light && transparent);
    assert(!SmoMap::DecodePlayerPin({}));
}
void TestRealArchive() {
    const char* path = std::getenv("SMO_MAP_ARCHIVE");
    if (!path || !*path) { std::puts("smo_map: real archive skipped (SMO_MAP_ARCHIVE unset)"); return; }
    std::ifstream file(path, std::ios::binary); assert(file);
    const Bytes bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto r = SmoMap::DecodeArchive(bytes, "SandWorldHomeStage"); assert(r);
    assert(r->image.width == 2048 && r->image.height == 2048);
    assert(r->image.rgba.size() == 2048 * 2048 * 4);
    const auto shop = SmoMap::Project(*r, {23055, -300, -11315}); assert(shop);
    Near(shop->x, 1018.276); Near(shop->y, -1621.466);
    std::puts("smo_map: real Sand texture and native matrix passed");
}
// Map marker polish (owner-reviewed capture of Sand with moons on): reduce moon-marker/POI
// overlap and keep markers from rendering under fixed HUD overlays.
void TestNudgeAwayFromConflicts() {
    const SmoMap::Point shop{100, -100};
    // Far from any conflict: untouched.
    {
        const auto r = SmoMap::NudgeAwayFromConflicts({500, -500}, {shop}, 28, 24);
        assert(r.x == 500 && r.y == -500);
    }
    // Inside the separation threshold: moved further away from the conflicting point, by
    // exactly `offset` pixels, along the line away from it.
    {
        const SmoMap::Point candidate{110, -100}; // 10px from shop, inside the 28px threshold
        const auto r = SmoMap::NudgeAwayFromConflicts(candidate, {shop}, 28, 24);
        const double moved = std::hypot(r.x - candidate.x, r.y - candidate.y);
        Near(moved, 24);
        const double after_dist = std::hypot(r.x - shop.x, r.y - shop.y);
        assert(after_dist > std::hypot(candidate.x - shop.x, candidate.y - shop.y));
    }
    // Exactly coincident: a deterministic direction is still produced (no NaN / zero vector).
    {
        const auto r = SmoMap::NudgeAwayFromConflicts(shop, {shop}, 28, 24);
        assert(std::isfinite(r.x) && std::isfinite(r.y));
        Near(std::hypot(r.x - shop.x, r.y - shop.y), 24);
    }
    // Multiple conflicts in range each push the candidate once, cumulatively (asymmetric
    // placement, so the pushes cannot exactly cancel out as a perfectly symmetric pair would).
    {
        const SmoMap::Point a{90, -100}, b{100, -112};
        const auto r = SmoMap::NudgeAwayFromConflicts({100, -100}, {a, b}, 28, 10);
        assert(r.x != 100 || r.y != -100);
    }
    // Empty conflict list: untouched.
    {
        const auto r = SmoMap::NudgeAwayFromConflicts({5, -5}, {}, 28, 24);
        assert(r.x == 5 && r.y == -5);
    }
}
} // namespace
int main() {
    for (const auto [slots,rows] : std::array<std::pair<int,int>,9>{{
            {0,0},{1,1},{10,1},{11,2},{20,2},{21,2},{30,2},{31,3},{1000,3}}}) {
        const auto layout=SmoMap::LayoutForMoonSlots(slots);
        assert(layout.rows==rows && layout.map_height==856-(layout.large?88:56)*rows);
        assert(layout.footer_y==112+layout.map_height);
        const int columns=layout.large?10:15;
        for(int i=0;i<std::min(slots,40);++i){
            const int y=layout.footer_y+(layout.large?100:108)+(layout.large?88:56)*(i/columns);
            assert(y>=layout.footer_y+96 && y+(layout.large?80:40)<=1080);
        }
        // A smaller Reset remains inside every viewport, with a 112px touch target.
        assert(layout.footer_y-124>=112 && layout.footer_y-12<=layout.footer_y);
    }
    const auto projection = SmoMap::DecodeProjection(Matrix(Sand().view_proj), 2048, 2048);
    assert(projection && projection->image.rgba.empty());
    const auto p = SmoMap::Project(*projection, {32900, -1150, -12700});
    assert(p); Near(p->x, 1052.044); Near(p->y, -1861.502);
    assert(!SmoMap::DecodeProjection({}, 2048, 2048));
    assert(!SmoMap::DecodeProjection(Matrix(Sand().view_proj), 0, 2048));
    assert(!SmoMap::DecodeProjection(Matrix(Sand().view_proj), 2048, 4097));
    TestProjection(); TestArchive(); TestYaz0(); TestFacingArc(); TestPlayerPin(); TestRealArchive();
    TestNudgeAwayFromConflicts();
    std::puts("smo_map: projection, heading, archive bounds, matrix validation and pin colour passed");
}
