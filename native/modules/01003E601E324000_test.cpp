// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Unit tests for the Dragon Quest III HD-2D Remake module's pure parts: SHA-1, the pinned pak
// member table, the FPakEntry header check, the ooz wrapper's hash guard (synthetic Kraken
// blocks), texture/art helpers, font styling and the party reader on canned guest memory.
//
// Real-data checks run when DQ3_PAK names your own merged Nicola-Switch.pak (Switch 1.1.0.0):
// every pinned member is read, hash-checked, Oodle-decoded and decoded (textures, font atlas).
// Nothing from the game is stored in this repository.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <cmath>
#include "dq3_art.h"
#include "dq3_battle.h"
#include "dq3_battle_art.h"
#include "dq3_battle_view.h"
#include "dq3_element_art.h"
#include "dq3_tables.h"
#include "dq3_font.h"
#include "dq3_heal.h"
#include "dq3_info.h"
#include "dq3_info_view.h"
#include "dq3_keys.h"
#include "dq3_map.h"
#include "dq3_map_swap.h"
#include "dq3_page_layout.h"
#include "dq3_party_order.h"
#include "dq3_pak.h"
#include "dq3_reader.h"
#include "dq3_sprites.h"
#include "dq3_system.h"

namespace {

using dq3::u32;
using dq3::u64;
using dq3::u8;

int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

std::string Hex(std::span<const u8> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (const u8 b : bytes) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

void PutLe(std::vector<u8>& v, u64 x, int n) {
    for (int i = 0; i < n; ++i)
        v.push_back(static_cast<u8>(x >> (8 * i)));
}

void TestSha1() {
    const auto hex = [](std::string_view s) {
        return Hex(dq3::Sha1({reinterpret_cast<const u8*>(s.data()), s.size()}));
    };
    CHECK(hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    std::string million(1000000, 'a');
    CHECK(hex(million) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

void TestMemberTable() {
    for (u32 i = 0; i < static_cast<u32>(dq3::MemberId::Count); ++i) {
        const auto& m = dq3::Member(static_cast<dq3::MemberId>(i));
        CHECK(m.compression == 1);
        CHECK(!m.blocks.empty());
        CHECK(m.blocks.front().start == m.header_bytes);
        CHECK(m.header_bytes == 53 + 4 + 16 * m.blocks.size());
        u64 total = 0, prev = m.blocks.front().start;
        for (const auto& b : m.blocks) {
            CHECK(b.start == prev && b.end > b.start);
            total += b.end - b.start;
            prev = b.end;
        }
        CHECK(total == m.compressed_size);
        CHECK(m.block_size >= (m.blocks.size() == 1 ? m.size : 1));
        CHECK(static_cast<u64>(m.block_size) * m.blocks.size() >= m.size);
        CHECK(m.offset + m.header_bytes + m.compressed_size <= dq3::PakSize);
    }
    CHECK(dq3::Member(dq3::MemberId::MapWindowBase).blocks.size() == 8);
    CHECK(dq3::TextureLayout(dq3::MemberId::FontBold) == std::nullopt);
    CHECK(dq3::TextureLayout(dq3::MemberId::StatusIcon)->width == 192);
}

// A synthetic member: Kraken stream = one 256 KiB-style block of three quanta isn't needed; one
// block holds a 2-byte block header and quanta of the chosen kind.
struct Synthetic {
    std::vector<u8> archive;
    std::vector<dq3::Block> blocks;
    dq3::PakMember pin{};
    std::vector<u8> payload;
};

std::vector<u8> EntryHeader(const dq3::PakMember& pin) {
    std::vector<u8> h;
    PutLe(h, 0, 8);
    PutLe(h, pin.compressed_size, 8);
    PutLe(h, pin.size, 8);
    PutLe(h, pin.compression, 4);
    h.insert(h.end(), pin.stored_sha1.begin(), pin.stored_sha1.end());
    PutLe(h, pin.blocks.size(), 4);
    for (const auto& b : pin.blocks) {
        PutLe(h, b.start, 8);
        PutLe(h, b.end, 8);
    }
    h.push_back(0);
    PutLe(h, pin.block_size, 4);
    return h;
}

// kind 0: uncompressed Kraken block (0xCC 0x06 + raw); 1: memset quantum; 2: stored quantum.
Synthetic MakeSynthetic(int kind, u64 base_offset) {
    Synthetic s;
    std::vector<u8> stored;
    if (kind == 0) {
        for (int i = 0; i < 300; ++i)
            s.payload.push_back(static_cast<u8>(i * 7 + 3));
        stored = {0xCC, 0x06};
        stored.insert(stored.end(), s.payload.begin(), s.payload.end());
    } else if (kind == 1) {
        s.payload.assign(1000, 0x5A);
        stored = {0x8C, 0x06, 0x07, 0xFF, 0xFF, 0x5A}; // quantum: v>>18 == 1 -> memset(0x5A)
    } else {
        for (int i = 0; i < 64; ++i)
            s.payload.push_back(static_cast<u8>(255 - i));
        const u32 v = static_cast<u32>(s.payload.size()) - 1; // compressed == raw size -> memmove
        stored = {0x8C, 0x06, static_cast<u8>(v >> 16), static_cast<u8>(v >> 8),
                  static_cast<u8>(v)};
        stored.insert(stored.end(), s.payload.begin(), s.payload.end());
    }
    const u32 header_bytes = 53 + 4 + 16;
    s.blocks = {{header_bytes, header_bytes + stored.size()}};
    s.pin.path = "synthetic";
    s.pin.offset = base_offset;
    s.pin.compressed_size = stored.size();
    s.pin.size = s.payload.size();
    s.pin.compression = 1;
    s.pin.header_bytes = header_bytes;
    s.pin.block_size = static_cast<u32>(s.payload.size());
    s.pin.blocks = s.blocks;
    s.pin.stored_sha1 = dq3::Sha1(stored);
    s.pin.decoded_sha1 = dq3::Sha1(s.payload);
    s.archive.assign(base_offset, 0xEE);
    const auto header = EntryHeader(s.pin);
    s.archive.insert(s.archive.end(), header.begin(), header.end());
    s.archive.insert(s.archive.end(), stored.begin(), stored.end());
    return s;
}

dq3::RangeReader Over(const std::vector<u8>& bytes) {
    return [&bytes](u64 off, void* out, std::size_t n) {
        if (off > bytes.size() || n > bytes.size() - off)
            return false;
        std::memcpy(out, bytes.data() + off, n);
        return true;
    };
}

void TestOozGuard() {
    for (int kind = 0; kind < 3; ++kind) {
        auto s = MakeSynthetic(kind, 1000);
        std::string why;
        auto got = dq3::ReadMember(Over(s.archive), s.pin, &why);
        CHECK(got && *got == s.payload);
        // Corrupt one stored byte: rejected by the hash before ooz sees it.
        auto bad = s.archive;
        bad[1000 + s.pin.header_bytes + 2] ^= 0x01;
        why.clear();
        CHECK(!dq3::ReadMember(Over(bad), s.pin, &why));
        CHECK(why == "stored payload hash mismatch");
        // Header field mismatch (block size).
        bad = s.archive;
        bad[1000 + s.pin.header_bytes - 1] ^= 0x01;
        CHECK(!dq3::ReadMember(Over(bad), s.pin, &why) && why == "entry block size");
        // Encrypted flag set.
        bad = s.archive;
        bad[1000 + s.pin.header_bytes - 5] = 1;
        CHECK(!dq3::ReadMember(Over(bad), s.pin, &why) && why == "entry flags (encrypted?)");
        // Wrong decoded hash pin.
        auto pin = s.pin;
        pin.decoded_sha1[0] ^= 1;
        CHECK(!dq3::ReadMember(Over(s.archive), pin, &why) && why == "decoded hash mismatch");
        // A pinned size the stream does not produce: ooz fails, bounded destination (a memset
        // quantum fills any size; the decoded hash catches that case).
        pin = s.pin;
        pin.size += 1;
        pin.block_size += 1;
        CHECK(kind == 1 || !dq3::DecodeStored({s.archive.data() + 1000 + s.pin.header_bytes,
                                  static_cast<std::size_t>(s.pin.compressed_size)},
                                 pin));
        // Short archive.
        bad.assign(s.archive.begin(), s.archive.end() - 1);
        CHECK(!dq3::ReadMember(Over(bad), s.pin, &why) && why == "payload read");
    }
}

void TestTextureParse() {
    // A 4x4 PF_BC7 texture in the pinned .uexp layout; BC7 mode 6 block with all-zero endpoints
    // decodes to transparent black.
    std::vector<u8> uexp(321 + 16 + 12 + 16, 0);
    std::memcpy(uexp.data() + 282, "PF_BC7", 7);
    const auto put32 = [&uexp](std::size_t at, u32 v) { std::memcpy(uexp.data() + at, &v, 4); };
    put32(278, 7);
    put32(266, 4);
    put32(270, 4);
    put32(274, 1);
    const u32 mip[6] = {0, 1, 1, 72, 16, 16};
    for (int i = 0; i < 6; ++i)
        put32(289 + 4 * i, mip[i]);
    uexp[321] = 0x40; // mode 6
    put32(337, 4);
    put32(341, 4);
    put32(345, 1);
    std::string why;
    auto img = dq3::DecodeTexture(uexp, {4, 4, 16}, &why);
    CHECK(img && img->width == 4 && img->height == 4 && img->rgba.size() == 64);
    CHECK(!dq3::DecodeTexture(uexp, {8, 4, 32}, &why));
    uexp[284] = 'X';
    CHECK(!dq3::DecodeTexture(uexp, {4, 4, 16}, &why) && why == "texture format");

    // A 2x1 PF_B8G8R8A8 texture: format at +282 (12 bytes), mip header at +294, data at +326.
    std::vector<u8> bgra(326 + 8 + 12, 0);
    const auto put = [&bgra](std::size_t at, u32 v) { std::memcpy(bgra.data() + at, &v, 4); };
    put(266, 2);
    put(270, 1);
    put(274, 1);
    put(278, 12);
    std::memcpy(bgra.data() + 282, "PF_B8G8R8A8", 12);
    const u32 mip2[6] = {0, 1, 1, 72, 8, 8};
    for (int i = 0; i < 6; ++i)
        put(294 + 4 * i, mip2[i]);
    const u8 px[8] = {1, 2, 3, 4, 10, 20, 30, 40}; // B G R A
    std::memcpy(bgra.data() + 326, px, 8);
    put(334, 2);
    put(338, 1);
    put(342, 1);
    const dq3::TexturePin pin2{2, 1, 8, dq3::TextureFormat::Bgra8};
    img = dq3::DecodeTexture(bgra, pin2, &why);
    CHECK(img && img->rgba == std::vector<u8>({3, 2, 1, 4, 30, 20, 10, 40}));
    CHECK(!dq3::DecodeTexture(bgra, {2, 1, 8}, &why)); // as BC7: wrong size / format
    bgra[290] = 'X';
    CHECK(!dq3::DecodeTexture(bgra, pin2, &why) && why == "texture format");
}

void TestSprites() {
    // Pinned table: sorted rows, valid indices, every sprite member single-block and small.
    const auto rows = dq3::LooksRows();
    const auto pins = dq3::SpritePins();
    CHECK(rows.size() == 122 && pins.size() == 113);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CHECK(rows[i].sprite < pins.size());
        if (i)
            CHECK(rows[i - 1].id < rows[i].id);
    }
    for (const auto& p : pins) {
        CHECK(p.sprite.size < 4096 && p.texture.size == u64{p.tex_w} * p.tex_h * 4 + 354);
        CHECK(p.rect[0] + p.rect[2] <= p.tex_w && p.rect[1] + p.rect[3] <= p.tex_h);
        CHECK(p.uv_at + 8 <= p.sprite.size && p.dim_at + 8 <= p.sprite.size);
        CHECK(p.data_at == 326);
    }
    CHECK(dq3::FindLooks("UNIT_LOOKS_PC_HERO_00").has_value());
    CHECK(dq3::FindLooks("UNIT_LOOKS_PC_WARRIOR_00").has_value());
    CHECK(dq3::FindLooks("UNIT_LOOKS_PC_COFFIN_HERO") == dq3::FindLooks("UNIT_LOOKS_PC_COFFIN_SAGE"));
    CHECK(!dq3::FindLooks("UNIT_LOOKS_PC_WARRIOR_00_MIZUGI"));
    CHECK(!dq3::FindLooks("UNIT_LOOKS_PC__00"));

    // Canvas placement: 35x60 frame -> 70x120 at (20 + 5, 34); a tall frame is cropped to 168.
    dq3::Image frame{35, 60, std::vector<u8>(35 * 60 * 4, 0)};
    frame.rgba[0] = 200;
    frame.rgba[3] = 255; // top-left pixel opaque
    auto canvas = dq3::SpriteCanvas(frame);
    CHECK(canvas.width == 120 && canvas.height == 188);
    const auto a = [&canvas](int x, int y) { return canvas.rgba[(std::size_t(y) * 120 + x) * 4 + 3]; };
    CHECK(a(25, 34) == 255 && a(26, 35) == 255 && a(27, 34) == 0 && a(24, 34) == 0 && a(25, 33) == 0);
    dq3::Image tall{41, 90, std::vector<u8>(41 * 90 * 4, 255)};
    canvas = dq3::SpriteCanvas(tall);
    CHECK(a(19, 10) == 255 && a(18, 10) == 0 && a(19, 177) == 255 && a(19, 178) == 0 && a(19, 9) == 0);

    CHECK(dq3::JobTextId(1, false) == std::string_view{"Txt_Status_Job_Hero"});
    CHECK(dq3::JobTextId(1, true) == std::string_view{"Txt_Status_Job_Hero_Roto"});
    CHECK(dq3::JobTextId(2, true) == std::string_view{"Txt_Status_Job_Warrior"});
    CHECK(dq3::JobTextId(3, false) == std::string_view{"Txt_Status_Job_MartialArtist"});
    CHECK(dq3::JobTextId(10, false) == std::string_view{"Txt_Status_Job_MonsterTamer"});
    CHECK(!dq3::JobTextId(0, false) && !dq3::JobTextId(11, false));
    CHECK(dq3::LinearToSrgb8(1.0f) == 255 && dq3::LinearToSrgb8(0.0f) == 0);
    CHECK(dq3::LinearToSrgb8(0.74282f) == 224 && dq3::LinearToSrgb8(0.123857f) == 99);
    CHECK(dq3::LinearToSrgb8(0.017273f) == 36 && dq3::LinearToSrgb8(0.995f) == 254);
    CHECK(dq3::LinearToSrgb8(0.964687f) == 251); // Color_0_Top R, (251, 250, 240) on screen

    // HP state (main+0xce24b0): float ratio, <= 0.3f -> 3, <= 0.5f -> 4.
    dq3::PartyMember m;
    m.hp_max = 28;
    for (auto [hp, want] : {std::pair{28, 0}, {15, 0}, {14, 4}, {12, 4}, {9, 4}, {8, 3}, {1, 3}, {0, 2}})
        CHECK((m.hp = hp, dq3::HpState(m) == want));
    m.hp_max = 10;
    CHECK((m.hp = 3, dq3::HpState(m) == 3)); // 3/10 == 0.3f exactly
    CHECK((m.hp = 5, dq3::HpState(m) == 4)); // 0.5f
    m.hp = 999;
    m.hp_max = 999;
    m.status = 1;
    CHECK(dq3::HpState(m) == 2);
    m.status = 0;
    m.hp_max = 0;
    CHECK(dq3::HpState(m) == 0);

    // Looks rows (GetLooksId).
    m.status = 0;
    m.looks = 4;
    CHECK(dq3::LooksId(m, "WARRIOR", "") == "UNIT_LOOKS_PC_WARRIOR_04");
    CHECK(dq3::LooksId(m, "WARRIOR", "ITEM_EQUIP_ARMOR_MAGIC_BIKINI") == "UNIT_LOOKS_PC_WARRIOR_04_MIZUGI");
    CHECK(dq3::LooksId(m, "WARRIOR", "ITEM_EQUIP_ARMOR_CAT_SUIT") == "UNIT_LOOKS_PC_NUIGURUMI_00");
    CHECK(dq3::LooksId(m, "WARRIOR", "ITEM_EQUIP_ARMOR_PLAIN_CLOTHES") == "UNIT_LOOKS_PC_WARRIOR_04");
    m.status = 1;
    CHECK(dq3::LooksId(m, "HERO", "ITEM_EQUIP_ARMOR_CAT_SUIT") == "UNIT_LOOKS_PC_COFFIN_HERO");
}

void TestArtHelpers() {
    CHECK((dq3::ParseSize("268x200") == std::pair<u32, u32>(268, 200)));
    CHECK(!dq3::ParseSize("268x"));
    CHECK(!dq3::ParseSize("0x5"));
    CHECK(!dq3::ParseSize("5000x5"));
    dq3::Image box{9, 9, std::vector<u8>(9 * 9 * 4, 0)};
    const u8 fill[4] = {18, 18, 16, 230}, line[4] = {90, 88, 76, 255};
    dq3::RoundedBox(box, 0, 0, 8, 8, 3, fill, line);
    CHECK(box.rgba[(4 * 9 + 4) * 4 + 3] == 230); // centre filled
    CHECK(box.rgba[(4 * 9 + 0) * 4 + 3] == 255); // left edge outline
    CHECK(box.rgba[3] == 0);                     // rounded corner stays empty
    // Resize of a flat opaque image stays flat; nine-slice covers the whole box.
    dq3::Image flat{20, 20, {}};
    for (int i = 0; i < 400; ++i)
        flat.rgba.insert(flat.rgba.end(), {200, 100, 50, 255});
    const auto r = dq3::Resize(flat, 0, 0, 20, 20, 7, 33);
    CHECK(r.width == 7 && r.height == 33);
    for (std::size_t i = 0; i < r.rgba.size(); i += 4)
        CHECK(r.rgba[i] == 200 && r.rgba[i + 1] == 100 && r.rgba[i + 2] == 50 && r.rgba[i + 3] == 255);
    // OpaqueRect: the bounding box at the threshold, nothing below it, nullopt when empty.
    dq3::Image pad{12, 8, std::vector<u8>(12 * 8 * 4, 0)};
    CHECK(!dq3::OpaqueRect(pad, 128));
    for (u32 y = 2; y <= 5; ++y)
        for (u32 x = 3; x <= 9; ++x)
            pad.rgba[(y * 12 + x) * 4 + 3] = 255;
    pad.rgba[(1 * 12 + 1) * 4 + 3] = 100; // a soft shadow texel: below the threshold
    CHECK((dq3::OpaqueRect(pad, 128) == dq3::PixelRect{3, 2, 7, 4}));
    CHECK((dq3::OpaqueRect(pad, 100) == dq3::PixelRect{1, 1, 9, 5}));
    dq3::Image dst{40, 30, std::vector<u8>(40 * 30 * 4, 0)};
    dq3::Nine(flat, 5, 6, 0, 0, 40, 30, dst);
    for (std::size_t i = 3; i < dst.rgba.size(); i += 4)
        CHECK(dst.rgba[i] == 255);
    const auto translucent = dq3::FillAlpha(dq3::Image{1, 1, {10, 20, 30, 200}}, 0.86);
    CHECK(translucent.rgba[3] == 172); // 200 * 0.86 = 172.0
}

void TestFontStyling() {
    CHECK(dq3::Styled(std::string_view{"Lv 9"}, dq3::FontStyle::Value) == "Lv 9");
    // 'H' in style 3 (Semi28) -> U+E248 (EE 89 88 in UTF-8); 'A' in style 1 (Bold26) -> U+E041.
    CHECK(dq3::Styled(std::string_view{"H"}, dq3::FontStyle::Semi28) == "\xEE\x89\x88");
    CHECK(dq3::Styled(std::string_view{"A"}, dq3::FontStyle::Bold26) == "\xEE\x81\x81");
    // the last style's range ends inside the Private Use Area (U+E000..U+F8FF)
    static_assert(0xE000 + 0x100 * (static_cast<int>(dq3::FontStyle::Count) - 1) - 1 <= 0xF8FF);
    // Non-Latin-1 stays plain.
    CHECK(dq3::Styled(std::u32string{U"—"}, dq3::FontStyle::Bold26) == "\xE2\x80\x94");
    const std::uint16_t name[] = {'A', 'r', 'e', 'n'};
    CHECK(dq3::FromUtf16(name) == std::u32string{U"Aren"});
    const std::uint16_t lone[] = {0xD800};
    CHECK(!dq3::FromUtf16(lone));
    const u8 spec[] = "# comment\n  dq3-font 1  \n";
    CHECK(dq3::IsFontSpec({spec, sizeof(spec) - 1}));
    const u8 other[] = "dq3-font 2\n";
    CHECK(!dq3::IsFontSpec({other, sizeof(other) - 1}));
}

// ---- reader on canned guest memory ----------------------------------------------------------

struct FakeGuest {
    std::map<u64, std::vector<u8>> regions;
    int reads = 0;
    std::function<void(int)> on_read; // mutate between passes (torn-read test)
    void Put(u64 at, const void* p, std::size_t n) {
        auto& r = regions[at];
        r.assign(static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
    }
    template <class T>
    void Put(u64 at, const T& v) {
        Put(at, &v, sizeof(T));
    }
    u8* At(u64 at) {
        for (auto& [base, bytes] : regions)
            if (at >= base && at < base + bytes.size())
                return bytes.data() + (at - base);
        return nullptr;
    }
    bool Read(u64 at, void* out, std::size_t n) {
        if (on_read)
            on_read(reads);
        ++reads;
        for (auto& [base, bytes] : regions)
            if (at >= base && at + n <= base + bytes.size()) {
                std::memcpy(out, bytes.data() + (at - base), n);
                return true;
            }
        return false;
    }
};

constexpr u64 MainBase = 0x80000000ULL, MainSize = 0x5c00000ULL;

void PutPins(FakeGuest& g) {
    // The accessors with the real 1.1.0 ADRP/ADD words (page and offset are image-relative).
    const u32 acc[] = {0xd0026d88, 0x910ba108, 0xf9400108, 0xf9429508, 0x91006100, 0xd65f03c0};
    g.Put(MainBase + 0x87437c, acc, sizeof(acc));
    std::vector<u32> names = dq3::Pins()[1].words;
    CHECK(dq3::Pins()[1].offset == 0x112d84c);
    names[0] = 0xf0024eb7;
    names[1] = 0x912502f7;
    g.Put(MainBase + 0x112d84c, names.data(), names.size() * 4);
    for (const auto& pin : dq3::Pins())
        if (pin.offset != 0x87437c && pin.offset != 0x112d84c)
            g.Put(MainBase + pin.offset, pin.words.data(), pin.words.size() * 4);
}

struct CannedChar {
    std::u16string name;
    u8 membership;
    u32 slot;
    std::int32_t level, base_hp, hp, base_mp, mp, bonus_hp, bonus_mp;
    u32 status;
};

// Lays out G -> GD -> P -> PM -> roster -> C/F/name in fake heap memory.
void PutParty(FakeGuest& g, const std::vector<CannedChar>& chars, u64 heap = 0x1200000000ULL) {
    const u64 G = heap, GD = heap + 0x1000, P = heap + 0x2000, PM = heap + 0x3000,
              arr = heap + 0x4000;
    g.Put(MainBase + 0x56262e8, G);
    std::vector<u8> gobj(0x600, 0), gdobj(0x100, 0), pobj(0x400, 0), pmobj(0x40, 0);
    std::memcpy(gobj.data() + 0x528, &GD, 8);
    std::memcpy(gdobj.data() + 0x18, &P, 8);
    std::memcpy(pobj.data() + 0x220, &PM, 8);
    std::memcpy(pmobj.data() + 0x10, &arr, 8);
    const std::int32_t n = static_cast<std::int32_t>(chars.size());
    std::memcpy(pmobj.data() + 0x18, &n, 4);
    g.Put(G, gobj.data(), gobj.size());
    g.Put(GD, gdobj.data(), gdobj.size());
    g.Put(P, pobj.data(), pobj.size());
    g.Put(PM, pmobj.data(), pmobj.size());
    std::vector<u64> entries;
    for (std::size_t k = 0; k < chars.size(); ++k) {
        const auto& c = chars[k];
        const u64 C = heap + 0x10000 + k * 0x1000, F = C + 0x400, name = C + 0x800;
        entries.push_back(C);
        entries.push_back(C + 0x100); // controller (unused)
        std::vector<u8> cobj(0xc8, 0), fobj(0x100, 0);
        std::memcpy(cobj.data() + 0x28, &name, 8);
        const std::int32_t num = static_cast<std::int32_t>(c.name.size() + 1);
        std::memcpy(cobj.data() + 0x30, &num, 4);
        std::memcpy(cobj.data() + 0x34, &num, 4);
        std::memcpy(cobj.data() + 0x58, &F, 8);
        cobj[0xc0] = c.membership;
        std::memcpy(cobj.data() + 0xc4, &c.slot, 4);
        const auto put = [&fobj](std::size_t off, std::int32_t v) { std::memcpy(fobj.data() + off, &v, 4); };
        put(0x1c, c.level);
        put(0x28, c.base_hp);
        put(0x2c, c.hp);
        put(0x30, c.base_mp);
        put(0x34, c.mp);
        put(0x68, c.bonus_hp);
        put(0x6c, c.bonus_mp);
        put(0x8c, static_cast<std::int32_t>(c.status));
        g.Put(C, cobj.data(), cobj.size());
        g.Put(F, fobj.data(), fobj.size());
        std::u16string with_nul = c.name;
        with_nul.push_back(0);
        g.Put(name, with_nul.data(), with_nul.size() * 2);
    }
    g.Put(arr, entries.data(), entries.size() * 8);
}

void TestReader() {
    FakeGuest g;
    PutPins(g);
    const dq3::GuestRead read = [&g](u64 at, void* out, std::size_t n) { return g.Read(at, out, n); };
    dq3::Roots roots;
    CHECK(dq3::Resolve(read, MainBase, MainSize, roots).empty());
    CHECK(roots.g_slot == MainBase + 0x56262e8 && roots.name_pool == MainBase + 0x5b04940);
    const u64 slot = roots.g_slot;

    // A changed pinned word fails closed with the location.
    g.At(MainBase + 0x8d08d8)[0] ^= 4;
    CHECK(dq3::Resolve(read, MainBase, MainSize, roots).starts_with("code pin main+0x8d08d0 word 2"));
    g.At(MainBase + 0x8d08d8)[0] ^= 4;
    CHECK(dq3::Resolve(read, MainBase, MainSize, roots).empty());

    dq3::PartySnapshot snap;
    std::string why;
    g.Put(MainBase + 0x56262e8, u64{0});
    CHECK(dq3::ReadParty(read, slot, snap, &why) == dq3::PartyState::NoGame);

    // Roster order != Line-Up order; a bag entry (4) and a standby member (3) are skipped.
    PutParty(g, {
                    {u"Han", 1, 1, 99, 4284, 999, 3611, 997, 0, 0, 0},
                    {u"Merl", 1, 2, 2, 16, 0, 41, 32, 0, 3, 1},     // KO
                    {u"Item Bag", 4, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                    {u"Erdrick", 1, 0, 99, 672, 999, 484, 937, 0, 0, 2}, // poisoned
                    {u"P", 1, 3, 1, 23, 20, 8, 8, 3, 0, 6},             // poison + paralysis
                    {u"Ace", 3, 0, 1, 25, 25, 23, 23, 0, 0, 0},
                });
    CHECK(dq3::ReadParty(read, slot, snap, &why) == dq3::PartyState::Ok);
    CHECK(snap.roster_count == 6 && snap.members.size() == 4);
    CHECK(snap.members[0].name == U"Erdrick" && snap.members[1].name == U"Han");
    CHECK(snap.members[2].name == U"Merl" && snap.members[3].name == U"P");
    CHECK(snap.members[0].hp == 999 && snap.members[0].hp_max == 672 && snap.members[0].mp == 937);
    CHECK(snap.members[1].hp_max == 999 && snap.members[1].mp_max == 999); // capped
    CHECK(snap.members[2].Dead() && !snap.members[2].Poisoned() && snap.members[2].mp_max == 44);
    CHECK(snap.members[0].Poisoned() && !snap.members[0].Paralysed());
    CHECK(snap.members[3].Poisoned() && snap.members[3].Paralysed() && snap.members[3].hp_max == 26);
    CHECK(snap.members[3].level == 1);
    CHECK(!snap.roto && !snap.members[0].armour_read && snap.members[0].vocation == 0);

    // Vocation / looks bytes, the Erdrick-title flag and an equipped armour (bag map key 4).
    {
        const u64 C = 0x1200010000ULL + 3 * 0x1000, bag = 0x1600000000ULL, elems = bag + 0x100,
                  slot_obj = bag + 0x200, def = bag + 0x300;
        g.At(C + 0xaa)[0] = 1;
        const u32 looks = 3;
        std::memcpy(g.At(C + 0xb8), &looks, 4);
        std::memcpy(g.At(C + 0x78), &bag, 8);
        std::vector<u8> b(0x80, 0), e(0x40, 0), so(0x20, 0), d(0x20, 0);
        std::memcpy(b.data() + 0x38, &elems, 8);
        const std::int32_t num = 2;
        std::memcpy(b.data() + 0x40, &num, 4);
        b[0x48] = 0x3; // both elements allocated
        e[0] = 1;      // weapon
        e[0x20] = 4;   // armour
        std::memcpy(e.data() + 0x28, &slot_obj, 8);
        std::memcpy(so.data() + 8, &def, 8);
        const u32 fname[2] = {77, 2};
        std::memcpy(d.data() + 8, fname, 8);
        g.Put(bag, b.data(), b.size());
        g.Put(elems, e.data(), e.size());
        g.Put(slot_obj, so.data(), so.size());
        g.Put(def, d.data(), d.size());
        const u64 flags = u64{1} << 11; // bit 0x8b of the bitset at P+0x128+0xC8 -> qword P+0x200, bit 11
        std::memcpy(g.At(0x1200002000ULL + 0x200), &flags, 8);
        CHECK(dq3::ReadParty(read, slot, snap, &why) == dq3::PartyState::Ok);
        CHECK(snap.roto && snap.members[0].vocation == 1 && snap.members[0].looks == 3);
        CHECK(snap.members[0].armour_read && snap.members[0].armour[0] == 77 &&
              snap.members[0].armour[1] == 2);
        b[0x48] = 0x1; // armour element freed -> no armour, still readable
        g.Put(bag, b.data(), b.size());
        CHECK(dq3::ReadParty(read, slot, snap, &why) == dq3::PartyState::Ok);
        CHECK(snap.members[0].armour_read && snap.members[0].armour[0] == 0);
        const u64 none = 0;
        std::memcpy(g.At(0x1200002000ULL + 0x200), &none, 8);
    }

    CHECK(dq3::DisplayedMax(23, 3) == 26);
    CHECK(dq3::DisplayedMax(995, 10) == 999);
    CHECK(dq3::DisplayedMax(-5, 0) == 0);

    // Torn read: HP changes between the two passes.
    g.on_read = [&g, first = g.reads](int n) {
        if (n == first + 25)
            *reinterpret_cast<std::int32_t*>(g.At(0x1200010000ULL + 0x400 + 0x2c)) = 998;
    };
    CHECK(dq3::ReadParty(read, slot, snap, &why) == dq3::PartyState::Invalid && why == "torn");
    g.on_read = nullptr;
    CHECK(dq3::ReadParty(read, slot, snap, &why) == dq3::PartyState::Ok);
    CHECK(snap.members[1].hp == 998);

    // Duplicate Line-Up slot, missing name terminator, empty roster.
    FakeGuest d;
    PutPins(d);
    const dq3::GuestRead dread = [&d](u64 at, void* out, std::size_t n) { return d.Read(at, out, n); };
    PutParty(d, {{u"A", 1, 0, 5, 30, 30, 0, 0, 0, 0, 0}, {u"B", 1, 0, 5, 30, 30, 0, 0, 0, 0, 0}});
    CHECK(dq3::ReadParty(dread, slot, snap, &why) == dq3::PartyState::Invalid && why == "party slots");
    PutParty(d, {{u"A", 1, 0, 5, 30, 30, 0, 0, 0, 0, 0}});
    d.At(0x1200010800ULL + 2)[0] = 'x'; // overwrite the NUL
    CHECK(dq3::ReadParty(dread, slot, snap, &why) == dq3::PartyState::Invalid && why == "name unreadable");
    PutParty(d, {});
    CHECK(dq3::ReadParty(dread, slot, snap, &why) == dq3::PartyState::NoGame && why == "empty roster");
    PutParty(d, {{u"A", 1, 0, 0, 30, 30, 0, 0, 0, 0, 0}});
    CHECK(dq3::ReadParty(dread, slot, snap, &why) == dq3::PartyState::Invalid && why == "member values");

    // Scene gate: FNamePool block 0 with "title" (ASCII), "FieldTop" (UTF-16) entries.
    const u64 pool = roots.name_pool, block = 0x1300000000ULL;
    g.Put(pool + 0x28, block);
    std::vector<u8> entries(64, 0);
    const std::uint16_t h_title = 5 << 6, h_field = (8 << 6) | 1;
    std::memcpy(entries.data() + 2, &h_title, 2); // index 1 -> offset 2
    std::memcpy(entries.data() + 4, "title", 5);
    std::memcpy(entries.data() + 10, &h_field, 2); // index 5 -> offset 10
    const char16_t field[] = u"FieldTop";
    std::memcpy(entries.data() + 12, field, 16);
    g.Put(block, entries.data(), entries.size());
    CHECK(dq3::ReadFName(read, pool, 1, 0) == std::string{"title"});
    CHECK(dq3::ReadFName(read, pool, 5, 3) == std::string{"FieldTop_2"});
    CHECK(!dq3::ReadFName(read, pool, 0x20000000, 0));
    // cached: the game's pool never changes an entry, so a later change is not seen until a clear
    std::memcpy(g.At(block + 4), "TITLE", 5);
    CHECK(dq3::ReadFName(read, pool, 1, 0) == std::string{"title"});
    CHECK(dq3::ReadFName(read, pool, 1, 1) == std::string{"TITLE_0"}); // another (index, number) key
    dq3::ClearFNameCache();
    CHECK(dq3::ReadFName(read, pool, 1, 0) == std::string{"TITLE"});
    std::memcpy(g.At(block + 4), "title", 5);
    dq3::ClearFNameCache();
    const u64 G = 0x1200000000ULL, engine = 0x1400000000ULL, vp = engine + 0x1000,
              world = engine + 0x2000, lp = engine + 0x3000, pc = engine + 0x4000,
              players = engine + 0x5000;
    std::memcpy(g.At(G + 0x20), &engine, 8);
    std::memcpy(g.At(G + 0x38), &players, 8);
    std::vector<u8> eobj(0x800, 0), vobj(0x80, 0), wobj(0x20, 0), lpobj(0x40, 0), pcobj(0x2a8, 0);
    std::memcpy(eobj.data() + 0x7a0, &vp, 8);
    std::memcpy(vobj.data() + 0x70, &world, 8);
    const u32 title_name[2] = {1, 0}, field_name[2] = {5, 0};
    std::memcpy(wobj.data() + 0x18, title_name, 8);
    std::memcpy(lpobj.data() + 0x30, &pc, 8);
    g.Put(engine, eobj.data(), eobj.size());
    g.Put(vp, vobj.data(), vobj.size());
    g.Put(world, wobj.data(), wobj.size());
    g.Put(lp, lpobj.data(), lpobj.size());
    g.Put(pc, pcobj.data(), pcobj.size());
    g.Put(players, lp);
    dq3::SceneState scene;
    CHECK(dq3::ReadScene(read, roots, scene) && scene.world == "title" && !scene.pawn &&
          !scene.InAdventure());
    std::memcpy(g.At(world + 0x18), field_name, 8);
    CHECK(dq3::ReadScene(read, roots, scene) && scene.world == "FieldTop" && !scene.InAdventure());
    const u64 pawn = 0x1500000000ULL;
    std::memcpy(g.At(pc + 0x2a0), &pawn, 8);
    CHECK(dq3::ReadScene(read, roots, scene) && scene.InAdventure());
}

// ---- battle page (synthetic) -------------------------------------------------------------------

void TestBattleView() {
    // icon cells: game order, flag vs buff classes, turns > 0, the unit's active gate
    dq3::BattleUnit u;
    u.active = 1;
    u.effects = {{0x4e01950, 3, {}, 0x1000},   // Oomph -> cell 1
                 {0x4e00010, 2, {}, 0x8},      // Sap (DEFENSE_DOWN) -> cell 4
                 {0x4e01b18, 2, {}, 0x8000},   // Snooze: no battle panel cell
                 {0x4e02010, 999, {}, 0x1000}, // passive class: never an icon
                 {0x4e01bb8, 0, {}, 0x80000}}; // BUILD_UP with 0 turns: hidden
    CHECK((dq3::BattleIconCells(u) == std::vector<u32>{1, 4}));
    u.active = 0;
    CHECK(dq3::BattleIconCells(u).empty());
    std::vector<u32> order{3};
    dq3::MergeIconOrder(order, {1, 3}); // Kabuff first, then Oomph: the older entry stays first
    CHECK((order == std::vector<u32>{3, 1}));
    dq3::MergeIconOrder(order, {1});
    CHECK((order == std::vector<u32>{1}));
    CHECK(dq3::RecordKey("UNIT_MASTER_MN_103_OROCHI") == "UNIT_MASTER_MN_102_OROCHI");
    CHECK(dq3::RecordKey("UNIT_MASTER_MN_001_SLIME") == "UNIT_MASTER_MN_001_SLIME");
    CHECK(dq3::MonsterScale(24, 22, 184, 70) == 3 && dq3::MonsterScale(230, 224, 184, 70) == 0);

    // four enemies: one row of four centred cells (revision 7 battle-nothing-tapped-4)
    dq3::BattleSnapshot b;
    b.in_battle = true;
    b.state = 5;
    b.prev = 4;
    b.groups = {{{1, 0}, {11, 0}, 2, 2}, {{2, 0}, {12, 0}, 2, 2}};
    for (int i = 0; i < 4; ++i) {
        dq3::BattleUnit e;
        e.slot = static_cast<u32>(i);
        e.id = i;
        e.group = i / 2;
        e.index = i % 2;
        e.hp = i == 3 ? 0 : 7;
        e.hp_max = 8;
        e.level = 2;
        e.monster[0] = e.group ? 2u : 1u;
        e.master[0] = e.group ? 12u : 11u;
        e.name_text[0] = e.group ? 22u : 21u;
        b.enemies.push_back(e);
    }
    dq3::BattleUnit p;
    p.slot = 20;
    p.id = 20;
    p.hp = 999;
    p.hp_max = 999;
    p.mp = 10;
    p.mp_max = 20;
    p.level = 99;
    p.field = 0x1234;
    p.active = 1;
    b.party.push_back(p);
    dq3::BattleData data;
    data.nouns["Txt_Monster_Name_Starkraven"] = U"Stark Raven";
    data.nouns["Txt_Monster_Name_Slime"] = U"Slime";
    data.monsters["MONSTER_MN_001_SLIME"] = {2, 7, 1, 13, 18, 8, "BATTLE_RESIST_MN_001_SLIME", {}};
    data.resists["BATTLE_RESIST_MN_001_SLIME"] = {{1.0f, 1.5f, 0.5f, 1.0f, 0.0f, 1.25f}};
    for (const auto t : dq3::ResistSpellText)
        data.nouns[std::string{t}] = U"Frizz";
    dq3::BattleViewInput in;
    in.battle = &b;
    dq3::BattleMember m;
    m.name = U"Erdrick";
    m.field = 0x1234;
    in.members.push_back(m);
    in.data = &data;
    in.fname = [](const std::uint32_t n[2]) -> std::optional<std::string> {
        switch (n[0]) {
        case 1: return "MONSTER_MN_002_STARKRAVEN";
        case 2: return "MONSTER_MN_001_SLIME";
        case 11: return "UNIT_MASTER_MN_002_STARKRAVEN";
        case 12: return "UNIT_MASTER_MN_001_SLIME";
        case 21: return "Txt_Monster_Name_Starkraven";
        case 22: return "Txt_Monster_Name_Slime";
        default: return std::nullopt;
        }
    };
    const std::set<std::string> known{"UNIT_MASTER_MN_001_SLIME"};
    in.known = &known;
    const auto find_i = [](const dq3::BattleView& v, std::string_view k) -> std::optional<std::int64_t> {
        for (const auto& [kk, vv] : v.ints)
            if (kk == k)
                return vv;
        return std::nullopt;
    };
    const auto find_t = [](const dq3::BattleView& v, std::string_view k) -> std::optional<std::string> {
        for (const auto& [kk, vv] : v.texts)
            if (kk == k)
                return vv;
        return std::nullopt;
    };
    auto v = dq3::BuildBattleView(in);
    // the dynamic party row: one member -> layout gates 1 / 1 (normal layout)
    CHECK(find_i(v, "dq3.b.pn") == 1 && find_i(v, "dq3.b.pl") == 1);
    const auto enemy_cells = [](const dq3::BattleView& bv) {
        return std::count_if(bv.ints.begin(), bv.ints.end(), [](const auto& kv) {
            return kv.first.starts_with("dq3.e") && (kv.first.ends_with(".on_n") || kv.first.ends_with(".on_b"));
        });
    };
    CHECK(v.sel == dq3::BattleSel::None && enemy_cells(v) == 4);
    // values no manifest widget binds are not published
    for (const char* gone : {"dq3.b.m_none", "dq3.b.ecount", "dq3.e0.on", "dq3.bp0.sp_on", "dq3.bp0.nounit", "dq3.ed.known"})
        CHECK(!find_i(v, gone));
    CHECK(!find_t(v, "dq3.b.state"));
    // one row of four 221.6 px cells, 6 px apart, centred: 54 + (1132 - 904.4) / 2 + c * 227.6
    CHECK(find_i(v, "dq3.e0.x") == 167 && find_i(v, "dq3.e3.x") == 850);
    CHECK(find_i(v, "dq3.e0.y") == 200); // 128 + (244 - 100) / 2
    CHECK(find_t(v, "dq3.e3.img") == "module:dq3:ecell/0/221x100/56/1/g");
    CHECK(find_t(v, "dq3.e0.name").value_or("?").find("\xEE") == std::string::npos); // Semibold 26 = the plain range
    CHECK(find_i(v, "dq3.e0.ly") == 200 + 90 - 15);
    CHECK(find_i(v, "dq3.bp0.full") == 1 && find_i(v, "dq3.bp0.ny") == 956 - 280 + 168);
    in.follow = true;
    b.state = 8; b.actor = static_cast<int>(b.enemies[2].slot);
    auto followed = dq3::BuildBattleView(in);
    CHECK(followed.sel == dq3::BattleSel::Enemy);
    b.actor = 999;
    CHECK(dq3::BuildBattleView(in).sel == dq3::BattleSel::None);
    b.state = 5; b.actor = static_cast<int>(b.enemies[0].slot);
    CHECK(dq3::BuildBattleView(in).sel == dq3::BattleSel::None);
    in.follow = false;
    in.sel = dq3::BattleSel::Enemy;
    in.sel_index = 2;
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == dq3::BattleSel::Enemy && find_t(v, "dq3.ed.a0.val") && find_i(v, "dq3.ed.a0.q") == 0); // known
    CHECK(find_t(v, "dq3.ed.sprite") == "module:dq3:mon/0/240x258/6");
    // affinity words: x1 Normal (ink), x1.5 Weak (warm), x0.5 Resist and x0 Immune (cool), x1.25 Weak
    CHECK(find_t(v, "dq3.ed.a0.val") == dq3::Styled(U"Normal", dq3::FontStyle::Bold32));
    CHECK(find_t(v, "dq3.ed.a1.val") == "{c:#FF6E1C00}" + dq3::Styled(U"Weak", dq3::FontStyle::Bold32) + "{/c}" ||
          find_t(v, "dq3.ed.a1.val").value_or("").starts_with("{c:#FF6E1C00}"));
    CHECK(find_t(v, "dq3.ed.a1.val").value_or("").find(dq3::Styled(U"Weak", dq3::FontStyle::Bold32)) != std::string::npos);
    CHECK(find_t(v, "dq3.ed.a2.val").value_or("").starts_with("{c:#FF10328C}")); // x0.5 resist
    CHECK(find_t(v, "dq3.ed.a2.val").value_or("").find(dq3::Styled(U"Resist", dq3::FontStyle::Bold32)) != std::string::npos);
    CHECK(find_t(v, "dq3.ed.a4.val").value_or("").starts_with("{c:#FF10328C}") &&
          find_t(v, "dq3.ed.a4.val").value_or("").find(dq3::Styled(U"Immune", dq3::FontStyle::Bold32)) != std::string::npos);
    CHECK(find_t(v, "dq3.ed.a5.val").value_or("").find(dq3::Styled(U"Weak", dq3::FontStyle::Bold32)) != std::string::npos);
    // element icons (0.10.1): one key per family, independent of the GOP_Magic rows (none in this data)
    for (int k = 0; k < dq3::ResistElements; ++k)
        CHECK(find_t(v, "dq3.ed.a" + std::to_string(k) + ".icon") == "module:dq3:elem/" + std::to_string(k) + "/56");
    CHECK(find_i(v, "dq3.ed.group.on") == 1 && find_i(v, "dq3.ed.act.on") == 0 && find_i(v, "dq3.b.swap") == 0);
    CHECK(find_i(v, "dq3.bp0.short") == 1 && find_i(v, "dq3.bp0.ny") == 806 + 40); // name baseline (short card)
    // Possible spells remain available for a species absent from the record (including
    // bosses). Empty and unavailable differ, and switching cards clears prior rows.
    auto& info = data.monsters.at("MONSTER_MN_001_SLIME");
    info.action_group = "TEST_SPELLS";
    data.enemy_spells["TEST_SPELLS"] = {"Txt_Magic_Name_Heal", "Txt_Magic_Name_Frizz", "Txt_Magic_Name_HealAlias"};
    data.nouns["Txt_Magic_Name_Heal"] = U"Heal";
    data.nouns["Txt_Magic_Name_HealAlias"] = U"Heal";
    data.nouns["Txt_Magic_Name_Frizz"] = U"Frizz";
    in.known = nullptr;
    v = dq3::BuildBattleView(in);
    CHECK(find_i(v, "dq3.ed.a0.q") == 1 && find_i(v, "dq3.ed.sp0.on") == 1); // not known
    CHECK(find_t(v, "dq3.ed.sp0.name") == dq3::Styled(U"Frizz", dq3::FontStyle::Semi28));
    CHECK(find_t(v, "dq3.ed.sp1.name") == dq3::Styled(U"Heal", dq3::FontStyle::Semi28));
    CHECK(find_i(v, "dq3.ed.sp2.on") == 0); // duplicate localized names collapse
    data.nouns.erase("Txt_Magic_Name_Heal");
    v = dq3::BuildBattleView(in);
    CHECK(find_i(v, "dq3.ed.sp.unavailable") == 1 && find_i(v, "dq3.ed.sp0.on") == 0);
    info.action_group = "MISSING_GROUP";
    v = dq3::BuildBattleView(in);
    CHECK(find_i(v, "dq3.ed.sp.unavailable") == 1 && find_i(v, "dq3.ed.sp.none") == 0);
    info.action_group = "None";
    v = dq3::BuildBattleView(in);
    CHECK(find_i(v, "dq3.ed.sp.none") == 1 && find_i(v, "dq3.ed.sp0.on") == 0);
    // stat icons: the game's status icon cells per detail stat (no buffed value exists in the game)
    CHECK((dq3::StatIconCells({1, 4, 3, 19, 2}, 0) == std::vector<u32>{1, 2}));
    CHECK((dq3::StatIconCells({1, 4, 3, 19, 2}, 1) == std::vector<u32>{4, 3}));
    CHECK(dq3::StatIconCells({1, 4, 3}, 2).empty() && dq3::StatIconCells({1}, 3).empty());
    CHECK(!find_i(v, "dq3.ed.def.fx0.on") && !find_t(v, "dq3.ed.def.fx0.src")); // no effects: no icons
    b.enemies[2].active = 1;
    b.enemies[2].effects = {{0x4e00010, 3, {}, 0x8}}; // Sap on the selected Slime -> Defence cell 4
    v = dq3::BuildBattleView(in);
    CHECK(find_i(v, "dq3.ed.def.fx0.on") == 1 && find_t(v, "dq3.ed.def.fx0.src") == "module:dq3:status/4/28" &&
          find_i(v, "dq3.ed.def.fx0.x") == -36 && !find_i(v, "dq3.ed.def.fx1.on") && !find_i(v, "dq3.ed.atk.fx0.on") &&
          !find_i(v, "dq3.ed.agi.fx0.on")); // no font in the test: value width 0 -> -(0 + 6 + 30)
    b.enemies[2].effects.clear();
    b.enemies[2].active = 0;
    in.sel_index = 0; // Stark Raven: not in the record -> "?"
    v = dq3::BuildBattleView(in);
    CHECK(find_t(v, "dq3.ed.a0.val") == "" && find_i(v, "dq3.ed.a0.q") == 1);
    in.sel = dq3::BattleSel::Party;
    in.sel_index = 0;
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == dq3::BattleSel::Party && find_i(v, "dq3.bp0.grow") == 1 && find_i(v, "dq3.b.bare") == 1);
    CHECK(find_i(v, "dq3.ed.sp.none") == 0 && find_i(v, "dq3.ed.sp.unavailable") == 0 &&
          find_i(v, "dq3.ed.sp0.on") == 0);
    CHECK(find_t(v, "dq3.e0.img") == "module:dq3:ecell/1/221x64/52/0/");
    CHECK(!find_i(v, "dq3.pd.s0.fx0.on") && !find_i(v, "dq3.pd.s1.fx0.on"));
    // Oomph and Kabuff on the member: Attack cell 1, Defence cell 3, in the game's icon order
    b.party[0].effects = {{0x4e00010, 4, {}, 0x4}, {0x4e01950, 3, {}, 0x1000}};
    v = dq3::BuildBattleView(in);
    CHECK(find_t(v, "dq3.pd.s0.fx0.src") == "module:dq3:status/1/28" && find_i(v, "dq3.pd.s0.fx0.on") == 1 &&
          find_t(v, "dq3.pd.s1.fx0.src") == "module:dq3:status/3/28" && !find_i(v, "dq3.pd.s2.fx0.on") &&
          !find_i(v, "dq3.pd.s0.fx1.on") && !find_i(v, "dq3.pd.s3.fx0.on"));
    b.party[0].effects.clear();
    in.sel_index = 3; // no such member -> selection cleared
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == dq3::BattleSel::None);
}

// Follow: selection, layout swap, group glow, pending command and the acting enemy's line
void TestBattleFollow() {
    using dq3::BattleSel;
    using dq3::MenuFocus;
    CHECK(dq3::AffinityOf(0.0f) == dq3::Affinity::Immune && dq3::AffinityOf(0.1f) == dq3::Affinity::Resist &&
          dq3::AffinityOf(0.9f) == dq3::Affinity::Resist && dq3::AffinityOf(1.0f) == dq3::Affinity::Normal &&
          dq3::AffinityOf(1.05f) == dq3::Affinity::Weak && dq3::AffinityOf(2.0f) == dq3::Affinity::Weak);
    CHECK(dq3::AffinityWord(dq3::Affinity::Immune) == "Immune" && dq3::AffinityWord(dq3::Affinity::Normal) == "Normal");
    dq3::BattleSnapshot b;
    b.in_battle = true;
    b.state = 5;
    b.prev = 4;
    b.groups = {{{1, 0}, {11, 0}, 3, 3}, {{2, 0}, {12, 0}, 3, 3}};
    for (int i = 0; i < 6; ++i) {
        dq3::BattleUnit e;
        e.slot = static_cast<u32>(i);
        e.id = i;
        e.group = i / 3;
        e.index = i % 3;
        e.hp = i == 3 ? 0 : 7; // Slime A is down
        e.hp_max = 8;
        e.monster[0] = e.group ? 2u : 1u;
        e.master[0] = e.group ? 12u : 11u;
        e.name_text[0] = e.group ? 22u : 21u;
        e.ctrl = 0x5000 + static_cast<u64>(i);
        b.enemies.push_back(e);
    }
    const char32_t* names[] = {U"Erdrick", U"Han", U"Biggs", U"Wedge"};
    dq3::BattleViewInput in;
    for (int k = 0; k < 4; ++k) {
        dq3::BattleUnit p;
        p.slot = static_cast<u32>(20 + k);
        p.id = 20 + k;
        p.hp = p.hp_max = 999;
        p.field = 0x100 + static_cast<u64>(k);
        p.ctrl = 0x6000 + static_cast<u64>(k);
        b.party.push_back(p);
        dq3::BattleMember m;
        m.name = names[k];
        m.field = p.field;
        in.members.push_back(m);
    }
    b.commands.resize(4);
    dq3::BattleData data;
    data.nouns["Txt_Monster_Name_Starkraven"] = U"Stark Raven";
    data.nouns["Txt_Monster_Name_Slime"] = U"Slime";
    data.nouns["Txt_Magic_Name_Buff"] = U"Buff";
    data.monsters["MONSTER_MN_001_SLIME"] = {2, 7, 1, 13, 18, 8, "R", {}};
    data.monsters["MONSTER_MN_002_STARKRAVEN"] = {2, 7, 1, 13, 18, 8, "R", {}};
    data.resists["R"] = {{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}};
    data.magic["MAGIC_ATTACK"] = {0, 0, 0, false, true, "None"};
    data.magic["MAGIC_RUN_AWAY"] = {0, 0, 0, false, false, "None"};
    data.monster_actions["BATTLE_MONSTER_ACTION_RUN_AWAY"] = "MAGIC_RUN_AWAY";
    data.magic["MAGIC_BUFF"] = {0, 3, 3, true, false, "Txt_Magic_Name_Buff"};
    data.magic["MAGIC_ATTACK_MAGIC_MERA"] = {1, 2, 0, true, false, "Txt_Magic_Name_Mera"};
    data.magic["MAGIC_MONSTER_ATTACK_NORMAL"] = {0, 0, 0, false, true, "Txt_Magic_Name_MonsterAttackNormal"};
    data.monster_actions["BATTLE_MONSTER_ACTION_ATTACK"] = "MAGIC_MONSTER_ATTACK_NORMAL";
    for (const auto t : dq3::ResistSpellText)
        data.nouns[std::string{t}] = U"Frizz";
    in.battle = &b;
    in.data = &data;
    in.fname = [](const std::uint32_t n[2]) -> std::optional<std::string> {
        switch (n[0]) {
        case 1: return "MONSTER_MN_002_STARKRAVEN";
        case 2: return "MONSTER_MN_001_SLIME";
        case 11: return "UNIT_MASTER_MN_002_STARKRAVEN";
        case 12: return "UNIT_MASTER_MN_001_SLIME";
        case 21: return "Txt_Monster_Name_Starkraven";
        case 22: return "Txt_Monster_Name_Slime";
        case 31: return "MAGIC_ATTACK";
        case 32: return "MAGIC_BUFF";
        case 33: return "BATTLE_MONSTER_ACTION_ATTACK";
        case 34: return "BATTLE_MONSTER_ACTION_RUN_AWAY";
        default: return std::nullopt;
        }
    };
    const auto find_i = [](const dq3::BattleView& v, std::string_view k) -> std::optional<std::int64_t> {
        for (const auto& [kk, vv] : v.ints)
            if (kk == k)
                return vv;
        return std::nullopt;
    };
    const auto find_t = [](const dq3::BattleView& v, std::string_view k) -> std::optional<std::string> {
        for (const auto& [kk, vv] : v.texts)
            if (kk == k)
                return vv;
        return std::nullopt;
    };
    in.follow = true;
    // 1. command root: overview
    dq3::BattleMenu menu;
    menu.focus = MenuFocus::Root;
    in.menu = menu;
    auto v = dq3::BuildBattleView(in);
    CHECK(v.sel == BattleSel::None && !v.swap && find_i(v, "dq3.bp0.full") == 1);
    // unreadable / unresolved menu during command input: nothing selected either
    in.menu.reset();
    CHECK(dq3::BuildBattleView(in).sel == BattleSel::None);
    // 2. Han's command menu: Han selected (grown), even though Erdrick has a committed Attack
    b.commands[0] = {1, 1, {31, 0}, {}, 0x21};
    menu.focus = MenuFocus::Member;
    menu.member = 21;
    in.menu = menu;
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == BattleSel::Party && v.sel_index == 1 && find_i(v, "dq3.bp1.grow") == 1);
    CHECK(find_i(v, "dq3.bp0.mv") == 1 && find_t(v, "dq3.bp0.tgt").value_or("").find(dq3::Styled(U"Slime", dq3::FontStyle::Semi28)) != std::string::npos);
    // 3. Han choosing an enemy target: cursor on group 1 (Slimes) -> first living Slime selected, every
    // living Slime glows, Han's pending Attack points at the cursor's group, Erdrick's committed move stays
    menu.focus = MenuFocus::TargetEnemy;
    menu.target_group = 1;
    menu.pending = {0, 1, {31, 0}, {}, 0};
    in.menu = menu;
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == BattleSel::Enemy && v.sel_index == 4 && !v.swap);
    CHECK(find_t(v, "dq3.e3.img").value_or("").ends_with("/g") && find_t(v, "dq3.e4.img").value_or("").ends_with("/s") &&
          find_t(v, "dq3.e5.img").value_or("").ends_with("/s") && find_t(v, "dq3.e0.img").value_or("").ends_with("/"));
    CHECK(find_i(v, "dq3.bp1.mv") == 1 && find_t(v, "dq3.bp1.cmd") == dq3::Styled(U"Attack", dq3::FontStyle::Bold32));
    CHECK(find_t(v, "dq3.bp1.tgt").value_or("").find(dq3::Styled(U"Slime", dq3::FontStyle::Semi28)) != std::string::npos);
    CHECK(find_i(v, "dq3.bp0.mv") == 1 && !find_i(v, "dq3.bp2.mv"));
    menu.target_group = 0;
    in.menu = menu;
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == BattleSel::Enemy && v.sel_index == 0 && find_t(v, "dq3.e1.img").value_or("").ends_with("/s") &&
          !find_t(v, "dq3.e4.img").value_or("").ends_with("/s"));
    // 5. Han casting Buff on Biggs: Biggs (the target) selected, Han's pending Buff -> Biggs
    menu.focus = MenuFocus::TargetAlly;
    menu.target_unit = 22;
    menu.pending = {0, 3, {32, 0}, {}, 0};
    in.menu = menu;
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == BattleSel::Party && v.sel_index == 2 && find_i(v, "dq3.bp2.grow") == 1);
    CHECK(find_t(v, "dq3.bp1.cmd") == dq3::Styled(U"Buff", dq3::FontStyle::Bold32) &&
          find_t(v, "dq3.bp1.tgt").value_or("").find(dq3::Styled(U"Biggs", dq3::FontStyle::Semi28)) != std::string::npos);
    // a member choosing a target for himself: "Self"
    menu.target_unit = 21;
    in.menu = menu;
    v = dq3::BuildBattleView(in);
    CHECK(v.sel_index == 1 && find_t(v, "dq3.pd.tgt") == dq3::Styled(U"Self", dq3::FontStyle::Bold30));
    // 4. an enemy acting: Stark Raven B attacks Wedge -> swapped layout, its card with the action line
    b.state = 8;
    b.prev = 9;
    b.actor = 1;
    b.enemies[1].action[0] = 33; // BATTLE_MONSTER_ACTION_ATTACK -> MAGIC_MONSTER_ATTACK_NORMAL (ATTACK, no noun)
    b.enemies[1].target = 23;
    for (int k = 0; k < 4; ++k) {
        b.party[static_cast<std::size_t>(k)].action[0] = 31;
        b.party[static_cast<std::size_t>(k)].target = 0x20;
    }
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == BattleSel::Enemy && v.sel_index == 1 && v.swap && find_i(v, "dq3.b.swap") == 1);
    // the swap layout moves the party row's gate to N + 4
    CHECK(find_i(v, "dq3.b.pl") == *find_i(v, "dq3.b.pn") + 4 && *find_i(v, "dq3.b.pn") == static_cast<std::int64_t>(in.members.size()));
    // bottom box (two rows: 804 + (244 - 206) / 2), top cards (name baseline 278 - 150 + 40); the other enemies grey
    CHECK(find_i(v, "dq3.e0.y") == 804 + 19 && find_i(v, "dq3.bp0.ny") == 278 - 150 + 40);
    CHECK(find_t(v, "dq3.e0.img").value_or("").ends_with("/g") && find_t(v, "dq3.ed.sprite").value_or("").ends_with("/240x208/6"));
    CHECK(find_i(v, "dq3.bp0.short") == 1 && find_i(v, "dq3.bp0.mv") == 1 && find_i(v, "dq3.bp3.mv") == 1);
    CHECK(find_i(v, "dq3.ed.act.on") == 1 && find_t(v, "dq3.ed.act.cmd") == dq3::Styled(U"Attack", dq3::FontStyle::Bold30) &&
          find_i(v, "dq3.ed.act.tgt.on") == 1 && find_t(v, "dq3.ed.act.tgt") == dq3::Styled(U"Wedge", dq3::FontStyle::Bold30) &&
          find_i(v, "dq3.ed.group.on") == 0);
    CHECK(find_t(v, "dq3.e1.img").value_or("").ends_with("/s") && !find_t(v, "dq3.e0.img").value_or("").ends_with("/s"));
    // running away (the row has no game name): no action line, the group line stays
    b.enemies[1].action[0] = 34;
    b.enemies[1].target = 1;
    v = dq3::BuildBattleView(in);
    CHECK(v.swap && find_i(v, "dq3.ed.act.on") == 0 && find_i(v, "dq3.ed.group.on") == 1);
    // an actor already out of the battle (a flee sets HP 0 when it starts; or defeated): not shown, normal layout
    {
        const auto hp = b.enemies[1].hp;
        b.enemies[1].hp = 0;
        v = dq3::BuildBattleView(in);
        CHECK(v.sel == BattleSel::None && !v.swap && find_i(v, "dq3.b.swap") == 0);
        b.enemies[1].hp = hp;
    }
    b.enemies[1].action[0] = 33;
    b.enemies[1].target = 23;
    // the actor id is not current yet at the turn's first ACTION_START (still the TURN_START value)
    b.prev = 6;
    CHECK(!dq3::ActorCurrent(b, 1) && dq3::ActorCurrent(b, 5));
    in.actor_valid = dq3::ActorCurrent(b, 1);
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == BattleSel::None && !v.swap && find_i(v, "dq3.b.swap") == 0);
    b.prev = 13;
    CHECK(dq3::ActorCurrent(b, 1)); // ACTION_SCRIPT etc.: the id is the actor
    in.actor_valid = true;
    // a party member acting: shown like its command menu (party detail), normal layout
    b.actor = 22;
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == BattleSel::Party && v.sel_index == 2 && !v.swap && find_i(v, "dq3.bp0.ny") == 956 - 150 + 40);
    // turn end / results: nothing
    b.state = 7;
    CHECK(dq3::BuildBattleView(in).sel == BattleSel::None);
    b.state = 5;
    b.prev = 15; // WAIT_MENU after WIN: result messages
    CHECK(dq3::BuildBattleView(in).sel == BattleSel::None);
    // Tactical ignores the menu: manual selection only, no swap, no pending line
    b.prev = 4;
    in.follow = false;
    in.sel = BattleSel::Enemy;
    in.sel_index = 2;
    v = dq3::BuildBattleView(in);
    CHECK(v.sel == BattleSel::Enemy && v.sel_index == 2 && !v.swap && find_t(v, "dq3.e2.img").value_or("").ends_with("/s") &&
          !find_t(v, "dq3.e1.img").value_or("").ends_with("/s"));
    // the family icon (0.10.1): its own element icon, not the shared GOP_Magic UIIconNo cell (all six 0 in 1.1.0.0)
    CHECK(find_t(v, "dq3.ed.a0.icon") == "module:dq3:elem/0/56" && find_t(v, "dq3.ed.a1.icon") == "module:dq3:elem/1/56" &&
          find_t(v, "dq3.ed.a5.icon") == "module:dq3:elem/5/56");
    CHECK(dq3::bl::AffIconPx == 56);
}

// The native battle list reader on canned guest memory
void TestBattleMenuReader() {
    FakeGuest g;
    dq3::Roots roots;
    roots.g_slot = 0x1000000000ULL;
    roots.name_pool = 0x1300001000ULL;
    const u64 block = 0x1300002000ULL, game = 0x1200000000ULL, ui = game + 0x1000, manager = game + 0x2000,
              controls = game + 0x3000, members = 0x1600000000ULL, allies = 0x1600001000ULL,
              groups = 0x1600002000ULL;
    std::vector<u8> names(2, 0);
    u64 next_class = 0x1400000000ULL;
    const auto fname = [&](std::string_view text) {
        const u32 index = static_cast<u32>(names.size() / 2);
        const std::uint16_t header = static_cast<std::uint16_t>(text.size() << 6);
        PutLe(names, header, 2);
        names.insert(names.end(), text.begin(), text.end());
        if (names.size() % 2)
            names.push_back(0);
        g.Put(block, names.data(), names.size());
        return std::array<u32, 2>{index, 0};
    };
    const auto object = [&](u64 addr, std::string_view name) {
        const auto nm = fname(name);
        const u64 cls = next_class;
        next_class += 0x100;
        std::vector<u8> bytes(0x1000, 0), cb(0x28, 0);
        std::memcpy(bytes.data() + 0x10, &cls, 8);
        std::memcpy(cb.data() + 0x18, nm.data(), 8);
        g.Put(addr, bytes.data(), bytes.size());
        g.Put(cls, cb.data(), cb.size());
    };
    const auto put = [&](u64 addr, const auto& value) { std::memcpy(g.At(addr), &value, sizeof(value)); };
    g.Put(roots.name_pool + 0x28, block);
    g.Put(roots.g_slot, game);
    object(game, "Game");
    object(ui, "BP_NicolaUIManager_C");
    object(manager, "BP_UIWidgetManager_C");
    put(game + 0x418, ui);
    put(ui + 0x7c0, manager);
    std::vector<u64> ctrl;
    for (const auto cl : {"UIBattlePlayerStatus", "UIBattleTopMenu", "UIBattleTopMenuListTop", "UIBattleUnitMenu",
                          "UIBattleUnitMenuListUnit", "UIBattleUnitMenuListTargetEnemy"}) {
        const u64 addr = 0x1500000000ULL + ctrl.size() * 0x1000;
        object(addr, cl);
        put(addr + 0x30, ui);
        ctrl.push_back(addr);
    }
    // the snapshot: party controllers 0x6000.., enemies 0x5000..
    dq3::BattleSnapshot snap;
    snap.groups.resize(2);
    for (int k = 0; k < 4; ++k) {
        dq3::BattleUnit p;
        p.id = 20 + k;
        p.ctrl = 0x6000 + static_cast<u64>(k);
        snap.party.push_back(p);
    }
    std::vector<u64> mem{0x6010, 0x6000, 0x6011, 0x6001, 0x6012, 0x6002, 0x6013, 0x6003};
    g.Put(members, mem.data(), mem.size() * 8);
    g.Put(allies, mem.data(), mem.size() * 8);
    std::vector<u8> gr(0x50, 0);
    const std::int32_t g0 = 0, g1 = 1;
    std::memcpy(gr.data(), &g0, 4);
    std::memcpy(gr.data() + 0x28, &g1, 4);
    g.Put(groups, gr.data(), gr.size());
    put(ui + 0x78, members);
    put(ui + 0x80, std::int32_t{4});
    put(ui + 0xa0, std::int32_t{1});
    put(ui + 0x120, allies);
    put(ui + 0x128, std::int32_t{4});
    put(ui + 0x150, groups);
    put(ui + 0x158, std::int32_t{2});
    const u8 rec[0x24] = {0, 1, 0, 0, 31};
    std::memcpy(g.At(ui + 0xe0), rec, sizeof(rec));
    g.Put(controls, ctrl.data(), 5 * 8);
    put(manager + 0x298, controls);
    put(manager + 0x2a0, std::int32_t{3});
    const dq3::GuestRead read = [&](u64 a, void* o, std::size_t n) { return g.Read(a, o, n); };
    // root: the top list active
    put(ctrl[2] + 0x88, std::int32_t{1});
    auto m = dq3::ReadBattleMenu(read, roots, snap);
    CHECK(m && m->focus == dq3::MenuFocus::Root);
    // the top list suspended, nothing else: no active list (the caller keeps its state)
    put(ctrl[2] + 0x88, std::int32_t{2});
    m = dq3::ReadBattleMenu(read, roots, snap);
    CHECK(m && m->focus == dq3::MenuFocus::None);
    // Han's command list active
    put(manager + 0x2a0, std::int32_t{5});
    put(ctrl[4] + 0x88, std::int32_t{1});
    std::map<u64, std::string> cache;
    m = dq3::ReadBattleMenu(read, roots, snap, &cache);
    CHECK(m && m->focus == dq3::MenuFocus::Member && m->member == 21);
    CHECK(dq3::ReadBattleMenu(read, roots, snap, &cache) == m);
    // enemy target list active on its second entry (group 1)
    g.Put(controls, ctrl.data(), 6 * 8);
    put(manager + 0x2a0, std::int32_t{6});
    put(ctrl[4] + 0x88, std::int32_t{2});
    put(ctrl[5] + 0x88, std::int32_t{1});
    put(ctrl[5] + 0x98, std::int32_t{1});
    put(ctrl[5] + 0x9c, std::int32_t{2});
    m = dq3::ReadBattleMenu(read, roots, snap);
    CHECK(m && m->focus == dq3::MenuFocus::TargetEnemy && m->member == 21 && m->target_group == 1 &&
          m->pending.command == 1 && m->pending.action[0] == 31 && m->pending.valid == 0);
    put(ctrl[5] + 0x98, std::int32_t{2}); // cursor past the count
    CHECK(!dq3::ReadBattleMenu(read, roots, snap));
    put(ctrl[5] + 0x98, std::int32_t{1});
    put(ctrl[5] + 0x88, std::int32_t{3}); // closing: not active
    m = dq3::ReadBattleMenu(read, roots, snap);
    CHECK(m && m->focus == dq3::MenuFocus::None);
    // ally target list (the same class slot re-used as TargetPlayer)
    object(ctrl[5], "UIBattleUnitMenuListTargetPlayer");
    put(ctrl[5] + 0x30, ui);
    put(ctrl[5] + 0x88, std::int32_t{1});
    put(ctrl[5] + 0x98, std::int32_t{2});
    put(ctrl[5] + 0x9c, std::int32_t{4});
    m = dq3::ReadBattleMenu(read, roots, snap);
    CHECK(m && m->focus == dq3::MenuFocus::TargetAlly && m->target_unit == 22);
    // wrong owner / bad state / unknown unit are rejected
    put(ctrl[5] + 0x30, game);
    CHECK(!dq3::ReadBattleMenu(read, roots, snap));
    put(ctrl[5] + 0x30, ui);
    put(ctrl[5] + 0x88, std::int32_t{7});
    CHECK(!dq3::ReadBattleMenu(read, roots, snap));
    put(ctrl[5] + 0x88, std::int32_t{1});
    put(ui + 0xa0, std::int32_t{4});
    CHECK(!dq3::ReadBattleMenu(read, roots, snap));
    put(ui + 0xa0, std::int32_t{1});
    snap.party[1].ctrl = 0x7777;
    CHECK(!dq3::ReadBattleMenu(read, roots, snap)); // the member in control is not a battle unit
}

void TestNativeUI() {
    FakeGuest g;
    dq3::Roots roots;
    roots.g_slot=0x1000000000ULL;
    roots.name_pool=0x1300001000ULL;
    const u64 block=0x1300002000ULL, game=0x1200000000ULL, ui=game+0x1000,
        manager=game+0x2000, controls=game+0x3000, guide_manager=game+0x4000;
    std::vector<u8> names(2,0);
    u64 next_class=0x1400000000ULL;
    const auto fname=[&](std::string_view text) {
        const u32 index=static_cast<u32>(names.size()/2);
        const std::uint16_t header=static_cast<std::uint16_t>(text.size()<<6);
        PutLe(names,header,2);
        names.insert(names.end(),text.begin(),text.end());
        if (names.size()%2) names.push_back(0);
        g.Put(block,names.data(),names.size());
        return std::array<u32,2>{index,0};
    };
    const auto object=[&](u64 addr,std::string_view name) {
        const auto nm=fname(name);
        const u64 cls=next_class;next_class+=0x100;
        std::vector<u8> bytes(0x1000,0), cb(0x28,0);
        std::memcpy(bytes.data()+0x10,&cls,8);std::memcpy(cb.data()+0x18,nm.data(),8);
        g.Put(addr,bytes.data(),bytes.size());g.Put(cls,cb.data(),cb.size());
    };
    const auto put=[&](u64 addr,const auto& value) { std::memcpy(g.At(addr),&value,sizeof(value)); };
    g.Put(roots.name_pool+0x28,block);g.Put(roots.g_slot,game);
    object(game,"Game");object(ui,"BP_NicolaUIManager_C");object(manager,"BP_UIWidgetManager_C");
    object(guide_manager,"NicolaMapGuideManager");
    put(game+0x418,ui);put(ui+0x7c0,manager);put(game+0x480,guide_manager);
    std::vector<u64> ctrl;
    for (const auto cl : {"UICommonMenuCheck","UIMiniMapMenu","UIFieldPopup","UIEventMovieControl",
        "UIFieldEfxMenu","UIFieldTopMenu","UIFieldTopMenuListTop","UIFieldTacticsMenu","UIFieldTacticsMenuListTop"}) {
        const u64 addr=0x1500000000ULL+ctrl.size()*0x1000;
        object(addr,cl);ctrl.push_back(addr);
    }
    g.Put(controls,ctrl.data(),ctrl.size()*8);
    put(manager+0x298,controls);put(manager+0x2a0,std::int32_t{5});
    const dq3::GuestRead read=[&](u64 a,void* o,std::size_t n){return g.Read(a,o,n);};
    auto menu=dq3::ReadFieldMenu(read,roots);
    CHECK(menu && menu->mode==dq3::FieldMenuMode::Field);
    std::map<u64, std::string> class_cache; // perf: a persistent UClass* -> name cache gives the same answers
    CHECK(dq3::ReadFieldMenu(read,roots,&class_cache)==menu && class_cache.size()==7); // UI, manager, 5 controls
    CHECK(dq3::ReadFieldMenu(read,roots,&class_cache)==menu);
    {   // the same chain lists the controller classes (system screens: the title's save load)
        const auto list=dq3::ReadUiControllers(read,roots,&class_cache);
        CHECK(list && list->size()==5 && (*list)[0].name=="UICommonMenuCheck");
    }
    const u64 prompt=0x150000b000ULL;
    object(prompt,"UIFieldEfxMenuWindowGuide");
    auto prompt_controls=ctrl;prompt_controls.resize(5);prompt_controls.push_back(prompt);
    g.Put(controls,prompt_controls.data(),prompt_controls.size()*8);put(manager+0x2a0,std::int32_t{6});
    menu=dq3::ReadFieldMenu(read,roots);CHECK(menu && menu->mode==dq3::FieldMenuMode::Field);
    prompt_controls.erase(prompt_controls.begin()+4);
    g.Put(controls,prompt_controls.data(),prompt_controls.size()*8);put(manager+0x2a0,std::int32_t{5});
    CHECK(!dq3::ReadFieldMenu(read,roots)); // guide without effect parent
    g.Put(controls,ctrl.data(),ctrl.size()*8);put(manager+0x2a0,std::int32_t{5});
    const u64 service=0x150000a000ULL;
    object(service,"UIBarMenu");
    auto bar_controls=ctrl;bar_controls.resize(5);bar_controls.push_back(service);
    g.Put(controls,bar_controls.data(),bar_controls.size()*8);put(manager+0x2a0,std::int32_t{6});
    menu=dq3::ReadFieldMenu(read,roots);CHECK(menu && menu->mode==dq3::FieldMenuMode::Other); // a service panel
    CHECK(dq3::ReadFieldMenu(read,roots,&class_cache)==menu);
    g.Put(controls,ctrl.data(),ctrl.size()*8);
    put(manager+0x2a0,std::int32_t{129});CHECK(!dq3::ReadFieldMenu(read,roots) && !dq3::ReadUiControllers(read,roots));
    put(manager+0x2a0,std::int32_t{0});CHECK(dq3::ReadUiControllers(read,roots) && dq3::ReadUiControllers(read,roots)->empty());
    put(manager+0x2a0,std::int32_t{7});put(ctrl[6]+0x94,std::int32_t{5});
    menu=dq3::ReadFieldMenu(read,roots);
    CHECK(menu && menu->mode==dq3::FieldMenuMode::Top && menu->cursor==5);
    put(manager+0x2a0,std::int32_t{9});put(ctrl[8]+0x94,std::int32_t{1});
    const u64 commands=0x1600000000ULL;const u8 ids[]={1,2};g.Put(commands,ids,sizeof(ids));
    put(ctrl[8]+0xc0,commands);put(ctrl[8]+0xc8,std::int32_t{2});
    menu=dq3::ReadFieldMenu(read,roots);
    CHECK(menu && menu->mode==dq3::FieldMenuMode::Misc && menu->cursor==1 && menu->command==2);
    const u64 formation=0x1500009000ULL;
    object(formation,"UIFieldTacticsFormationMenuListTop");
    auto with_formation=ctrl;with_formation.push_back(formation);
    g.Put(controls,with_formation.data(),with_formation.size()*8);
    put(manager+0x2a0,std::int32_t{10});put(ctrl[8]+0x94,std::int32_t{3});
    put(formation+0x94,std::int32_t{2});put(formation+0x98,std::int32_t{4});put(formation+0xb0,std::int32_t{1});
    menu=dq3::ReadFieldMenu(read,roots);
    CHECK(menu && menu->mode==dq3::FieldMenuMode::LineUp && menu->cursor==2 && menu->selected==1 && menu->count==4);
    auto formation_prompt=with_formation;formation_prompt.push_back(prompt);
    g.Put(controls,formation_prompt.data(),formation_prompt.size()*8);put(manager+0x2a0,std::int32_t{11});
    menu=dq3::ReadFieldMenu(read,roots);CHECK(menu && menu->mode==dq3::FieldMenuMode::LineUp);
    g.Put(controls,with_formation.data(),with_formation.size()*8);put(manager+0x2a0,std::int32_t{10});
    put(formation+0xb0,std::int32_t{4});CHECK(!dq3::ReadFieldMenu(read,roots));
    put(formation+0xb0,std::int32_t{-1});put(formation+0x94,std::int32_t{4});CHECK(!dq3::ReadFieldMenu(read,roots));
    put(formation+0x94,std::int32_t{0});put(formation+0x98,std::int32_t{5});CHECK(!dq3::ReadFieldMenu(read,roots));
    put(ctrl[8]+0x94,std::int32_t{1});
    auto overworld = ctrl;
    overworld.erase(overworld.begin()+4);
    g.Put(controls,overworld.data(),overworld.size()*8);
    put(manager+0x2a0,std::int32_t{4});
    CHECK(dq3::ReadFieldMenu(read,roots)->mode==dq3::FieldMenuMode::Field);
    put(manager+0x2a0,std::int32_t{6});
    CHECK(dq3::ReadFieldMenu(read,roots)->mode==dq3::FieldMenuMode::Top);
    put(manager+0x2a0,std::int32_t{8});
    menu=dq3::ReadFieldMenu(read,roots);
    CHECK(menu && menu->mode==dq3::FieldMenuMode::Misc && menu->cursor==1 && menu->command==2);
    g.Put(controls,ctrl.data(),ctrl.size()*8);
    put(manager+0x2a0,std::int32_t{9});
    put(ctrl[8]+0x94,std::int32_t{2});CHECK(!dq3::ReadFieldMenu(read,roots));
    put(ctrl[8]+0x94,std::int32_t{1});
    put(manager+0x2a0,std::int32_t{129});CHECK(!dq3::ReadFieldMenu(read,roots));
    put(manager+0x2a0,std::int32_t{9});object(ctrl[7],"UnexpectedDialog");
    CHECK(dq3::ReadFieldMenu(read,roots)->mode==dq3::FieldMenuMode::Other);
    // Journal state (0xc02da0 current guide, tips read bytes, medals): G+0x528 GD, GD+0x18 P, GD+0x48 R
    const u64 gd=0x1600000000ULL,p=gd+0x1000,r=gd+0x2000,tips=gd+0x3000,medals=gd+0x4000;
    std::vector<u8> gdb(0x100,0),pb(0x400,0),rb(0x40,0);
    g.Put(gd,gdb.data(),gdb.size());g.Put(p,pb.data(),pb.size());g.Put(r,rb.data(),rb.size());
    put(game+0x528,gd);put(gd+0x18,p);put(gd+0x48,r);
    const auto main_guide=fname("MAPGUIDE_Aliahan_03"),lancel=fname("MAPGUIDE_Lancel_12"),reward=fname("MEDAL_003");
    put(r+0xc,main_guide);put(guide_manager+0x28,lancel);
    const u8 read_bytes[4]={0,1,0,0};g.Put(tips,read_bytes,4);put(r+0x18,tips);put(r+0x20,std::int32_t{4});
    g.Put(medals,reward.data(),8);put(p+0x40,medals);put(p+0x48,std::int32_t{1});put(p+0x34,std::int32_t{7});
    put(p+0x1f0,std::uint64_t{0x3e});
    auto js=dq3::ReadJournalState(read,roots);
    CHECK(js && js->guide=="MAPGUIDE_Aliahan_03" && js->tips_read==std::vector<u8>({0,1,0,0}) && js->medals==7 &&
          js->medals_got==std::vector<std::string>{"MEDAL_003"} && js->progress.Test(5) && !js->progress.Test(6));
    // Lancel trial (progress 56 set, 59 not): the manager's +0x28 guide
    put(p+0x1f0,(std::uint64_t{1}<<56)|0x3e);
    js=dq3::ReadJournalState(read,roots);
    CHECK(js && js->guide=="MAPGUIDE_Lancel_12");
    put(p+0x1f0,(std::uint64_t{1}<<56)|(std::uint64_t{1}<<59));
    js=dq3::ReadJournalState(read,roots);
    CHECK(js && js->guide=="MAPGUIDE_Aliahan_03");
    put(r+0x20,std::int32_t{513});CHECK(!dq3::ReadJournalState(read,roots));
    put(r+0x20,std::int32_t{4});put(p+0x48,std::int32_t{65});CHECK(!dq3::ReadJournalState(read,roots));
}

// ---- real data (optional) ---------------------------------------------------------------------

// Party tab readers on canned guest memory: member detail (personality, EXP, stat inputs, bag, skills),
// the override record, the LUCK_ZORO effect, torn reads and the three shared bags.
void TestInfoReaders() {
    FakeGuest g;
    dq3::Roots roots;
    roots.g_slot = 0x1000000000ULL;
    roots.name_pool = 0x1300001000ULL;
    const u64 block = 0x1300002000ULL;
    std::vector<u8> names(2, 0);
    const auto fname = [&](std::string_view text) {
        const u32 index = static_cast<u32>(names.size() / 2);
        const std::uint16_t header = static_cast<std::uint16_t>(text.size() << 6);
        PutLe(names, header, 2);
        names.insert(names.end(), text.begin(), text.end());
        if (names.size() % 2)
            names.push_back(0);
        g.Put(block, names.data(), names.size());
        return std::array<u32, 2>{index, 0};
    };
    g.Put(roots.name_pool + 0x28, block);
    const dq3::GuestRead read = [&](u64 a, void* o, std::size_t n) { return g.Read(a, o, n); };
    const u64 heap = 0x1400000000ULL, C = heap, F = heap + 0x1000, bag = heap + 0x2000, slots = heap + 0x3000,
              skills = heap + 0x4000, fx = heap + 0x5000;
    std::vector<u8> c(0x100, 0), f(0x1c0, 0), b(0x100, 0);
    const auto put = [](std::vector<u8>& v, std::size_t off, const auto& x) { std::memcpy(v.data() + off, &x, sizeof(x)); };
    put(c, 0x58, F);
    put(c, 0x78, bag);
    c[0xaa] = 3; // Fighter
    put(c, 0xac, fname("PERSONALITY_PARAGON"));
    put(f, 8, C);
    put(f, 0x1c, std::int32_t{12});
    put(f, 0x20, std::int32_t{3456});
    const double base[6] = {40.7, 30.2, 20.9, 10.1, 5.5, 7.99};
    const std::int32_t bonus[6] = {1, 2, 3, 4, 5, 6};
    std::memcpy(f.data() + 0x38, base, sizeof(base));
    std::memcpy(f.data() + 0x70, bonus, sizeof(bonus));
    put(f, 0xa8, skills);
    put(f, 0xb0, std::int32_t{2});
    // two bag slots: an equipped weapon with an instance extra entry, an unequipped item
    const u64 def0 = heap + 0x6000, def1 = heap + 0x6100, extras = heap + 0x6200;
    std::vector<u8> s(0x40, 0), d0(0x20, 0), d1(0x20, 0);
    put(s, 0, std::int32_t{1});
    s[4] = 1;
    put(s, 8, def0);
    put(s, 0x20, std::int32_t{5});
    put(s, 0x28, def1);
    g.Put(slots, s.data(), s.size());
    const u64 slot_ptrs[4] = {slots, 0, slots + 0x20, 0};
    g.Put(heap + 0x3800, slot_ptrs, sizeof(slot_ptrs));
    put(d0, 8, fname("ITEM_EQUIP_WEAPON_COPPER_SWORD"));
    put(d0, 0x10, extras);
    put(d0, 0x18, std::int32_t{1});
    put(d1, 8, fname("ITEM_USE_ITEM_MEDICAL_HERB"));
    const u8 extra[8] = {0, 5, 1, 0, 9, 0, 0, 0};
    g.Put(extras, extra, 8);
    g.Put(def0, d0.data(), d0.size());
    g.Put(def1, d1.data(), d1.size());
    put(b, 0x28, heap + 0x3800);
    put(b, 0x30, std::int32_t{2});
    put(b, 0x88, fx);
    put(b, 0x90, std::int32_t{0});
    std::vector<u8> sk(0x48, 0);
    const auto mera = fname("MAGIC_ATTACK_MAGIC_MERA"), heal = fname("MAGIC_RECOVERY_MAGIC_HOIMI");
    std::memcpy(sk.data() + 4, mera.data(), 8);
    sk[0xc] = 2;
    sk[0xd] = 2;
    std::memcpy(sk.data() + 0x24 + 4, heal.data(), 8);
    sk[0x24 + 0xc] = 1;
    sk[0x24 + 0xd] = 1;
    g.Put(skills, sk.data(), sk.size());
    g.Put(C, c.data(), c.size());
    g.Put(F, f.data(), f.size());
    g.Put(bag, b.data(), b.size());
    auto m = dq3::ReadMemberDetail(read, roots, F);
    CHECK(m && m->personality == "PERSONALITY_PARAGON" && m->level == 12 && m->exp == 3456 && m->stats.vocation == 3);
    CHECK(m && m->stats.base[0] == 40.7 && m->stats.bonus[5] == 6 && m->stats.items.size() == 2);
    CHECK(m && m->stats.items[0].id == "ITEM_EQUIP_WEAPON_COPPER_SWORD" && m->stats.items[0].equip == 1 &&
          m->stats.items[0].extras.size() == 1 && m->stats.items[1].count == 5 && !m->stats.luck_override);
    CHECK(m && m->skills.size() == 2 && m->skills[0].id == "MAGIC_ATTACK_MAGIC_MERA" && m->skills[1].learn == 1);
    const auto raw = dq3::ReadSkillEntries(read, F);
    CHECK(raw && raw->size() == 2 && (*raw)[1].learn == 1 && dq3::StatRecord(read, F) == F);
    const auto str_f = dq3::ReadStrength(read, F); // GetStr: floor((float)40.7) + F's bonus
    CHECK(str_f && *str_f == 40 + m->stats.bonus[0]);
    // the stat record override: [F+0x1A8] while F+0x1B0's shared count >= 1
    const u64 alt = heap + 0x7000, ctrl = heap + 0x7800;
    std::vector<u8> a(0x100, 0);
    const double abase[6] = {99, 98, 97, 96, 95, 94};
    std::memcpy(a.data() + 0x38, abase, sizeof(abase));
    g.Put(alt, a.data(), a.size());
    g.Put(ctrl, std::int32_t{0});
    put(f, 0x1a8, alt);
    put(f, 0x1b0, ctrl);
    g.Put(F, f.data(), f.size());
    m = dq3::ReadMemberDetail(read, roots, F);
    CHECK(m && m->stats.base[0] == 40.7); // no shared reference: F itself
    std::vector<u8> cb(0x10, 0);
    put(cb, 8, std::int32_t{1});
    g.Put(ctrl, cb.data(), cb.size());
    m = dq3::ReadMemberDetail(read, roots, F);
    CHECK(m && m->stats.base[0] == 99 && m->stats.bonus[0] == 0);
    CHECK(dq3::StatRecord(read, F) == alt);
    // battle detail Strength follows the same record (base 99 and bonus 0 from the override, not F's)
    CHECK(dq3::ReadStrength(read, F) == 99);
    CHECK(!dq3::ReadStrength(read, 0));
    // LUCK_ZORO (0x5E) in the bag's effect list
    const u64 fxo = heap + 0x8000;
    std::vector<u8> fo(0x20, 0);
    fo[0x18] = 0x5e;
    g.Put(fxo, fo.data(), fo.size());
    const u64 fxe[2] = {fxo, 0};
    g.Put(fx, fxe, sizeof(fxe));
    put(b, 0x90, std::int32_t{1});
    g.Put(bag, b.data(), b.size());
    m = dq3::ReadMemberDetail(read, roots, F);
    CHECK(m && m->stats.luck_override);
    // structure checks: owner mismatch, bad counts, a torn second pass
    put(f, 8, C + 8);
    g.Put(F, f.data(), f.size());
    CHECK(!dq3::ReadMemberDetail(read, roots, F));
    put(f, 8, C);
    g.Put(F, f.data(), f.size());
    put(b, 0x30, std::int32_t{513});
    g.Put(bag, b.data(), b.size());
    CHECK(!dq3::ReadMemberDetail(read, roots, F));
    put(b, 0x30, std::int32_t{2});
    g.Put(bag, b.data(), b.size());
    int reads_seen = 0;
    g.on_read = [&](int) { ++reads_seen; };
    CHECK(dq3::ReadMemberDetail(read, roots, F)); // two equal passes (FName text cached)
    const int per_pass = reads_seen / 2;
    reads_seen = 0;
    g.on_read = [&](int) {
        if (++reads_seen == per_pass + 1) {
            const std::int32_t changed = 3457;
            std::memcpy(g.At(F + 0x20), &changed, 4); // the EXP moves between the two passes
        }
    };
    CHECK(!dq3::ReadMemberDetail(read, roots, F));
    g.on_read = nullptr;
    const auto again = dq3::ReadMemberDetail(read, roots, F);
    CHECK(again && again->exp == 3457);
    // shared bags: roster entries with vocation 11 / 12 / 13
    const u64 G = 0x1500000000ULL, GD = G + 0x1000, P = G + 0x2000, PM = G + 0x3000, arr = G + 0x4000;
    std::vector<u8> go(0x600, 0), gdo(0x100, 0), po(0x400, 0), pmo(0x40, 0);
    put(go, 0x528, GD);
    put(gdo, 0x18, P);
    put(po, 0x220, PM);
    put(pmo, 0x10, arr);
    put(pmo, 0x18, std::int32_t{4});
    g.Put(roots.g_slot, G);
    g.Put(G, go.data(), go.size());
    g.Put(GD, gdo.data(), gdo.size());
    g.Put(P, po.data(), po.size());
    g.Put(PM, pmo.data(), pmo.size());
    std::vector<u64> entries{C, 0};
    for (u8 voc = 11; voc <= 13; ++voc) {
        const u64 cc = G + 0x10000 + voc * 0x100;
        std::vector<u8> co(0x100, 0);
        co[0xaa] = voc;
        put(co, 0x78, bag);
        g.Put(cc, co.data(), co.size());
        entries.push_back(cc);
        entries.push_back(0);
    }
    g.Put(arr, entries.data(), entries.size() * 8);
    const auto bags = dq3::ReadBags(read, roots);
    CHECK(bags && bags->bags[0].size() == 2 && bags->bags[2][1].id == "ITEM_USE_ITEM_MEDICAL_HERB");
    const auto owners = dq3::ReadBagOwners(read, roots);
    CHECK(owners && (*owners)[0] == bag && (*owners)[1] == bag && (*owners)[2] == bag);
    const auto one = owners ? dq3::ReadBagItems(read, roots, (*owners)[1]) : std::nullopt;
    CHECK(one && bags && *one == bags->bags[1]);
    entries.resize(6);
    g.Put(arr, entries.data(), entries.size() * 8);
    put(pmo, 0x18, std::int32_t{3});
    g.Put(PM, pmo.data(), pmo.size());
    CHECK(!dq3::ReadBags(read, roots)); // the Important Items entry is missing
    CHECK(!dq3::ReadBagOwners(read, roots));
}

void TestWorldIconDiscovery() {
    FakeGuest g;
    PutParty(g, {});
    dq3::Roots roots;
    roots.g_slot = MainBase + 0x56262e8;
    roots.name_pool = 0x1300001000ULL;
    const u64 block = 0x1300002000ULL, arr = 0x1300003000ULL, p = 0x1200002000ULL;
    g.Put(roots.name_pool + 0x28, block);
    std::vector<u8> names(64, 0);
    const std::uint16_t h = 5 << 6;
    std::memcpy(names.data()+2, &h, 2); std::memcpy(names.data()+4, "known", 5);
    g.Put(block, names.data(), names.size());
    const u32 ids[] = {1, 0}; g.Put(arr, ids, sizeof(ids));
    std::memcpy(g.At(p+0x230), &arr, 8);
    const std::int32_t count = 1; std::memcpy(g.At(p+0x238), &count, 4);
    dq3::MapData data;
    data.rows["floor"] = {};
    data.destinations["known"] = {"floor", "tag", 0, {}};
    data.destinations["unvisited"] = {"floor", "other", 1, {}};
    dq3::LocationCache cache;
    cache.world_anchors["tag"] = {10,20}; cache.world_anchors["other"] = {30,40};
    const dq3::GuestRead read = [&](u64 a, void* out, std::size_t n) { return g.Read(a,out,n); };
    auto icons = dq3::ReadWorldIcons(read,roots,data,{},cache);
    CHECK(icons && icons->size()==1 && icons->front().x==10 && icons->front().type==0);
    // Unreadable acquired array fails, rather than revealing every row in the game table.
    const std::int32_t bad_count = 4097; std::memcpy(g.At(p+0x238), &bad_count, 4);
    CHECK(!dq3::ReadWorldIcons(read,roots,data,{},cache));
    const std::int32_t zero = 0; std::memcpy(g.At(p+0x238), &zero, 4);
    icons = dq3::ReadWorldIcons(read,roots,data,{},cache);
    CHECK(icons && icons->empty());
}

// ReadChests (dungeon treasure count): treasure triggers of the loaded _SearchObj levels, opened per the
// progress bits P+0x128. A level whose actor array is unchanged reuses its scan (only the bits are read
// again); a changed array or an actor that failed to read is scanned again.
void TestChests() {
    FakeGuest g;
    PutParty(g, {});
    dq3::Roots roots;
    roots.g_slot = MainBase + 0x56262e8;
    roots.name_pool = 0x1300001000ULL;
    const u64 P = 0x1200002000ULL, block = 0x1300002000ULL;
    g.Put(roots.name_pool + 0x28, block);
    std::vector<u8> names(256, 0);
    std::size_t at = 0;
    const auto name = [&](std::string_view s) {
        const u32 index = static_cast<u32>(at / 2);
        const auto h = static_cast<std::uint16_t>(s.size() << 6);
        std::memcpy(names.data() + at, &h, 2);
        std::memcpy(names.data() + at + 2, s.data(), s.size());
        at += (2 + s.size() + 1) & ~std::size_t{1};
        return index;
    };
    const u32 n_map = name("MAPLIST_TEST"), n_search = name("Lv_Test_SearchObj"), n_art = name("Lv_Test_Art"),
              n_trigger = name("SearchObjEventTrigger"), n_mesh = name("StaticMeshActor");
    g.Put(block, names.data(), names.size());
    const u64 llm = 0x1400000000ULL, info = llm + 0x1000, list = llm + 0x2000, lsd_art = llm + 0x3000,
              lsd_search = llm + 0x4000, level = llm + 0x5000, array = llm + 0x6000, cls_trigger = llm + 0x7000,
              cls_mesh = llm + 0x7100;
    const u32 key[2] = {n_map, 0};
    const auto put32 = [](std::vector<u8>& o, std::size_t off, u32 v) { std::memcpy(o.data() + off, &v, 4); };
    const auto put64 = [](std::vector<u8>& o, std::size_t off, u64 v) { std::memcpy(o.data() + off, &v, 8); };
    std::vector<u8> o(0x200, 0);
    std::memcpy(o.data() + 0x1b4, key, 8);
    put64(o, 0x38, info);
    put32(o, 0x40, 1);
    g.Put(llm, o.data(), o.size());
    o.assign(0x30, 0);
    std::memcpy(o.data(), key, 8);
    put64(o, 0x18, list);
    put32(o, 0x20, 2);
    g.Put(info, o.data(), o.size());
    const u64 sublevels[4] = {lsd_art, 0, lsd_search, 0};
    g.Put(list, sublevels, sizeof(sublevels));
    for (const auto& [lsd, pkg, loaded] : {std::tuple{lsd_art, n_art, u64{0}}, std::tuple{lsd_search, n_search, level}}) {
        o.assign(0x130, 0);
        put32(o, 0x50, pkg);
        put64(o, 0x128, loaded);
        g.Put(lsd, o.data(), o.size());
    }
    for (const auto& [cls, n] : {std::pair{cls_trigger, n_trigger}, std::pair{cls_mesh, n_mesh}}) {
        o.assign(0x20, 0);
        put32(o, 0x18, n);
        g.Put(cls, o.data(), o.size());
    }
    // actor k at llm + 0x10000 * (k + 1), its trigger 0x1000 above it
    const auto actor = [&](int k, u64 cls, std::uint16_t flag, u8 type) {
        const u64 a = llm + 0x10000ULL * static_cast<u64>(k + 1), t = a + 0x1000;
        o.assign(0x260, 0);
        put64(o, 0x258, cls ? t : 0);
        g.Put(a, o.data(), o.size());
        o.assign(0x108, 0);
        put64(o, 0x10, cls);
        std::memcpy(o.data() + 0x102, &flag, 2);
        o[0x105] = type;
        g.Put(t, o.data(), o.size());
        return a;
    };
    std::vector<u64> actors{actor(0, cls_trigger, 3, 11),  actor(1, cls_trigger, 70, 14), 0,
                            actor(2, cls_trigger, 4, 5),   actor(3, cls_mesh, 5, 11),     actor(4, 0, 0, 0)};
    const auto put_actors = [&] {
        g.Put(array, actors.data(), actors.size() * 8);
        o.assign(0xa8, 0);
        put64(o, 0x98, array);
        put32(o, 0xa0, static_cast<u32>(actors.size()));
        g.Put(level, o.data(), o.size());
    };
    put_actors();
    const auto set_bit = [&](unsigned flag) { g.At(P + 0x128 + 8 * (flag >> 6))[(flag & 63) >> 3] |= u8(1u << (flag & 7)); };
    const dq3::GuestRead read = [&](u64 a, void* out, std::size_t n) { return g.Read(a, out, n); };
    using Count = std::optional<std::pair<int, int>>;
    dq3::ChestScan scan;
    CHECK((dq3::ReadChests(read, roots, llm, "MAPLIST_TEST", scan) == Count{{2, 0}}));
    CHECK(scan.levels.size() == 1 && scan.levels[0].flags == (std::vector<std::uint16_t>{3, 70}));
    set_bit(3);
    int before = g.reads;
    CHECK((dq3::ReadChests(read, roots, llm, "MAPLIST_TEST", scan) == Count{{2, 1}}));
    const int cached_reads = g.reads - before;
    dq3::ChestScan fresh;
    before = g.reads;
    CHECK((dq3::ReadChests(read, roots, llm, "MAPLIST_TEST", fresh) == Count{{2, 1}}));
    CHECK(g.reads - before > cached_reads + 5);
    set_bit(70);
    CHECK((dq3::ReadChests(read, roots, llm, "MAPLIST_TEST", scan) == Count{{2, 2}}));
    // a changed array: one more treasure (flag past the bit set: never counted as opened)
    actors.push_back(actor(5, cls_trigger, 0x55f, 12));
    put_actors();
    CHECK((dq3::ReadChests(read, roots, llm, "MAPLIST_TEST", scan) == Count{{3, 2}}));
    // an unreadable actor is skipped and not cached: once readable it counts
    actors.push_back(llm + 0x10000ULL * 7);
    put_actors();
    CHECK((dq3::ReadChests(read, roots, llm, "MAPLIST_TEST", scan) == Count{{3, 2}}) && scan.levels.empty());
    CHECK(actor(6, cls_trigger, 6, 13) == actors.back());
    set_bit(6);
    CHECK((dq3::ReadChests(read, roots, llm, "MAPLIST_TEST", scan) == Count{{4, 3}}) && scan.levels.size() == 1);
    // another map, or the level not loaded: unreadable
    CHECK(!dq3::ReadChests(read, roots, llm, "MAPLIST_OTHER", scan));
    std::memset(g.At(lsd_search + 0x128), 0, 8);
    CHECK(!dq3::ReadChests(read, roots, llm, "MAPLIST_TEST", scan));
    scan.Clear();
    CHECK(scan.levels.empty() && scan.classes.empty());
}

void TestMapData(const dq3::MapData& d, const dq3::RangeReader& read) {
    CHECK(d.destinations.size() == 196);
    const auto& aliahan = d.destinations.at("RURA_RIREMITO_RURA_ALIAHAN");
    CHECK(aliahan.tag == "MapIconTag_C01" && aliahan.type == 0 && aliahan.floor == "MAPLIST_C01F0101");
    const auto* town = d.Row("MAPLIST_C01F0101");
    CHECK(town && town->name_text == "Txt_Map_Place_Name_Aliahan" && town->floor_group == 2 && town->floor_sort == 2);
    CHECK(town && town->ox == 194.0f && town->oy == -1392.0f && town->sx == 18720.0f && town->sy == 19937.0f);
    CHECK(town && town->image >= 0 && dq3::MapImagePins()[static_cast<std::size_t>(town->image)].name == "T_UI_Map_Town_Aliahan_00");
    CHECK(town && town->spot_day == "MAP_SPOTICON_Aliahan_DayTime" && town->spot_night == "MAP_SPOTICON_Aliahan_Night");
    const auto* patty = d.Row("MAPLIST_C01R0201");
    CHECK(patty && patty->spot_index == 10 && patty->floor_group == 0);
    const auto* f1 = d.Row("MAPLIST_FIELD1");
    const auto* f2 = d.Row("MAPLIST_FIELD2");
    CHECK(f1 && f2 && !f1->underground && f2->underground && f1->image < 0);
    const auto& sp = d.spots.at("MAP_SPOTICON_Aliahan_DayTime");
    CHECK(sp[0].type == 1 && sp[0].x == 540.0f && sp[0].y == 340.0f && sp[9].type == 15 && sp[9].x == 156.0f &&
          sp[9].y == 332.0f && sp[11].type == 0);
    CHECK(d.hour_frame[0] == 3 && d.hour_frame[4] == 1 && d.hour_frame[15] == 1 && d.hour_frame[16] == 2 &&
          d.hour_frame[18] == 2 && d.hour_frame[19] == 3);
    CHECK(*d.Noun("Txt_Map_Floor_Name_Najimi_Tower_2") == U"Dreamer's Tower - 2F");
    CHECK(*d.Noun("Txt_Map_Place_Name_Aliahan") == U"Aliahan");
    CHECK(d.menu.at("Txt_Map_Menu_World_Map") == U"World Map");
    std::printf("map data: %zu rows, %zu spot rows, %zu nouns, %zu menu texts\n", d.rows.size(), d.spots.size(),
                d.nouns.size(), d.menu.size());
    // every pinned map texture: member hashes and layout (ASTC textures need the host decoder)
    int ok = 0;
    for (const auto& pin : dq3::MapImagePins()) {
        std::string why;
        const auto bytes = dq3::ReadMember(read, pin.member, &why);
        CHECK(bytes.has_value());
        if (!bytes) {
            std::fprintf(stderr, "map image %.*s: %s\n", static_cast<int>(pin.name.size()), pin.name.data(), why.c_str());
            continue;
        }
        const auto img = dq3::DecodeMapTexture(*bytes, pin, {}, &why);
        CHECK(img.has_value() || pin.format == 1);
        ok += img.has_value();
    }
    std::printf("map images: %d of %zu decoded\n", ok, dq3::MapImagePins().size());
    std::string world_why;
    const auto world_art = dq3::ComposeMapArt(read, "mapimg/" +
        std::to_string(dq3::FindMapImage("T_UI_Map_WorldMapImage_FL_1")) +
        "/664x592/592x592/36,0/-/0/2,296,296;", {}, &world_why);
    CHECK(world_art.has_value());
    CHECK(!dq3::ComposeMapArt(read, "mapimg/" +
        std::to_string(dq3::FindMapImage("T_UI_Map_WorldMapImage_FL_1")) +
        "/664x592/592x592/36,0/-/0/48,296,296;", {}, &world_why));
    // Detail tile orientation/seams: a crop across four tiles must copy their actual edge pixels.
    for (int source : {-1, -2}) {
        const auto patch = dq3::ComposeMapArt(read,"fieldimg/"+std::to_string(source)+"/0/4x4/1022,1022,4,4",{},&world_why);
        CHECK(patch.has_value());
        if (patch) for (int y=0; y<4; ++y) for (int x=0; x<4; ++x) {
            const int gx=1022+x, gy=1022+y;
            const auto name="T_UI_Map_FL_"+std::to_string(-source)+"_x"+std::to_string(3-gy/1024)+"_y"+std::to_string(3-gx/1024);
            const auto& pin=dq3::MapImagePins()[dq3::FindMapImage(name)];
            const auto bytes=dq3::ReadMember(read,pin.member,&world_why);
            const auto tile=bytes ? dq3::DecodeMapTexture(*bytes,pin,{},&world_why) : std::nullopt;
            CHECK(tile.has_value());
            if (tile) CHECK(std::memcmp(&patch->rgba[(y*4+x)*4], &tile->rgba[((gy%1024)*1024+gx%1024)*4],4)==0);
        }
    }
    for (const auto key : {"fieldimg/-1/0/664x592/-1,0,409,409", "fieldimg/-1/0/664x592/4095,4095,409,409",
                           "fieldimg/-3/0/664x592/0,0,409,409", "fieldimg/-1/0/664x592/0,0,99999999999999,409"})
        CHECK(!dq3::ComposeMapArt(read,key,{},&world_why));
    // composed Map page art (ASTC keys need the host decoder: nowloc, treasure, heal)
    const std::string town_key = "mapimg/" + std::to_string(dq3::FindMapImage("T_UI_Map_Town_Aliahan_00")) +
                             "/664x592/632x592/16,0/MAP_SPOTICON_Aliahan_DayTime/7d";
    for (const std::string& key : {town_key, std::string{"mframe/732x660"}, std::string{"quill/64"}, std::string{"fac/0/34"},
                                  std::string{"tsym/1/36"}, std::string{"coin/0/32"}, std::string{"titlebg/372x56"},
                                  std::string{"mlight/372x442"}, std::string{"mapbtn/64"}, std::string{"stepper/176x64/10"},
                                  std::string{"selrow/286x38"}, std::string{"ftabs/1120x76/0/90,370,650,930/50"}}) {
        std::string why;
        const auto img = dq3::ComposeMapArt(read, key, {}, &why);
        CHECK(img.has_value());
        if (!img)
            std::fprintf(stderr, "map art %s: %s\n", key.c_str(), why.c_str());
        if (const char* dir = std::getenv("DQ3_ART_DUMP"); img && dir && *dir) {
            std::string name = key;
            for (char& c : name)
                if (c == '/' || c == ',')
                    c = '_';
            std::ofstream out(std::string{dir} + "/" + name + ".rgba", std::ios::binary);
            out.write(reinterpret_cast<const char*>(img->rgba.data()), static_cast<std::streamsize>(img->rgba.size()));
            std::printf("dumped %s %ux%u\n", key.c_str(), img->width, img->height);
        }
    }
    const auto plain = dq3::ComposeMapArt(read, town_key, {}, nullptr);
    const auto marked = dq3::ComposeMapArt(read, town_key + "~1,300,200,0", {}, nullptr);
    CHECK(plain && marked && marked->width == plain->width && marked->height == plain->height);
    CHECK(plain && marked && marked->rgba != plain->rgba);
    // the cached base is never drawn into: another marker and the first one again give the same pixels
    const auto moved = dq3::ComposeMapArt(read, town_key + "~1,20,40,0", {}, nullptr);
    const auto again = dq3::ComposeMapArt(read, town_key + "~1,300,200,0", {}, nullptr);
    CHECK(moved && again && marked && moved->rgba != marked->rgba && again->rgba == marked->rgba);
    // the overlay equals the base with the marker drawn over it
    if (plain && marked) {
        auto manual = *plain;
        const auto quill = dq3::ComposeMapArt(read, "quill/64", {}, nullptr);
        CHECK(quill.has_value());
        if (quill) {
            dq3::Over(manual, *quill, 300, 200);
            CHECK(manual.rgba == marked->rgba);
        }
    }
    // A marker wholly outside the viewport is clipped, without changing the map underneath.
    const auto clipped = dq3::ComposeMapArt(read, town_key + "~1,-100,-100,0", {}, nullptr);
    CHECK(plain && clipped && plain->rgba == clipped->rgba);
    for (const auto suffix : {"~1,0,0", "~2,0,0,0", "~1,999999,0,0", "~1,0,0,361", "~1,0,0,0~1,0,0,0"})
        CHECK(!dq3::ComposeMapArt(read, town_key + suffix, {}, nullptr));
    CHECK(!dq3::ComposeMapArt(read, "nowloc/52", {}, nullptr)); // ASTC without a decoder fails closed
}


/// The location marker drawn into dq3.m.img ("~<world quill>,<x>,<y>,<rot>", x / y relative to the
/// map box), with x / y made absolute again: the box is the image widget's rect (dq3_map.h).
struct Marker {
    bool on{};
    int world{};
    long x{}, y{}, rot{};
};
Marker MarkerOf(const dq3::MapView& v, bool full) {
    Marker m;
    const auto it = v.texts.find("dq3.m.img");
    if (it == v.texts.end())
        return m;
    const auto t = it->second.find('~');
    if (t == std::string::npos)
        return m;
    m.on = std::sscanf(it->second.c_str() + t + 1, "%d,%ld,%ld,%ld", &m.world, &m.x, &m.y, &m.rot) == 4;
    m.x += (full ? dq3::MapX0 : dq3::MapRightX) + dq3::MapFin;
    m.y += (full ? dq3::FullMapY : dq3::MapPageY) + dq3::MapFin;
    return m;
}

void TestMapView() {
    dq3::MapData d;
    const int town_img = dq3::FindMapImage("T_UI_Map_Town_Aliahan_00");
    const int najima1 = dq3::FindMapImage("T_UI_Map_Dungeon_NajimiTower_01");
    CHECK(town_img >= 0 && najima1 >= 0 && dq3::FindMapImage("T_UI_Map_WorldMapImage_FL_1_2") >= 0);
    CHECK(dq3::FindMapImage("T_UI_Map_None") < 0);
    dq3::MapRow town{"Txt_Map_Place_Name_Aliahan", "Txt_Map_Place_Name_Aliahan", "C01", town_img,
                     194, -1392, 18720, 19937, 2, 2, "SPOT_DAY", "SPOT_NIGHT", 0, false, {}, {}};
    dq3::MapRow medal = town;
    medal.name_text = "Txt_Map_Floor_Name_Medal";
    medal.floor_sort = 1;
    dq3::MapRow patty = town;
    patty.floor_group = 0;
    patty.floor_sort = 0;
    patty.spot_index = 2;
    d.rows["MAPLIST_C01F0101"] = town;
    d.rows["MAPLIST_C01R0701"] = medal;
    d.rows["MAPLIST_C01R0201"] = patty;
    for (int k = 1; k <= 4; ++k) {
        dq3::MapRow f{"Txt_Map_Floor_Name_Tower_" + std::to_string(k), "Txt_Map_Place_Name_Tower", "D02", najima1,
                      -870, -3165, 13157, 14026, 1, k + 2, "None", "None", 0, false, {}, {}};
        d.rows["MAPLIST_D02R0" + std::to_string(k) + "01"] = f;
    }
    d.rows["MAPLIST_D02R0501"] = d.rows["MAPLIST_D02R0101"];
    d.rows["MAPLIST_D02R0501"].floor_sort = 2; // B1, never visited below
    d.rows["MAPLIST_FIELD1"] = {"None", "None", "", -1, 0, 0, 0, 0, 0, 0, "None", "None", 0, false, {}, {}};
    std::array<dq3::SpotEntry, 12> day{};
    day[0] = {1, 540, 340, 0, 0};   // church
    day[1] = {15, 156, 332, 0, 0};  // house (interior anchor, no icon)
    day[2] = {6, 268, 544, 0, 0};   // inn
    day[3] = {1, 160, 432, 0, 0};   // second church: one legend row
    day[4] = {14, 172, 308, 37, 0}; // bank, shown only with progress 37
    std::array<dq3::SpotEntry, 12> night = day;
    night[2].type = 0;
    d.spots["SPOT_DAY"] = day;
    d.spots["SPOT_NIGHT"] = night;
    for (int h = 0; h < 24; ++h)
        d.hour_frame[static_cast<std::size_t>(h)] = h < 4 || h >= 19 ? 3 : h >= 16 ? 2 : 1;
    d.nouns["Txt_Map_Place_Name_Aliahan"] = U"Aliahan";
    d.nouns["Txt_Map_Floor_Name_Medal"] = U"Mini Medal Manor";
    for (int k = 1; k <= 4; ++k)
        d.nouns["Txt_Map_Floor_Name_Tower_" + std::to_string(k)] = U"Tower - " + std::u32string(1, static_cast<char32_t>(U'0' + k)) + U"F";
    d.menu["Txt_Map_Menu_World_Map"] = U"World Map";
    dq3::MapRoots roots;
    roots.ok = true;
    roots.facility_cell = {0, 2, 3, 5, 4, 1, 6, 10, 11, 7, 8, 9, 12, 13};
    std::vector<std::string> visited{"MAPLIST_C01F0101", "MAPLIST_D02R0101", "MAPLIST_D02R0201", "MAPLIST_D02R0401"};
    dq3::WorldExtent world{true, -100800, -100800, 201600, 201600};
    dq3::LocationSnapshot loc;
    loc.map_id = "MAPLIST_C01F0101";
    loc.have_pos = true;
    loc.x = 1406;
    loc.y = 8740;
    loc.hour = 10;
    loc.gold = 1240;
    loc.facing_deg = 90;
    loc.facing_ok = true;
    dq3::MapViewInput in;
    in.data = &d;
    in.roots = &roots;
    in.loc = &loc;
    in.visited = &visited;
    in.world = &world;
    in.visible = true;
    auto v = dq3::BuildMapView(in);
    // values no widget binds are not published (the marker is in the image key)
    for (const char* gone : {"dq3.m.leg", "dq3.m.img.on", "dq3.m.browse", "dq3.m.step.up", "dq3.m.step.dn",
                             "dq3.m.mk.on", "dq3.m.mk.x", "dq3.m.mk.y", "dq3.m.mk.rot", "dq3.m.mk.world",
                             "dq3.m.mk.town", "dq3.m.mk.wq"})
        CHECK(!v.ints.contains(gone));
    // town: one visited floor of its group -> legend, unique types, bank hidden without progress 37
    CHECK(v.ints["dq3.m.flo"] == 0 && v.ints["dq3.mv.l"] == 1);
    CHECK(v.ints["dq3.m.l0.on"] == 1 && v.ints["dq3.m.l1.on"] == 1 && v.ints["dq3.m.l2.on"] == 0);
    CHECK(v.texts["dq3.m.l0.src"] == "module:dq3:fac/0/38" && v.texts["dq3.m.l1.src"] == "module:dq3:fac/1/38");
    CHECK(v.ints["dq3.m.l0.y"] == 0 && v.ints["dq3.m.l1.y"] == 48); // 48 px pitch (44 with seven rows)
    CHECK(v.texts["dq3.m.gold"] == dq3::Styled("1,240 G", dq3::FontStyle::Bold32));
    CHECK(v.texts["dq3.m.time"] == dq3::Styled("Daytime", dq3::FontStyle::Bold32));
    CHECK(v.texts["dq3.m.time_src"] == "module:dq3:tsym/0/38");
    // no font: the banner's largest size (Bold 44), baseline round(44 * 0.35) = 15 below the middle
    CHECK(v.texts["dq3.m.title"] == dq3::Styled(std::string_view{"Aliahan"}, dq3::FontStyle::Bold44) &&
          v.ints["dq3.m.title.dy"] == 15);
    // map box 716 x 664 (776 x 724 - 2 x 30), 688 x 644 image fitted at 1.031 -> 709 x 664; church (bit 0) +
    // inn (bit 2) + second church (bit 3) drawn, house (bit 1) and the hidden bank not
    CHECK(v.texts["dq3.m.img"].starts_with("module:dq3:mapimg/" + std::to_string(town_img) + "/716x664/709x664/3,0/SPOT_DAY/d~0,"));
    CHECK(v.ints["dq3.m.zoom.box"] == 1 && v.texts["dq3.m.zoom.label"] == dq3::Styled("Area map", dq3::FontStyle::Value));
    // marker: u = (8740 + 1392) * 688 / 19937, v = 644 - (1406 - 194) * 644 / 18720
    const double s = 664.0 / 644.0, u = (8740.0 + 1392.0) * 688.0 / 19937.0, vv = 644.0 - 1212.0 * 644.0 / 18720.0;
    auto mk = MarkerOf(v, false);
    CHECK(mk.on && mk.world == 0 && mk.rot == 90);
    CHECK(mk.x == std::lround(428 + (776 - 709) / 2 + u * s - 26));
    CHECK(mk.y == std::lround(228 + (724 - 664) / 2 + vv * s - 26));
    // with progress 37 the bank shows; at night the night row (no inn) and the moon
    loc.progress[0] = 1ull << 37;
    loc.hour = 20;
    v = dq3::BuildMapView(in);
    CHECK(v.texts["dq3.m.time_src"] == "module:dq3:tsym/1/38" &&
          v.texts["dq3.m.time"] == dq3::Styled("Night", dq3::FontStyle::Bold32));
    CHECK(v.ints["dq3.m.l1.on"] == 1 && v.texts["dq3.m.l1.src"] == "module:dq3:fac/13/38" && v.ints["dq3.m.l2.on"] == 0);
    loc.hour = 17;
    v = dq3::BuildMapView(in);
    CHECK(v.texts["dq3.m.time"] == dq3::Styled("Evening", dq3::FontStyle::Bold32) &&
          v.texts["dq3.m.time_src"] == "module:dq3:tsym/0/38");
    // Zooming a town out must never paint its local facilities onto island/world art.
    in.area_anchor = std::pair{-73909.7f, 33745.5f};
    for (int zoom : {1, 2}) {
        in.zoom = zoom;
        const auto town_world = dq3::BuildMapView(in);
        CHECK(town_world.texts.at("dq3.m.img").find("/-/0") != std::string::npos);
        CHECK(MarkerOf(town_world, false).world == 1);
    }
    in.zoom = 0;
    in.area_anchor.reset();
    // interior: marker at spot entry 2 (the house: no Y offset), no rotation
    loc.map_id = "MAPLIST_C01R0201";
    v = dq3::BuildMapView(in);
    mk = MarkerOf(v, false);
    CHECK(mk.x == std::lround(428 + 33 + 156 * s - 26) && mk.y == std::lround(258 + 332 * s - 26));
    CHECK(mk.rot == 0);
    // dungeon: visited 1F, 2F, 4F (not 3F, not B1), listed top floor first
    loc.map_id = "MAPLIST_D02R0201";
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.m.flo"] == 1 && !v.ints.contains("dq3.m.leg"));
    CHECK(v.texts["dq3.m.title"] == dq3::Styled(std::string_view{"Tower"}, dq3::FontStyle::Bold44));
    CHECK(v.texts["dq3.m.f0.name"] == dq3::Styled(std::u32string_view{U"4F"}, dq3::FontStyle::Bold32));
    CHECK(v.texts["dq3.m.f1.name"] == dq3::Styled(std::u32string_view{U"2F"}, dq3::FontStyle::Bold32));
    CHECK(v.texts["dq3.m.f2.name"] == dq3::Styled(std::u32string_view{U"1F"}, dq3::FontStyle::Bold32));
    CHECK(v.ints["dq3.m.f3.on"] == 0 && v.ints["dq3.m.f1.here"] == 1 && v.ints["dq3.m.f0.here"] == 0);
    CHECK(v.ints["dq3.m.step.n"] == 1 && v.texts["dq3.m.step.src"] == "module:dq3:stepper/246x88/11");
    // browse up twice: clamped at 4F, the marker hides while another floor is shown
    in.browse = -2;
    v = dq3::BuildMapView(in);
    CHECK(v.browse == -1 && !MarkerOf(v, false).on);
    CHECK(v.texts["dq3.m.step.src"] == "module:dq3:stepper/246x88/01" && v.ints["dq3.m.f0.sel"] == 1);
    in.browse = 0;
    // chests: shown only with a treasure on the map
    in.chests = std::pair{6, 1};
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.m.chest.on"] == 1 && v.texts["dq3.m.chest"] == dq3::Styled("Chests opened 1", dq3::FontStyle::Semi30) &&
          v.ints["dq3.m.chest.y"] == 3 * 52); // under the three visited floors
    in.chests = std::pair{0, 0};
    CHECK(dq3::BuildMapView(in).ints["dq3.m.chest.on"] == 0);
    // Counts belong to the current floor, never the browsed floor.
    in.chests = std::pair{6, 1};
    in.browse = -1;
    CHECK(dq3::BuildMapView(in).ints["dq3.m.chest.on"] == 0);
    in.browse = 0;
    // full map: left column gates closed, the full-size widgets open
    in.full = true;
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.mv.l"] == 0 && v.ints["dq3.mv.f"] == 1 && v.ints["dq3.m.flo"] == 0 && v.ints["dq3.m.step.f"] == 1);
    in.full = false;
    // world: FL_1 until progress 0x5b, then FL_1_2; the marker from the data asset extent
    in.zoom = 2;
    loc.map_id = "MAPLIST_FIELD1";
    loc.progress = {};
    loc.hour = 10;
    loc.x = -73909.7f;
    loc.y = 33745.5f;
    v = dq3::BuildMapView(in);
    CHECK(v.texts["dq3.m.img"].starts_with("module:dq3:mapimg/" + std::to_string(dq3::FindMapImage("T_UI_Map_WorldMapImage_FL_1")) + "/"));
    CHECK(v.texts["dq3.m.title"] == dq3::Styled(std::string_view{"World Map"}, dq3::FontStyle::Bold44));
    loc.progress[1] = 1ull << (0x5b - 64);
    v = dq3::BuildMapView(in);
    CHECK(v.texts["dq3.m.img"].starts_with("module:dq3:mapimg/" + std::to_string(dq3::FindMapImage("T_UI_Map_WorldMapImage_FL_1_2")) + "/"));
    const double ws = 664.0 / 1024.0, wu = (33745.5 + 100800) / 201600 * 1024, wv = (1 - (-73909.7 + 100800) / 201600) * 1024;
    mk = MarkerOf(v, false);
    CHECK(mk.world == 1);
    CHECK(v.ints["dq3.m.live.on"] == 1);
    CHECK(std::abs(mk.x - (428 + 56 + wu * ws - 12 * 64 / 176.0)) <= 1);
    CHECK(std::abs(mk.y - (258 + wv * ws - 156 * 64 / 176.0)) <= 1);
    // Night changes the time symbol, not the world's coordinate system or image.
    const auto daytime_world = v;
    loc.hour = 23;
    v = dq3::BuildMapView(in);
    CHECK(v.texts["dq3.m.time_src"] == "module:dq3:tsym/1/38");
    CHECK(v.texts["dq3.m.img"] == daytime_world.texts.at("dq3.m.img"));
    CHECK(MarkerOf(v, false).x == MarkerOf(daytime_world, false).x &&
          MarkerOf(v, false).y == MarkerOf(daytime_world, false).y);
    loc.x += 12600; // actual movement moves the marker in either map layout
    v = dq3::BuildMapView(in);
    CHECK(MarkerOf(v, false).y < MarkerOf(daytime_world, false).y);
    in.full = true;
    v = dq3::BuildMapView(in);
    CHECK(MarkerOf(v, true).world == 1 && v.ints["dq3.m.live.on"] == 0);
    in.full = false;
    loc.x -= 12600;
    // Detailed view tracks movement by scrolling the native tile crop; island uses its own extent.
    in.zoom = 0;
    v = dq3::BuildMapView(in);
    CHECK(v.texts["dq3.m.img"].starts_with("module:dq3:fieldimg/-1/1/716x664/"));
    CHECK(MarkerOf(v, false).on && MarkerOf(v, false).world == 0 && v.ints["dq3.m.zoom.in"] == 0 &&
          v.ints["dq3.m.zoom.out"] == 1);
    const auto before_walk = v.texts["dq3.m.img"];
    loc.x += 500;
    CHECK(dq3::BuildMapView(in).texts["dq3.m.img"] != before_walk);
    loc.x = -50000; loc.y = 50000;
    in.zoom = 1;
    v = dq3::BuildMapView(in);
    CHECK(v.texts["dq3.m.img"].starts_with("module:dq3:mapimg/" +
        std::to_string(dq3::FindMapImage("T_UI_Map_WorldMap_AllArea_Aliahan")) + "/"));
    in.full = true;
    CHECK(dq3::BuildMapView(in).texts["dq3.m.img"].find("/1108x956/") != std::string::npos);
    in.full = false;
    loc.x = 0; loc.y = 0;
    CHECK(dq3::BuildMapView(in).texts["dq3.m.img"].starts_with("module:dq3:fieldimg/"));
    in.zoom = 2;
    // Only icons in this world extent are encoded; underground and out-of-range anchors hide.
    std::vector<dq3::WorldIcon> icons{{0,0,0,false}, {1,0,0,true}, {2,900000,0,false}};
    in.world_icons = &icons;
    v = dq3::BuildMapView(in);
    CHECK(v.texts["dq3.m.img"].find("/2,332,332;~1,") != std::string::npos);
    CHECK(dq3::WorldVariantVisible("RURA_RIREMITO_RURA_VOLCANO_AFTER", loc));
    CHECK(!dq3::WorldVariantVisible("RURA_RIREMITO_RURA_VOLCANO", loc));
    CHECK(dq3::WorldVariantVisible("RURA_RIREMITO_RURA_BURG1", loc));
    loc.progress[1] |= 1ull << (0x4d - 64);
    CHECK(!dq3::WorldVariantVisible("RURA_RIREMITO_RURA_BURG1", loc));
    CHECK(dq3::WorldVariantVisible("RURA_RIREMITO_RURA_BURG6", loc));
    // another tab: every gate closed
    in.visible = false;
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.mv"] == 0 && v.ints["dq3.m.live.on"] == 0 && !v.ints.contains("dq3.m.img.on"));
    // unknown map: wait
    loc.map_id = "MAPLIST_NONE";
    in.visible = true;
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.m.wait"] == 1 && v.ints["dq3.m.on"] == 0);
    CHECK(dq3::SpotLabel(1) == "Church" && dq3::SpotLabel(15).empty());
    CHECK(dq3::FacilityCell(roots, 5, false) == 19 && dq3::FacilityCell(roots, 15, false) == -1);
}

// Party / Journal tab rules against native ground truth (research runs/party-journal-tabs: the
// read-only probes b-member-stats.json / b-journal-state.json / a-journal-state.json and the native
// Status, Traveller's Tips, Info and objective captures b03..b06, a04..a08, b02).
void TestInfoData(const dq3::InfoData& d, const dq3::MapData& map) {
    using dq3::OwnedItem;
    // the Misc menu's Heal All / Handy Heal All result lines the companion shows (dq3_heal.h)
    for (const auto o : {dq3::HealOutcome::Healed, dq3::HealOutcome::NothingToHeal, dq3::HealOutcome::MagicFailed,
                         dq3::HealOutcome::King, dq3::HealOutcome::Queen, dq3::HealOutcome::NoSpellsHere})
        for (int mode = 0; mode < 2; ++mode)
            for (const bool single : {false, true}) {
                const char* id = dq3::HealMessageId(o, mode, single);
                CHECK(id && d.Text(id) && !d.Text(id)->empty());
            }
    CHECK(*d.Text("Txt_FieldMenu_Message_AllHeal") == U"The party's wounds are healed!");
    CHECK(dq3::CleanGameText(dq3::NameHealMessage(*d.Text("Txt_FieldMenu_Message_Heal"), U"Erdrick")) ==
          U"Erdrick's wounds are healed!");
    CHECK(dq3::CleanGameText(*d.Text("Txt_FieldMenu_Message_Mantan_FailMagic")) == U"But the magic couldn't be cast!");
    CHECK(d.Noun("Txt_Status_Personality_AMAZON") && *d.Noun("Txt_Status_Personality_AMAZON") == U"Bodybuilder");
    CHECK(d.personality.at("PERSONALITY_SHOW-OFF") == "Txt_Status_Personality_SHOW-OFF");
    CHECK(*d.Noun(d.personality.at("PERSONALITY_SLIPPERY_DEVIL")) == U"Slippery Devil");
    CHECK(*d.Noun(d.personality.at("PERSONALITY_PARAGON")) == U"Paragon");
    // Exp. to Next Level (native Status): AA Hero Lv2 36 -> 43, Marcella Warrior Lv3 -> 40, Merl Mage Lv2 -> 5,
    // Pierre Priest Lv2 -> 2, Erdrick Lv99 -> 0
    CHECK(dq3::ExpToNext(d, 2, 1, 36) == 43);
    CHECK(dq3::ExpToNext(d, 3, 2, 36) == 40);
    CHECK(dq3::ExpToNext(d, 2, 4, 36) == 5);
    CHECK(dq3::ExpToNext(d, 2, 5, 36) == 2);
    CHECK(dq3::ExpToNext(d, 99, 1, 9999999) == 0);
    CHECK(!dq3::LevelExp(d, 5, 0) && !dq3::LevelExp(d, 5, 11) && dq3::LevelExp(d, 2, 10) == 27);
    // native Status stats (b03..b06) from the probed records
    struct Case {
        const char* name;
        dq3::StatInputs in;
        std::array<int, 8> want; // Str Res Agi Sta Wis Luck Atk Def
    };
    const auto items = [](std::initializer_list<std::pair<const char*, int>> l) {
        std::vector<OwnedItem> v;
        for (const auto& [id, eq] : l)
            v.push_back(OwnedItem{id, 1, static_cast<u8>(eq), {}});
        return v;
    };
    const Case cases[] = {
        {"Marcella", {{14.702, 16.785, 9.086, 16.657, 7.206, 7.839}, {4, 2, 2, 2, 0, 2}, 2,
                      items({{"ITEM_EQUIP_ARMOR_PLAIN_CLOTHES", 4}}), false}, {18, 18, 11, 18, 7, 9, 18, 23}},
        {"Merl", {{8.9, 12.241, 9.035, 8.984, 13.785, 9.125}, {0, 4, 0, 0, 8, 0}, 4,
                  items({{"ITEM_EQUIP_ARMOR_PLAIN_CLOTHES", 4}}), false}, {8, 16, 9, 8, 21, 9, 8, 21}},
        {"Pierre", {{8.901, 11.77, 8.147, 9.756, 12.02, 5.575}, {8, 4, 0, 0, 0, 0}, 5,
                    items({{"ITEM_EQUIP_ARMOR_PLAIN_CLOTHES", 4}, {"ITEM_USE_ITEM_MEDICAL_HERB", 0}}), false},
         {16, 15, 8, 9, 12, 5, 16, 20}},
        {"AA", {{23.82, 12.825, 6.3, 14.756, 4.883, 7.146}, {0, 0, 0, 0, 0, 0}, 1,
                items({{"ITEM_EQUIP_WEAPON_COPPER_SWORD", 1}, {"ITEM_EQUIP_SHIELD_LEATHER_SHIELD", 2},
                       {"ITEM_EQUIP_ARMOR_WAYFARERS_CLOTHES", 4}}), false}, {23, 12, 6, 14, 4, 7, 33, 25}},
    };
    for (const auto& c : cases) {
        const auto s = dq3::ComputeStats(d, c.in);
        const std::array<int, 8> got{s.strength, s.resilience, s.agility, s.stamina, s.wisdom, s.luck, s.attack, s.defence};
        CHECK(got == c.want);
        if (got != c.want)
            std::fprintf(stderr, "stats %s: %d %d %d %d %d %d %d %d\n", c.name, got[0], got[1], got[2], got[3], got[4],
                         got[5], got[6], got[7]);
        CHECK(s.luck_ok);
    }
    {
        // Fighter: ParamFighterDown replaces the weapon's OFFENSE value; RATIO and clamps
        dq3::StatInputs in{{20, 20, 50, 20, 20, 20}, {0, 0, 10, 0, 0, 0}, 3,
                           items({{"ITEM_EQUIP_WEAPON_COPPER_SWORD", 1}, {"ITEM_EQUIP_ACCESSORY_METEORITE_BRACER", 5}}),
                           true};
        const auto s = dq3::ComputeStats(d, in);
        CHECK(s.attack == 20 - 6);
        CHECK(s.agility == 50 + (50 + 10) * 100 / 100 + 10);
        CHECK(!s.luck_ok);
        in.base[0] = 5000;
        CHECK(dq3::ComputeStats(d, in).strength == 999);
    }
    // conditions (0x84a330)
    dq3::Progress p;
    for (const int i : {1, 2, 3, 4, 5})
        p.bits[0] |= std::uint64_t{1} << i;
    CHECK(dq3::EvalCondition("MAIN_ALIAHAN_MeetKing", p));
    CHECK(!dq3::EvalCondition("MAIN_NAJIMITOWER_GetKey", p));
    CHECK(dq3::EvalCondition("MAIN_REEVE_TalkNorthHouseNPC | MAIN_ALIAHAN_Wake", p));
    CHECK(!dq3::EvalCondition("MAIN_REEVE_TalkNorthHouseNPC | MAIN_NAJIMITOWER_Arrival1F", p));
    CHECK(!dq3::EvalCondition("BOGUS | MAIN_ALIAHAN_Wake", p));
    CHECK(dq3::EvalCondition("MAIN_ALIAHAN_Wake & MAIN_ALIAHAN_MeetKing", p));
    CHECK(!dq3::EvalCondition("MAIN_ALIAHAN_Wake & MAIN_REEVE_GetMagicBall", p));
    CHECK(!dq3::EvalCondition("", p) && !dq3::EvalCondition("NONE", p));
    // early objective (native b02: banner "Obtain the thief's key." + "Gather information in Reeve.")
    const auto early = dq3::ObjectiveFor(d, map, "MAPGUIDE_Aliahan_03", p);
    CHECK(early && early->main == U"Obtain the thief's key." && early->subs.size() == 1 &&
          early->subs[0] == U"Gather information in Reeve.");
    // postgame (native a01 banner "Defeat Zoma.")
    const auto zoma = dq3::ObjectiveFor(d, map, "MAPGUIDE_ZomaCastle_23", p);
    CHECK(zoma && zoma->main == U"Defeat Zoma." && zoma->subs.empty());
    CHECK(!dq3::ObjectiveFor(d, map, "MAPGUIDE_Nope", p));
    // Traveller's Tips (native a04 page 1: Autosaving, Running, The Main Menu, Cast Your Mind Back, Patty's
    // Party Planning Place, Perfecting Your Party, Helping Hands, Aiding Adventurers, Asking for Assistance,
    // Shops, Saving, How to Heal; all unread)
    const std::vector<std::uint8_t> none(68, 0);
    const auto tips = dq3::VisibleTips(d, p, none);
    static constexpr const char32_t* page1[] = {U"Autosaving", U"Running", U"The Main Menu", U"Cast Your Mind Back",
                                                U"Patty's Party Planning Place", U"Perfecting Your Party",
                                                U"Helping Hands", U"Aiding Adventurers", U"Asking for Assistance",
                                                U"Shops", U"Saving", U"How to Heal"};
    CHECK(tips.size() >= 12);
    for (std::size_t k = 0; k < 12 && k < tips.size(); ++k) {
        const auto* t = d.Text(d.info[tips[k].row].title);
        CHECK(t && *t == page1[k] && tips[k].unread);
    }
    std::vector<std::uint8_t> one = none;
    one[tips[0].row] = 1;
    CHECK(!dq3::VisibleTips(d, p, one)[0].unread);
    // 13 progress-gated rows (job change, key, ruler, ship, Ramia, ...) hidden at progress 1..5
    std::size_t gated = 0;
    for (const auto& r : d.info)
        gated += r.flag != 0;
    CHECK(tips.size() == d.info.size() - gated);
    // medals: nothing received -> MEDAL_003 (2 medals, Thorn Whip)
    const auto* m = dq3::NextMedal(d, {});
    CHECK(m && m->id == "MEDAL_003" && m->require == 2 && m->product == "ITEM_EQUIP_WEAPON_THORN_WHIP");
    const std::string got[] = {"MEDAL_003", "MEDAL_005"};
    m = dq3::NextMedal(d, got);
    CHECK(m && m->id == "MEDAL_008" && m->require == 5);
    std::vector<std::string> all;
    for (const auto& r : d.medals)
        all.push_back(r.id);
    CHECK(!dq3::NextMedal(d, all));
    // Defeated Monster List (native a06..a08): numerical order, 163 rows, names, EXP / gold / drop / habitat
    CHECK(d.library.size() == 163);
    CHECK(d.library[0].unit_master == "UNIT_MASTER_MN_001_SLIME" && d.library[1].unit_master == "UNIT_MASTER_MN_002_STARKRAVEN");
    CHECK(*d.Noun(d.library[1].name_text) == U"Stark Raven");
    const auto& slime = d.monsters.at(d.library[0].monster);
    CHECK(slime.exp == 4 && slime.gold == 1 && *d.Noun(d.items.at(slime.drop).name_text) == U"Medicinal Herb");
    const auto& raven = d.monsters.at(d.library[1].monster);
    CHECK(raven.exp == 8 && raven.gold == 2 && *d.Noun(d.items.at(raven.drop).name_text) == U"Chimaera Wing");
    CHECK(map.menu.at(d.library[0].habitat[0]) == U"Aliahan Region");
    CHECK(*map.Noun(d.library[0].habitat[1]) == U"Promontory Passage" && d.library[0].more);
    const auto* flav = d.Text(d.library[1].trivia);
    CHECK(flav && flav->starts_with(U"Colossal crows that mate"));
    for (const auto& r : d.library) {
        CHECK(d.monsters.contains(r.monster) && d.Text(r.trivia) && d.Noun(r.name_text));
        if (!d.Noun(r.name_text))
            std::fprintf(stderr, "library name %s\n", r.name_text.c_str());
    }
    // native list length: B (4 species, no boss) 148 rows = 15 pages; a recorded boss is listed
    std::map<std::string, std::int32_t> rec{{"UNIT_MASTER_MN_001_SLIME", 18}, {"UNIT_MASTER_MN_002_STARKRAVEN", 5},
                                            {"UNIT_MASTER_MN_003_BUNICORN", 3}, {"UNIT_MASTER_MN_005_BATTERFLY", 2}};
    CHECK(dq3::VisibleMonsters(d, rec).size() == 148);
    const auto boss = std::find_if(d.library.begin(), d.library.end(),
                                   [&](const dq3::LibraryRow& r) { return d.monsters.at(r.monster).boss; });
    CHECK(boss != d.library.end());
    rec[boss->unit_master] = 1;
    CHECK(dq3::VisibleMonsters(d, rec).size() == 149);
    // descriptions
    CHECK(*d.Text(d.items.at("ITEM_USE_ITEM_MEDICAL_HERB").flavor_text) ==
          U"Restores at least 30 HP to a single ally. Consumed upon use.");
    CHECK(dq3::CleanGameText(U"a <b>meathead</> b<--->c\r\nd") == U"a meathead b—c d");
    CHECK(d.magic_flavor.contains("MAGIC_ATTACK_MAGIC_MERA") && d.Text(d.magic_flavor.at("MAGIC_ATTACK_MAGIC_MERA")));
    // native field-menu labels (GOP_Text_FieldMenu): the Equipment screen's slot names (native/n02), stat names,
    // bag names and the counts the Party / Bag tabs show
    const std::pair<const char*, std::u32string> labels[] = {
        {"Txt_FieldMenu_Equip_BLANK_01", U"Weapons"}, {"Txt_FieldMenu_Equip_BLANK_02", U"Shields"},
        {"Txt_FieldMenu_Equip_BLANK_03", U"Headgear"}, {"Txt_FieldMenu_Equip_BLANK_04", U"Armour"},
        {"Txt_FieldMenu_Equip_BLANK_05", U"Main Accessory"}, {"Txt_FieldMenu_Equip_BLANK_06", U"Sub-Accessory"},
        {"Txt_FieldMenu_STATUS_PARAMNAME_03", U"Attack"}, {"Txt_FieldMenu_STATUS_PARAMNAME_07", U"Wisdom"},
        {"Txt_FieldMenu_STATUS_PARAMNAME_17", U"Max. HP"}, {"Txt_FieldMenu_Item_ITEMBAG", U"Item Bag"},
        {"Txt_FieldMenu_Item_EQUIPBAG", U"Equipment Bag"}, {"Txt_FieldMenu_Item_IMPOTANT", U"Important Items"},
        {"Txt_FieldMenu_Item_COUNTA_01", U"Number Held"}, {"Txt_FieldMenu_Item_NONE", U"Empty"},
        {"Txt_FieldMenu_Magic_NONE", U"No Spells Learnt"}, {"Txt_FieldMenu_Top_ITEM", U"Items"},
        {"Txt_FieldMenu_Top_MAGIC", U"Spells"}, {"Txt_FieldMenu_Top_EQUIP", U"Equipment"}};
    for (const auto& [id, want] : labels) {
        CHECK(d.Text(id) && *d.Text(id) == want);
        if (!d.Text(id) || *d.Text(id) != want)
            std::fprintf(stderr, "menu text %s\n", id);
    }
    // Goddess Ring: INTELLIGENCE ADD 33 -> "Wisdom +33" (the native item box names Wisdom, a10)
    const auto& ring = d.items.at("ITEM_EQUIP_ACCESSORY_GODDESS_RING");
    CHECK(ring.param_type == 4 && ring.param_up == 1 && ring.param_value == 33 && ring.icon == 4);
    // GetMaxHP / GetMaxMP equipment term: the 1.1.0.0 GOP_Item has no MAXHP / MAXMP ADD row (sum always 0)
    CHECK(!dq3::HasMaxHpMpItems(d));
    std::printf("info data: %zu items, %zu library rows, %zu tips, %zu medals, %zu guides\n", d.items.size(),
                d.library.size(), d.info.size(), d.medals.size(), d.guides.size());
}

// The literal key tables (dq3_keys.h) equal the formats they replace.
void TestKeyTables() {
    namespace keys = dq3::keys;
    char buf[64];
    static const char* member_fields[keys::MemberKeyCount] = {"on",     "name",  "lv",     "hp_t",   "mp_t",   "fcard",
                                                              "hp",     "hp_max", "mp",    "mp_max", "st0.on", "st0.x",
                                                              "st0.src", "st1.on", "st1.x", "st1.src"};
    for (int i = 0; i < 4; ++i)
        for (int f = 0; f < keys::MemberKeyCount; ++f) {
            std::snprintf(buf, sizeof(buf), "dq3.p%d.%s", i, member_fields[f]);
            CHECK(std::string_view{keys::Member[i][f]} == buf);
        }
    for (int k = 0; k < 6; ++k) {
        for (int p = 0; p < 2; ++p) {
            std::snprintf(buf, sizeof(buf), "dq3.party.s%dp%d", k, p);
            CHECK(std::string_view{keys::PartyPress[k][p]} == buf);
        }
        std::snprintf(buf, sizeof(buf), "@dq3_party_button%d", k);
        CHECK(std::string_view{keys::PartyCounter[k]} == buf);
    }
    for (int k = 0; k < 4; ++k) {
        std::snprintf(buf, sizeof(buf), "dq3.party.drag%d", k);
        CHECK(std::string_view{keys::PartyDrag[k]} == buf);
        std::snprintf(buf, sizeof(buf), "dq3.p%d.nodrag", k);
        CHECK(std::string_view{keys::NoDrag[k]} == buf);
    }
    for (int k = 0; k < 4; ++k) {
        std::snprintf(buf, sizeof(buf), "dq3.p%d.lvx", k);
        CHECK(std::string_view{keys::LvX[k]} == buf);
    }
    for (int k = 0; k < 3; ++k) {
        std::snprintf(buf, sizeof(buf), "dq3.ftab%d.x", k);
        CHECK(std::string_view{keys::FtabX[k]} == buf);
        std::snprintf(buf, sizeof(buf), "dq3.ftab%d.t", k);
        CHECK(std::string_view{keys::FtabT[k]} == buf);
    }
}

// The dynamic top strip / battle party row (owner 2026-10-10): N = 1..4 cards share the 36..1204 content box
// with 8 px gaps and no blank space; card i of N is the tap / drag rect the manifest repeats (gen_manifest
// check_party_strip asserts the manifest side).
void TestPartyStrip() {
    static constexpr int want_w[5] = {0, 1168, 580, 384, 286};
    for (int n = 1; n <= 4; ++n) {
        const int w = dq3::StripCardW(n);
        CHECK(w == want_w[n]);
        CHECK(w * n + dq3::StripGap * (n - 1) == dq3::FTabW);
        CHECK(dq3::StripCardX(n, 0) == dq3::FTabX0);
        CHECK(dq3::StripCardX(n, n - 1) + w == dq3::FTabX0 + dq3::FTabW); // no blank space on the right
        for (int i = 1; i < n; ++i)                                         // exactly one gap between neighbours
            CHECK(dq3::StripCardX(n, i) - (dq3::StripCardX(n, i - 1) + w) == dq3::StripGap);
        // every x of the content box is either one card or a gap; the card rects never overlap
        int covered = 0;
        for (int x = dq3::FTabX0; x < dq3::FTabX0 + dq3::FTabW; ++x) {
            int hits = 0;
            for (int i = 0; i < n; ++i)
                hits += x >= dq3::StripCardX(n, i) && x < dq3::StripCardX(n, i) + w;
            CHECK(hits <= 1);
            covered += hits;
        }
        CHECK(covered == n * w);
    }
    CHECK(dq3::StripCardW(4) == dq3::FieldCardW && dq3::StripCardW(0) == 0 && dq3::StripCardW(5) == 0);
    // the wide cards' text column (gen_manifest field_cards tw) fits the native job line at N <= 2
    CHECK(dq3::StripCardW(2) - 98 - 14 == 468 && dq3::StripCardW(1) - 98 - 14 == 1056);
}

// Heal All / Handy Heal All through the load plan's mailbox (dq3_heal.h), against a synthetic guest: the
// mailbox words, the stub's per-frame step (tools/dq3/gen_load_plan.py STUB: heartbeat, then one request
// when seq != taken) and a model of the game's routine on a synthetic party (the real routine is the
// game's; the model only has to answer like it and change state like it, so the module's protocol and
// its "no partial / no repeated request" rules can be checked).
struct GuestMailbox {
    std::map<u32, u32> w32;
    std::map<u32, u64> w64;
    std::vector<std::pair<u32, u32>> stores; ///< order of the module's stores
    bool fail_loads = false;
    dq3::HealMailboxIo Io() {
        return {[this](u32 off, u32* v) {
                    if (fail_loads)
                        return false;
                    *v = w32[off];
                    return true;
                },
                [this](u32 off, u32 v) {
                    stores.emplace_back(off, v);
                    w32[off] = v;
                    return true;
                },
                [this](u32 off, u64* v) {
                    if (fail_loads)
                        return false;
                    *v = w64[off];
                    return true;
                }};
    }
};

// The synthetic party the routine model heals: Heal All restores every living member to max with the
// cheapest caster that has the MP (a stand-in for the game's spell choice), never above max; a member at
// 0 HP is dead and stays dead without a reviver; no MP at all = "the magic couldn't be cast".
struct SyntheticParty {
    struct Unit {
        int hp, hp_max, mp;
        bool dead;
    };
    std::vector<Unit> units;
    bool battle = false;   ///< the routine is never reached in battle (the module's gate)
    int calls = 0;
    u64 Helper(u32 mode) {
        if (mode > 1)
            return 0xFF; // a cancelled request: no call
        ++calls;
        bool hurt = false, healed = false;
        for (auto& u : units)
            if (!u.dead && u.hp < u.hp_max - (mode ? 50 : 0))
                hurt = true;
        if (!hurt)
            return 0x10606;
        for (auto& u : units) {
            if (u.dead || u.hp >= u.hp_max - (mode ? 50 : 0))
                continue;
            for (auto& c : units)
                if (!c.dead && c.mp >= 9) {
                    c.mp -= 9;
                    u.hp = u.hp_max;
                    healed = true;
                    break;
                }
        }
        return healed ? 0x10601 : 0x10605;
    }
};

// One game frame of the stub (gen_load_plan.py STUB words 0x000..0x0a8).
void StubFrame(GuestMailbox& g, SyntheticParty& party) {
    namespace mb = dq3::heal_mb;
    ++g.w32[mb::Heartbeat];
    if (g.w32[mb::Seq] == g.w32[mb::Taken])
        return;
    const u32 seq = g.w32[mb::Seq];
    g.w32[mb::Taken] = seq;
    g.w64[mb::Result] = party.Helper(g.w32[mb::Mode]);
    g.w32[mb::Done] = seq;
}

void TestHealMailbox() {
    using Clock = dq3::HealMailbox::Clock;
    namespace mb = dq3::heal_mb;
    using ms = std::chrono::milliseconds;
    GuestMailbox g;
    SyntheticParty party{{{500, 999, 999, false}, {300, 999, 0, false}, {999, 999, 999, false}, {0, 999, 999, true}}};
    dq3::HealMailbox box;
    auto now = Clock::time_point{} + std::chrono::hours(1);
    // no mailbox (no load plan): never alive, never requests
    CHECK(!box.Present() && !box.Alive(now) && !box.Request(0, now));
    box.Configure(g.Io());
    CHECK(box.Present());
    // alive only once the heartbeat has moved (the stub runs), and not for a stale heartbeat
    box.Beat(now);
    CHECK(!box.Alive(now) && !box.Request(0, now));
    StubFrame(g, party);
    now += ms(33);
    box.Beat(now);
    CHECK(box.Alive(now));
    CHECK(!box.Alive(now + ms(1500)));
    // bad modes are refused before anything is written
    CHECK(!box.Request(2, now) && !box.Request(-1, now) && g.stores.empty());
    // a request writes the mode first, then publishes seq
    CHECK(box.Request(0, now) && box.Busy());
    CHECK(g.stores.size() == 2 && g.stores[0] == std::make_pair(mb::Mode, 0u) && g.stores[1] == std::make_pair(mb::Seq, 1u));
    // one request at a time
    CHECK(!box.Request(1, now));
    CHECK(!box.Poll(now)); // not served yet
    StubFrame(g, party);
    const auto f = box.Poll(now + ms(33));
    CHECK(f && !f->timed_out && f->mode == 0 && f->raw == 0x10601 && !box.Busy());
    CHECK(f && dq3::DecodeHealResult(f->raw) == dq3::HealOutcome::Healed);
    // the model: everyone alive at max (never above), 9 MP per heal from the first caster with MP, the dead
    // member untouched, the MP-less member's MP unchanged
    CHECK(party.units[0].hp == 999 && party.units[1].hp == 999 && party.units[2].hp == 999 && party.units[3].hp == 0);
    CHECK(party.units[0].mp == 999 - 18 && party.units[1].mp == 0 && party.units[2].mp == 999 && party.calls == 1);
    // the stub never runs a request twice: more frames, no more calls
    for (int k = 0; k < 5; ++k)
        StubFrame(g, party);
    CHECK(party.calls == 1 && !box.Poll(now + ms(200)));
    // a full party: "Nothing happens" (Heal All) / "almost at full health" (Handy), no state change
    now += ms(300);
    box.Beat(now);
    CHECK(box.Request(1, now));
    StubFrame(g, party);
    const auto full = box.Poll(now);
    CHECK(full && dq3::DecodeHealResult(full->raw) == dq3::HealOutcome::NothingToHeal && full->mode == 1);
    CHECK(party.units[0].mp == 999 - 18 && party.calls == 2);
    // nobody has the MP: "the magic couldn't be cast", nothing changes (no partial heal)
    party.units = {{100, 999, 3, false}, {200, 999, 8, false}};
    const auto before = party.units;
    now += ms(300);
    box.Beat(now);
    CHECK(box.Request(0, now));
    StubFrame(g, party);
    const auto nomp = box.Poll(now);
    CHECK(nomp && dq3::DecodeHealResult(nomp->raw) == dq3::HealOutcome::MagicFailed);
    CHECK(party.units[0].hp == before[0].hp && party.units[1].hp == before[1].hp && party.units[0].mp == 3 &&
          party.units[1].mp == 8);
    // the game loop stops (a pause): the request is cancelled after ServeFor; the stub, once it runs again,
    // takes the cancelled request and the helper refuses it (no heal); only then is a new request accepted
    party.units = {{100, 999, 999, false}};
    now += ms(300);
    box.Beat(now);
    CHECK(box.Request(0, now));
    CHECK(!box.Poll(now + ms(1000)));
    const auto late = box.Poll(now + dq3::HealMailbox::ServeFor + ms(1));
    CHECK(late && late->timed_out && !box.Busy() && g.w32[mb::Mode] == mb::Cancelled);
    now += dq3::HealMailbox::ServeFor + ms(40);
    StubFrame(g, party); // the heartbeat moves again
    box.Beat(now);
    CHECK(box.Alive(now));
    CHECK(g.w64[mb::Result] == 0xFF && party.units[0].hp == 100 && party.units[0].mp == 999);
    CHECK(dq3::DecodeHealResult(0xFF) == dq3::HealOutcome::Cancelled);
    // a cancelled request still owning seq blocks a new one until the stub has taken it
    g.w32[mb::Seq] = g.w32[mb::Done] + 1;
    CHECK(!box.Request(0, now));
    StubFrame(g, party);
    CHECK(box.Request(0, now));
    StubFrame(g, party);
    CHECK(box.Poll(now) && party.units[0].hp == 999);
    // unreadable mailbox: no request
    g.fail_loads = true;
    CHECK(!box.Request(0, now));
    g.fail_loads = false;
}

void TestHealResults() {
    using O = dq3::HealOutcome;
    // the Misc menu's own decisions on the routine's two bytes (main+0xBA04E0..0xBA0738)
    CHECK(dq3::DecodeHealResult(0x10606) == O::NothingToHeal);
    CHECK(dq3::DecodeHealResult(0x10604) == O::MagicFailed && dq3::DecodeHealResult(0x10405) == O::MagicFailed &&
          dq3::DecodeHealResult(0x10506) == O::MagicFailed);
    CHECK(dq3::DecodeHealResult(0x10601) == O::Healed && dq3::DecodeHealResult(0x10101) == O::Healed &&
          dq3::DecodeHealResult(0x10106) == O::Healed && dq3::DecodeHealResult(0x10307) == O::Healed &&
          dq3::DecodeHealResult(0x10003) == O::Healed);
    // the Romaria throne (FE169): King / Queen line, or nothing; spells forbidden here; cancelled; garbage
    CHECK(dq3::DecodeHealResult(0x120) == O::King && dq3::DecodeHealResult(0x220) == O::Queen &&
          dq3::DecodeHealResult(0x20) == O::Royal && dq3::DecodeHealResult(0x2120) == O::Royal);
    CHECK(dq3::DecodeHealResult(0x30) == O::NoSpellsHere && dq3::DecodeHealResult(0xFF) == O::Cancelled);
    CHECK(!dq3::DecodeHealResult(0) && !dq3::DecodeHealResult(0x31) && !dq3::DecodeHealResult(0x20606) &&
          !dq3::DecodeHealResult(0x1FF) && !dq3::DecodeHealResult(0x21));
    // the menu's message ids by mode and party size
    CHECK(std::string_view{dq3::HealMessageId(O::Healed, 0, false)} == "Txt_FieldMenu_Message_AllHeal");
    CHECK(std::string_view{dq3::HealMessageId(O::Healed, 1, true)} == "Txt_FieldMenu_Message_Heal");
    CHECK(std::string_view{dq3::HealMessageId(O::NothingToHeal, 0, false)} == "Txt_FieldMenu_Message_Mantan_Fail");
    CHECK(std::string_view{dq3::HealMessageId(O::NothingToHeal, 1, false)} == "Txt_FieldMenu_Message_AlmostMantan_Fail");
    CHECK(std::string_view{dq3::HealMessageId(O::MagicFailed, 1, false)} == "Txt_FieldMenu_Message_Mantan_FailMagic");
    CHECK(std::string_view{dq3::HealMessageId(O::King, 0, false)} == "Txt_FieldMenu_King_Message_MAXHEAL");
    CHECK(std::string_view{dq3::HealMessageId(O::Queen, 1, false)} == "Txt_FieldMenu_Queen_Message_ALMOSTHEAL");
    CHECK(std::string_view{dq3::HealMessageId(O::NoSpellsHere, 0, false)} == "Txt_Magic_Battle_Invalid_Common");
    CHECK(!dq3::HealMessageId(O::Royal, 0, false) && !dq3::HealMessageId(O::Cancelled, 0, false));
    CHECK(dq3::IsHealMessageId("Txt_FieldMenu_Message_AllHeal") && dq3::IsHealMessageId("Txt_Magic_Battle_Invalid_Common") &&
          !dq3::IsHealMessageId("Txt_FieldMenu_Message_BlockSave") && !dq3::IsHealMessageId(""));
    // the lone member's line names them; the other tags are CleanGameText's
    CHECK(dq3::CleanGameText(dq3::NameHealMessage(U"<Cap><DefSgl_WORD>'s wounds are healed!", U"Erdrick")) ==
          U"Erdrick's wounds are healed!");
    CHECK(dq3::CleanGameText(dq3::NameHealMessage(U"Nothing happens<--->everyone's fine!", U"")) ==
          U"Nothing happens—everyone's fine!");
    // the gate: battle, a busy field, an open game menu, the party drive, a dead or busy helper all refuse
    const dq3::HealGate open{false, true, true, false, true, false};
    CHECK(open.Open());
    auto g = open;
    g.battle = true;
    CHECK(!g.Open());
    g = open;
    g.adventure_free = false;
    CHECK(!g.Open());
    g = open;
    g.menu_closed = false;
    CHECK(!g.Open());
    g = open;
    g.party_drive = true;
    CHECK(!g.Open());
    g = open;
    g.helper_alive = false;
    CHECK(!g.Open());
    g = open;
    g.busy = true;
    CHECK(!g.Open());
}

// Party tab values (dq3_info_view.h): status, equipped slots, Carried (equipped excluded) and Spells.
void TestPartyView() {
    dq3::InfoData info;
    info.items["ITEM_HERB"] = {"Txt_Item_Name_Herb", "Txt_Item_Flavor_Herb", 5, 0, 0, 0, 0};
    info.items["ITEM_SWORD"] = {"Txt_Item_Name_Sword", "Txt_Item_Flavor_Sword", 0, 1, 1, 10, 5}; // OFFENSE ADD
    info.items["ITEM_RING"] = {"Txt_Item_Name_Ring", "Txt_Item_Flavor_Ring", 4, 4, 2, 110, 0};  // INTELLIGENCE RATIO
    info.nouns["Txt_Item_Name_Herb"] = U"Medicinal Herb";
    info.nouns["Txt_Item_Name_Sword"] = U"Copper Sword";
    info.nouns["Txt_Item_Name_Ring"] = U"Ring";
    info.texts["Txt_Item_Flavor_Herb"] = U"Restores <b>HP</>.";
    for (const auto& [id, t] : std::initializer_list<std::pair<const char*, std::u32string>>{
             {"Txt_FieldMenu_Equip_BLANK_01", U"Weapons"}, {"Txt_FieldMenu_Equip_BLANK_06", U"Sub-Accessory"},
             {"Txt_FieldMenu_Top_ITEM", U"Items"}, {"Txt_FieldMenu_Top_MAGIC", U"Spells"},
             {"Txt_FieldMenu_Top_EQUIP", U"Equipment"}, {"Txt_FieldMenu_STATUS_PARAMNAME_03", U"Attack"},
             {"Txt_FieldMenu_STATUS_PARAMNAME_07", U"Wisdom"}, {"Txt_FieldMenu_Item_NONE", U"Empty"},
             {"Txt_FieldMenu_Magic_NONE", U"No Spells Learnt"}})
        info.texts[id] = t;
    info.personality["PERSONALITY_X"] = "Txt_Status_Personality_X";
    info.nouns["Txt_Status_Personality_X"] = U"Bodybuilder";
    info.magic_flavor["M_FIELD"] = "Txt_Magic_Flavor_Field";
    info.texts["Txt_Magic_Flavor_Field"] = U"Heals one ally.";
    dq3::BattleData bd;
    bd.magic["M_FIELD"] = {1, 3, 1, true, false, "Txt_Magic_Name_Field"};
    bd.magic["M_BATTLE"] = {2, 4, 0, true, false, "Txt_Magic_Name_Battle"};
    bd.magic["M_LATER"] = {3, 5, 2, true, false, "Txt_Magic_Name_Later"};
    bd.magic["A_SKILL"] = {4, 0, 0, false, false, "Txt_Magic_Name_Skill"};
    bd.nouns["Txt_Magic_Name_Field"] = U"Heal";
    bd.nouns["Txt_Magic_Name_Battle"] = U"Frizz";
    dq3::PartyMember m;
    m.name = U"Erdrick";
    m.hp_max = 30;
    m.mp = 7;
    m.mp_max = 12;
    dq3::MemberDetail d;
    d.level = 5;
    d.personality = "PERSONALITY_X";
    d.limited = true;
    d.stats.hp_base = 25; // GetMaxHP: base F+0x28 + bonus F+0x68 (+ no MAXHP item) = the card's 30
    d.stats.hp_bonus = 5;
    d.stats.mp_base = 10;
    d.stats.mp_bonus = 2;
    d.stats.items = {{"ITEM_SWORD", 1, 1, {}}, {"ITEM_HERB", 2, 0, {}}, {"ITEM_UNKNOWN", 1, 0, {}}, {"ITEM_RING", 1, 6, {}}};
    d.skills = {{"M_BATTLE", 2, 2}, {"A_SKILL", 2, 1}, {"M_LATER", 1, 1}, {"M_FIELD", 3, 1}};
    dq3::PartyViewInput in;
    in.info = &info;
    in.battle = &bd;
    // nothing to show: empty view, the selections come back unchanged
    in.sel = 4;
    in.slot = 3;
    auto v = dq3::BuildPartyView(in);
    CHECK(v.ints.empty() && v.texts.empty() && v.sel == 4 && v.slot == 3);
    in.member = &m;
    in.detail = &d;
    in.job = U"Hero";
    in.sprite = "12";
    in.key = 7;
    in.sel = 0;
    in.slot = -1;
    v = dq3::BuildPartyView(in);
    CHECK(v.ints["dq3.pp.on"] == 1 && v.ints["dq3.pp.key"] == 7 && v.ints["dq3.pp.pers.on"] == 1);
    CHECK(v.texts["dq3.pp.portrait"] == "module:dq3:portrait/12" && v.texts["dq3.pr.head"] == "module:dq3:head/12");
    CHECK(v.texts["dq3.pp.job"] == dq3::Styled(U"Hero · Lv 5", dq3::FontStyle::Semi28));
    CHECK(v.texts["dq3.pp.s7"] == dq3::Styled("30", dq3::FontStyle::Bold30) &&
          v.texts["dq3.pp.s8"] == dq3::Styled("12", dq3::FontStyle::Bold30) &&
          v.texts["dq3.pp.s9"] == dq3::Styled("-", dq3::FontStyle::Bold30)); // no gold read
    {
        // GetMaxHP / GetMaxMP full path: equipped MAXHP (7) / MAXMP (8) ADD rows only; no instance extras, no
        // RATIO, unequipped rows ignored; clamp 0..999 after base + items + bonus
        CHECK(!dq3::HasMaxHpMpItems(info));
        dq3::InfoData hi = info;
        hi.items["ITEM_HP_AMULET"] = {"", "", 0, 7, 1, 40, 0};
        hi.items["ITEM_MP_RING"] = {"", "", 0, 8, 1, 15, 0};
        hi.items["ITEM_HP_RATIO"] = {"", "", 0, 7, 2, 150, 0};
        CHECK(dq3::HasMaxHpMpItems(hi));
        dq3::StatInputs si = d.stats;
        si.items = {{"ITEM_HP_AMULET", 1, 5, {{0, 7, 1, 99}}}, {"ITEM_MP_RING", 1, 6, {}}, {"ITEM_HP_AMULET", 1, 0, {}},
                    {"ITEM_HP_RATIO", 1, 4, {}}};
        CHECK(dq3::EquipMaxParam(hi, si.items, 7) == 40 && dq3::EquipMaxParam(hi, si.items, 8) == 15);
        auto st = dq3::ComputeStats(hi, si);
        CHECK(st.max_hp == 70 && st.max_mp == 27);
        si.hp_base = 990;
        CHECK(dq3::ComputeStats(hi, si).max_hp == 999 && dq3::ComputeStats(info, si).max_hp == 995);
    }
    // equipped slots: native names, icon + 1, empty slots keep only their name
    CHECK(v.texts["dq3.pp.eqh"] == dq3::Styled(U"Equipment", dq3::FontStyle::Bold28));
    CHECK(v.texts["dq3.pp.e0.l"] == dq3::Styled(U"Weapons", dq3::FontStyle::Semi24) && v.ints["dq3.pp.e0.on"] == 1 &&
          v.ints["dq3.pp.e0.i"] == 1 && v.texts["dq3.pp.e0.n"] == dq3::Styled(U"Copper Sword", dq3::FontStyle::Semi24));
    CHECK(v.ints["dq3.pp.e5.on"] == 1 && v.ints["dq3.pp.e5.i"] == 5 && !v.ints.contains("dq3.pp.e1.on") &&
          !v.texts.contains("dq3.pp.e1.n") && v.ints["dq3.pp.e1.off"] == 1 && !v.ints.contains("dq3.pp.e0.sel"));
    // Carried: equipped gear excluded, xN only above 1, the unknown row's star, the held count against 20
    CHECK(v.texts["dq3.pr.title"] == dq3::Styled(U"Erdrick's Items", dq3::FontStyle::Bold32) &&
          v.ints["dq3.pr.t0.sel"] == 1 && v.ints["dq3.pr.t1.off"] == 1);
    CHECK(v.texts["dq3.pr.count"] == dq3::Styled(U"2 carried · 4 / 20 held", dq3::FontStyle::Semi24) &&
          v.ints["dq3.dt.sub.on"] == 1 && v.ints["dq3.dt.text.on"] == 1 && !v.ints.contains("dq3.dt.f0.on"));
    CHECK(v.ints["dq3.pr.n"] == 2 && v.ints["qgi0"] == 6 && !v.ints.contains("qgj0") && v.ints["qgi1"] == 8 &&
          !v.texts.contains("qgc0") && !v.texts.contains("qgc1")); // a member's rows never stack: no xN
    // without a font atlas every name takes one line (qgy 13: 13 px lower, render.py party_page)
    CHECK(v.ints["qgx0"] == 1 &&
          v.texts["qgn0"] == dq3::Colour(0xFFECD292, dq3::Styled(U"Medicinal Herb", dq3::FontStyle::Bold24)) &&
          v.texts["qgn1"] == dq3::Styled(U"ITEM_UNKNOWN", dq3::FontStyle::Semi24) && v.ints["qgy0"] == 13);
    CHECK(v.ints["dq3.dt.ion"] == 1 && v.ints["dq3.dt.ii"] == 6 &&
          v.texts["dq3.dt.name"] == dq3::Styled(U"Medicinal Herb", dq3::FontStyle::Bold32) &&
          v.texts["dq3.dt.sub"] == dq3::Styled(U"Carried by Erdrick", dq3::FontStyle::Semi24) &&
          !v.texts.contains("dq3.dt.f0") &&
          v.texts["dq3.dt.text"] == dq3::StyledWrap(U"Restores HP.", dq3::FontStyle::Value));
    in.sel = 9; // clamped to the last carried row; the unknown row has no description
    v = dq3::BuildPartyView(in);
    CHECK(v.sel == 1 && v.ints["qgx1"] == 1 && !v.texts.contains("dq3.dt.text"));
    // an equipped slot: its item in the detail (param line, slot name), no grid selection
    in.slot = 0;
    v = dq3::BuildPartyView(in);
    CHECK(v.slot == 0 && v.ints["dq3.pp.e0.sel"] == 1 && !v.ints.contains("qgx0") && !v.ints.contains("qgx1"));
    CHECK(v.texts["dq3.dt.name"] == dq3::Styled(U"Copper Sword", dq3::FontStyle::Bold32) &&
          v.texts["dq3.dt.sub"] == dq3::Styled(U"Weapons", dq3::FontStyle::Semi24) &&
          v.texts["dq3.dt.f0"] == dq3::Styled(U"Attack +10", dq3::FontStyle::Value) && v.ints["dq3.dt.ii"] == 1);
    d.stats.vocation = 3; // Fighter: ParamFighterDown (0x87f24c)
    CHECK(dq3::BuildPartyView(in).texts["dq3.dt.f0"] == dq3::Styled(U"Attack +5", dq3::FontStyle::Value));
    in.slot = 5; // RATIO: value - 100 as a percentage
    CHECK(dq3::BuildPartyView(in).texts["dq3.dt.f0"] == dq3::Styled(U"Wisdom +10%", dq3::FontStyle::Value));
    in.slot = 1; // an empty slot cannot be selected
    CHECK(dq3::BuildPartyView(in).slot == -1);
    // nothing carried: the native "Empty"; an unlimited bag shows no held count
    const auto items = d.stats.items;
    d.stats.items = {{"ITEM_SWORD", 1, 1, {}}};
    d.limited = false;
    in.slot = -1;
    v = dq3::BuildPartyView(in);
    CHECK(v.ints["dq3.pr.n"] == 0 && v.texts["dq3.pr.none"] == dq3::Styled(U"Empty", dq3::FontStyle::Value) &&
          v.texts["dq3.pr.count"] == dq3::Styled("0 carried", dq3::FontStyle::Semi24) && !v.ints.contains("dq3.dt.on") &&
          v.ints["dq3.pr.empty"] == 1);
    d.stats.items = items;
    // Spells: learnt field spells, then learnt battle-only (greyed icon), then unlearnt "???"; abilities skipped
    in.sub = 1;
    in.sel = 9;
    v = dq3::BuildPartyView(in);
    CHECK(v.texts["dq3.pr.title"] == dq3::Styled(U"Erdrick's Spells", dq3::FontStyle::Bold32) &&
          v.ints["dq3.pr.t1.sel"] == 1);
    // owner 2026-10-10 (0.10.2): a Bag-style list (qs rows, dq3.ps.n), the Carried grid empty, the detail below
    CHECK(v.ints["dq3.pr.n"] == 0 && v.ints["dq3.ps.n"] == 3 && v.ints["dq3.ps.on"] == 1 && v.sel == 2 &&
          v.ints["qsx2"] == 1 && v.texts["dq3.pr.count"] == dq3::Styled("2 spells", dq3::FontStyle::Semi24));
    for (const auto& [k, val] : v.texts)
        CHECK(!k.starts_with("qg")); // no grid cell on the Spells sub-tab
    for (const auto& [k, val] : v.ints)
        CHECK(!k.starts_with("qg"));
    CHECK(v.texts["qsn0"] == dq3::Styled(U"Heal", dq3::FontStyle::Semi30) && v.ints["qss0"] == 2 &&
          !v.ints.contains("qsz0") && !v.ints.contains("qsl0") && !v.ints.contains("qsx0"));
    CHECK(v.texts["qsn1"] == dq3::Colour(0xFFA89A80, dq3::Styled(U"Frizz", dq3::FontStyle::Semi30)) &&
          v.ints["qsz1"] == 1 && v.ints["qsl1"] == 1);
    CHECK(v.texts["qsn2"] == dq3::Colour(0xFFECD292, dq3::Styled(U"???", dq3::FontStyle::Bold30)) &&
          !v.texts.contains("qsm2") && !v.ints.contains("qss2") && !v.ints.contains("qsz2") &&
          !v.ints.contains("qsm2.on") && !v.ints.contains("qsl2"));
    CHECK(v.texts["qsm0"] == dq3::Colour(0xFF78D4F4, dq3::Styled("3 MP", dq3::FontStyle::Semi28)) &&
          v.texts["qsm1"] == dq3::Colour(0xFFA8B0BC, dq3::Styled("4 MP", dq3::FontStyle::Semi28)) &&
          v.ints["qsm0.on"] == 1 && v.ints["qsm1.on"] == 1);
    // the unlearnt row selected: name only in the detail
    CHECK(v.texts["dq3.dt.name"] == dq3::Styled(U"???", dq3::FontStyle::Bold32) && !v.texts.contains("dq3.dt.sub") &&
          !v.texts.contains("dq3.dt.f0") && !v.ints.contains("dq3.dt.son") && !v.ints.contains("dq3.dt.zon"));
    in.sel = 0;
    v = dq3::BuildPartyView(in);
    CHECK(v.ints["qsx0"] == 1 && v.ints["qsl1"] == 1 && v.ints["qsl2"] == 1 &&
          v.texts["qsn0"] == dq3::Colour(0xFFECD292, dq3::Styled(U"Heal", dq3::FontStyle::Bold30)));
    // the detail: name, MP cost / the member's MP, where it is cast (use timing 1 = field and battle), description
    CHECK(v.texts["dq3.dt.name"] == dq3::Styled(U"Heal", dq3::FontStyle::Bold32) && v.ints["dq3.dt.son"] == 1 &&
          v.ints["dq3.dt.si"] == 2 && v.texts["dq3.dt.sub"] == dq3::Styled("MP 3 / 7", dq3::FontStyle::Semi24) &&
          v.texts["dq3.dt.f0"] == dq3::Styled(U"Field and battle", dq3::FontStyle::Value) &&
          v.ints["dq3.dt.f0.on"] == 1 &&
          v.texts["dq3.dt.text"] == dq3::StyledWrap(U"Heals one ally.", dq3::FontStyle::Value));
    in.sel = 1; // battle-only: greyed icon, "Battle only"
    v = dq3::BuildPartyView(in);
    CHECK(v.ints["dq3.dt.zon"] == 1 && v.texts["dq3.dt.f0"] == dq3::Styled(U"Battle only", dq3::FontStyle::Value) &&
          v.texts["qsn1"] == dq3::Colour(0xFFECD292, dq3::Styled(U"Frizz", dq3::FontStyle::Bold30)));
    d.skills[3].timing = 3; // field only (SEARCH / FIELD / TOWNDUNGEON)
    in.sel = 0;
    CHECK(dq3::BuildPartyView(in).texts["dq3.dt.f0"] == dq3::Styled(U"Field only", dq3::FontStyle::Value));
    d.skills[3].timing = 1;
    // a long name is fitted to the list's name room (SpellNameW, gen_manifest SPELL_NAME_W) when a font is given
    // (no font here: the first style); back on Carried the list is gone
    in.sub = 0;
    v = dq3::BuildPartyView(in);
    CHECK(!v.ints.contains("dq3.ps.on") && !v.ints.contains("dq3.ps.n") && v.ints["dq3.pr.n"] == 2);
    for (const auto& [k, val] : v.texts)
        CHECK(!k.starts_with("qs"));
    in.sub = 1;
    in.sprite = "-";
    CHECK(!dq3::BuildPartyView(in).texts.contains("dq3.pr.head"));
}

// Map tab no-map states (owner's mix, dq3_map.h NoMapState) on fixture rows: the selection rules and the page values.
void TestNoMap() {
    using dq3::NoMapState;
    dq3::MapData d;
    const auto row = [&](const char* id, int image, const char* memory, const char* name = "None") {
        dq3::MapRow r;
        r.image = image;
        r.memory = memory;
        r.name_text = name;
        r.spot_day = r.spot_night = "None";
        d.rows[id] = r;
    };
    const int town_img = dq3::FindMapImage("T_UI_Map_Town_Aliahan_00");
    CHECK(town_img >= 0);
    row("MAPLIST_C01F0101", town_img, "MEMORY_ALIAHAN", "Txt_Map_Place_Name_Aliahan");
    row("MAPLIST_C06F0101", town_img, "MEMORY_JIPANG", "Txt_Map_Place_Name_Jipang");
    row("MAPLIST_S09R0101", -1, "None");                  // the opening dream forest
    row("MAPLIST_C01R1001", -1, "MEMORY_ALIAHAN");        // pinned CardRows (no playable sublevel)
    row("MAPLIST_C01R1101", -1, "MEMORY_ALIAHAN");        // the Aliahan event map (house + castle 2F)
    row("MAPLIST_C06R0801", -1, "MEMORY_JIPANG");
    row("MAPLIST_H19R0101", -1, "MEMORY_SPIRITSPRING");   // a place without a still
    d.memory_area = {{"MEMORY_ALIAHAN", "Txt_Map_Place_Name_Aliahan"}, {"MEMORY_JIPANG", "Txt_Map_Place_Name_Jipang"},
                     {"MEMORY_SPIRITSPRING", "Txt_Map_Place_Name_Spirit_Fountain"}};
    d.destinations["RURA_RIREMITO_RURA_ALIAHAN"] = {"MAPLIST_C01F0101", "", 0, "Txt_Map_Place_Name_Aliahan"};
    d.destinations["RURA_RIREMITO_RURA_JIPANG"] = {"MAPLIST_C06F0101", "", 0, "Txt_Map_Place_Name_Jipang"};
    d.nouns["Txt_Map_Place_Name_Aliahan"] = U"Aliahan";
    d.menu["Txt_Map_PlaceAround_Name_Aliahan"] = U"Aliahan Region";
    d.menu["Txt_Map_Menu_Town_Map"] = U"Town Map";
    d.menu["Txt_Map_Menu_World_Map"] = U"World Map";
    for (int h = 0; h < 24; ++h)
        d.hour_frame[static_cast<std::size_t>(h)] = h < 16 ? 1 : h < 19 ? 2 : 3;
    CHECK((dq3::PlaceArtPlaces() == std::vector<std::string_view>{"MEMORY_ALIAHAN", "MEMORY_JIPANG", "MEMORY_SAMANOSA"}));
    const std::vector<std::string> fresh{"MAPLIST_S09R0101"}, later{"MAPLIST_C01F0101", "MAPLIST_H19R0101"};
    const auto sel = [&](const char* id, const std::vector<std::string>* v) {
        return dq3::SelectNoMap(&d, id ? d.Row(id) : nullptr, id ? id : "", v);
    };
    // not read / unknown: the card; "journey begins" only while Aliahan's town map is known to be unvisited
    CHECK(sel(nullptr, nullptr).state == NoMapState::Card && sel(nullptr, nullptr).variant == 2);
    CHECK(sel(nullptr, &fresh).variant == 0 && sel(nullptr, &later).variant == 2);
    CHECK(dq3::SelectNoMap(nullptr, nullptr, "", &fresh).variant == 2);
    // the dream maps and the pinned card row: the card
    CHECK(sel("MAPLIST_S09R0101", &fresh).state == NoMapState::Card && sel("MAPLIST_S09R0101", &fresh).variant == 0);
    CHECK(sel("MAPLIST_S09R0101", &later).variant == 1);
    CHECK(sel("MAPLIST_C01R1001", &fresh).state == NoMapState::Card);
    // the event map: the place card with Aliahan's still, region line and town map (not the parent-map marker)
    const auto ev = sel("MAPLIST_C01R1101", &fresh);
    CHECK(ev.state == NoMapState::Place && ev.art == 0 && ev.area == "Txt_Map_Place_Name_Aliahan" &&
          ev.region == "Txt_Map_PlaceAround_Name_Aliahan" && ev.town_row == "MAPLIST_C01F0101");
    const auto jp = sel("MAPLIST_C06R0801", &later);
    CHECK(jp.state == NoMapState::Place && jp.art == 1 && jp.town_row == "MAPLIST_C06F0101");
    // a place without a still, and rows with a map
    CHECK(sel("MAPLIST_H19R0101", &later).state == NoMapState::Card && sel("MAPLIST_H19R0101", &later).variant == 1);
    CHECK(sel("MAPLIST_C01F0101", &later).state == NoMapState::Map);

    // page values
    dq3::MapRoots roots;
    roots.ok = true;
    dq3::LocationSnapshot loc;
    loc.map_id = "MAPLIST_S09R0101";
    loc.hour = 10;
    loc.gold = 50;
    std::vector<std::string> visited = fresh;
    dq3::WorldExtent world{};
    dq3::MapViewInput in;
    in.data = &d;
    in.roots = &roots;
    in.loc = &loc;
    in.visited = &visited;
    in.world = &world;
    in.visible = true;
    in.hero_sprite = "12";
    auto v = dq3::BuildMapView(in);
    // the card: the composed page, no map box, no left column, no title
    CHECK(v.ints["dq3.mv.c"] == 1 && v.ints["dq3.m.on"] == 0 && !v.ints["dq3.mv.n"] && !v.ints["dq3.mv.l"] &&
          v.texts["dq3.m.card"] == "module:dq3:sys/card/1168x724/0/12/0" && !v.texts.contains("dq3.m.title") &&
          !v.ints.contains("dq3.m.obj.on"));
    const std::u32string objective = U"Leave your room.";
    in.objective = &objective;
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.m.obj.on"] == 1 && v.texts["dq3.m.obj"] == dq3::Styled(objective, dq3::FontStyle::Bold32) &&
          v.texts["dq3.m.card"] == "module:dq3:sys/card/1168x724/0/12/120" && v.ints["dq3.m.obj.x"] == (1168 - 120) / 2 + 72);
    in.visible = false;
    CHECK(dq3::BuildMapView(in).ints["dq3.mv.c"] == 0 && !dq3::BuildMapView(in).texts.contains("dq3.m.card"));
    in.visible = true;
    // location not read: the card instead of the blank page
    in.loc = nullptr;
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.m.wait"] == 1 && v.ints["dq3.mv.c"] == 1 && v.texts["dq3.m.card"].starts_with("module:dq3:sys/card/1168x724/0/"));
    in.loc = &loc;
    // the event map: the place card (still for the time frame, names, Town Map) and the objective in the left card;
    // the title is the place, never "World Map"; no frame / zoom / Reset / full-map controls (no image)
    loc.map_id = "MAPLIST_C01R1101";
    loc.hour = 17; // EVENING -> the dusk still
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.mv.p"] == 1 && v.ints["dq3.mv.l"] == 1 && v.ints["dq3.mv.n"] == 0 && !v.texts.contains("dq3.m.img") &&
          v.ints["dq3.mv.c"] == 0);
    CHECK(v.texts["dq3.m.art"] == "module:dq3:areaart/0/1/716x664" &&
          v.texts["dq3.m.place"] == dq3::Styled(U"Aliahan", dq3::FontStyle::Bold54) &&
          v.texts["dq3.m.region.t"] == dq3::Styled(U"Aliahan Region", dq3::FontStyle::Semi32) &&
          v.texts["dq3.m.town"] == dq3::Styled(U"Town Map", dq3::FontStyle::Bold32) && v.ints["dq3.m.town.on"] == 1);
    CHECK(v.texts["dq3.m.title"] == dq3::Styled(U"Aliahan", dq3::FontStyle::Bold44) && v.ints["dq3.m.lobj.on"] == 1 &&
          v.texts["dq3.m.lobj"] == dq3::StyledWrap(objective, dq3::FontStyle::Semi32));
    // Town Map = the full map of the place's town, without a marker
    in.full = true;
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.mv.f"] == 1 && v.ints["dq3.mv.p"] == 0 &&
          v.texts["dq3.m.img"].starts_with("module:dq3:mapimg/" + std::to_string(town_img) + "/1108x956/") &&
          v.texts["dq3.m.img"].find('~') == std::string::npos);
    in.full = false;
    // a map row keeps the map: frame and controls only with its image
    loc.map_id = "MAPLIST_C01F0101";
    v = dq3::BuildMapView(in);
    CHECK(v.ints["dq3.mv.n"] == 1 && v.ints["dq3.mv.p"] == 0 && v.ints["dq3.mv.c"] == 0 && v.texts.contains("dq3.m.img"));
}

// Bag tab values (dq3_info_view.h): native bag names, waiting, the row cap, xN, the detail facts.
void TestBagView() {
    dq3::InfoData info;
    info.items["ITEM_HERB"] = {"Txt_Item_Name_Herb", "Txt_Item_Flavor_Herb", 5, 0, 0, 0, 0};
    info.items["ITEM_SWORD"] = {"Txt_Item_Name_Sword", "Txt_Item_Flavor_Sword", 0, 1, 1, 10, 5};
    info.nouns["Txt_Item_Name_Herb"] = U"Medicinal Herb";
    info.nouns["Txt_Item_Name_Sword"] = U"Copper Sword";
    info.texts["Txt_FieldMenu_Item_ITEMBAG"] = U"Item Bag";
    info.texts["Txt_FieldMenu_Item_EQUIPBAG"] = U"Equipment Bag";
    info.texts["Txt_FieldMenu_Item_COUNTA_01"] = U"Number Held";
    info.texts["Txt_FieldMenu_Item_NONE"] = U"Empty";
    info.texts["Txt_FieldMenu_STATUS_PARAMNAME_03"] = U"Attack";
    dq3::BagViewInput in;
    CHECK(dq3::BuildBagView(in).ints.empty());
    in.info = &info;
    in.key = 3;
    auto v = dq3::BuildBagView(in);
    CHECK(v.ints["dq3.bg.wait"] == 1 && v.ints["dq3.bg.b0.sel"] == 1 && v.ints["dq3.bg.b1.off"] == 1 &&
          v.ints["dq3.bg.key"] == 3 && v.texts["dq3.bg.title"] == dq3::Styled(U"Item Bag", dq3::FontStyle::Bold32) &&
          v.ints["dq3.bg.subx"] == dq3::BagSubGap); // no font: the title's width counts 0
    std::vector<dq3::OwnedItem> bag(400, dq3::OwnedItem{"ITEM_HERB", 99, 0, {}});
    bag[1].count = 1;
    in.rows = &bag;
    in.sel = 1000;
    v = dq3::BuildBagView(in);
    CHECK(v.ints["dq3.bg.n"] == dq3::MaxRows && v.sel == 399 && !v.ints.contains("qbx319") &&
          !v.texts.contains("qbn320"));
    CHECK(v.texts["dq3.bg.count"] == dq3::Styled("400 kinds", dq3::FontStyle::Value));
    // list rows: icon + 1, the name, xN on every row (native), the separator above every unselected row but the first
    CHECK(v.ints["qbi0"] == 6 && v.texts["qbc0"] == dq3::Styled(U"×99", dq3::FontStyle::Semi28) && v.ints["qbi1"] == 6 &&
          v.texts["qbc1"] == dq3::Styled(U"×1", dq3::FontStyle::Semi28) &&
          v.texts["qbn0"] == dq3::Styled(U"Medicinal Herb", dq3::FontStyle::Semi30) && !v.ints.contains("qbl0") &&
          v.ints["qbl1"] == 1 && !v.ints.contains("qgi0") && !v.texts.contains("qgn0"));
    in.sel = 2;
    v = dq3::BuildBagView(in);
    CHECK(v.ints["qbx2"] == 1 && !v.ints.contains("qbl2") && v.ints["qbl3"] == 1 &&
          v.texts["qbn2"] == dq3::Colour(0xFFECD292, dq3::Styled(U"Medicinal Herb", dq3::FontStyle::Bold30)));
    // the description pane: name, the native "Number Held", the bag, no stat effect for a herb
    CHECK(v.ints["dq3.bd.on"] == 1 && v.ints["dq3.bd.ii"] == 6 &&
          v.texts["dq3.bd.name"] == dq3::Styled(U"Medicinal Herb", dq3::FontStyle::Bold34) &&
          v.texts["dq3.bd.held"] == dq3::Styled(U"Number Held  99", dq3::FontStyle::Value) &&
          v.texts["dq3.bd.by"] == dq3::Styled(U"Item Bag (whole party)", dq3::FontStyle::Value) &&
          !v.ints.contains("dq3.bd.fact.on") && !v.ints.contains("dq3.bd.ty") && !v.ints.contains("dq3.dt.on"));
    std::vector<dq3::OwnedItem> gear{{"ITEM_SWORD", 1200, 0, {}}};
    in.rows = &gear;
    in.page = 1;
    v = dq3::BuildBagView(in);
    CHECK(v.sel == 0 && v.ints["dq3.bg.b1.sel"] == 1 &&
          v.texts["dq3.bg.count"] == dq3::Styled("1 kind", dq3::FontStyle::Value));
    CHECK(v.texts["dq3.bd.fact"] == dq3::Styled(U"Attack +10", dq3::FontStyle::Semi28) && v.ints["dq3.bd.ty"] == 36 &&
          v.texts["dq3.bd.held"] == dq3::Styled(U"Number Held  1,200", dq3::FontStyle::Value) &&
          v.texts["dq3.bd.by"] == dq3::Styled(U"Equipment Bag (whole party)", dq3::FontStyle::Value));
    std::vector<dq3::OwnedItem> none;
    in.rows = &none;
    v = dq3::BuildBagView(in);
    CHECK(v.ints["dq3.bg.n"] == 0 && v.texts["dq3.bg.none"] == dq3::Styled(U"Empty", dq3::FontStyle::Semi28) &&
          v.ints["dq3.bg.empty"] == 1 && !v.ints.contains("dq3.bd.on"));
}

// Journal tab values (dq3_info_view.h): tips order / unread, medals, the monster grid and detail.
void TestJournalView() {
    dq3::InfoData info;
    info.info = {{"INFO_B", "Txt_Info_B", 2, 0}, {"INFO_A", "Txt_Info_A", 1, 0}, {"INFO_C", "Txt_Info_C", 0, 50}};
    info.texts["Txt_Info_A"] = U"Autosaving";
    info.texts["Txt_Info_B"] = U"<b>Battles</>";
    info.medals = {{"MEDAL_001", "ITEM_A", 2}, {"MEDAL_002", "ITEM_B", 5}};
    info.items["ITEM_B"] = {"Txt_Item_Name_B", "", 0, 0, 0, 0, 0};
    info.nouns["Txt_Item_Name_B"] = U"Thorn Whip";
    info.library = {{"LIB_1", "UNIT_MASTER_MN_001_SLIME", "MONSTER_MN_001_SLIME", {"PLACE_A", ""}, "Txt_Trivia_1",
                     "Txt_Monster_Name_Slime", 1, true},
                    {"LIB_2", "UNIT_MASTER_BOSS", "MONSTER_BOSS", {"", ""}, "", "Txt_Monster_Name_Boss", 2, false}};
    info.monsters["MONSTER_MN_001_SLIME"] = {4, 1, "ITEM_B", false};
    info.monsters["MONSTER_BOSS"] = {9999, 999, "None", true};
    info.nouns["Txt_Monster_Name_Slime"] = U"Slime";
    info.texts["Txt_Trivia_1"] = U"A blob.";
    dq3::MapData map;
    map.menu["PLACE_A"] = U"Aliahan";
    dq3::JournalState js;
    js.guide = "MAPGUIDE_NONE";
    js.tips_read = {0, 1};
    js.medals = 10002;
    js.medals_got = {"MEDAL_001"};
    dq3::JournalViewInput in;
    in.info = &info;
    in.map = &map;
    auto v = dq3::BuildJournalView(in);
    CHECK(v.ints.empty() && v.mon_sel == -1); // no state yet
    in.state = &js;
    v = dq3::BuildJournalView(in);
    CHECK(v.ints["dq3.jn.on"] == 1 && v.ints["dq3.jo.none"] == 1);
    // tips: SortOrder order, INFO_C hidden behind progress 50, "new" from the read bytes
    CHECK(v.ints["dq3.jt.count"] == 2 && v.texts["qtn0"] == dq3::Styled(U"Autosaving", dq3::FontStyle::Semi30) &&
          !v.ints.contains("qtw0"));
    CHECK(v.texts["qtn1"] == dq3::Styled(U"Battles", dq3::FontStyle::Bold30) && v.ints["qtw1"] == 1);
    CHECK(v.texts["dq3.jm.count"] == dq3::Styled("10,002", dq3::FontStyle::Bold32));
    CHECK(v.texts["dq3.jm.next"] == dq3::StyledWrap(U"Reward ready at 5 medals: Thorn Whip", dq3::FontStyle::Semi28));
    // no record: the boss stays hidden, the slime is listed as "?"
    CHECK(v.ints["dq3.jr.count"] == 1 && v.ints["dq3.jr.key"] == 0 && v.ints["qc0"] == -1 && v.ints["dq3.jr.d.q"] == 1);
    CHECK(v.texts["dq3.jr.counts"] == dq3::Styled("Records unavailable", dq3::FontStyle::Value) &&
          v.texts["dq3.jr.d.name"] == dq3::Styled(U"???", dq3::FontStyle::Bold32));
    // with the record: both rows, the first defeated one selected, its detail
    const std::map<std::string, dq3::s32> record{{"UNIT_MASTER_MN_001_SLIME", 18}, {"UNIT_MASTER_BOSS", 1}};
    in.record = &record;
    v = dq3::BuildJournalView(in);
    const auto slime = dq3::FindMonsterSprite("UNIT_MASTER_MN_001_SLIME");
    CHECK(slime && v.ints["dq3.jr.count"] == 2 && v.mon_sel == 0 && v.ints["qcs0"] == 1 &&
          v.ints["qc0"] == static_cast<std::int64_t>(*slime) && v.ints["qc1"] == -1);
    CHECK(v.texts["dq3.jr.counts"] == dq3::Styled("Defeated 2 / 2", dq3::FontStyle::Value));
    CHECK(v.texts["dq3.jr.d.name"] == dq3::Styled(U"Slime", dq3::FontStyle::Bold32) &&
          v.texts["dq3.jr.d.hab"] == dq3::StyledWrap(U"Habitat  Aliahan and Elsewhere", dq3::FontStyle::Value));
    CHECK(v.texts["dq3.jr.d.drop"] == dq3::Styled(U"Drops  Thorn Whip", dq3::FontStyle::Value) &&
          v.texts["dq3.jr.d.stat"] == dq3::Styled(U"Defeated 18 · EXP 4 · Gold 1", dq3::FontStyle::Value));
    in.mon_sel = 7; // clamped to the last row
    v = dq3::BuildJournalView(in);
    CHECK(v.mon_sel == 1 && v.ints["qcs1"] == 1 && v.texts["dq3.jr.d.name"] == dq3::Styled(U"???", dq3::FontStyle::Bold32));
}

// The byte-wise text path (Styled(std::string_view), AsciiToU32) takes 7-bit ASCII only. A UTF-8 literal
// passed there was the 0.6.0 build-1 "Â·" mojibake: it must be a visible error here, never silent.
int expected_non_ascii = 0;   // rejections the current check provokes on purpose
int unexpected_non_ascii = 0; // any other rejection fails the run (main)
void NonAsciiSeen(std::string_view text) {
    if (expected_non_ascii > 0) {
        --expected_non_ascii;
        return;
    }
    ++unexpected_non_ascii;
    std::fprintf(stderr, "non-ASCII text in the byte-wise path: \"%.*s\"\n", static_cast<int>(text.size()), text.data());
}

void TestAsciiTextPath() {
    using dq3::FontStyle;
    const auto before = dq3::NonAsciiTextCount();
    // ASCII passes unchanged, also through Styled, and is not reported
    CHECK(dq3::AsciiToU32("Lv 42 / 20 held") == std::u32string(U"Lv 42 / 20 held"));
    CHECK(dq3::Styled("2 carried", FontStyle::Semi20) == dq3::Styled(U"2 carried", FontStyle::Semi20));
    CHECK(dq3::AsciiToU32("").empty() && dq3::NonAsciiTextCount() == before);
    // UTF-8 middle dot (C2 B7) and Latin-1 e-acute: every byte >= 0x80 becomes '?', no sign extension,
    // reported once per string
    expected_non_ascii = 2;
    CHECK(dq3::AsciiToU32("A\xe9") == std::u32string(U"A?"));
    CHECK(dq3::Styled("2 carried \xc2\xb7 4 held", FontStyle::Semi20) ==
          dq3::Styled(U"2 carried ?? 4 held", FontStyle::Semi20));
    CHECK(expected_non_ascii == 0 && dq3::NonAsciiTextCount() == before + 2);
    // the first rejected string is kept for the module's single log line (bytes escaped)
    if (before == 0)
        CHECK(dq3::FirstNonAsciiText() == "A\\xE9");
    // the correct form of the same text: UTF-32 in, no rejection, the dot kept
    CHECK(dq3::Styled(U"2 carried · 4 held", FontStyle::Semi20) != dq3::Styled(U"2 carried ?? 4 held", FontStyle::Semi20) &&
          dq3::NonAsciiTextCount() == before + 2);
}

// Shared helpers that replaced per-file copies.
void TestSharedHelpers() {
    CHECK(dq3::Grouped(0) == "0" && dq3::Grouped(999) == "999" && dq3::Grouped(1000) == "1,000" &&
          dq3::Grouped(1234567) == "1,234,567" && dq3::Grouped(-1234) == "-1,234");
    CHECK(dq3::Colour(0xFF123456, "x") == "{c:#FF123456}x{/c}");
    CHECK(dq3::HpState(0, 10) == 2 && dq3::HpState(3, 10) == 3 && dq3::HpState(5, 10) == 4 &&
          dq3::HpState(6, 10) == 0 && dq3::HpState(5, 0) == 0);
    int n = 0;
    CHECK(dq3::art_util::ParseInt("42", n) && n == 42 && !dq3::art_util::ParseInt("4x", n) &&
          !dq3::art_util::ParseInt("", n));
    n = 73;
    CHECK(!dq3::art_util::ParseInt("2147483648", n) && n == 73);
    CHECK(!dq3::art_util::ParseInt("-2147483649", n) && n == 73);
    CHECK(!dq3::art_util::ParseInt("123tail", n) && n == 73);
    CHECK(dq3::art_util::ParseInt("2147483647", n) && n == 2147483647);
    CHECK(dq3::art_util::ParseInt("-2147483648", n) && n == (-2147483647 - 1));
    CHECK(!dq3::ParseSize("4294967296x5"));
    CHECK(!dq3::ParseSize("5x4294967296"));
    // unsigned key fields: no empty, signed, trailing or overflowing text (the output is kept on failure)
    u32 u = 7;
    CHECK(dq3::art_util::ParseU32("4294967295", u) && u == 4294967295u);
    u = 7;
    CHECK(!dq3::art_util::ParseU32("4294967296", u) && !dq3::art_util::ParseU32("", u) &&
          !dq3::art_util::ParseU32("-1", u) && !dq3::art_util::ParseU32("+1", u) &&
          !dq3::art_util::ParseU32("12x", u) && u == 7);
    CHECK(dq3::art_util::ParseU32("0fA0", u, 16) && u == 0xfa0);
    CHECK(!dq3::art_util::ParseU32("100000000", u, 16) && !dq3::art_util::ParseU32("", u, 16) &&
          !dq3::art_util::ParseU32("1g", u, 16) && u == 0xfa0);
    {
        // status/<cell>/<px>: an empty or overflowing number is a bad key (it used to parse as cell 0 and
        // reach the texture read); a valid key gets as far as the (failing) archive read
        const dq3::RangeReader none = [](u64, void*, std::size_t) { return false; };
        dq3::ArtLibrary lib;
        for (const char* key : {"status//28", "status/3/", "status/4294967299/28", "status/3/4294967324",
                                "status/+3/28", "status/3/28x", "status/24/28", "status/3/0"}) {
            std::string why;
            CHECK(lib.Load(none, key, &why) == nullptr && why == "bad key");
        }
        std::string why;
        CHECK(lib.Load(none, "status/3/28", &why) == nullptr && !why.empty() && why != "bad key");
        // mapimg/...: the visible-icon mask is whole hex, checked before the map image is read
        for (const char* key : {"mapimg/0/400x300/200x150/0,0/-/", "mapimg/0/400x300/200x150/0,0/-/100000000",
                                "mapimg/0/400x300/200x150/0,0/-/00zz"}) {
            std::string reason;
            CHECK(!dq3::ComposeMapArt(none, key, {}, &reason) && reason.empty());
        }
    }
    // base + equipment term saturates (guest-read bases are unchecked), then the game's 0..999 clamp
    CHECK(dq3::AddClamped(25, 5) == 30 && dq3::AddClamped(-25, 5) == -20);
    CHECK(dq3::AddClamped(INT32_MAX, 1) == INT32_MAX && dq3::AddClamped(INT32_MAX - 5, 100000) == INT32_MAX);
    CHECK(dq3::AddClamped(INT32_MIN, -1) == INT32_MIN && dq3::AddClamped(INT32_MIN + 5, -100000) == INT32_MIN);
    CHECK(dq3::DisplayedMax(dq3::AddClamped(INT32_MAX - 5, 100000), 0) == 999);
    CHECK(dq3::DisplayedMax(dq3::AddClamped(INT32_MIN + 5, -100000), 0) == 0);
    // GOP_LevelUpExp row ids
    int level = 5;
    CHECK(dq3::ParseLevelRowId("LEVELUPEXP_42", level) && level == 42);
    level = 5;
    CHECK(!dq3::ParseLevelRowId("LEVELUPEXP_", level) && !dq3::ParseLevelRowId("LEVELUPEXP_99999999999", level) &&
          !dq3::ParseLevelRowId("LEVELUPEXP_4x", level) && !dq3::ParseLevelRowId("LEVELUP_4", level) && level == 5);
    const auto parts = dq3::art_util::Split("a/b//c");
    CHECK(parts.size() == 4 && parts[2].empty() && parts[3] == "c");
    CHECK(dq3::AdrpPage(0x1000, 0x90000008) == 0x1000 && dq3::AdrpPage(0x1fff, 0xb0000008) == 0x2000);
    const u8 le[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(dq3::Le32(le) == 0x04030201u && dq3::Le64(le) == 0x0807060504030201ull);
}

// Element icons (0.10.1): effect texture decode, key parsing, ramp colours (synthetic, no archive)
void TestElementArt() {
    // pins: eight effect textures, the uexp + bulk pairs consistent with their layout
    const auto pins = dq3::ElementTexturePins();
    CHECK(pins.size() == static_cast<std::size_t>(dq3::ElemTex::Count));
    for (const auto& p : pins) {
        CHECK(p.uexp.path.ends_with(std::string{p.name} + ".uexp"));
        CHECK(p.has_bulk == !p.bulk.path.empty() && (!p.has_bulk || p.bulk.path.ends_with(std::string{p.name} + ".ubulk")));
        CHECK(p.data_bytes == ((p.w + 3) / 4) * ((p.h + 3) / 4) * (p.format == 0 ? 16u : 8u));
        CHECK(p.has_bulk ? (p.flags == 0x10501 && p.data_at == 0) : (p.flags == 72 && p.data_at == p.pf_at + 7 + 32));
    }
    CHECK(pins[static_cast<std::size_t>(dq3::ElemTex::IceBlock01)].format == 0 &&
          pins[static_cast<std::size_t>(dq3::ElemTex::Thunder02)].format == 3 &&
          !pins[static_cast<std::size_t>(dq3::ElemTex::Thunder02)].has_bulk);
    // a synthetic inline BC4 8 x 4 texture: block 0 = endpoint 255 (index 0 everywhere), block 1 = 0
    dq3::EffectTexturePin pin{"T", {}, {}, 8, 4, 3, 40, 1, 72, 16, 40 + 7 + 32, 0, false};
    std::vector<u8> uexp(40 + 7 + 32 + 16 + 12, 0);
    const auto put = [&](std::size_t at, u32 v) { std::memcpy(&uexp[at], &v, 4); };
    put(24, 8); put(28, 4); put(32, 1); put(36, 7);
    std::memcpy(&uexp[40], "PF_BC4", 7);
    put(47, 0); put(51, 1); put(55, 1); put(59, 72); put(63, 16); put(67, 16);
    uexp[79] = 255; uexp[80] = 254; // block 0: c0 255 > c1 254, indices 0
    uexp[87] = 0; uexp[88] = 0;     // block 1: c0 = c1 = 0
    put(95, 8); put(99, 4); put(103, 1);
    std::string why;
    auto img = dq3::DecodeEffectTexture(uexp, {}, pin, &why);
    CHECK(img && img->width == 8 && img->height == 4);
    if (img) {
        CHECK(img->rgba[0] == 255 && img->rgba[1] == 255 && img->rgba[3] == 255); // R copied to G / B, opaque
        CHECK(img->rgba[(8 * 1 + 3) * 4] == 255 && img->rgba[(8 * 3 + 4) * 4] == 0);
    }
    auto bad = uexp;
    bad[43] = '7'; // "PF_BC7" while the pin says BC4
    CHECK(!dq3::DecodeEffectTexture(bad, {}, pin, &why) && why == "effect format");
    bad = uexp;
    put(59, 0x10501); // the uexp claims bulk data
    CHECK(!dq3::DecodeEffectTexture(uexp, {}, pin, &why) && why == "effect mip header");
    put(59, 72);
    CHECK(!dq3::DecodeEffectTexture(std::span<const u8>{uexp}.first(90), {}, pin, &why) && why == "effect texture too short");
    auto bulk_pin = pin;
    bulk_pin.has_bulk = true;
    bulk_pin.flags = 0x10501;
    CHECK(!dq3::DecodeEffectTexture(uexp, {}, bulk_pin, &why) && why == "effect mip header");
    // keys
    CHECK(dq3::IsElementArtKey("elem/0/52") && !dq3::IsElementArtKey("spell/0/32"));
    for (const char* key : {"elem/6/52", "elem/-1/52", "elem/0/8", "elem/0/200", "elem/x/52", "elem/0", "elem/0/52/1"}) {
        why.clear();
        CHECK(!dq3::ComposeElementArt(nullptr, key, &why) && why == "bad key");
    }
    why.clear();
    CHECK(!dq3::ComposeElementArt(nullptr, "elem/0/52", &why) && !why.empty()); // no reader: nothing cached
    // the tonemapped material ramps (element_ref.py values): Frizz red, Sizz orange, Bang pink -> cream,
    // Woosh blue, Zap yellow
    const auto rgb = [](int r, double t) { return dq3::ElementRampColour(r, t); };
    const auto near = [](std::array<u8, 3> a, std::array<u8, 3> b) {
        return std::abs(a[0] - b[0]) <= 1 && std::abs(a[1] - b[1]) <= 1 && std::abs(a[2] - b[2]) <= 1;
    };
    CHECK(near(rgb(0, 0.5), {225, 0, 0}) && near(rgb(0, 1.0), {242, 0, 0}));
    CHECK(near(rgb(1, 0.5), {226, 183, 0}) && near(rgb(2, 0.5), {226, 119, 0}) && near(rgb(2, 0.0), {85, 31, 0}));
    CHECK(near(rgb(3, 0.0), {233, 174, 231}) && near(rgb(3, 1.0), {242, 239, 214}));
    CHECK(near(rgb(4, 0.5), {149, 162, 225}) && near(rgb(4, 0.0), {1, 0, 21}));
    CHECK(near(rgb(5, 0.5), {228, 220, 65}) && near(rgb(5, 1.0), {242, 238, 104}));
    CHECK(rgb(6, 0.5) == (std::array<u8, 3>{}));
}

bool TestRealArchive() {
    const char* path = std::getenv("DQ3_PAK");
    if (!path || !*path) {
        std::printf("DQ3_PAK not set: real-archive checks skipped\n");
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("DQ3_PAK unreadable: real-archive checks skipped\n");
        return false;
    }
    std::mutex m;
    const dq3::RangeReader read = [&](u64 off, void* out, std::size_t n) {
        std::scoped_lock lock{m};
        f.clear();
        f.seekg(static_cast<std::streamoff>(off));
        f.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
        return static_cast<std::size_t>(f.gcount()) == n;
    };
    for (u32 i = 0; i < static_cast<u32>(dq3::MemberId::Count); ++i) {
        const auto id = static_cast<dq3::MemberId>(i);
        std::string why;
        const auto bytes = dq3::ReadMember(read, dq3::Member(id), &why);
        CHECK(bytes.has_value());
        if (!bytes) {
            std::fprintf(stderr, "member %u: %s\n", i, why.c_str());
            continue;
        }
        if (const auto layout = dq3::TextureLayout(id)) {
            const auto img = dq3::DecodeTexture(*bytes, *layout, &why);
            CHECK(img.has_value());
            if (img && id == dq3::MemberId::StatusHp) {
                // Reference pixels of the research decode (texture2ddecoder BC7).
                const auto px = [&](u32 x, u32 y) { return &img->rgba[(y * img->width + x) * 4]; };
                CHECK(std::memcmp(px(40, 12), "\x42\xb0\x00\xff", 4) == 0);
                CHECK(std::memcmp(px(80, 10), "\xa3\xd8\x82\xff", 4) == 0);
                CHECK(px(0, 0)[3] == 0);
            }
        }
    }
    const auto font = dq3::BuildFontAtlas(read);
    CHECK(font != nullptr);
    if (font) {
        CHECK(font->line_height == 15 && font->first_codepoint == 0x20);
        // Bold22 (style 18) is the last range, U+F100..U+F1FF
        CHECK(font->glyphs.size() == 0xE000 + 0x100 * (static_cast<u32>(dq3::FontStyle::Count) - 2) + 0xFF - 0x20 + 1);
        const auto glyph = [&](dq3::FontStyle st, char c) -> const auto& {
            return font->glyphs[0xE000 + 0x100 * (static_cast<u32>(st) - 1) + static_cast<u32>(c) - 0x20];
        };
        const auto& b36 = glyph(dq3::FontStyle::Bold36, 'H');
        CHECK(b36.w > 0 && b36.bearing_y >= 23 && b36.bearing_y <= 27);
        const auto& b44 = glyph(dq3::FontStyle::Bold44, 'H'); // the largest style (the Map banner)
        CHECK(b44.w > 0 && b44.bearing_y >= 29 && b44.bearing_y <= 33);
        const auto& H = font->glyphs['H' - 0x20];
        CHECK(H.w > 0 && H.bearing_y >= 17 && H.bearing_y <= 19); // cap height 18 px at 26 px/em
        // the HUD font (Avenir Next W1G Demi), the native HP / MP labels: a different face than Plantin
        const auto& ah = glyph(dq3::FontStyle::Avenir24, 'H');
        CHECK(ah.w > 0 && ah.bearing_y >= 15 && ah.bearing_y <= 18);
        CHECK(font->Measure(dq3::Styled(std::string_view{"HP"}, dq3::FontStyle::Avenir24)) !=
              font->Measure(dq3::Styled(std::string_view{"HP"}, dq3::FontStyle::Bold24)));
        // the owner's floor: no style below 20 px (fallbacks), main text 24 px and up
        CHECK(font->Measure(dq3::Styled(std::u32string_view{U"Warrior · Lv 99"}, dq3::FontStyle::Semi24)) <= dq3::FieldTextW);
        std::printf("font atlas %ux%u\n", font->atlas.width, font->atlas.height);
    }
    // Every pinned sprite: members hash-checked, rect decoded from the sprite asset, atlas decoded.
    for (u32 i = 0; i < dq3::SpritePins().size(); ++i) {
        std::string why;
        const auto img = dq3::ComposeSprite(read, i, &why);
        CHECK(img.has_value());
        if (!img)
            std::fprintf(stderr, "sprite %u: %s\n", i, why.c_str());
    }
    // The cached rect decode equals the frame cropped out of a whole-atlas decode (every sprite).
    {
        int compared = 0;
        std::map<std::uint64_t, std::optional<dq3::Image>> atlases; // by texture member offset
        for (u32 i = 0; i < dq3::SpritePins().size(); ++i) {
            const auto& pin = dq3::SpritePins()[i];
            auto& atlas = atlases[pin.texture.offset];
            if (!atlas) {
                const auto tex = dq3::ReadMember(read, pin.texture);
                if (tex)
                    atlas = dq3::DecodeTexture(*tex, dq3::TexturePin{pin.tex_w, pin.tex_h, pin.tex_w * pin.tex_h * 4,
                                                                     dq3::TextureFormat::Bgra8});
            }
            const auto frame = dq3::SpriteFrame(read, i);
            CHECK(frame && atlas);
            if (!frame || !atlas)
                continue;
            const auto [x, y, w, h] = std::array<u32, 4>{pin.rect[0], pin.rect[1], pin.rect[2], pin.rect[3]};
            bool same = frame->width == w && frame->height == h;
            for (u32 r = 0; same && r < h; ++r)
                same = std::memcmp(frame->rgba.data() + std::size_t{r} * w * 4,
                                   atlas->rgba.data() + ((std::size_t{y} + r) * atlas->width + x) * 4, std::size_t{w} * 4) == 0;
            CHECK(same);
            ++compared;
            if (atlases.size() > 4) // bound the memory: atlases are 1.5..4 MB
                atlases.clear();
        }
        std::printf("sprite frames: %d equal to the whole-atlas crop\n", compared);
    }
    {
        std::string why;
        const auto text = dq3::LoadGameText(read, &why);
        CHECK(text != nullptr);
        if (text) {
            CHECK(text->jobs.at("Txt_Status_Job_Hero") == U"Hero");
            CHECK(text->jobs.at("Txt_Status_Job_Hero_Roto") == U"Erdrick");
            CHECK(text->jobs.at("Txt_Status_Job_MonsterTamer") == U"Monster Wrangler");
            CHECK(text->jobs.at("Txt_Status_Job_MartialArtist") == U"Martial Artist");
            CHECK(text->colours.half == 0xFFFFE000u); // measured on the native X menu
            CHECK(text->colours.low == 0xFFFF6300u && text->colours.dead == 0xFFFE0024u);
        } else {
            std::fprintf(stderr, "game text: %s\n", why.c_str());
        }
    }
    // Battle page: every monster sprite pin composes; the tables parse with known rows.
    for (u32 i = 0; i < dq3::MonsterSpritePins().size(); ++i) {
        std::string why;
        const auto img = dq3::ComposeBattleArt(read, "mon/" + std::to_string(i) + "/250x226", &why);
        CHECK(img.has_value());
        if (!img)
            std::fprintf(stderr, "monster sprite %u: %s\n", i, why.c_str());
    }
    CHECK(dq3::FindMonsterSprite("UNIT_MASTER_MN_001_SLIME") == 0u);
    CHECK(dq3::FindMonsterSprite("UNIT_MASTER_MN_079_MAGMALICE").has_value());
    CHECK(!dq3::FindMonsterSprite("UNIT_MASTER_MN_999_NONE"));
    {
        std::string why;
        const auto data = dq3::LoadBattleData(read, &why);
        CHECK(data != nullptr);
        if (data) {
            const auto& slime = data->monsters.at("MONSTER_MN_001_SLIME");
            CHECK(slime.level == 2 && slime.max_hp == 7 && slime.attack == 13 && slime.defense == 18 &&
                  slime.speed == 8 && slime.resist == "BATTLE_RESIST_MN_001_SLIME");
            CHECK(data->resists.contains("BATTLE_RESIST_MN_001_SLIME"));
            CHECK(data->resists.at("BATTLE_RESIST_PC_DEFAULT").mult[0] == 1.0f);
            CHECK(*data->Noun("Txt_Monster_Name_Slime") == U"Slime");
            CHECK(*data->Noun("Txt_Monster_Name_Starkraven") == U"Stark Raven");
            CHECK(*data->Noun("Txt_Magic_Name_Mera") == U"Frizz");
            const auto& mera = data->magic.at("MAGIC_ATTACK_MAGIC_MERA");
            CHECK(mera.consume_mp == 2 && mera.magic && mera.icon == 0 && mera.name_text == "Txt_Magic_Name_Mera");
            const auto spells = [&](const char* monster) {
                const auto& group = data->monsters.at(monster).action_group;
                std::set<std::u32string> names;
                for (const auto& id : data->enemy_spells.at(group)) {
                    const auto* name = data->Noun(id);
                    CHECK(name != nullptr);
                    if (name) names.insert(*name);
                }
                return names;
            };
            CHECK(spells("MONSTER_MN_001_SLIME").empty());
            CHECK((spells("MONSTER_MN_038_MUSHROOMMAGE") == std::set<std::u32string>{U"Crack", U"Heal"}));
            const auto baramos = spells("MONSTER_MN_134_BARAMOS");
            CHECK(baramos.size() == 5 && baramos.contains(U"Kafrizz") && baramos.contains(U"Kaboom"));
            CHECK((spells("MONSTER_MN_135_ZOMA") == std::set<std::u32string>{U"Bounce", U"Kacrackle"}));
            CHECK(spells("MONSTER_MN_151_GRANDRAGON").size() == dq3::MaxEnemySpells);
            for (const auto& [group, ids] : data->enemy_spells) {
                CHECK(ids.size() <= dq3::MaxEnemySpells);
                for (const auto& id : ids) {
                    const auto* name = data->Noun(id);
                    CHECK(name != nullptr);
                    if (name && font) {
                        // one column, or (a long hyphenated name such as "Cock-a-Doodle-Doo") the whole row
                        const auto cells = dq3::EnemySpellCells({*name, U"Heal"}, font.get());
                        const bool wide = cells[1].second == 1;
                        const int w = font->Measure(dq3::EnemySpellLabel(
                            *name, font.get(), wide ? dq3::bl::EnemySpellRowW : dq3::bl::EnemySpellW));
                        CHECK(w <= (wide ? dq3::bl::EnemySpellRowW : dq3::bl::EnemySpellW));
                        if (*name == U"Cock-a-Doodle-Doo")
                            CHECK(wide && (cells[0] == std::pair{0, 0}));
                    }
                }
            }
            // 0.10.2 Party Spells list: every spell name fits the list's name room (SpellNameW) at Semibold 30 or
            // its fallbacks (28 / 26), the widest MP cost fits the MP column (gen_manifest S_MP_W - 16)
            if (font) {
                int widest = 0, fallback = 0, spells_n = 0;
                std::u32string widest_name;
                for (const auto& [id, mg] : data->magic) {
                    if (!mg.magic)
                        continue;
                    const auto* name = data->Noun(mg.name_text);
                    if (!name)
                        continue;
                    ++spells_n;
                    const int w30 = font->Measure(dq3::Styled(*name, dq3::FontStyle::Semi30));
                    if (w30 > widest) {
                        widest = w30;
                        widest_name = *name;
                    }
                    if (w30 > dq3::SpellNameW) {
                        ++fallback;
                        CHECK(font->Measure(dq3::Styled(*name, dq3::FontStyle::Value)) <= dq3::SpellNameW);
                    }
                    CHECK(font->Measure(dq3::Styled(std::to_string(mg.consume_mp) + " MP", dq3::FontStyle::Semi28)) <=
                          136 - 16);
                }
                std::printf("spells list: %d spell names, widest %d px at Semibold 30 (room %d), %d need a smaller size\n",
                            spells_n, widest, dq3::SpellNameW, fallback);
                CHECK(spells_n > 60);
            }
            // enemy action rows resolve to GOP_Magic (192 in 1.1.0.0); the attack row is ATTACK; the six
            // affinity first-spell rows exist with icon cell 0
            CHECK(data->monster_actions.size() == 192 &&
                  data->monster_actions.at("BATTLE_MONSTER_ACTION_ATTACK") == "MAGIC_MONSTER_ATTACK_NORMAL" &&
                  data->magic.at("MAGIC_MONSTER_ATTACK_NORMAL").attack &&
                  data->monster_actions.at("BATTLE_MONSTER_ACTION_RUN_AWAY") == "MAGIC_RUN_AWAY");
            for (const auto row : dq3::ResistSpellRow)
                CHECK(data->magic.contains(row) && data->magic.at(std::string{row}).icon == 0);
            std::printf("battle data: %zu monsters, %zu resist rows, %zu magic rows, %zu nouns\n",
                        data->monsters.size(), data->resists.size(), data->magic.size(), data->nouns.size());
        } else {
            std::fprintf(stderr, "battle data: %s\n", why.c_str());
        }
    }
    {
        std::string why;
        const auto map = dq3::LoadMapData(read, &why);
        CHECK(map != nullptr);
        if (!map)
            std::fprintf(stderr, "map data: %s\n", why.c_str());
        else
            TestMapData(*map, read);
        if (map) {
            // no-map states on the real tables (1.1.0.0): the dream map and C01R1001 -> card; the event map, Jipang's
            // and Manoza's image-less rows -> the place card with the place's town map; the Spirit Fountain row -> card
            const std::vector<std::string> none;
            const auto s = [&](const char* id) { return dq3::SelectNoMap(map.get(), map->Row(id), id, &none); };
            CHECK(s("MAPLIST_S09R0101").state == dq3::NoMapState::Card && s("MAPLIST_S09R0101").variant == 0);
            CHECK(s("MAPLIST_C01R1001").state == dq3::NoMapState::Card);
            CHECK(s("MAPLIST_C01R1101").state == dq3::NoMapState::Place && s("MAPLIST_C01R1101").art == 0 &&
                  s("MAPLIST_C01R1101").town_row == "MAPLIST_C01F0101");
            CHECK(s("MAPLIST_C06R0801").state == dq3::NoMapState::Place && s("MAPLIST_C06R0801").town_row == "MAPLIST_C06F0101");
            CHECK(s("MAPLIST_C08R0801").state == dq3::NoMapState::Place && s("MAPLIST_C08R0801").town_row == "MAPLIST_C08F0101");
            CHECK(s("MAPLIST_H19R0101").state == dq3::NoMapState::Card);
            CHECK(s("MAPLIST_C01F0101").state == dq3::NoMapState::Map);
            CHECK(map->Noun("Txt_Map_Place_Name_Aliahan") && map->menu.contains("Txt_Map_PlaceAround_Name_Aliahan") &&
                  map->menu.contains("Txt_Map_Menu_Town_Map"));
            int image_less = 0, cards = 0;
            for (const auto& [id, r] : map->rows) // the two world fields draw the world map images
                if (r.image < 0 && !id.starts_with("MAPLIST_FIELD")) {
                    ++image_less;
                    cards += dq3::SelectNoMap(map.get(), &r, id, &none).state == dq3::NoMapState::Card;
                }
            CHECK(image_less == 124 && cards == 124 - 3); // the place cards: C01R1101, C06R0801, C08R0801
            std::printf("no-map: %d image-less rows, %d cards\n", image_less, cards);
        }
        const auto info = dq3::LoadInfoData(read, &why);
        CHECK(info != nullptr);
        if (!info)
            std::fprintf(stderr, "info data: %s\n", why.c_str());
        if (info && map)
            TestInfoData(*info, *map);
    }
    dq3::ArtLibrary art;
    // system screens (dq3_system.h): composed at the page size from the pinned members; the text metrics match
    // Pillow's getmetrics for the design sizes (gen_manifest.py METRIC)
    {
        const auto m26 = dq3::GameTextMetrics(read, 26.0f, false), m28 = dq3::GameTextMetrics(read, 28.0f, true);
        CHECK(m26 && *m26 == std::make_pair(19, 8) && m28 && *m28 == std::make_pair(20, 9));
        if (m26 && m28)
            std::printf("game text metrics 26: %d/%d, bold 28: %d/%d\n", m26->first, m26->second, m28->first, m28->second);
        for (const char* key : {"sys/start/1240x1080", "sys/loading/1240x1080", "sys/wrong/1240x1080", "sys/quiet/1240x1080"}) {
            std::string why;
            const auto img = art.Load(read, key, &why);
            CHECK(img != nullptr && img->width == 1240 && img->height == 1080);
            if (!img)
                std::fprintf(stderr, "art %s: %s\n", key, why.c_str());
            if (const char* dir = std::getenv("DQ3_ART_DUMP"); img && dir && *dir) {
                std::string name = key;
                for (char& c : name)
                    if (c == '/')
                        c = '_';
                std::ofstream out(std::string{dir} + "/" + name + ".rgba", std::ios::binary);
                out.write(reinterpret_cast<const char*>(img->rgba.data()), static_cast<std::streamsize>(img->rgba.size()));
                std::printf("dumped %s %ux%u\n", key, img->width, img->height);
            }
        }
        std::string why;
        CHECK(art.Load(read, "sys/start/100x100", &why) == nullptr && why == "bad key");
        CHECK(art.Load(read, "sys/nosuch/1240x1080", &why) == nullptr && why == "bad key");
        // the no-map card: composed with and without the objective window; bad variants fail
        for (const char* key : {"sys/card/1168x724/0/12/0", "sys/card/1168x724/1/-/360", "sys/card/1168x724/2/12/200"}) {
            const auto img = art.Load(read, key, &why);
            CHECK(img && img->width == 1168 && img->height == 724);
        }
        CHECK(art.Load(read, "sys/card/1168x724/3/12/0", &why) == nullptr && why == "bad key");
        CHECK(art.Load(read, "sys/card/1000x724/0/12/0", &why) == nullptr);
    }
    // removed keys and malformed keys fail with this key's reason (never a previous key's)
    for (const char* key : {"orderdrag/12", "sprite/12", "win/", "status/99/28", "nosuchkey/1"}) {
        std::string why;
        CHECK(art.Load(read, key, &why) == nullptr && !why.empty());
    }
    {
        std::string why;
        CHECK(art.Load(read, "win/10x10", &why) == nullptr && why == "bad key");
        CHECK(art.Load(read, "win/88x88/5", &why) == nullptr && why == "bad key");
        CHECK(art.Load(read, "win/88x88/x", &why) == nullptr && why == "bad key");
    }
    for (const char* key : {"light/1168x418", "light/1168x368", "ecell/0/221x100/56/1/", "ecell/1/221x100/56/1/s",
                            "ecell/1/221x100/56/1/g", "ecell/0/221x64/52/0/", "ecell/-1/156x92/78/0/s", "mon/1/240x258/6",
                            "mon/1/110x110/4", "tabs/400x88/0/48", "outline/286x242", "chev/26", "down/24", "shadowq/36",
                            "affgrid/516x232/3/2", "spell/1/32", "spell/2/36/g", "rowglow/580x46", "elem/0/56", "elem/1/56",
                            "elem/2/56", "elem/3/56", "elem/4/56", "elem/5/56", "elem/0/52", "elem/3/128"}) {
        std::string why;
        const auto t0 = std::chrono::steady_clock::now();
        const auto img = art.Load(read, key, &why);
        if (std::string_view{key}.starts_with("elem/")) // first use: decode + glyph + tile (glyph kept)
            std::printf("art %s composed in %.1f ms\n", key,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        CHECK(img != nullptr);
        if (!img)
            std::fprintf(stderr, "art %s: %s\n", key, why.c_str());
        if (const char* dir = std::getenv("DQ3_ART_DUMP"); img && dir && *dir) {
            std::string name = key;
            for (char& c : name)
                if (c == '/')
                    c = '_';
            std::ofstream out(std::string{dir} + "/" + name + ".rgba", std::ios::binary);
            out.write(reinterpret_cast<const char*>(img->rgba.data()),
                      static_cast<std::streamsize>(img->rgba.size()));
            std::printf("dumped %s %ux%u\n", key, img->width, img->height);
        }
    }
    for (const char* key : {"fcard/12/0/1168", "fcard/12/1/580", "fcard/-/0/384", "fcard/12/1/286"}) {
        // the dynamic strip widths: the same window nine-sliced, the sprite in its 80 px column, outline inset
        const auto img = dq3::ComposeInfoArt(read, key);
        const int w = std::atoi(std::strrchr(key, '/') + 1);
        CHECK(img && static_cast<int>(img->width) == w && img->height == dq3::FieldCardH);
    }
    {
        // N = 4 keeps the 0.10.0 key: the width part only names the same 286 px card
        const auto a = dq3::ComposeInfoArt(read, "fcard/12/1"), b = dq3::ComposeInfoArt(read, "fcard/12/1/286");
        CHECK(a && b && a->rgba == b->rgba);
        // the wide card keeps the window's 6 px rim at 1:1 on both ends (nine-slice 6 -> 6, only the centre
        // fill is resized, as for every win/ key) and the same sprite pixels in its 80 px column
        const auto c = dq3::ComposeInfoArt(read, "fcard/12/0/580"), d = dq3::ComposeInfoArt(read, "fcard/12/0");
        bool same = c && d;
        for (u32 y = 0; same && y < dq3::FieldCardH; ++y)
            same = std::memcmp(&c->rgba[(std::size_t{y} * c->width) * 4], &d->rgba[(std::size_t{y} * d->width) * 4], 6 * 4) == 0 &&
                   std::memcmp(&c->rgba[(std::size_t{y} * c->width + c->width - 6) * 4],
                               &d->rgba[(std::size_t{y} * d->width + d->width - 6) * 4], 6 * 4) == 0;
        CHECK(same);
        const auto bare_c = dq3::ComposeInfoArt(read, "fcard/-/0/580"), bare_d = dq3::ComposeInfoArt(read, "fcard/-/0");
        int sprite_c = 0, sprite_d = 0;   // pixels the sprite changed inside its column, wide vs 286
        for (u32 y = 0; c && d && bare_c && bare_d && y < dq3::FieldCardH; ++y)
            for (u32 x = 6; x < 98; ++x) {
                const std::size_t ic = (std::size_t{y} * c->width + x) * 4, id = (std::size_t{y} * d->width + x) * 4;
                sprite_c += std::memcmp(&c->rgba[ic], &bare_c->rgba[ic], 4) != 0;
                sprite_d += std::memcmp(&d->rgba[id], &bare_d->rgba[id], 4) != 0;
            }
        CHECK(sprite_c > 500 && sprite_c == sprite_d);
    }
    for (const char* key : {"fcard/12/0/500", "fcard/12/0/0", "fcard/12/0/x"})
        CHECK(!dq3::ComposeInfoArt(read, key));
    for (const char* key : {"fcard/12/0", "fcard/12/1", "fcard/-/0", "portrait/12", "portrait/-", "head/12",
                            "itemicon/5/38", "itemicon/6/38", "ui/bag/38x31", "ui/bagfit/46x38", "ui/nextguide/34x34",
                            "ui/medal/36x36", "itemtype/0/46", "itemtype/11/54", "box/253x80/slot", "box/253x80/slotsel",
                            "box/130x44/sub"}) {
        std::string why;
        const auto img = dq3::ComposeInfoArt(read, key, &why);
        CHECK(img.has_value());
        if (!img)
            std::fprintf(stderr, "info art %s: %s\n", key, why.c_str());
        if (const char* dir = std::getenv("DQ3_ART_DUMP"); img && dir && *dir) {
            std::string name = key;
            for (char& c : name)
                if (c == '/')
                    c = '_';
            std::ofstream out(std::string{dir} + "/" + name + ".rgba", std::ios::binary);
            out.write(reinterpret_cast<const char*>(img->rgba.data()), static_cast<std::streamsize>(img->rgba.size()));
            std::printf("dumped %s %ux%u\n", key, img->width, img->height);
        }
    }
    for (const char* key : {"fcard/x/0", "fcard/1/2", "itemicon/12/38", "ui/nope/10x10", "box/10x10/zz"})
        CHECK(!dq3::ComposeInfoArt(read, key));
    // the card sprite canvas (dq3_sprites.h), composed directly (fcard/ embeds it)
    for (const u32 n : {12u, 37u, 64u, 101u}) {
        std::string why;
        const auto img = dq3::ComposeSprite(read, n, &why);
        CHECK(img && img->width == dq3::SpriteBoxW && img->height == dq3::SpriteBoxH);
        if (const char* dir = std::getenv("DQ3_ART_DUMP"); img && dir && *dir) {
            std::ofstream out(std::string{dir} + "/sprite_" + std::to_string(n) + ".rgba", std::ios::binary);
            out.write(reinterpret_cast<const char*>(img->rgba.data()), static_cast<std::streamsize>(img->rgba.size()));
            std::printf("dumped sprite/%u %ux%u\n", n, img->width, img->height);
        }
    }
    {
        // Gauges (owner bug 0.9.0: the fill looked empty at both ends, the transparent padding of
        // the 160 x 24 texture was stretched in). The derived rects are the 1.1.0.0 textures'.
        std::string why;
        for (const auto id : {dq3::MemberId::StatusHp, dq3::MemberId::StatusMp}) {
            const auto b = dq3::ReadMember(read, dq3::Member(id), &why);
            const auto t = b ? dq3::DecodeTexture(*b, *dq3::TextureLayout(id), &why) : std::nullopt;
            CHECK(t && (dq3::OpaqueRect(*t, dq3::GaugeFillAlpha) == dq3::PixelRect{13, 9, 134, 5}));
        }
        const auto bg = dq3::ReadMember(read, dq3::Member(dq3::MemberId::StatusGaugeBg), &why);
        const auto bgt = bg ? dq3::DecodeTexture(*bg, *dq3::TextureLayout(dq3::MemberId::StatusGaugeBg), &why)
                            : std::nullopt;
        CHECK(bgt && (dq3::OpaqueRect(*bgt, dq3::GaugeFrameAlpha) == dq3::PixelRect{10, 6, 139, 11}));
        // Every manifest gauge size (field / Party cards, battle cards, enemy detail, battle
        // member detail): opaque in its first and last column (and row), so a 100 % bar reaches
        // both inner edges of its track; the track is the game's frame with its 2 px rim at 1:1.
        const std::pair<u32, u32> sizes[] = {{174, 10}, {152, 10}, {608, 12}, {538, 12}};
        for (const auto& [w, h] : sizes)
            for (const char* kind : {"hp", "mp"}) {
                const std::string key = "gauge/" + std::string{kind} + "/" + std::to_string(w) + "x" + std::to_string(h);
                const auto g = art.Load(read, key);
                CHECK(g && g->width == w && g->height == h);
                if (!g)
                    continue;
                const auto a = [&](u32 x, u32 y) { return g->rgba[(std::size_t{y} * w + x) * 4 + 3]; };
                bool edges = true;
                for (u32 y = 0; y < h; ++y)
                    edges = edges && a(0, y) >= 250 && a(w - 1, y) >= 250;
                for (u32 x = 0; x < w; ++x)
                    edges = edges && a(x, 0) >= 250 && a(x, h - 1) >= 250;
                CHECK(edges);
                const auto px = [&](u32 x, u32 y) { return &g->rgba[(std::size_t{y} * w + x) * 4]; };
                if (std::string_view{kind} == "hp")
                    CHECK(px(w / 2, h - 1)[1] > px(w / 2, h - 1)[0] + 60); // green body, not padding
                else
                    CHECK(px(w / 2, h - 1)[2] > px(w / 2, h - 1)[0] + 60); // cyan-blue body
                const u32 tw = w + 4, th = h + 4, my = th / 2;
                const auto tr = art.Load(read, "track/" + std::to_string(tw) + "x" + std::to_string(th));
                CHECK(tr && tr->width == tw && tr->height == th);
                if (!tr)
                    continue;
                const auto tp = [&](u32 x, u32 y) { return &tr->rgba[(std::size_t{y} * tw + x) * 4]; };
                CHECK(tp(0, my)[3] >= 230 && tp(tw - 1, my)[3] >= 230 && tp(1, my)[3] >= 230); // 2 px rim
                CHECK(tp(0, my)[0] >= 40 && tp(tw - 1, my)[0] >= 40);                         // grey
                CHECK(tp(tw / 2, 0)[0] >= 40 && tp(tw / 2, th - 1)[0] >= 40);
                // the well = the bar rect (2, 2, w, h): black from its first to its last column and row
                CHECK(tp(2, my)[0] <= 20 && tp(tw - 3, my)[0] <= 20 && tp(tw / 2, 2)[0] <= 20 && tp(tw / 2, th - 3)[0] <= 20);
                CHECK(tp(2, my)[3] == 255 && tp(tw / 2, my)[0] <= 20);
            }
        CHECK(!art.Load(read, "gauge/sp/124x7"));
        CHECK(!art.Load(read, "track/4x9"));
    }
    for (const char* key : {"scrap/1240x1080", "win/286x188", "win/88x88/120", "gauge/hp/174x10", "gauge/mp/174x10",
                            "track/178x14", "track/156x14", "track/612x16", "status/10/28", "status/11/28"}) {
        const auto img = art.Load(read, key);
        CHECK(img != nullptr);
        if (const char* dir = std::getenv("DQ3_ART_DUMP"); img && dir && *dir) {
            std::string name = key;
            for (char& c : name)
                if (c == '/')
                    c = '_';
            std::ofstream out(std::string{dir} + "/" + name + ".rgba", std::ios::binary);
            out.write(reinterpret_cast<const char*>(img->rgba.data()),
                      static_cast<std::streamsize>(img->rgba.size()));
            std::printf("dumped %s %ux%u\n", key, img->width, img->height);
        }
    }
    // Offline page preview (research runs/compact-0.10.0 tools): DQ3_ART_KEYS names a file with one module key
    // per line; each is composed like the module does (map keys without an ASTC decoder) and written to
    // DQ3_ART_DUMP as raw RGBA, with index.tsv (file, width, height, key) and the font atlas + glyph table.
    if (const char* list = std::getenv("DQ3_ART_KEYS"), *dir = std::getenv("DQ3_ART_DUMP"); list && dir && *list && *dir) {
        std::ifstream in(list);
        std::ofstream index(std::string{dir} + "/index.tsv");
        std::string key;
        int n = 0;
        while (std::getline(in, key)) {
            if (key.empty())
                continue;
            std::string why;
            std::optional<dq3::Image> img;
            if (dq3::IsMapArtKey(key)) {
                img = dq3::ComposeMapArt(read, key, {}, &why);
            } else if (const auto shared = art.Load(read, key, &why)) {
                img = *shared;
            }
            if (!img) {
                index << "-\t0\t0\t" << key << "\t" << why << "\n";
                continue;
            }
            const std::string file = "k" + std::to_string(n++) + ".rgba";
            std::ofstream out(std::string{dir} + "/" + file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(img->rgba.data()), static_cast<std::streamsize>(img->rgba.size()));
            index << file << "\t" << img->width << "\t" << img->height << "\t" << key << "\n";
        }
        if (const auto font = dq3::CachedFontAtlas()) {
            std::ofstream atlas(std::string{dir} + "/font.rgba", std::ios::binary);
            atlas.write(reinterpret_cast<const char*>(font->atlas.rgba.data()),
                        static_cast<std::streamsize>(font->atlas.rgba.size()));
            std::ofstream glyphs(std::string{dir} + "/font.tsv");
            glyphs << font->atlas.width << "\t" << font->atlas.height << "\t" << font->first_codepoint << "\n";
            for (std::size_t i = 0; i < font->glyphs.size(); ++i) {
                const auto& g = font->glyphs[i];
                if (g.w || g.advance)
                    glyphs << font->first_codepoint + i << "\t" << g.x << "\t" << g.y << "\t" << g.w << "\t" << g.h << "\t"
                           << g.bearing_x << "\t" << g.bearing_y << "\t" << g.advance << "\n";
            }
        }
        std::printf("preview art: %d keys composed\n", n);
    }
    // Element icons (0.10.1): every pinned effect texture decodes (reference pixels of the research decode,
    // texture2ddecoder), each glyph matches element_ref.py (coverage, mean colour), the six are distinct, the
    // tiles are cached
    {
        const auto pins = dq3::ElementTexturePins();
        for (std::size_t i = 0; i < pins.size(); ++i) {
            const auto& p = pins[i];
            std::string why;
            const auto uexp = dq3::ReadMember(read, p.uexp, &why);
            CHECK(uexp.has_value());
            std::optional<std::vector<u8>> bulk;
            if (p.has_bulk) {
                bulk = dq3::ReadMember(read, p.bulk, &why);
                CHECK(bulk.has_value());
            }
            const auto img = uexp ? dq3::DecodeEffectTexture(*uexp, bulk ? std::span<const u8>{*bulk} : std::span<const u8>{}, p, &why)
                                  : std::nullopt;
            CHECK(img && img->width == p.w && img->height == p.h);
            if (!img) {
                std::fprintf(stderr, "effect %s: %s\n", std::string{p.name}.c_str(), why.c_str());
                continue;
            }
            const auto px = [&](u32 x, u32 y) { return &img->rgba[(std::size_t{y} * img->width + x) * 4]; };
            if (p.name == "T_EF_Thunder_02")
                CHECK(px(64, 128)[0] == 28 && px(60, 100)[0] == 155 && px(10, 10)[0] == 0);
            if (p.name == "T_EF_Glow_05")
                CHECK(px(64, 64)[0] == 193 && px(64, 100)[0] == 190 && px(5, 5)[0] == 0);
            if (p.name == "T_EF_IceBlock_01")
                CHECK(std::memcmp(px(64, 64), "\x02\x68\x8b\xff", 4) == 0 && std::memcmp(px(190, 190), "\x09\x68\x90\xff", 4) == 0);
        }
        struct Ref { double cov, r, g, b; };
        constexpr Ref refs[dq3::ElementCount] = {{0.317, 205.8, 45.5, 0.4}, {0.300, 151.9, 82.8, 1.3},
                                                 {0.389, 155.4, 140.7, 147.7}, {0.409, 100.6, 172.2, 196.9},
                                                 {0.443, 127.0, 134.9, 174.4}, {0.248, 165.4, 159.5, 61.0}};
        std::shared_ptr<const dq3::Image> glyphs[dq3::ElementCount];
        for (int k = 0; k < dq3::ElementCount; ++k) {
            std::string why;
            glyphs[k] = dq3::ElementGlyph(read, k, &why);
            CHECK(glyphs[k] && glyphs[k]->width == 256 && glyphs[k]->height == 256);
            if (!glyphs[k]) {
                std::fprintf(stderr, "element %d: %s\n", k, why.c_str());
                continue;
            }
            CHECK(dq3::ElementGlyph(read, k) == glyphs[k]); // built once
            double n = 0, r = 0, g = 0, b = 0;
            for (std::size_t i = 0; i < glyphs[k]->rgba.size(); i += 4)
                if (glyphs[k]->rgba[i + 3] > 128) {
                    ++n;
                    r += glyphs[k]->rgba[i];
                    g += glyphs[k]->rgba[i + 1];
                    b += glyphs[k]->rgba[i + 2];
                }
            const double cov = n / (256.0 * 256.0);
            std::printf("element %d: coverage %.3f mean %.1f %.1f %.1f (reference %.3f %.1f %.1f %.1f)\n", k, cov, r / n,
                        g / n, b / n, refs[k].cov, refs[k].r, refs[k].g, refs[k].b);
            CHECK(std::abs(cov - refs[k].cov) < 0.03);
            CHECK(std::abs(r / n - refs[k].r) < 12 && std::abs(g / n - refs[k].g) < 12 && std::abs(b / n - refs[k].b) < 12);
        }
        // distinct: every pair differs on at least a quarter of the pixels (alpha or colour by > 64)
        for (int a = 0; a < dq3::ElementCount; ++a)
            for (int c = a + 1; c < dq3::ElementCount; ++c) {
                if (!glyphs[a] || !glyphs[c])
                    continue;
                std::size_t differ = 0;
                for (std::size_t i = 0; i < glyphs[a]->rgba.size(); i += 4) {
                    int d = 0;
                    for (int ch = 0; ch < 4; ++ch)
                        d = std::max(d, std::abs(glyphs[a]->rgba[i + ch] - glyphs[c]->rgba[i + ch]));
                    differ += d > 64;
                }
                CHECK(differ > 256 * 256 / 4);
            }
        for (int k = 0; k < dq3::ElementCount; ++k) {
            std::string why;
            const std::string key = "elem/" + std::to_string(k) + "/52";
            const auto tile = dq3::ComposeElementArt(read, key, &why);
            CHECK(tile && tile->width == 52 && tile->height == 52);
            if (!tile)
                continue;
            CHECK(tile->rgba[(26 * 52 + 8) * 4 + 3] > 200 && tile->rgba[(26 * 52 + 1) * 4 + 3] == 0); // the native tile shape
            const auto again = dq3::ComposeElementArt(read, key, &why);      // cached: identical
            CHECK(again && again->rgba == tile->rgba);
            const auto via = art.Load(read, key, &why);                       // routed through the library
            CHECK(via && via->rgba == tile->rgba);
        }
    }
    return true;
}

} // namespace

void TestPartyOrdering() {
    using M=dq3::FieldMenuMode;
    CHECK(!dq3::PartyDragOrder::Start(0,0,{10,20}));
    CHECK(!dq3::PartyDragOrder::Start(-1,1,{10,20}));
    CHECK(!dq3::PartyDragOrder::Start(0,2,{10,20}));
    CHECK(!dq3::PartyDragOrder::Start(0,1,{10}));
    CHECK(!dq3::PartyDragOrder::Start(0,1,{10,10}));
    CHECK(!dq3::PartyDragOrder::Start(0,1,{10,0}));
    CHECK(!dq3::PartyDragOrder::Start(0,1,{10,20,30,40,50}));
    CHECK(!dq3::PlanPartyOrder({M::Other},-2,4));
    CHECK(!dq3::PlanPartyOrder({M::Misc,3,3},-2,4));
    CHECK(!dq3::PlanPartyOrder({M::LineUp,0,-1,-1,3},1,4));
    CHECK(!dq3::PlanPartyOrder({M::LineUp,0,-1,4,4},1,4));
    CHECK(!dq3::PlanPartyOrder({M::LineUp,0,-1,-1,4},4,4));
    CHECK(!dq3::PlanPartyOrder({M::Field},-2,1));
    for (int count=2;count<=4;++count) for (int src=0;src<count;++src) for (int dst=0;dst<count;++dst) {
        if (src==dst) continue;
        std::vector<dq3::u64> fields;
        for (int k=0;k<count;++k) fields.push_back(100+k);
        const auto before=fields;
        auto request=dq3::PartyDragOrder::Start(src,dst,fields);
        CHECK(request.has_value());
        dq3::FieldMenuState menu{M::Field};
        int inputs=0,confirms=0;
        while (request && request->stage<4 && inputs<40) {
            CHECK(request->MatchesRoster(fields));
            const auto step=dq3::PlanPartyOrder(menu,request->Goal(),count);
            CHECK(step.has_value());
            if (!step) break;
            if (step->button==3 && menu.mode==M::LineUp) {
                ++confirms;
                if (menu.selected>=0) std::swap(fields[menu.selected],fields[menu.cursor]);
            }
            menu=step->expected;
            if (menu.mode==M::Misc) menu.command=menu.cursor+1;
            if (menu.mode==M::LineUp) menu.count=count;
            ++inputs;
            if (step->terminal) {
                auto bad=fields;std::swap(bad[0],bad[1]);
                CHECK(!request->FinishStage(menu,bad));
                CHECK(request->FinishStage(menu,fields));
            }
        }
        CHECK(request->stage==4);
        CHECK(menu.mode==M::Field);
        CHECK(confirms==2);
        auto swapped=before;std::swap(swapped[src],swapped[dst]);
        CHECK(fields==swapped);
    }
}

// 0.10.2 map flash fix (dq3_map_swap.h): a host model (an image widget draws nothing for a key not in its cache; the
// asset worker composes one key at a time, `compose` samples each, and DrainModuleImages installs it at the start of the
// next sample) driven by a walk that changes the marker key every ReadEvery (6) samples. The old direct publish leaves
// the map box empty for samples on every step; the swap never shows a key the host lacks, keeps one request in flight,
// and ends on the final position.
namespace map_swap_test {
struct HostModel {
    int compose{1};                      ///< samples per compose
    std::set<std::string> fail;          ///< keys whose compose fails
    bool stalled{};                      ///< the worker never finishes
    std::set<std::string> cache;         ///< installed pictures
    std::deque<std::string> queue;       ///< requested, not composed yet
    std::vector<std::string> completed;  ///< composed this sample, installed at the next drain
    std::string busy;
    std::uint64_t busy_until{};
    std::size_t max_queue{};
    dq3::MapImageRegistry* reg{};
    void Drain() {
        for (auto& k : completed)
            cache.insert(k);
        completed.clear();
    }
    void Request(const std::string& k) {
        if (k.empty() || cache.contains(k) || k == busy ||
            std::find(queue.begin(), queue.end(), k) != queue.end() ||
            std::find(completed.begin(), completed.end(), k) != completed.end())
            return;
        queue.push_back(k);
        max_queue = std::max(max_queue, queue.size() + (busy.empty() ? 0 : 1));
    }
    void Work(std::uint64_t s) {
        if (stalled)
            return;
        if (!busy.empty() && s >= busy_until) {
            const bool ok = !fail.contains(busy);
            reg->Note(busy, ok, 716 * 664 * 4);
            if (ok)
                completed.push_back(busy);
            busy.clear();
        }
        if (busy.empty() && !queue.empty()) {
            busy = queue.front();
            queue.pop_front();
            busy_until = s + static_cast<std::uint64_t>(compose);
        }
    }
};
std::string TownKey(int x, const char* box = "716x664") {
    return std::string{"module:dq3:mapimg/7/"} + box + "/709x664/3,0/SPOT_DAY/d~0," + std::to_string(x) + ",300,0";
}
struct Run {
    int blank{};        ///< samples with a shown key the host lacks (after the first picture could land)
    int stale_max{};    ///< samples since the shown key was first wanted (longest)
    std::size_t max_queue{};
    std::string last_shown;
};
/// `wants(s)` = the page's key at sample s; `swap` false = the 0.10.1 behaviour (dq3.m.img = the wanted key).
template <class Wants>
Run Drive(HostModel& host, int samples, Wants&& wants, bool swap) {
    dq3::MapImageRegistry reg;
    host.reg = &reg;
    dq3::MapImageSwap sw;
    Run r;
    std::map<std::string, int> wanted_at;
    for (int s = 1; s <= samples; ++s) {
        const auto u = static_cast<std::uint64_t>(s);
        host.Drain(); // the start of the tick
        const std::string want = wants(s);
        wanted_at.try_emplace(want, s);
        std::string shown = want, request;
        if (swap) {
            const auto out = sw.Step(want, u, [&](const std::string& k) { return reg.Get(k); });
            shown = out.shown;
            request = out.request;
        }
        // the redraw: the prefetch widget (drawn first) and the map image ask for their keys
        host.Request(request);
        host.Request(shown);
        // the first picture needs one compose and a drain on any version
        if (s > host.compose + 2 && !shown.empty() && !host.cache.contains(shown))
            ++r.blank;
        if (!shown.empty() && shown != want)
            r.stale_max = std::max(r.stale_max, s - wanted_at[shown]);
        host.Work(u);
        r.last_shown = shown;
    }
    r.max_queue = host.max_queue;
    return r;
}
} // namespace map_swap_test

void TestMapImageSwap() {
    using namespace map_swap_test;
    using St = dq3::MapImageRegistry::State;
    // the box of a key
    CHECK(dq3::MapKeyBox(TownKey(5)) == "716x664" && dq3::MapKeyBox(TownKey(5, "1108x956")) == "1108x956");
    CHECK(dq3::MapKeyBox("module:dq3:fieldimg/-1/1/716x664/10,20,30,40~1,3,4,0") == "716x664");
    CHECK(dq3::MapKeyBox("module:dq3:fieldimg/-1/1/716x664/10,20,30,40") == "716x664");
    CHECK(dq3::MapKeyBox("module:dq3:mframe/776x724").empty() && dq3::MapKeyBox("").empty() &&
          dq3::MapKeyBox("module:dq3:mapimg/7").empty());
    // the registry keeps the newest keys up to 32 MB of pictures; a key noted again moves to the front
    {
        dq3::MapImageRegistry reg;
        constexpr std::size_t town = 716 * 664 * 4, full = 1108 * 956 * 4;
        for (int k = 0; k < 30; ++k)
            reg.Note(TownKey(k), k != 4, town);
        CHECK(reg.Bytes() <= dq3::MapImageRegistry::BudgetBytes && reg.Get(TownKey(29)) == St::Ready);
        const int keep = static_cast<int>(dq3::MapImageRegistry::BudgetBytes / town); // 17 town pictures
        CHECK(keep == 17 && reg.Get(TownKey(29 - keep)) == St::Unknown && reg.Get(TownKey(30 - keep)) == St::Ready);
        reg.Note(TownKey(12), true, town);
        CHECK(reg.Get(TownKey(12)) == St::Ready && reg.Get(TownKey(30 - keep)) == St::Unknown);
        for (int k = 0; k < 8; ++k)
            reg.Note(TownKey(100 + k, "1108x956"), true, full);
        CHECK(reg.Bytes() <= dq3::MapImageRegistry::BudgetBytes && reg.Get(TownKey(107, "1108x956")) == St::Ready &&
              reg.Get(TownKey(100, "1108x956")) == St::Unknown); // 7 full-map pictures fit
        CHECK(reg.Get(TownKey(12)) == St::Unknown); // pushed out by the full-map pictures
        dq3::MapImageRegistry fails;
        fails.Note(TownKey(1), false, town); // a failure keeps no bytes
        CHECK(fails.Get(TownKey(1)) == St::Failed && fails.Bytes() == 0);
        reg.Clear();
        CHECK(reg.Get(TownKey(107, "1108x956")) == St::Unknown && reg.Bytes() == 0);
        CHECK(dq3::MapImageRegistry::BudgetBytes * 2 <= std::size_t{64} << 20); // half the host budget
    }
    // a walk: the marker moves one px per read (every 6 samples) for 600 samples, then stops
    const auto walk = [](int s) { return TownKey(std::min(s, 600) / 6); };
    for (const int compose : {1, 2, 5, 9, 20}) {
        HostModel before;
        before.compose = compose;
        const Run old = Drive(before, 900, walk, false);
        HostModel after;
        after.compose = compose;
        const Run fixed = Drive(after, 900, walk, true);
        std::printf("map swap, compose %2d samples: 0.10.1 blank samples %d (queue max %zu); 0.10.2 blank %d, "
                    "lag max %d samples, queue max %zu\n",
                    compose, old.blank, old.max_queue, fixed.blank, fixed.stale_max, fixed.max_queue);
        CHECK(old.blank >= 99);          // the reported flash: at least one empty sample per step (or no map at all)
        CHECK(fixed.blank == 0);         // never a key the host lacks
        CHECK(fixed.max_queue <= 2);     // the prefetch and nothing stale
        CHECK(fixed.last_shown == walk(900)); // ends on the final position
        CHECK(fixed.stale_max <= 3 * (compose + 3) + 6); // one or two composes behind the game
    }
    // walking back over composed positions: the registry knows them, the switch is immediate
    {
        HostModel host;
        host.compose = 3;
        // ten steps out and back: every position back is in the registry, so it shows as soon as it is wanted
        const auto back = [](int s) { return TownKey(s <= 60 ? s / 6 : std::max(0, 20 - s / 6)); };
        const Run r = Drive(host, 200, back, true);
        std::printf("map swap, 10 steps back: blank %d, lag max %d samples\n", r.blank, r.stale_max);
        CHECK(r.blank == 0 && r.last_shown == back(200) && r.stale_max <= 3 * (host.compose + 3) + 6);
        // thirty steps back: positions older than the registry (the host model never evicts, so it never composes
        // them again) wait for the timeout; still no blank and it ends on the final position
        HostModel far;
        far.compose = 3;
        const auto back30 = [](int s) { return TownKey(s <= 180 ? s / 6 : std::max(0, 60 - s / 6)); };
        const Run f = Drive(far, 1800, back30, true);
        std::printf("map swap, 30 steps back: blank %d, lag max %d samples\n", f.blank, f.stale_max);
        CHECK(f.blank == 0 && f.last_shown == back30(1800));
    }
    // the first picture and a box change (normal <-> full map) switch at once; closing the tab clears both keys
    {
        dq3::MapImageRegistry reg;
        dq3::MapImageSwap sw;
        const auto st = [&](const std::string& k) { return reg.Get(k); };
        auto out = sw.Step(TownKey(1), 1, st);
        CHECK(out.shown == TownKey(1) && out.request.empty());
        out = sw.Step(TownKey(2), 2, st);
        CHECK(out.shown == TownKey(1) && out.request == TownKey(2)); // kept until composed
        out = sw.Step(TownKey(3), 3, st);
        CHECK(out.shown == TownKey(1) && out.request == TownKey(2)); // one in flight
        reg.Note(TownKey(2), true);
        out = sw.Step(TownKey(3), 4, st);
        CHECK(out.shown == TownKey(1));                              // composed: settle two samples
        out = sw.Step(TownKey(3), 5, st);
        CHECK(out.shown == TownKey(1));
        out = sw.Step(TownKey(3), 6, st);
        CHECK(out.shown == TownKey(2) && out.request == TownKey(3)); // switched, the next position requested
        out = sw.Step(TownKey(3, "1108x956"), 7, st);
        CHECK(out.shown == TownKey(3, "1108x956") && out.request.empty());
        out = sw.Step({}, 8, st);
        CHECK(out.shown.empty() && out.request.empty() && sw.shown().empty());
        // a failed compose: no better picture is coming, the key shows (as before)
        sw.Step(TownKey(10), 9, st);
        out = sw.Step(TownKey(11), 10, st);
        CHECK(out.request == TownKey(11));
        reg.Note(TownKey(11), false);
        out = sw.Step(TownKey(11), 11, st);
        CHECK(out.shown == TownKey(11) && out.request.empty());
        // no news for 1.5 s (the host already held the key and never asked): switch anyway
        out = sw.Step(TownKey(12), 20, st);
        CHECK(out.shown == TownKey(11) && out.request == TownKey(12));
        out = sw.Step(TownKey(12), 20 + dq3::MapImageSwap::TimeoutSamples - 1, st);
        CHECK(out.shown == TownKey(11));
        out = sw.Step(TownKey(12), 20 + dq3::MapImageSwap::TimeoutSamples, st);
        CHECK(out.shown == TownKey(12) && out.request.empty());
    }
    // a stalled worker: the shown picture stays, then the timeout moves on
    {
        HostModel host;
        host.stalled = true;
        const Run r = Drive(host, 300, walk, true);
        CHECK(r.max_queue <= 2 + 300 / dq3::MapImageSwap::TimeoutSamples); // one new request per timeout
    }
}

int main() {
    // Debug/test build: a rejected non-ASCII byte string is counted as a failure (see TestAsciiTextPath)
    // instead of stopping at the assert, so every view and helper check below also proves its call sites ASCII.
    dq3::SetNonAsciiHook(NonAsciiSeen);
    // Each test builds its own fake FName pool at a shared address: the ReadFName cache (keyed by pool,
    // index and number, valid for the game's append-only pool) is dropped before every test.
    const auto run = [](void (*test)()) {
        dq3::ClearFNameCache();
        test();
    };
    run(TestSha1);
    run(TestMemberTable);
    run(TestOozGuard);
    run(TestTextureParse);
    run(TestSprites);
    run(TestArtHelpers);
    run(TestElementArt);
    run(TestFontStyling);
    run(TestReader);
    run(TestBattleView);
    run(TestBattleFollow);
    run(TestBattleMenuReader);
    run(TestMapView);
    run(TestNoMap);
    run(TestMapImageSwap);
    run(TestWorldIconDiscovery);
    run(TestChests);
    run(TestNativeUI);
    run(TestPartyOrdering);
    run(TestInfoReaders);
    run(TestKeyTables);
    run(TestPartyStrip);
    run(TestHealMailbox);
    run(TestHealResults);
    run(TestPartyView);
    run(TestBagView);
    run(TestJournalView);
    run(TestSharedHelpers);
    run(TestAsciiTextPath);
    dq3::ClearFNameCache();
    const bool real = TestRealArchive();
    CHECK(unexpected_non_ascii == 0);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("dsmod-dq3: all checks passed%s\n", real ? " (with real archive)" : "");
    return 0;
}
