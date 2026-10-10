// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Synthetic checks for lp_unity: a hand-built UnityFS bundle (LZ4 block) holding a
// SerializedFile v21 with an embedded typetree, the typetree walker, the Switch deswizzle (inverse
// of UnityPy's swizzle) and the texture orientation rules. Needs no game data.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

#include "lp_unity.h"
#include "lp_vanilla.h"
#include "lz4.h"

#include <cstdlib>
#include <new>

// The largest single allocation since the last reset (TestDamagedCount): a damaged array count
// must not allocate ahead of the data that is actually there.
std::atomic<std::size_t> largest_new{0};
void* operator new(std::size_t n) {
    for (std::size_t cur = largest_new.load(); n > cur && !largest_new.compare_exchange_weak(cur, n);) {
    }
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

using Bytes = std::vector<std::uint8_t>;

void Be32(Bytes& b, std::uint32_t v) {
    for (int k = 3; k >= 0; --k)
        b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
void Be64(Bytes& b, std::uint64_t v) {
    for (int k = 7; k >= 0; --k)
        b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
void Le(Bytes& b, std::uint64_t v, int n) {
    for (int k = 0; k < n; ++k)
        b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
void Str(Bytes& b, const char* s) {
    b.insert(b.end(), s, s + std::strlen(s) + 1);
}
void Align(Bytes& b, std::size_t a) {
    while (b.size() % a)
        b.push_back(0);
}

/// SerializedFile v21: one object (class 49) of type {string m_Name; int x; vector<int> arr;
/// PPtr<Object> ref; float f}.
Bytes MakeSerialized(std::int32_t arr_count = 2, std::size_t pad = 0) {
    struct N {
        int level;
        const char* type;
        const char* name;
        std::uint32_t meta;
    };
    const N nodes[] = {
        {0, "TextAsset", "Base", 0x8000}, {1, "string", "m_Name", 0x8000},
        {2, "Array", "Array", 0x4000},    {3, "int", "size", 0},
        {3, "char", "data", 0},           {1, "int", "x", 0},
        {1, "vector", "arr", 0},          {2, "Array", "Array", 0x4000},
        {3, "int", "size", 0},            {3, "int", "data", 0},
        {1, "PPtr<Object>", "ref", 0},    {2, "int", "m_FileID", 0},
        {2, "SInt64", "m_PathID", 0},     {1, "float", "f", 0},
    };
    // Local string buffer for names that are not common strings (common strings use 0x80000000).
    Bytes strings;
    const auto local = [&](const char* s) {
        const auto at = static_cast<std::uint32_t>(strings.size());
        strings.insert(strings.end(), s, s + std::strlen(s) + 1);
        return at;
    };
    Bytes tree;
    Le(tree, sizeof(nodes) / sizeof(nodes[0]), 4);
    Bytes node_bytes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> offs;
    for (const N& n : nodes) {
        // "int" is Unity common string 222 (0x80000000 | offset); the rest are local strings.
        const std::uint32_t t = std::strcmp(n.type, "int") == 0 ? (0x80000000u | 222u) : local(n.type);
        const std::uint32_t nm = local(n.name);
        offs.emplace_back(t, nm);
    }
    Le(tree, strings.size(), 4);
    for (std::size_t k = 0; k < offs.size(); ++k) {
        Le(tree, 1, 2);
        tree.push_back(static_cast<std::uint8_t>(nodes[k].level));
        tree.push_back(0);
        Le(tree, offs[k].first, 4);
        Le(tree, offs[k].second, 4);
        Le(tree, 0xFFFFFFFF, 4);
        Le(tree, k, 4);
        Le(tree, nodes[k].meta, 4);
        Le(tree, 0, 8);
    }
    tree.insert(tree.end(), strings.begin(), strings.end());

    Bytes obj;
    Le(obj, 3, 4);
    Str(obj, "abc");
    obj.pop_back();
    Align(obj, 4);
    Le(obj, 7, 4);
    Le(obj, static_cast<std::uint32_t>(arr_count), 4);
    if (arr_count == 2) {
        Le(obj, 5, 4);
        Le(obj, static_cast<std::uint32_t>(-6), 4);
    }
    Le(obj, 0, 4);
    Le(obj, 1234, 8);
    float f = 1.5f;
    std::uint32_t fb;
    std::memcpy(&fb, &f, 4);
    Le(obj, fb, 4);
    obj.insert(obj.end(), pad, 0);

    Bytes meta;
    Str(meta, "2019.4.27f1");
    Le(meta, 38, 4);
    meta.push_back(1);
    Le(meta, 1, 4);         // types
    Le(meta, 49, 4);        // class id
    meta.push_back(0);      // stripped
    Le(meta, 0xFFFF, 2);    // script type index
    meta.insert(meta.end(), 16, 0);
    meta.insert(meta.end(), tree.begin(), tree.end());
    Le(meta, 0, 4); // type dependencies
    Le(meta, 1, 4); // objects
    // Object entries are 4-aligned in the absolute file position (header is 20 bytes).
    while ((20 + meta.size()) % 4)
        meta.push_back(0);
    Le(meta, 42, 8);
    Le(meta, 0, 4);
    Le(meta, obj.size(), 4);
    Le(meta, 0, 4);
    Le(meta, 0, 4); // scripts
    Le(meta, 0, 4); // externals
    Le(meta, 0, 4); // ref types
    meta.push_back(0);

    const std::uint32_t data_offset = static_cast<std::uint32_t>((20 + meta.size() + 15) / 16 * 16);
    Bytes file;
    Be32(file, static_cast<std::uint32_t>(meta.size()));
    Be32(file, data_offset + static_cast<std::uint32_t>(obj.size()));
    Be32(file, 21);
    Be32(file, data_offset);
    file.push_back(0);
    file.insert(file.end(), 3, 0);
    file.insert(file.end(), meta.begin(), meta.end());
    file.resize(data_offset, 0);
    file.insert(file.end(), obj.begin(), obj.end());
    return file;
}

Bytes MakeBundle(const Bytes& cab, bool lz4) {
    Bytes block = cab;
    std::uint16_t block_flags = 0;
    if (lz4) {
        Bytes out(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(cab.size()))));
        const int n = LZ4_compress_default(reinterpret_cast<const char*>(cab.data()),
                                           reinterpret_cast<char*>(out.data()),
                                           static_cast<int>(cab.size()), static_cast<int>(out.size()));
        assert(n > 0);
        out.resize(static_cast<std::size_t>(n));
        block = out;
        block_flags = 2;
    }
    Bytes info;
    info.insert(info.end(), 16, 0);
    Be32(info, 1);
    Be32(info, static_cast<std::uint32_t>(cab.size()));
    Be32(info, static_cast<std::uint32_t>(block.size()));
    info.push_back(static_cast<std::uint8_t>(block_flags >> 8));
    info.push_back(static_cast<std::uint8_t>(block_flags));
    Be32(info, 1);
    Be64(info, 0);
    Be64(info, cab.size());
    Be32(info, 4);
    Str(info, "CAB-test");

    Bytes b;
    Str(b, "UnityFS");
    Be32(b, 7);
    Str(b, "5.x.x");
    Str(b, "2019.4.27f1");
    const std::size_t size_at = b.size();
    Be64(b, 0);
    Be32(b, static_cast<std::uint32_t>(info.size()));
    Be32(b, static_cast<std::uint32_t>(info.size()));
    Be32(b, 0x40);
    Align(b, 16);
    b.insert(b.end(), info.begin(), info.end());
    b.insert(b.end(), block.begin(), block.end());
    for (int k = 0; k < 8; ++k)
        b[size_at + k] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(b.size()) >> (8 * (7 - k)));
    return b;
}

lp_unity::RangeReader MemoryReader(const Bytes& file) {
    return lp_unity::MakeRangeReader([file](std::string_view path) -> std::optional<Bytes> {
        if (path == "/Data/StreamingAssets/AssetAssistant/test")
            return file;
        return std::nullopt;
    });
}

void TestBundle(bool lz4) {
    const Bytes bundle = MakeBundle(MakeSerialized(), lz4);
    lp_unity::Reader reader{MemoryReader(bundle)};
    const auto objects = reader.ListObjects("test");
    assert(objects.size() == 1 && objects[0].first == "abc" && objects[0].second == 42);
    const auto v = reader.ReadObjectByName("test", "abc", -1);
    assert(v);
    assert(v->Get("x") && v->Get("x")->AsInt() == 7);
    const auto* arr = v->Get("arr");
    assert(arr && arr->items.size() == 2 && arr->items[0].AsInt() == 5 && arr->items[1].AsInt() == -6);
    assert(v->Path("ref.m_PathID") && v->Path("ref.m_PathID")->AsInt() == 1234);
    assert(v->Get("f") && v->Get("f")->AsFloat() == 1.5);
    assert(!reader.ReadObjectByName("test", "missing", -1));
    assert(!reader.ReadObjectByName("test", "abc")); // default class 114: not a MonoBehaviour
    assert(reader.ReadObjectByName("test", "abc", 49));
    assert(!reader.ReadObjectByName("missing", "abc", -1));
    // Truncated / garbage bundles are rejected without throwing.
    for (std::size_t cut : {std::size_t{10}, std::size_t{60}, bundle.size() - 3}) {
        Bytes bad(bundle.begin(), bundle.begin() + static_cast<std::ptrdiff_t>(cut));
        lp_unity::Reader r{MemoryReader(bad)};
        assert(r.ListObjects("test").empty());
    }
    Bytes flipped = bundle;
    for (std::size_t k = 64; k < flipped.size(); k += 7)
        flipped[k] ^= 0x5A;
    lp_unity::Reader r{MemoryReader(flipped)};
    (void)r.ReadObjectByName("test", "abc", -1); // must not crash
}

// An array count the remaining bytes cannot hold (100000 ints in about 150 KB) is refused before
// the walker allocates its values (each Value is ~100 bytes: 10 MB for this count).
void TestDamagedCount() {
    const Bytes bundle = MakeBundle(MakeSerialized(100000, 150000), true);
    lp_unity::Reader reader{MemoryReader(bundle)};
    assert(reader.ListObjects("test").size() == 1);
    largest_new = 0;
    assert(!reader.ReadObjectByName("test", "abc", -1));
    assert(largest_new < 1024 * 1024);
    // the same count with the data present still reads
    const Bytes whole = MakeBundle(MakeSerialized(2, 150000), true);
    lp_unity::Reader ok{MemoryReader(whole)};
    const auto v = ok.ReadObjectByName("test", "abc", -1);
    assert(v && v->Get("arr") && v->Get("arr")->items.size() == 2 && v->Get("f")->AsFloat() == 1.5);
}

Bytes Swizzle(const Bytes& data, std::uint32_t w, std::uint32_t h, std::uint32_t bw, std::uint32_t bh,
              std::uint32_t gobs) {
    const std::uint32_t bcx = (w + bw - 1) / bw, bcy = (h + bh - 1) / bh;
    Bytes out(data.size());
    std::size_t dst = 0;
    for (std::uint32_t i = 0; i < bcy / 8 / gobs; ++i)
        for (std::uint32_t j = 0; j < bcx / 4; ++j)
            for (std::uint32_t k = 0; k < gobs; ++k)
                for (std::uint32_t v = 0; v < 32; ++v) {
                    const std::uint32_t gx = ((v >> 3) & 2) | ((v >> 1) & 1);
                    const std::uint32_t gy = ((v >> 1) & 6) | (v & 1);
                    const std::size_t src = ((static_cast<std::size_t>((i * gobs + k) * 8 + gy)) * bcx + j * 4 + gx) * 16;
                    std::memcpy(out.data() + dst, data.data() + src, 16);
                    dst += 16;
                }
    return out;
}

void TestDeswizzle() {
    const std::uint32_t w = 64, h = 128, bw = 4, bh = 1, gobs = 4; // RGBA32: 4 px per 16 bytes
    Bytes linear(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t k = 0; k < linear.size(); ++k)
        linear[k] = static_cast<std::uint8_t>(k * 31 + (k >> 9));
    const Bytes sw = Swizzle(linear, w, h, bw, bh, gobs);
    assert(lp_unity::SwitchDeswizzle(sw, w, h, bw, bh, gobs) == linear);

    // Swizzled RGBA32 texture through DecodeTextureData, platform blob gobs shift 2.
    lp_unity::TextureInfo tex;
    tex.width = static_cast<std::int32_t>(w);
    tex.height = static_cast<std::int32_t>(h);
    tex.format = 4;
    tex.platform_blob.assign(12, 0);
    tex.platform_blob[8] = 2;
    const auto img = lp_unity::DecodeTextureData(sw, tex, {}, 38);
    assert(img && img->width == w && img->height == h);
    // Top-left output row 0 = last stored row.
    assert(std::memcmp(img->rgba.data(), linear.data() + (h - 1) * w * 4, w * 4) == 0);
}

void TestPixelFormats() {
    lp_unity::TextureInfo tex;
    tex.width = 2;
    tex.height = 2;
    tex.format = 1; // Alpha8
    const Bytes a8{10, 20, 30, 40};
    const auto img = lp_unity::DecodeTextureData(a8, tex, {}, 38);
    assert(img && img->rgba.size() == 16);
    // Flipped: first output row is stored row 1.
    assert(img->rgba[3] == 30 && img->rgba[7] == 40 && img->rgba[11] == 10 && img->rgba[0] == 0);
    tex.format = 50; // ASTC without a decoder: fails cleanly
    const Bytes blocks(16, 0);
    assert(!lp_unity::DecodeTextureData(blocks, tex, {}, 38));
    tex.format = 4;
    assert(!lp_unity::DecodeTextureData(a8, tex, {}, 38)); // too short for RGBA32
}

} // namespace

void TestThreads() {
    const Bytes bundle = MakeBundle(MakeSerialized(), true);
    lp_unity::Reader reader{MemoryReader(bundle), {}, std::string{lp_unity::DefaultAssetRoot}, 1};
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            for (int k = 0; k < 200; ++k) {
                if (k % 50 == 0)
                    reader.ClearCache();
                const auto v = reader.ReadObjectByName("test", "abc", -1);
                if (v && v->Get("x") && v->Get("x")->AsInt() == 7)
                    ++ok;
            }
        });
    for (auto& t : threads)
        t.join();
    assert(ok == 8 * 200);
}

void TestMessageIndices() {
    using V = lp_unity::Value;
    const auto num = [](int n) { V v; v.kind = V::Kind::Int; v.i = n; return v; };
    const auto text = [](const char* t) { V v; v.kind = V::Kind::String; v.s = t; return v; };
    V word; word.kind = V::Kind::Object;
    word.fields = {{"str", text("Incense Burner")}, {"eventID", num(1)}};
    V words; words.kind = V::Kind::Array; words.items = {word};
    V label; label.kind = V::Kind::Object;
    label.fields = {{"labelIndex", num(1633)}, {"arrayIndex", num(1833)}, {"wordDataArray", words}};
    V labels; labels.kind = V::Kind::Array; labels.items = {label};
    V table; table.kind = V::Kind::Object; table.fields = {{"labelDataArray", labels}};
    const auto named = lp_unity::DecodeMessageTable(table);
    const auto numeric = lp_unity::DecodeMessageTable(table, false, true);
    assert(named.size() == 1634 && named[1633] == "Incense Burner");
    assert(numeric.size() == 1834 && numeric[1833] == "Incense Burner" && numeric[1633].empty());
    assert(lp_unity::DecodeMessageTable(table, true, true)[1833] == "Incense Burner\n");
    labels.items[0].fields[1].second = num(-1);
    table.fields[0].second = labels;
    assert(lp_unity::DecodeMessageTable(table, false, true).empty());
}

void TestBagAssetPolicy() {
    using lp_vanilla::BagSprites;
    assert((BagSprites(0, true) == std::vector<std::string>{"menu_ico_01_03_000_on"}));
    assert((BagSprites(100, false) == std::vector<std::string>{"menu_ico_01_03_100_on"}));
    assert((BagSprites(1, true) == std::vector<std::string>{"menu_ico_01_03_001_on", "menu_ico_01_03_01_on", "menu_ico_01_03_000_on"}));
    assert((BagSprites(101, false) == std::vector<std::string>{"menu_ico_01_03_101_on", "menu_ico_01_03_02_on", "menu_ico_01_03_100_on"}));
    for (const int fashion : {6, 106, 113, 999}) {
        assert(BagSprites(fashion, true).back() == "menu_ico_01_03_000_on");
        assert(BagSprites(fashion, false).back() == "menu_ico_01_03_100_on");
    }
    // Corrupt/unsupported IDs never format arbitrary asset paths.
    assert((BagSprites(-1, false) == std::vector<std::string>{"menu_ico_01_03_100_on"}));
    assert((BagSprites(1000, true) == std::vector<std::string>{"menu_ico_01_03_000_on"}));
}

void TestTitleAssetPolicy() {
    // Pearl LP includes a pea bad-install diagnostic. Its actual branding must stay dia.
    assert(lp_vanilla::LogoFamily(false, false) == "dia");
    assert(lp_vanilla::LogoFamily(false, true) == "dia");
    assert(lp_vanilla::LogoFamily(true, false) == "pea");
    assert(lp_vanilla::LogoFamily(true, true) == "dia");
    // Data follows native save version independently of the branding/mod choice.
    assert(lp_vanilla::DexTable(false) == "dp_pokedex_diamond");
    assert(lp_vanilla::DexTable(true) == "dp_pokedex_pearl");
    assert(lp_vanilla::EncounterTable(false) == "FieldEncountTable_d");
    assert(lp_vanilla::EncounterTable(true) == "FieldEncountTable_p");
}

int main() {
    TestBagAssetPolicy();
    TestTitleAssetPolicy();
    TestMessageIndices();
    TestBundle(false);
    TestBundle(true);
    TestDamagedCount();
    TestThreads();
    TestDeswizzle();
    TestPixelFormats();
    assert(lp_unity::PokemonIconBundle(445, 0, 0, false) ==
           "UIs/textures_mass/pokemon_l/pm0445_00_00_00_l");
    assert(lp_unity::PokemonIconSprite(445, 1, 2, true) == "pm0445_01_02_01_L");
    assert(lp_unity::ItemIconSprite(26) == "item_0026");
    std::printf("lp_unity synthetic tests passed\n");
    return 0;
}
