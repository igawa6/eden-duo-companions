// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_capture.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
void Put(Bytes& b, size_t p, uint64_t v, size_t n = 4) {
    assert(p <= b.size() && n <= b.size() - p);
    for (size_t i = 0; i < n; ++i) b[p + i] = uint8_t(v >> (8 * i));
}
void Text(Bytes& b, size_t p, const char* s) {
    assert(p + std::strlen(s) <= b.size()); std::memcpy(b.data() + p, s, std::strlen(s));
}
size_t Address(size_t x, size_t y) {
    // 64x64 ASTC block grid, 16 bytes/block, one GOB high.
    const size_t xb = x * 16;
    return (y / 8) * 512 * 16 + (xb / 64) * 512 + ((y % 8) / 2) * 64 +
           ((xb % 64) / 32) * 256 + ((xb % 32) / 16) * 32 + (y % 2) * 16;
}
Bytes Bntx(uint32_t selectors = 0x05040302) {
    Bytes b(0x100 + 65536);
    Text(b, 0, "BNTX"); Put(b, 0x24, 1); Put(b, 0x28, 0x30, 8); Put(b, 0x30, 0x40, 8);
    Text(b, 0x40, "BRTI"); Put(b, 0x52, 0, 2); Put(b, 0x56, 1, 2); Put(b, 0x5c, 0x2d06);
    Put(b, 0x64, 256); Put(b, 0x68, 256); Put(b, 0x6c, 1); Put(b, 0x70, 1);
    Put(b, 0x74, 0); Put(b, 0x90, 65536); Put(b, 0x98, selectors);
    Put(b, 0xa0, 0xc0, 8); Put(b, 0xb0, 0xf0, 8); Put(b, 0xc0, 4, 2); Text(b, 0xc2, "Frog");
    Put(b, 0xf0, 0x100, 8);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 64; ++x)
            for (size_t k = 0; k < 16; ++k)
                b[0x100 + Address(x, y) + k] = uint8_t((y * 64 + x) * 16 + k);
    return b;
}
Bytes Bfres(const Bytes& texture) {
    Bytes b(0x100 + texture.size());
    Text(b, 0, "FRES"); Put(b, 8, 0x80000); Put(b, 0xc, 0xfeff, 2); Put(b, 0x1c, b.size());
    Put(b, 0x98, 0xd0, 8); Put(b, 0xc8, 1, 2); Put(b, 0xd0, 0x100, 8);
    Put(b, 0xd8, texture.size(), 8); std::copy(texture.begin(), texture.end(), b.begin() + 0x100);
    return b;
}
Bytes Archive(const Bytes& bfres) {
    Bytes b(0x80);
    Text(b, 0, "SARC"); Put(b, 4, 0x14, 2); Put(b, 6, 0xfeff, 2); Put(b, 0xc, 0x80);
    Text(b, 0x14, "SFAT"); Put(b, 0x18, 0xc, 2); Put(b, 0x1a, 1, 2);
    Put(b, 0x24, 0x1000000); Put(b, 0x2c, bfres.size());
    Text(b, 0x30, "SFNT"); Put(b, 0x34, 8, 2); Text(b, 0x38, "HackListTexture.bfres");
    b.insert(b.end(), bfres.begin(), bfres.end()); Put(b, 8, b.size()); return b;
}
Bytes Packed(const Bytes& raw) {
    Bytes b(16); Text(b, 0, "Yaz0");
    for (size_t i = 0; i < 4; ++i) b[4 + i] = uint8_t(raw.size() >> (24 - 8 * i));
    for (size_t p = 0; p < raw.size();) {
        b.push_back(255);
        for (size_t k = 0; k < 8 && p < raw.size(); ++k) b.push_back(raw[p++]);
    }
    return b;
}
struct DecodeState { bool expect_blocks = true; unsigned calls = 0; };
EdenDsmodBool FakeDecode(void* ud, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh,
                         const uint8_t* in, size_t n, uint8_t* out, size_t size) {
    auto& state = *static_cast<DecodeState*>(ud); ++state.calls;
    assert(w == 256 && h == 256 && bw == 4 && bh == 4 && n == 65536 && size == 256 * 256 * 4);
    if (state.expect_blocks) for (size_t i = 0; i < n; ++i) assert(in[i] == uint8_t(i));
    for (size_t p = 0; p < size; p += 4) {
        out[p] = 200; out[p + 1] = 40; out[p + 2] = 10; out[p + 3] = 123;
    }
    return true;
}
EdenDsmodBool RejectDecode(void*, uint32_t, uint32_t, uint32_t, uint32_t,
                           const uint8_t*, size_t, uint8_t*, size_t) { return false; }
void Synthetic() {
    const auto texture = Bntx(), bfres = Bfres(texture), archive = Archive(bfres);
    DecodeState state;
    for (const auto& input : {archive, Packed(archive)}) {
        assert(SmoCapture::Names(input) == std::vector<std::string>{"Frog"});
        const auto image = SmoCapture::DecodeArchive(input, "Frog", &state, FakeDecode); assert(image);
        assert(image->name == "Frog" && image->width == 256 && image->height == 256);
        assert(image->rgba[0] == 200 && image->rgba[1] == 40 && image->rgba[2] == 10 && image->rgba[3] == 123);
    }
    assert(state.calls == 2);
    const auto swapped = SmoCapture::DecodeArchive(Archive(Bfres(Bntx(0x05020304))), "Frog", &state, FakeDecode);
    assert(swapped && swapped->rgba[0] == 10 && swapped->rgba[2] == 200 && swapped->rgba[3] == 123);
    assert(!SmoCapture::DecodeArchive(archive, "Frog", &state, nullptr));
    assert(!SmoCapture::DecodeArchive(archive, "Frog", &state, RejectDecode));
    assert(!SmoCapture::DecodeArchive(archive, "Missing", &state, FakeDecode));
    assert(SmoCapture::Names({}).empty());
    auto bad = bfres; Put(bad, 0xd0, UINT64_MAX, 8); assert(SmoCapture::Names(Archive(bad)).empty());
    bad = bfres; Put(bad, 8, 0x50003); assert(SmoCapture::Names(Archive(bad)).empty());
    bad = bfres; Put(bad, 0xd8, 4, 8); assert(SmoCapture::Names(Archive(bad)).empty());
    bad = texture; Put(bad, 0x64, 4096); assert(SmoCapture::Names(Archive(Bfres(bad))).empty());
    bad = texture; Put(bad, 0x98, 0xff040302); assert(SmoCapture::Names(Archive(Bfres(bad))).empty());
    bad = texture; Put(bad, 0xf0, UINT64_MAX, 8); assert(SmoCapture::Names(Archive(Bfres(bad))).empty());
    bad = Packed(archive); bad.pop_back(); assert(SmoCapture::Names(bad).empty());
    bad = Packed(archive); bad[4] = 0xff; assert(SmoCapture::Names(bad).empty());
    bad = Packed(archive); bad.resize(15); assert(SmoCapture::Names(bad).empty());
    bad = Packed(archive); bad[16] = 0; assert(SmoCapture::Names(bad).empty()); // backref before output
    bad = archive; bad.resize(0x80); assert(SmoCapture::Names(bad).empty());
}
void RealRegistry() {
    const char* path = std::getenv("SMO_CAPTURE_ARCHIVE");
    if (!path || !*path) { std::puts("smo_capture: real registry skipped (SMO_CAPTURE_ARCHIVE unset)"); return; }
    std::ifstream f(path, std::ios::binary); assert(f);
    const Bytes bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    const auto names = SmoCapture::Names(bytes);
    assert(names.size() == 52 && std::set<std::string>(names.begin(), names.end()).size() == 52);
    assert(std::find(names.begin(), names.end(), "Frog") != names.end());
    assert(std::find(names.begin(), names.end(), "TRex") != names.end());
    if (const char* dir = std::getenv("SMO_PACKAGE_DATA"); dir && *dir) {
        std::ifstream table(std::string(dir) + "/captures.tsv"); assert(table);
        std::string line; std::getline(table, line);
        std::set<std::string> expected;
        while (std::getline(table, line)) {
            const auto tab = line.find('\t');
            if (tab != line.npos) expected.insert(line.substr(0, tab));
        }
        assert(expected == std::set<std::string>(names.begin(), names.end()));
    }
    DecodeState state; state.expect_blocks = false;
    for (const auto& name : names) {
        // Checks native descriptors and ASTC transport only: this callback is synthetic RGBA.
        assert(SmoCapture::DecodeArchive(bytes, name, &state, FakeDecode));
    }
    assert(state.calls == 52);
    std::puts("smo_capture: real 52-name registry and ASTC service transport passed; pixel decode is host-owned");
}
} // namespace
int main() { Synthetic(); RealRegistry(); std::puts("smo_capture: archive bounds, swizzle, channel selectors and callback passed"); }
