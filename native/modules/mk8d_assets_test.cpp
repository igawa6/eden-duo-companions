// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// dsmod-mk8d-assets: synthetic checks of the Yaz0/SARC/BNTX/MSBT/map-camera decoders, then,
// when MK8D_ROMFS names a local 4.0.0 romfs dump, every course map, map camera, character /
// item / cup icon, course picture and USen message file through the module's own Library.
// MK8D_CONTACT_DIR additionally writes contact-sheet PNGs (and full-size map PNGs) for local
// visual review. Nothing here is ever packaged.

#include "mk8d_assets.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;

void Put16(Bytes& b, size_t o, std::uint16_t v) {
    std::memcpy(b.data() + o, &v, 2);
}
void Put32(Bytes& b, size_t o, std::uint32_t v) {
    std::memcpy(b.data() + o, &v, 4);
}
void Put64(Bytes& b, size_t o, std::uint64_t v) {
    std::memcpy(b.data() + o, &v, 8);
}

/// One-texture BNTX: descriptor at 0x60, name at 0x130, mip table at 0x120, pixels at 0x200.
Bytes Bntx(const std::string& name, std::uint32_t format, std::uint32_t w, std::uint32_t h,
           std::uint16_t tile, const Bytes& pixels) {
    Bytes b(0x200 + pixels.size());
    std::memcpy(b.data(), "BNTX", 4);
    Put32(b, 0x1c, static_cast<std::uint32_t>(b.size()));
    Put32(b, 0x24, 1);
    Put64(b, 0x28, 0x40);
    Put64(b, 0x40, 0x60);
    std::memcpy(b.data() + 0x60, "BRTI", 4);
    Put16(b, 0x72, tile);
    Put32(b, 0x7c, format);
    Put32(b, 0x84, w);
    Put32(b, 0x88, h);
    Put32(b, 0x90, 1);
    Put32(b, 0x94, 0);
    Put32(b, 0xb0, static_cast<std::uint32_t>(pixels.size()));
    Put32(b, 0xb8, 0x05040302);
    Put64(b, 0xc0, 0x130);
    Put64(b, 0xd0, 0x120);
    Put64(b, 0x120, 0x200);
    assert(name.size() < 0xc0);
    Put16(b, 0x130, static_cast<std::uint16_t>(name.size()));
    std::memcpy(b.data() + 0x132, name.data(), name.size());
    std::copy(pixels.begin(), pixels.end(), b.begin() + 0x200);
    return b;
}

/// SARC with or without a name table (little endian, hash multiplier 0x65).
Bytes Sarc(const std::vector<std::pair<std::string, Bytes>>& members, bool names) {
    std::vector<std::pair<std::string, Bytes>> sorted = members;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return Mk8dAssets::SarcHash(a.first) < Mk8dAssets::SarcHash(b.first);
    });
    Bytes name_table;
    std::vector<std::uint32_t> name_offsets;
    for (const auto& [n, _] : sorted) {
        name_offsets.push_back(static_cast<std::uint32_t>(name_table.size()));
        name_table.insert(name_table.end(), n.begin(), n.end());
        name_table.push_back(0);
        while (name_table.size() % 4)
            name_table.push_back(0);
    }
    if (!names)
        name_table.clear();
    const size_t sfat = 0x14, nodes = sfat + 12, sfnt = nodes + sorted.size() * 16;
    size_t data = (sfnt + 8 + name_table.size() + 0xff) & ~size_t{0xff};
    Bytes b(data);
    std::memcpy(b.data(), "SARC", 4);
    Put16(b, 4, 0x14);
    b[6] = 0xff;
    b[7] = 0xfe;
    std::memcpy(b.data() + sfat, "SFAT", 4);
    Put16(b, sfat + 4, 12);
    Put16(b, sfat + 6, static_cast<std::uint16_t>(sorted.size()));
    Put32(b, sfat + 8, 0x65);
    std::uint32_t offset = 0;
    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& [n, bytes] = sorted[i];
        Put32(b, nodes + i * 16, Mk8dAssets::SarcHash(n));
        Put32(b, nodes + i * 16 + 4, names ? 0x01000000u | (name_offsets[i] / 4) : 0);
        Put32(b, nodes + i * 16 + 8, offset);
        Put32(b, nodes + i * 16 + 12, offset + static_cast<std::uint32_t>(bytes.size()));
        offset += static_cast<std::uint32_t>((bytes.size() + 7) & ~size_t{7});
    }
    std::memcpy(b.data() + sfnt, "SFNT", 4);
    Put16(b, sfnt + 4, 8);
    std::copy(name_table.begin(), name_table.end(), b.begin() + sfnt + 8);
    Put32(b, 0xc, static_cast<std::uint32_t>(data));
    for (const auto& [_, bytes] : sorted) {
        b.insert(b.end(), bytes.begin(), bytes.end());
        while (b.size() % 8)
            b.push_back(0);
    }
    Put32(b, 8, static_cast<std::uint32_t>(b.size()));
    return b;
}

/// Yaz0 with literal-only groups (valid input for the decoder).
Bytes Yaz0Literal(const Bytes& raw) {
    Bytes b(16);
    std::memcpy(b.data(), "Yaz0", 4);
    const std::uint32_t n = static_cast<std::uint32_t>(raw.size());
    b[4] = n >> 24;
    b[5] = n >> 16;
    b[6] = n >> 8;
    b[7] = n;
    for (size_t i = 0; i < raw.size(); i += 8) {
        b.push_back(0xff);
        for (size_t j = i; j < std::min(raw.size(), i + 8); ++j)
            b.push_back(raw[j]);
    }
    return b;
}

/// MSBT (UTF-16LE): LBL1 with one hash slot per label, TXT2 with an inline tag in text 1.
Bytes Msbt(const std::vector<std::pair<std::string, std::u16string>>& entries) {
    auto section = [](const char* magic, const Bytes& body) {
        Bytes s(16);
        std::memcpy(s.data(), magic, 4);
        Put32(s, 4, static_cast<std::uint32_t>(body.size()));
        s.insert(s.end(), body.begin(), body.end());
        while (s.size() % 16)
            s.push_back(0xab);
        return s;
    };
    Bytes lbl(4 + entries.size() * 8);
    Put32(lbl, 0, static_cast<std::uint32_t>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        Put32(lbl, 4 + i * 8, 1);
        Put32(lbl, 8 + i * 8, static_cast<std::uint32_t>(lbl.size()));
        const auto& label = entries[i].first;
        lbl.push_back(static_cast<std::uint8_t>(label.size()));
        lbl.insert(lbl.end(), label.begin(), label.end());
        for (int k = 0; k < 4; ++k)
            lbl.push_back(static_cast<std::uint8_t>(i >> (8 * k)));
    }
    Bytes txt(4 + entries.size() * 4);
    Put32(txt, 0, static_cast<std::uint32_t>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        Put32(txt, 4 + i * 4, static_cast<std::uint32_t>(txt.size()));
        for (const char16_t c : entries[i].second) {
            txt.push_back(static_cast<std::uint8_t>(c));
            txt.push_back(static_cast<std::uint8_t>(c >> 8));
        }
        txt.push_back(0);
        txt.push_back(0);
    }
    Bytes b(0x20);
    std::memcpy(b.data(), "MsgStdBn", 8);
    b[8] = 0xff;
    b[9] = 0xfe;
    b[0xc] = 1;
    b[0xd] = 3;
    Put16(b, 0xe, 2);
    const auto l = section("LBL1", lbl), t = section("TXT2", txt);
    b.insert(b.end(), l.begin(), l.end());
    b.insert(b.end(), t.begin(), t.end());
    Put32(b, 0x12, static_cast<std::uint32_t>(b.size()));
    return b;
}

// ---- hosts -----------------------------------------------------------------------------------

struct Fake {
    std::unordered_map<std::string, Bytes> files;
};
size_t FakeRead(void* p, const char* path, std::uint64_t offset, void* out, size_t size) {
    auto& files = static_cast<Fake*>(p)->files;
    const auto it = files.find(path);
    if (it == files.end() || offset > it->second.size())
        return 0;
    if (!out)
        return it->second.size();
    const auto count = std::min(size, it->second.size() - static_cast<size_t>(offset));
    std::memcpy(out, it->second.data() + offset, count);
    return count;
}
/// A local romfs directory behind read_romfs (paths are "/<romfs path>", like the host's).
size_t DirRead(void* p, const char* path, std::uint64_t offset, void* out, size_t size) {
    const fs::path file = *static_cast<fs::path*>(p) / (path[0] == '/' ? path + 1 : path);
    std::error_code ec;
    const auto total = fs::file_size(file, ec);
    if (ec || offset > total)
        return 0;
    if (!out)
        return static_cast<size_t>(total);
    std::ifstream in(file, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    const auto count = std::min<std::uint64_t>(size, total - offset);
    in.read(static_cast<char*>(out), static_cast<std::streamsize>(count));
    return in ? static_cast<size_t>(count) : 0;
}

// ---- PNG (stored deflate; review images only) -------------------------------------------------

std::uint32_t Crc(const std::uint8_t* d, size_t n, std::uint32_t c = 0) {
    static std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t v = i;
            for (int k = 0; k < 8; ++k)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
        return t;
    }();
    c = ~c;
    for (size_t i = 0; i < n; ++i)
        c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
bool WritePng(const fs::path& path, std::uint32_t w, std::uint32_t h, const Bytes& rgba) {
    Bytes raw;
    raw.reserve((w * 4 + 1) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y) * w * 4,
                   rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 4);
    }
    Bytes z{0x78, 0x01};
    for (size_t i = 0; i < raw.size(); i += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back(n & 0xff);
        z.push_back(n >> 8);
        z.push_back(~n & 0xff);
        z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(i),
                 raw.begin() + static_cast<std::ptrdiff_t>(i + n));
    }
    std::uint32_t a = 1, b = 0;
    for (const auto c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const std::uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; --k)
        z.push_back(static_cast<std::uint8_t>(adler >> (8 * k)));
    Bytes out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto chunk = [&out](const char* type, const Bytes& data) {
        for (int k = 3; k >= 0; --k)
            out.push_back(static_cast<std::uint8_t>(data.size() >> (8 * k)));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        const auto c = Crc(out.data() + start, out.size() - start);
        for (int k = 3; k >= 0; --k)
            out.push_back(static_cast<std::uint8_t>(c >> (8 * k)));
    };
    Bytes ihdr(13);
    for (int k = 0; k < 4; ++k) {
        ihdr[k] = static_cast<std::uint8_t>(w >> (8 * (3 - k)));
        ihdr[4 + k] = static_cast<std::uint8_t>(h >> (8 * (3 - k)));
    }
    ihdr[8] = 8;
    ihdr[9] = 6;
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

/// Grid of images, each scaled (nearest) to fit a cell, over a grey checkerboard.
struct Sheet {
    std::uint32_t cell, cols;
    std::vector<std::pair<std::string, std::shared_ptr<const Mk8dAssets::Image>>> items;
    void Write(const fs::path& png) const {
        const std::uint32_t rows = static_cast<std::uint32_t>((items.size() + cols - 1) / cols);
        const std::uint32_t pad = 4, step = cell + pad;
        const std::uint32_t w = cols * step + pad,
                            h = std::max<std::uint32_t>(1, rows) * step + pad;
        Bytes rgba(static_cast<size_t>(w) * h * 4);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint8_t g = ((x / 8 + y / 8) & 1) ? 0x90 : 0x70;
                std::uint8_t* p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
                p[0] = p[1] = p[2] = g;
                p[3] = 255;
            }
        std::string index;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& img = *items[i].second;
            index += std::to_string(i) + "\t" + items[i].first + "\t" + std::to_string(img.width) +
                     "x" + std::to_string(img.height) + "\n";
            const float scale =
                std::min(1.0f, static_cast<float>(cell) / std::max(img.width, img.height));
            const auto sw = static_cast<std::uint32_t>(img.width * scale),
                       sh = static_cast<std::uint32_t>(img.height * scale);
            const std::uint32_t ox = pad + static_cast<std::uint32_t>(i % cols) * step,
                                oy = pad + static_cast<std::uint32_t>(i / cols) * step;
            for (std::uint32_t y = 0; y < sh; ++y)
                for (std::uint32_t x = 0; x < sw; ++x) {
                    const auto sx = std::min(img.width - 1, static_cast<std::uint32_t>(x / scale));
                    const auto sy = std::min(img.height - 1, static_cast<std::uint32_t>(y / scale));
                    const std::uint8_t* s =
                        img.rgba.data() + (static_cast<size_t>(sy) * img.width + sx) * 4;
                    std::uint8_t* d = rgba.data() + (static_cast<size_t>(oy + y) * w + ox + x) * 4;
                    const unsigned a = s[3];
                    for (int c = 0; c < 3; ++c)
                        d[c] = static_cast<std::uint8_t>((s[c] * a + d[c] * (255 - a)) / 255);
                }
        }
        assert(WritePng(png, w, h, rgba));
        std::ofstream(fs::path(png).replace_extension(".txt")) << index;
    }
};

void Synthetic() {
    using namespace Mk8dAssets;
    Fake fake;
    EdenDsmodHostApi host{};
    host.userdata = &fake;
    host.read_romfs = FakeRead;
    Library library;

    // map/<course>: the texture is chosen by name (ym_Map_<course>^*), never "the first one".
    fake.files["/Course/Test/course_maptexture.bntx"] =
        Bntx("ym_Map_Test^q", 0x0b01, 1, 1, 1, Bytes{10, 20, 30, 40});
    auto image = library.LoadImage(host, "module:mk8d:map/Test");
    assert(image && image->width == 1 && image->height == 1);
    assert((image->rgba == Bytes{10, 20, 30, 40}));
    assert(library.LoadImage(host, "map/Test") == image); // cached, prefix optional
    fake.files["/Course/Other/course_maptexture.bntx"] =
        Bntx("ym_Map_Test^q", 0x0b01, 1, 1, 1, Bytes{1, 2, 3, 4});
    // Another internal name but the file's only texture: used (CTGP-DX CT_xx maps keep vanilla
    // internal names, e.g. CT_00 = "ym_Map_Gu_City^q", and the game shows them).
    auto other = library.LoadImage(host, "map/Other");
    assert(other && (other->rgba == Bytes{1, 2, 3, 4}));
    // Block-linear BC1 (one 4x4 block in a GOB): a red RGB565 endpoint everywhere.
    Bytes bc1(512);
    bc1[1] = 0xf8;
    fake.files["/Course/Block/course_maptexture.bntx"] =
        Bntx("ym_Map_Block^q", 0x1a01, 4, 4, 0, bc1);
    auto block = library.LoadImage(host, "map/Block");
    assert(block && block->width == 4 && block->rgba.size() == 64);
    assert(block->rgba[0] == 255 && block->rgba[1] == 0 && block->rgba[2] == 0 &&
           block->rgba[3] == 255);
    // R5G6B5 (0x07): red in the low five bits.
    fake.files["/UI/cmn/race/ym_CupIconTest_130x130^h.bntx"] =
        Bntx("ym_CupIconTest_130x130^h", 0x0701, 1, 1, 1, Bytes{0x1f, 0x00});
    auto cup = library.LoadImage(host, "cup/Test/130x130");
    assert(cup && cup->rgba[0] == 255 && cup->rgba[1] == 0 && cup->rgba[2] == 0 &&
           cup->rgba[3] == 255);
    assert(!library.LoadImage(host, "cup/Test/Bogus"));
    // Rejected keys.
    for (const char* bad : {"map/../Test", "map/", "map/a/b", "chara/..", "item/x/y", "nope/x",
                            "lyt/cmn/race/rc_L_Lap_00", "coursepict/a b"})
        assert(!library.LoadImage(host, bad));
    fake.files["/Course/Bad/course_maptexture.bntx"] = Bytes{'B', 'N', 'T', 'X'};
    assert(!library.LoadImage(host, "map/Bad"));

    // chara/<name>: SARC member (named) -> Yaz0 -> hash-only SARC -> timg/__Combined.bntx.
    Bytes combined = Bntx("tc_MapChara_Test^l", 0x0b01, 1, 1, 1, Bytes{7, 8, 9, 255});
    Bytes inner = Sarc({{"blyt/x.bflyt", Bytes(16, 1)}, {"timg/__Combined.bntx", combined}}, false);
    fake.files["/UI/cmn/common.sarc"] = Sarc(
        {{"cm_L_Other_00.szs", Bytes(32, 2)}, {"cm_L_CharaIcon_00.szs", Yaz0Literal(inner)}}, true);
    auto chara = library.LoadImage(host, "chara/test"); // base-name match ignores case
    assert(chara && (chara->rgba == Bytes{7, 8, 9, 255}));
    assert(
        (library.ListTextures(host, "chara/_") == std::vector<std::string>{"tc_MapChara_Test^l"}));
    assert(library.LoadImage(host, "lyt/cmn/common/cm_L_CharaIcon_00/tc_MapChara_Test^l"));
    assert(!library.LoadImage(host, "lyt/cmn/common/cm_L_CharaIcon_00/tc_MapChara_Test"));
    assert(Library::CharaKey("Mario") == "module:mk8d:chara/Mario" &&
           Library::CharaKey("a/b").empty());

    // Messages: UI/<lang>/message.sarc#<file>.msbt, tags stripped.
    const std::u16string tagged = std::u16string(u"W") + char16_t{0x0e} + char16_t{0} +
                                  char16_t{2} + char16_t{2} + char16_t{0x0c80} + u" 25";
    fake.files["/UI/USen/message.sarc"] =
        Sarc({{"Common.msbt", Msbt({{"1001", u"Mario"}, {"1117", tagged}})}}, true);
    auto messages = library.LoadMessages(host, "USen", "Common");
    assert(messages && messages->texts.size() == 2);
    assert(library.Text(host, "USen", "Common", "1001") == "Mario");
    assert(library.Text(host, "USen", "Common", "1117") == "W 25");
    assert(library.Text(host, "USen", "Common", "9999").empty());

    // Map camera: eye (0,10,0) looking at the origin, up -Z: +X is right, -Z is up on the map.
    std::array<float, 11> cam{0, 10, 0, 0, 0, 0, 0, 0, -1, 100, 100};
    Bytes cam_bytes(45, '\n');
    std::memcpy(cam_bytes.data(), cam.data(), 44);
    fake.files["/Course/Test/course_mapcamera.bin"] = cam_bytes;
    auto camera = library.LoadMapCamera(host, "Test");
    assert(camera);
    float u = 0, v = 0;
    assert(camera->Project(0, 0, 0, u, v) && u == 0.5f && v == 0.5f);
    assert(camera->Project(25, 123, 0, u, v) && u == 0.75f && v == 0.5f); // height is ignored
    assert(camera->Project(0, 0, -25, u, v) && u == 0.5f && v == 0.25f);
    // Up parallel to the view direction has no right axis; a short file is rejected.
    std::array<float, 11> degenerate{0, 10, 0, 0, 0, 0, 0, 1, 0, 100, 100};
    Bytes degenerate_bytes(44);
    std::memcpy(degenerate_bytes.data(), degenerate.data(), 44);
    assert(!MapCamera::Parse(degenerate_bytes));
    assert(!MapCamera::Parse(Bytes(43)));
    // Zero / NaN ortho extents are rejected at parse time; Project never returns NaN.
    std::array<float, 11> flat{0, 10, 0, 0, 0, 0, 0, 0, -1, 0, 100};
    Bytes flat_bytes(44);
    std::memcpy(flat_bytes.data(), flat.data(), 44);
    assert(!MapCamera::Parse(flat_bytes));
    flat[9] = std::nanf("");
    std::memcpy(flat_bytes.data(), flat.data(), 44);
    assert(!MapCamera::Parse(flat_bytes));
    assert(!camera->Project(std::nanf(""), 0, 0, u, v));

    // Offsets near 2^64 in file headers must not wrap the bounds checks.
    const Bytes good = Bntx("ym_Map_Wrap^q", 0x0b01, 1, 1, 1, Bytes{1, 2, 3, 4});
    assert(ParseBntx(good));
    for (const auto& [at, value] : std::vector<std::pair<size_t, std::uint64_t>>{
             {0x28, ~std::uint64_t{7}},    // texture pointer array
             {0x40, ~std::uint64_t{0x9f}}, // BRTI descriptor
             {0xc0, ~std::uint64_t{1}},    // texture name
             {0xd0, ~std::uint64_t{7}}}) { // mip table
        Bytes bad = good;
        Put64(bad, at, value);
        assert(!ParseBntx(bad));
    }
    // A BFRES (emblem) whose external-file dictionary / name offsets point near 2^64.
    Bytes fres(0x200);
    std::memcpy(fres.data(), "FRES    ", 8);
    Put64(fres, 0x98, 0x100);
    Put64(fres, 0xa0, ~std::uint64_t{7});
    fake.files["/Kart/Emblem/Emblem_Wrap.szs"] = fres;
    assert(!library.LoadImage(host, "emblem/Wrap"));
    Put64(fres, 0xa0, 0x40);
    Put32(fres, 0x44, 1);
    Put64(fres, 0x40 + 8 + 16 + 8, ~std::uint64_t{1});
    fake.files["/Kart/Emblem/Emblem_Wrap2.szs"] = fres;
    assert(!library.LoadImage(host, "emblem/Wrap2"));
}

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

int RealData(const fs::path& romfs, const fs::path& contact) {
    using namespace Mk8dAssets;
    fs::path root = romfs;
    EdenDsmodHostApi host{};
    host.userdata = &root;
    host.read_romfs = DirRead;
    Library library(256 * 1024 * 1024, 64 * 1024 * 1024);
    int failures = 0;
    auto fail = [&failures](const std::string& what) {
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
        ++failures;
    };

    // Course maps + cameras.
    Sheet maps{192, 12, {}};
    std::vector<std::string> courses;
    for (const auto& e : fs::directory_iterator(romfs / "Course"))
        if (fs::exists(e.path() / "course_maptexture.bntx"))
            courses.push_back(e.path().filename().string());
    std::sort(courses.begin(), courses.end());
    std::size_t sizes480 = 0, sizes512 = 0;
    std::string camera_table;
    for (const auto& c : courses) {
        const auto names = library.ListTextures(host, "map/" + c);
        if (names != std::vector<std::string>{"ym_Map_" + c + "^q"})
            fail("map texture names of " + c);
        const auto img = library.LoadImage(host, Library::MapKey(c));
        if (!img || img->width != img->height || (img->width != 480 && img->width != 512)) {
            fail("map " + c);
            continue;
        }
        (img->width == 480 ? sizes480 : sizes512)++;
        maps.items.emplace_back(c, img);
        const auto cam = library.LoadMapCamera(host, c);
        float u = 0, v = 0;
        if (!cam || !cam->Project(cam->look_at[0], cam->look_at[1], cam->look_at[2], u, v) ||
            std::fabs(u - 0.5f) > 1e-3f || std::fabs(v - 0.5f) > 1e-3f)
            fail("camera " + c);
        if (cam) {
            char line[512];
            std::snprintf(line, sizeof(line),
                          "%s\t%g %g %g\t%g %g %g\t%g %g %g\t%g %g\t%g %g %g %g\t%g %g %g %g\n",
                          c.c_str(), cam->position[0], cam->position[1], cam->position[2],
                          cam->look_at[0], cam->look_at[1], cam->look_at[2], cam->up[0], cam->up[1],
                          cam->up[2], cam->width, cam->height, cam->row[0][0], cam->row[0][1],
                          cam->row[0][2], cam->row[0][3], cam->row[1][0], cam->row[1][1],
                          cam->row[1][2], cam->row[1][3]);
            camera_table += line;
        }
    }
    std::printf("maps: %zu courses (%zu at 480x480, %zu at 512x512), cameras parsed\n",
                courses.size(), sizes480, sizes512);
    if (courses.size() != 107)
        fail("expected 107 course maps, found " + std::to_string(courses.size()));

    // Character / item icons: every texture of the kind's archive.
    auto by_prefix = [&](const std::string& key, const std::string& prefix, std::uint32_t size,
                         const char* kind, Sheet& sheet) {
        std::size_t n = 0;
        for (const auto& name : library.ListTextures(host, key)) {
            std::string base = name.substr(0, name.find_first_of("^+"));
            if (base.size() <= prefix.size() ||
                !std::equal(prefix.begin(), prefix.end(), base.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                }))
                continue;
            const std::string component = base.substr(prefix.size());
            const auto img = library.LoadImage(host, std::string(kind) + "/" + component);
            if (!img || img->width != size || img->height != size) {
                fail(std::string(kind) + "/" + component);
                continue;
            }
            sheet.items.emplace_back(component, img);
            ++n;
        }
        return n;
    };
    Sheet charas{64, 12, {}}, items{96, 10, {}};
    const auto nchara = by_prefix("chara/_", "tc_MapChara_", 64, "chara", charas);
    const auto nitem = by_prefix("item/_", "tc_Item_", 192, "item", items);
    std::printf("chara icons: %zu, item icons: %zu\n", nchara, nitem);
    if (nchara != 79 || nitem != 30)
        fail("icon counts");

    // Cups (all four variants) and course pictures: loose BNTX files.
    Sheet cups{128, 8, {}}, picts{152, 10, {}};
    std::size_t ncup = 0, npict = 0;
    std::vector<fs::path> loose;
    for (const auto& e : fs::directory_iterator(romfs / "UI/cmn/race"))
        loose.push_back(e.path());
    for (const auto& e : fs::directory_iterator(romfs / "UI/cmn/menu"))
        loose.push_back(e.path());
    std::sort(loose.begin(), loose.end());
    for (const auto& p : loose) {
        const std::string f = p.filename().string();
        if (f.starts_with("ym_CupIcon") && f.ends_with("_Nml^l.bntx")) {
            const std::string name = f.substr(10, f.size() - 10 - 11);
            const std::array<std::pair<const char*, std::uint32_t>, 4> variants{
                {{"Nml", 404}, {"Line", 400}, {"Line128x128", 128}, {"130x130", 210}}};
            for (const auto& [variant, size] : variants) {
                const auto img = library.LoadImage(host, "cup/" + name + "/" + variant);
                if (!img || img->width != size || img->height != size) {
                    fail("cup/" + name + "/" + variant);
                    continue;
                }
                cups.items.emplace_back(name + "/" + variant, img);
                if (std::string(variant) == "Line" &&
                    library.LoadImage(host, Library::CupKey(name)) != img)
                    fail("cup default variant " + name);
            }
            ++ncup;
        } else if (f.starts_with("ym_CoursePict_") && f.ends_with("_00^u.bntx")) {
            const std::string name = f.substr(14, f.size() - 14 - 10);
            const auto img = library.LoadImage(host, Library::CoursePictKey(name));
            if (!img || img->width != 304 || img->height != 256) {
                fail("coursepict/" + name);
                continue;
            }
            picts.items.emplace_back(name, img);
            ++npict;
        }
    }
    std::printf("cups: %zu (x4 variants), course pictures: %zu\n", ncup, npict);
    if (ncup != 24 || npict != 105)
        fail("cup/course-picture counts");

    // A layout texture by exact name (the lap flag the race HUD uses).
    if (!library.LoadImage(host, "lyt/cmn/race/rc_L_Lap_00/ym_LapFlag_00^f"))
        fail("lyt lap flag");

    // Kart emblems: every Kart/Emblem/Emblem_<code>.szs (Yaz0 BFRES + embedded textures.bntx).
    Sheet emblems{128, 10, {}};
    std::vector<std::string> emblem_codes;
    for (const auto& e : fs::directory_iterator(romfs / "Kart/Emblem")) {
        const std::string f = e.path().filename().string();
        if (f.starts_with("Emblem_") && f.ends_with(".szs"))
            emblem_codes.push_back(f.substr(7, f.size() - 7 - 4));
    }
    std::sort(emblem_codes.begin(), emblem_codes.end());
    for (const auto& code : emblem_codes) {
        const auto key = Library::EmblemKey(code);
        if (library.ListTextures(host, key) != std::vector<std::string>{"Emblem_" + code}) {
            fail("emblem texture names of " + code);
            continue;
        }
        const auto img = library.LoadImage(host, key);
        if (!img || img->width == 0) {
            fail("emblem/" + code);
            continue;
        }
        emblems.items.emplace_back(code, img);
    }
    std::printf("kart emblems: %zu\n", emblems.items.size());
    if (emblem_codes.size() != 75 || emblems.items.size() != 75)
        fail("emblem count");

    // Layout materials: the map crown (yellow fill, dark red outline) and the menu's blue.
    {
        const auto crown =
            library.LoadImage(host, "lytmat/cmn/race/rc_L_DRC_MapIconChara_00/P_MapIconCrown_00");
        const auto crown_tex =
            library.LoadImage(host, "lyt/cmn/race/rc_L_DRC_MapIconChara_00/ym_MapIconCrown_00^t");
        if (!crown || !crown_tex || crown->width != crown_tex->width)
            fail("lytmat crown");
        const auto blue = library.LoadImage(host, "lytmat/cmn/menu/mn_Background_00/P_BlueBG_00");
        // ym_MenuBGGrd_00 runs white (top) -> black: the top row is the material's white colour.
        if (!blue || blue->rgba[0] != 0x12 || blue->rgba[1] != 0x8c || blue->rgba[2] != 0xd7)
            fail("lytmat blue background");
        if (library.LoadImage(host, "lytmat/cmn/race/rc_L_DRC_Map_00/NoSuchMaterial"))
            fail("lytmat unknown material");
    }

    // Map fit boxes: every course's box has the page aspect and contains its alpha bbox.
    {
        std::size_t nfit = 0;
        for (const auto& c : courses) {
            const auto box = library.MapFitBox(host, c);
            const auto img = library.LoadImage(host, Library::MapFitKey(c));
            if (!box || !img) {
                fail("mapfit " + c);
                continue;
            }
            const double aspect = static_cast<double>(img->width) / img->height;
            if (std::fabs(aspect - 1.0) > 0.01 || box->u0 > 0.5f || box->u1 < 0.5f)
                fail("mapfit aspect " + c);
            ++nfit;
        }
        std::printf("map fit boxes: %zu\n", nfit);
    }

    // Page font advances (r{i}.name_scale): the runtime's own measurement of the same font gave
    // 35 px per digit at scale 7 with cap height 51 -> advance("0") * 35 / 51 == 35.
    {
        const auto adv = library.LoadFontAdvances(host, "USen", "turbo_MARIOFont.bffnt");
        if (!adv)
            fail("font advances");
        if (adv) {
            const auto ros = adv->Measure("Rosalina");
            const auto dk = adv->Measure("Donkey Kong");
            std::printf(
                "font advances: '0' %u, Rosalina %u, Donkey Kong %u, Light-blue Shy Guy %u\n",
                adv->Measure("0"), ros, dk, adv->Measure("Light-blue Shy Guy"));
            if (adv->Measure("0") != 51 || ros == 0 || dk <= ros)
                fail("font advance values");
        }
    }

    // Page design review: MK8D_DUMP_KEYS="key,key,..." writes each decoded key as a PNG under
    // <contact>/keys/ (local review only, never packaged).
    std::vector<std::pair<std::string, std::shared_ptr<const Image>>> dumped;
    for (std::string rest = Env("MK8D_DUMP_KEYS"); !rest.empty();) {
        const auto comma = rest.find(',');
        const std::string key = rest.substr(0, comma);
        rest = comma == std::string::npos ? std::string{} : rest.substr(comma + 1);
        if (const auto img = library.LoadImage(host, key)) {
            dumped.emplace_back(key, img);
            std::printf("dump %s %ux%u\n", key.c_str(), img->width, img->height);
        } else {
            std::printf("dump %s: no image\n", key.c_str());
        }
    }

    // USen text.
    const std::array<std::pair<const char*, std::size_t>, 5> files{
        {{"Common", 393}, {"Menu", 427}, {"Race", 241}, {"DLC", 71}, {"Region", 133}}};
    for (const auto& [file, count] : files) {
        const auto table = library.LoadMessages(host, "USen", file);
        if (!table || table->texts.size() != count)
            fail(std::string("msbt ") + file);
    }
    for (const auto& [label, text] :
         std::array<std::pair<const char*, const char*>, 6>{{{"1001", "Mario"},
                                                             {"1075", "Peachette"},
                                                             {"1034", "Link"},
                                                             {"1301", "Mushroom Cup"},
                                                             {"1406", "Mario Circuit"},
                                                             {"1117", "W 25 Silver Arrow"}}}) {
        const auto got = library.Text(host, "USen", "Common", label);
        if (got != text)
            fail(std::string("text ") + label + " = '" + got + "'");
    }

    if (!contact.empty()) {
        fs::create_directories(contact / "maps");
        maps.Write(contact / "mk8d_maps.png");
        charas.Write(contact / "mk8d_chara.png");
        items.Write(contact / "mk8d_items.png");
        cups.Write(contact / "mk8d_cups.png");
        picts.Write(contact / "mk8d_coursepict.png");
        emblems.Write(contact / "mk8d_emblems.png");
        if (!dumped.empty())
            fs::create_directories(contact / "keys");
        for (const auto& [key, img] : dumped) {
            std::string file = key;
            std::replace(file.begin(), file.end(), '/', '_');
            std::replace(file.begin(), file.end(), '^', '-');
            WritePng(contact / "keys" / (file + ".png"), img->width, img->height, img->rgba);
        }
        for (const auto& [name, img] : maps.items)
            WritePng(contact / "maps" / (name + ".png"), img->width, img->height, img->rgba);
        std::ofstream(contact / "mk8d_map_cameras.tsv") << camera_table;
        std::printf("contact sheets written to %s\n", contact.string().c_str());
    }
    return failures;
}
/// 0.4.2: every CTGP-DX course (MK8D_CTGP_ROMFS = the CTGP-DX romfs layer, e.g.
/// game-data/mk8d/ctgp111/romfs; each Course/CT_xx carries its own map texture + camera): the
/// map, the map camera and the fit crop (the page's map tile and mode-button thumbnail) all
/// decode, non-empty, for CT_00..CT_3F. CT maps keep vanilla internal names, come in 480/512/
/// 1024 squares, one is uncompressed, some cameras are non-square or have unnormalised up.
int CtgpData(const fs::path& romfs) {
    using namespace Mk8dAssets;
    fs::path root = romfs;
    EdenDsmodHostApi host{};
    host.userdata = &root;
    host.read_romfs = DirRead;
    Library library(256 * 1024 * 1024, 64 * 1024 * 1024);
    int failures = 0, n = 0;
    std::map<std::uint32_t, int> sizes;
    for (int i = 0; i < 64; ++i) {
        char c[8];
        std::snprintf(c, sizeof(c), "CT_%02X", i);
        const auto map = library.LoadImage(host, Library::MapKey(c));
        const auto fit = library.LoadImage(host, Library::MapFitKey(c));
        const auto cam = library.LoadMapCamera(host, c);
        const auto opaque = [](const std::shared_ptr<const Image>& img) {
            std::size_t k = 0;
            if (img)
                for (std::size_t j = 3; j < img->rgba.size(); j += 4)
                    k += img->rgba[j] != 0;
            return k;
        };
        if (!map || map->width == 0 || opaque(map) == 0) {
            std::fprintf(stderr, "FAIL ctgp map %s\n", c);
            ++failures;
            continue;
        }
        if (!fit || fit->width == 0 || opaque(fit) == 0) {
            std::fprintf(stderr, "FAIL ctgp mapfit %s\n", c);
            ++failures;
            continue;
        }
        if (!cam) {
            std::fprintf(stderr, "FAIL ctgp camera %s\n", c);
            ++failures;
            continue;
        }
        sizes[map->width]++;
        ++n;
    }
    std::printf("ctgp maps: %d of 64 CT courses with map + fit + camera (sizes:", n);
    for (const auto& [w, k] : sizes)
        std::printf(" %ux%u x%d", w, w, k);
    std::printf(")\n");
    return failures;
}
} // namespace

int main() {
    Synthetic();
    std::printf("synthetic checks passed\n");
    int ctgp_failures = 0;
    if (const std::string ct = Env("MK8D_CTGP_ROMFS");
        !ct.empty() && fs::is_directory(fs::path(ct) / "Course")) {
        ctgp_failures = CtgpData(ct);
        std::printf("ctgp checks: %s (%d failure(s))\n", ctgp_failures ? "FAILED" : "passed",
                    ctgp_failures);
    }
    const std::string romfs = Env("MK8D_ROMFS");
    if (romfs.empty() || !fs::is_directory(fs::path(romfs) / "Course")) {
        if (ctgp_failures)
            return 1;
        std::printf("MK8D_ROMFS not set or not a romfs dump: real-data checks skipped\n");
        return 0;
    }
    const int failures = RealData(romfs, Env("MK8D_CONTACT_DIR"));
    std::printf("real-data checks: %s (%d failure(s))\n", failures ? "FAILED" : "passed", failures);
    return failures || ctgp_failures ? 1 : 0;
}
