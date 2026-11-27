// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Avatar name and portrait: synthetic checks always run (name decode on a synthetic buffer, the
// 0x90 block validator, the tint/cell composition); with XCX_ROMFS naming the player's RomFS, the
// BDAT lookups run on the real archive: hair 72 -> cell 1 and hair 232 -> cell 57, the two fixture
// avatars (Zanemato, Terra), and refusals.
#include "xcx_avatar.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
namespace fs = std::filesystem;
namespace {
int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
        }                                                                                          \
    } while (0)
using Block = std::array<std::uint8_t, xcx::AvatarBlockSize>;
Block MakeBlock(const char *name, unsigned face, unsigned hair, unsigned sex, unsigned c0,
                unsigned c5) {
    Block b{};
    std::memcpy(b.data(), name, std::strlen(name));
    b[0x40] = std::uint8_t(std::strlen(name));
    const auto put = [&](std::size_t o, unsigned v) {
        b[o] = std::uint8_t(v);
        b[o + 1] = std::uint8_t(v >> 8);
    };
    put(0x44, face);
    put(0x46, hair);
    put(0x52, sex);
    put(0x58, c0);
    put(0x62, c5);
    b[0x78] = 1;
    return b;
}
std::string Name(const std::string &raw) {
    return xcx::DecodeActorName(reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size());
}
void NameDecode() {
    std::string field(0x40, '\0');
    std::memcpy(field.data(), "Zanemato", 8);
    CHECK(Name(field) == "Zanemato");
    // Stale bytes after the terminator (the game does not clear the buffer) are ignored.
    std::memcpy(field.data() + 9, "SA B-Gatling", 12);
    CHECK(Name(field) == "Zanemato");
    // UTF-8 names (two, three and four byte sequences) pass through unchanged.
    std::string utf(0x40, '\0');
    const std::string jp = "\xc3\xa4Z\xe3\x82\xbf\xf0\x9f\x98\x80";
    std::memcpy(utf.data(), jp.data(), jp.size());
    CHECK(Name(utf) == jp);
    // Refusals: unterminated, empty, control byte, lone continuation byte, truncated sequence,
    // overlong lead, DEL.
    CHECK(Name(std::string(0x40, 'A')).empty());
    CHECK(Name(std::string(0x40, '\0')).empty());
    std::string bad(0x40, '\0');
    bad[0] = 'A';
    bad[1] = '\x07';
    CHECK(Name(bad).empty());
    bad = std::string(0x40, '\0');
    bad[0] = '\x80';
    CHECK(Name(bad).empty());
    bad = std::string(0x40, '\0');
    bad[0] = 'A';
    bad[1] = '\xe3';
    bad[2] = '\x82'; // terminator where the third byte belongs
    CHECK(Name(bad).empty());
    bad = std::string(0x40, '\0');
    bad[0] = '\xff';
    CHECK(Name(bad).empty());
    bad = std::string(0x40, '\0');
    bad[0] = '\x7f';
    CHECK(Name(bad).empty());
    CHECK(xcx::DecodeActorName(nullptr, 0).empty());
}
void BlockDecode() {
    auto b = MakeBlock("Zanemato", 58, 72, 1, 115, 447);
    auto look = xcx::DecodeAvatarBlock(b, "Zanemato");
    CHECK(look.valid && look.face == 58 && look.hair == 72 && look.sex == 1 && look.color0 == 115 &&
          look.color5 == 447);
    CHECK(!xcx::DecodeAvatarBlock(b, "Elma").valid);     // another actor's name
    CHECK(!xcx::DecodeAvatarBlock(b, "").valid);         // no actor name
    CHECK(!xcx::DecodeAvatarBlock(std::span(b).first(0x8f), "Zanemato").valid); // short
    auto bad = b;
    bad[0x40] = 7; // length field disagrees
    CHECK(!xcx::DecodeAvatarBlock(bad, "Zanemato").valid);
    bad = b;
    bad[0x78] = 2; // a story character's record
    CHECK(!xcx::DecodeAvatarBlock(bad, "Zanemato").valid);
    bad = b;
    bad[0x79] = 11;
    CHECK(!xcx::DecodeAvatarBlock(bad, "Zanemato").valid);
    bad = b;
    bad[0x46] = bad[0x47] = 0; // no hair
    CHECK(!xcx::DecodeAvatarBlock(bad, "Zanemato").valid);
    bad = b;
    bad[0x47] = 0xff; // absurd hair id
    CHECK(!xcx::DecodeAvatarBlock(bad, "Zanemato").valid);
    bad = b;
    bad[0x52] = 3;
    CHECK(!xcx::DecodeAvatarBlock(bad, "Zanemato").valid);
    bad = b;
    bad[0x63] = 0x7f; // palette id out of range
    CHECK(!xcx::DecodeAvatarBlock(bad, "Zanemato").valid);
    bad = b;
    std::fill(bad.begin(), bad.begin() + 0x40, std::uint8_t('A')); // unterminated name
    bad[0x40] = 0x40;
    CHECK(!xcx::DecodeAvatarBlock(bad, std::string(0x3f, 'A')).valid);
    // Terra: the female fixture avatar.
    auto t = xcx::DecodeAvatarBlock(MakeBlock("Terra", 66, 232, 2, 115, 336), "Terra");
    CHECK(t.valid && t.sex == 2 && t.hair == 232 && t.color5 == 336);
}
xcx::Image Flat(int v, int r, int g, int b) {
    xcx::Image im(1792, 1008);
    for (std::size_t p = 0; p < im.rgba.size(); p += 4) {
        im.rgba[p] = std::uint8_t(r);
        im.rgba[p + 1] = std::uint8_t(g);
        im.rgba[p + 2] = std::uint8_t(b);
        im.rgba[p + 3] = std::uint8_t(v);
    }
    return im;
}
void Compose() {
    const auto base = Flat(255, 100, 100, 100);
    auto mask = Flat(255, 0, 0, 0);
    // Cell 17 = column 1, row 1: its left half is hair (mask red), its right half is not.
    for (int y = 0; y < 112; ++y)
        for (int x = 0; x < 56; ++x)
            mask.rgba[((112 + y) * std::size_t(1792) + 112 + x) * 4] = 255;
    for (int y = 0; y < 112; ++y)
        for (int x = 56; x < 100; ++x) // skin: green mask only
            mask.rgba[((112 + y) * std::size_t(1792) + 112 + x) * 4 + 1] = 255;
    xcx::AvatarPortraitSpec spec{17, {0.404f, 0.243f, 0.161f, 1.148f}};
    const auto out = xcx::ComposeAvatar(base, mask, spec);
    CHECK(out.w == 112 && out.h == 112);
    const auto *hair = out.rgba.data() + (50 * 112 + 10) * 4;
    const auto *skin = out.rgba.data() + (50 * 112 + 70) * 4;
    CHECK(hair[0] == 176 && hair[1] == 92 && hair[2] == 59 && hair[3] == 255); // base x (2*colour*a) x gain
    CHECK(skin[0] == 176 && skin[1] == 176 && skin[2] == 176); // lit only, outside the hair mask
    const auto *cloth = out.rgba.data() + (50 * 112 + 105) * 4;
    CHECK(cloth[0] == 100 && cloth[1] == 100 && cloth[2] == 100); // no mask: not lit
    // A neutral palette entry (0.5, a = 1) leaves just the hair gain.
    spec.tint = {0.5f, 0.5f, 0.5f, 1.f};
    const auto neutral = xcx::ComposeAvatar(base, mask, spec);
    CHECK(neutral.rgba[(50 * 112 + 10) * 4] == 190);
    // Refusals: wrong atlas size, cell out of range.
    CHECK(xcx::ComposeAvatar(xcx::Image(64, 64), mask, spec).Empty());
    spec.cell = 144;
    CHECK(xcx::ComposeAvatar(base, mask, spec).Empty());
    spec.cell = -1;
    CHECK(xcx::ComposeAvatar(base, mask, spec).Empty());
}
struct Rom {
    fs::path root;
};
void RealArchive(const fs::path &root) {
    Rom rom{root};
    EdenDsmodHostApi host{};
    host.userdata = &rom;
    host.read_romfs = [](void *u, const char *path, std::uint64_t offset, void *buffer,
                         std::size_t size) -> std::uint64_t {
        auto &r = *static_cast<Rom *>(u);
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
    xcx::Assets assets;
    const xcx::AvatarLook zane{true, 58, 72, 1, 115, 447}, terra{true, 66, 232, 2, 115, 336};
    const auto z = xcx::ResolveAvatar(assets.Files(), host, zane);
    CHECK(z && z->cell == 1 && std::abs(z->tint[0] - 103 / 255.f) < 1e-4 &&
          std::abs(z->tint[1] - 62 / 255.f) < 1e-4 && std::abs(z->tint[2] - 41 / 255.f) < 1e-4 &&
          std::abs(z->tint[3] - 147 / 128.f) < 1e-4);
    const auto t = xcx::ResolveAvatar(assets.Files(), host, terra);
    CHECK(t && t->cell == 57 && std::abs(t->tint[0] - 30 / 255.f) < 1e-4);
    // A hair id the table does not know falls back to cell 0; palette id 0 / out of range is the
    // game's neutral grey.
    const auto u = xcx::ResolveAvatar(assets.Files(), host, {true, 1, 3999, 1, 0, 3999});
    CHECK(u && u->cell == 0 && u->tint[0] == 0.5f && u->tint[3] == 1.f);
    CHECK(!xcx::ResolveAvatar(assets.Files(), host, {}));
    const auto zi = xcx::AvatarPortrait(assets, host, zane);
    const auto ti = xcx::AvatarPortrait(assets, host, terra);
    CHECK(zi.w == 112 && ti.w == 112 && zi.rgba != ti.rgba);
    std::size_t covered = 0;
    for (std::size_t p = 3; p < zi.rgba.size(); p += 4)
        covered += zi.rgba[p] > 127;
    CHECK(covered > 112 * 112 / 4); // the head is a substantial part of the cell
    if (const char *out = std::getenv("XCX_AVATAR_OUT")) { // optional PPM dump over mid-grey
        for (const auto *pair : {&zi, &ti}) {
            std::ofstream f(fs::path(out) / (pair == &zi ? "zanemato.ppm" : "terra.ppm"),
                            std::ios::binary);
            f << "P6\n112 112\n255\n";
            for (std::size_t p = 0; p < pair->rgba.size(); p += 4) {
                const auto a = pair->rgba[p + 3];
                for (int c = 0; c < 3; ++c) {
                    const char v = char((pair->rgba[p + c] * a + 40 * (255 - a)) / 255);
                    f.write(&v, 1);
                }
            }
        }
    }
    // An empty archive yields no picture and no exception.
    EdenDsmodHostApi empty{};
    empty.read_romfs = [](void *, const char *, std::uint64_t, void *, std::size_t) -> std::uint64_t {
        return 0;
    };
    xcx::Assets none;
    CHECK(xcx::AvatarPortrait(none, empty, zane).Empty());
}
} // namespace
int main() {
    NameDecode();
    BlockDecode();
    Compose();
    const char *rom = std::getenv("XCX_ROMFS");
    if (rom)
        RealArchive(rom);
    else
        std::puts("XCX_ROMFS unset: real-archive checks skipped");
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("xcx avatar tests passed");
    return 0;
}
