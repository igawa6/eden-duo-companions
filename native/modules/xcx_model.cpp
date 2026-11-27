// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Format reference: ScanMountGoat/xc3_lib, commit 3e47cf2f20bc4bee35d804058f659507208e2d01,
// mxmd::{MxmdV40,legacy::{Models,Model,Mesh,VertexData}} and msrd::{StreamingData,StreamEntry}.
// This deliberately supports the inspected XCX DE geometry subset, not arbitrary XC models.
#include "xcx_model.h"
#include "dread_mapgen_util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <zstd.h>
namespace xcx {
namespace {
using Span = std::span<const std::uint8_t>;
constexpr std::size_t MaxGeometry = 32 * 1024 * 1024;
void Require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
Span Range(Span b, std::uint64_t at, std::uint64_t count) {
    Require(at <= b.size() && count <= b.size() - at, "XCX truncated model range");
    return b.subspan(at, count);
}
template <class T> T At(Span b, std::size_t at) {
    const auto s = Range(b, at, sizeof(T));
    T v;
    std::memcpy(&v, s.data(), sizeof v);
    return v;
}
unsigned U32(Span b, std::size_t p) { return At<std::uint32_t>(b, p); }
unsigned U16(Span b, std::size_t p) { return At<std::uint16_t>(b, p); }
Span Array(Span b, std::size_t at, unsigned count, unsigned stride, unsigned limit) {
    Require(count <= limit, "XCX excessive model array count");
    return Range(b, at, std::uint64_t(count) * stride);
}
std::string Name(Span b, std::size_t at) {
    Require(at < b.size(), "XCX material name offset");
    const auto bytes = Range(b, at, std::min<std::size_t>(129, b.size() - at));
    const auto end = std::find(bytes.begin(), bytes.end(), 0);
    Require(end != bytes.end() && end != bytes.begin() &&
                std::all_of(bytes.begin(), end, [](unsigned c) { return c >= 32 && c < 127; }),
            "XCX invalid material or texture name");
    return {reinterpret_cast<const char *>(bytes.data()), std::size_t(end - bytes.begin())};
}
void Materials(FrameGeometry &out, Span md, Span mt, unsigned s, Span entries, Span data) {
    // XCX DE's inspected 92-byte streaming header has the shared-texture descriptors.
    Require(U32(mt, s + 12) == 92, "XCX material streaming header size");
    const auto low_index = U32(mt, s + 32);
    Require(low_index < entries.size() / 20 && U16(entries, low_index * 20 + 10) == 2 &&
                U32(mt, s + 36) == 0,
            "XCX unsupported low texture stream");
    const auto low_bytes =
        Range(data, U32(entries, low_index * 20), U32(entries, low_index * 20 + 4));
    out.low_texture_data.assign(low_bytes.begin(), low_bytes.end());
    const std::size_t low = std::uint64_t(s) + U32(mt, s + 60);
    const auto low_count = U32(mt, low);
    const auto textures = Array(mt, low + U32(mt, low + 4), low_count, 16, 4096);
    Require(low_count > 0, "XCX missing model texture sources");
    for (unsigned n = 0; n < low_count; ++n) {
        const auto p = n * 16;
        ModelTextureSource source;
        source.usage = U32(textures, p);
        source.low_size = U32(textures, p + 4);
        source.low_offset = U32(textures, p + 8);
        Require(source.low_size >= 40, "XCX missing low texture data");
        Range(low_bytes, source.low_offset, source.low_size);
        source.name = Name(mt, low + U32(textures, p + 12));
        out.texture_sources.push_back(std::move(source));
    }
    const auto high_count = U32(mt, s + 68);
    Require(high_count == U32(mt, s + 52), "XCX shared texture mapping count");
    const auto high = Array(mt, std::uint64_t(s) + U32(mt, s + 72), high_count, 20, 4096);
    const auto map = Array(mt, std::uint64_t(s) + U32(mt, s + 56), high_count, 2, 4096);
    for (unsigned n = 0; n < high_count; ++n) {
        const auto index = U16(map, n * 2);
        Require(index < low_count, "XCX shared texture index");
        auto &source = out.texture_sources[index];
        char hash[9];
        std::snprintf(hash, sizeof hash, "%08x", U32(high, n * 20));
        Require(source.name == hash && source.mip_path.empty(), "XCX shared texture identity");
        Require(U32(high, n * 20 + 4) && U32(high, n * 20 + 8), "XCX missing shared mip data");
        source.mip_path =
            "/chr/cmntex/" + source.name.substr(0, 2) + "/" + source.name + "_m.wismt";
        if (U32(high, n * 20 + 12)) {
            Require(U32(high, n * 20 + 16) != 0, "XCX missing base mip data");
            source.base_mip_path =
                "/chr/cmntex/" + source.name.substr(0, 2) + "/" + source.name + "_h.wismt";
        } else
            Require(U32(high, n * 20 + 16) == 0, "XCX inconsistent base mip data");
    }
    const auto materials = U32(md, 12);
    const auto count = U32(md, materials + 4);
    const auto records = Array(md, std::uint64_t(materials) + U32(md, materials), count, 136, 1024);
    Require(count > 0, "XCX missing materials");
    for (unsigned n = 0; n < count; ++n) {
        const auto p = n * 136;
        ModelMaterial material;
        material.name = Name(md, std::uint64_t(materials) + U32(records, p));
        for (unsigned k = 0; k < 4; ++k) {
            material.color[k] = At<float>(records, p + 8 + k * 4);
            Require(std::isfinite(material.color[k]), "XCX invalid material color");
        }
        const auto bindings =
            Array(md, std::uint64_t(materials) + U32(records, p + 60), U32(records, p + 64), 4, 64);
        for (std::size_t q = 0; q < bindings.size(); q += 4) {
            const auto index = U16(bindings, q);
            Require(index < low_count, "XCX material references missing texture");
            material.textures.push_back(
                {std::uint16_t(index), std::uint16_t(U16(bindings, q + 2))});
        }
        out.materials.push_back(std::move(material));
    }
}
bool Resource(std::string_view s) {
    return s.size() == 8 && s.starts_with("dl") &&
           s.substr(2).find_first_not_of("0123456789") == std::string_view::npos;
}
Image ModelChannels(Span data, unsigned width, unsigned height, bool two_channels) {
    // BC4/5 endpoint interpolation quantized to nearest UNORM8, matching the model
    // reference decoder. The existing UI BC decoder truncates these interpolants.
    const unsigned wb = (width + 3) / 4, hb = (height + 3) / 4, bpb = two_channels ? 16 : 8;
    const auto hh = hb + hb / 2;
    const unsigned bh = hh >= 128 ? 16 : hh >= 64 ? 8 : hh >= 32 ? 4 : hh >= 16 ? 2 : 1;
    const unsigned gobs = (wb * bpb + 63) / 64;
    Image out(width, height);
    for (unsigned y = 0; y < hb; ++y)
        for (unsigned x = 0; x < wb; ++x) {
            std::array<std::uint8_t, 16> block{};
            for (unsigned k = 0; k < bpb; ++k) {
                const auto xx = x * bpb + k;
                const auto offset = std::size_t(y / (8 * bh)) * (bh * 512 * gobs) +
                                    (xx / 64) * (bh * 512) + ((y / 8) % bh) * 512 +
                                    ((xx % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
                                    ((xx % 32) / 16) * 32 + (y % 2) * 16 + xx % 16;
                block[k] = At<std::uint8_t>(data, offset);
            }
            std::array<std::array<unsigned, 8>, 2> palette{};
            std::array<std::uint64_t, 2> indices{};
            for (unsigned c = 0; c < (two_channels ? 2u : 1u); ++c) {
                const auto *b = block.data() + c * 8;
                auto &p = palette[c];
                p[0] = b[0];
                p[1] = b[1];
                if (p[0] > p[1]) {
                    for (unsigned n = 1; n <= 6; ++n)
                        p[n + 1] = ((7 - n) * p[0] + n * p[1] + 3) / 7;
                } else {
                    for (unsigned n = 1; n <= 4; ++n)
                        p[n + 1] = ((5 - n) * p[0] + n * p[1] + 2) / 5;
                    p[6] = 0;
                    p[7] = 255;
                }
                for (unsigned n = 0; n < 6; ++n)
                    indices[c] |= std::uint64_t(b[n + 2]) << (n * 8);
            }
            for (unsigned n = 0; n < 16; ++n) {
                const auto px = x * 4 + n % 4, py = y * 4 + n / 4;
                if (px >= width || py >= height)
                    continue;
                auto *p = out.rgba.data() + (std::size_t(py) * width + px) * 4;
                p[0] = palette[0][(indices[0] >> (n * 3)) & 7];
                p[1] = two_channels ? palette[1][(indices[1] >> (n * 3)) & 7] : p[0];
                p[2] = two_channels ? 0 : p[0];
                p[3] = 255;
            }
        }
    return out;
}
Bytes Stream(Span b, unsigned expected) {
    Require(b.size() >= 48 && U32(b, 0) == 0x31636278, "XCX missing model xbc1");
    const auto type = U32(b, 4), usize = U32(b, 8), csize = U32(b, 12);
    Require(usize && usize <= MaxGeometry && usize == expected, "XCX model stream size");
    const auto compressed = Range(b, 48, csize);
    Bytes out;
    if (type == 1) {
        Require(csize >= 6 && ((unsigned(compressed[0]) << 8 | compressed[1]) % 31) == 0 &&
                    (compressed[0] & 15) == 8 && !(compressed[1] & 32),
                "XCX model zlib header");
        Require(dread_mapgen::Inflate(compressed.subspan(2, csize - 6), out, usize) &&
                    out.size() == usize,
                "XCX model inflate failed");
        std::uint32_t a = 1, c = 0;
        for (const auto v : out) {
            a = (a + v) % 65521;
            c = (c + a) % 65521;
        }
        const auto p = csize - 4;
        const auto adler = (unsigned(compressed[p]) << 24) | (unsigned(compressed[p + 1]) << 16) |
                           (unsigned(compressed[p + 2]) << 8) | compressed[p + 3];
        Require((c << 16 | a) == adler, "XCX model zlib checksum");
    } else if (type == 3) {
        out.resize(usize);
        const auto n =
            ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
        Require(!ZSTD_isError(n) && n == usize, "XCX model zstd failed");
    } else if (type == 0) {
        Require(csize == usize, "XCX model raw stream size");
        out.assign(compressed.begin(), compressed.end());
    } else
        throw std::runtime_error("XCX model unsupported compression");
    return out;
}
} // namespace
Bytes DecodeModelStream(Span bytes, unsigned expected_size) {
    const auto payload_end = std::uint64_t(48) + U32(bytes, 12);
    Range(bytes, 0, payload_end);
    // Native streams allow only zero padding to16 bytes, never an arbitrary tail.
    Require(
        payload_end == bytes.size() ||
            (bytes.size() == ((payload_end + 15) & ~std::uint64_t(15)) &&
             std::all_of(bytes.begin() + payload_end, bytes.end(), [](auto b) { return b == 0; })),
        "XCX invalid model stream padding");
    return Stream(bytes, expected_size);
}
FrameModel OwnedFrameModel(Archive &archive, const EdenDsmodHostApi &host, Span record) {
    Require(record.size() == 0x400, "XCX owned model record size");
    Require(record[0x3f4] == 4 || record[0x3f4] == 6 || record[0x3f4] == 7,
            "XCX owned model record mode");
    return FrameModelForRow(archive, host, U16(record, 0x22c));
}
FrameModel FrameModelForRow(Archive &archive, const EdenDsmodHostApi &host, unsigned frame_row) {
    Require(frame_row <= 0xffff, "XCX model frame row out of range");
    FrameModel out;
    out.frame = static_cast<std::uint16_t>(frame_row);
    const auto bytes = archive.Read(host, "/bdat/common.bdat");
    const Span b = bytes;
    Require(U32(b, 0) == 0x54414442 && U32(b, 4) == 0x01001004 && U32(b, 8) > 130 &&
                U32(b, 12) == b.size(),
            "XCX model BDAT header");
    const auto t = U32(b, 16 + 130 * 4);
    Require(U32(b, t) == 0x54414442 && U32(b, t + 4) == 0x3004, "XCX model table header");
    const auto cols = U32(b, t + 8), rows = U32(b, t + 12), base = U32(b, t + 16);
    Require(cols > 7 && cols <= 128 && out.frame >= base && out.frame - base < rows,
            "XCX model frame row out of range");
    const std::size_t co = std::uint64_t(t) + U32(b, t + 24),
                      ro = std::uint64_t(t) + U32(b, t + 32),
                      so = std::uint64_t(t) + U32(b, t + 40);
    const auto stride = U32(b, t + 36), sl = U32(b, t + 44);
    const auto strings = Range(b, so, sl);
    Require(U32(strings, 1) == 0x103e9bdc, "XCX wrong model table identity");
    Array(b, co, cols, 3, 128);
    Array(b, ro, rows, stride, 65536);
    constexpr unsigned widths[]{0, 1, 2, 4, 1, 2, 4, 4, 4, 4, 1, 4, 4, 2};
    unsigned offset = 0;
    for (unsigned col = 0; col < cols; ++col) {
        const auto type = b[co + col * 3];
        Require(type > 0 && type < std::size(widths), "XCX model column type");
        const auto hash = U32(strings, U16(b, co + col * 3 + 1));
        const auto at = ro + std::uint64_t(out.frame - base) * stride + offset;
        if (col == 1) {
            Require(hash == 0x25efa387 && type == 2, "XCX model Name schema");
            out.name_id = U16(b, at);
        } else if (col == 7) {
            Require(hash == 0x5ba4bc44 && type == 7, "XCX model Resource schema");
            const auto p = U32(b, at);
            const auto text = Range(strings, p, 9);
            Require(text[8] == 0, "XCX model resource terminator");
            out.resource.assign(reinterpret_cast<const char *>(text.data()), 8);
            Require(Resource(out.resource), "XCX unsupported model resource");
        } else if (col == 10 && hash == 0x23797629 && type == 7) {
            const auto p = U32(b, at);
            if (p + 9 <= strings.size() && strings[p + 8] == 0) {
                std::string motion(reinterpret_cast<const char *>(strings.data() + p), 8);
                if (Resource(motion))
                    out.motion = std::move(motion);
            }
        }
        offset += widths[type];
    }
    Require(offset == stride, "XCX model row stride mismatch");
    return out;
}
FrameGeometry ReadFrameGeometry(Archive &archive, const EdenDsmodHostApi &host,
                                const FrameModel &frame) {
    Require(Resource(frame.resource), "XCX unsafe model resource path");
    const auto stem = "/chr/dl/" + frame.resource;
    const auto wimdo = archive.Read(host, stem + ".wimdo");
    const auto wismt = archive.Read(host, stem + ".wismt");
    return DecodeFrameGeometry(wimdo, wismt);
}
Bytes FrameTextureReader::ReadSource(const EdenDsmodHostApi &host, std::string_view path) {
    Require(host.read_romfs != nullptr, "XCX texture host unavailable");
    if (shared) {
        Bytes bytes = shared->ReadStored(host, path);
        Require(!bytes.empty() && bytes.size() <= MaxGeometry, "XCX missing or oversized model texture");
        if (bytes.size() >= 4 && U32(bytes, 0) == 0x31636278)
            return DecodeModelStream(bytes, U32(bytes, 8));
        return bytes;
    }
    const auto read = [&](const char *file, std::uint64_t offset, void *out, std::size_t size) {
        Require(host.read_romfs(host.userdata, file, offset, out, size) == size,
                "XCX texture archive read failed");
    };
    if (!initialized) {
        std::array<std::uint8_t, 16> header;
        read("/sts.arh", 0, header.data(), header.size());
        const auto count = U32(header, 4);
        Require(U32(header, 0) == 0x32687261 && count && count <= 200000 && U32(header, 8) == 16,
                "XCX texture archive index");
        Bytes index(std::size_t(count) * 16);
        read("/sts.arh", 16, index.data(), index.size());
        std::map<std::uint64_t, Entry> parsed;
        std::uint64_t offset = 0;
        for (unsigned n = 0; n < count; ++n) {
            offset = (offset + 15) & ~std::uint64_t(15);
            const auto size = U32(index, n * 16 + 8);
            Require(offset <= 64ULL * 1024 * 1024 * 1024, "XCX texture archive offset");
            parsed.emplace(At<std::uint64_t>(index, n * 16), Entry{offset, size});
            offset += size;
        }
        entries = std::move(parsed);
        initialized = true;
    }
    const auto it = entries.find(PathHash(path));
    Require(it != entries.end() && it->second.size && it->second.size <= MaxGeometry,
            "XCX missing or oversized model texture");
    Bytes bytes(it->second.size);
    read("/sts.ard", it->second.offset, bytes.data(), bytes.size());
    if (bytes.size() >= 4 && U32(bytes, 0) == 0x31636278) {
        return DecodeModelStream(bytes, U32(bytes, 8));
    }
    return bytes;
}
Image DecodeModelTexture(Span mibl, Span base_mip, std::uint64_t remaining_texels) {
    Require(mibl.size() >= 40, "XCX missing model texture footer");
    const auto footer = mibl.size() - 40;
    Require(U32(mibl, footer) && U32(mibl, footer) <= MaxGeometry && U32(mibl, footer) % 4096 == 0,
            "XCX model texture image size");
    Require(U32(mibl, footer + 36) == 0x4d49424c && U32(mibl, footer + 32) == 10001 &&
                U32(mibl, footer + 16) == 1 && U32(mibl, footer + 20) == 1,
            "XCX unsupported model texture surface");
    auto width = U32(mibl, footer + 8), height = U32(mibl, footer + 12);
    const auto format = U32(mibl, footer + 24), mipmaps = U32(mibl, footer + 28);
    Require(width && height && width <= 4096 && height <= 4096 && mipmaps && mipmaps <= 16,
            "XCX model texture dimensions");
    Require(format == 1 || format == 37 || format == 66 || format == 67 || format == 68 ||
                format == 73 || format == 75 || format == 77,
            "XCX unsupported model texture format");
    if (!base_mip.empty()) {
        width *= 2;
        height *= 2;
    }
    Require(std::uint64_t(width) * height <=
                std::min<std::uint64_t>(4 * 1024 * 1024, remaining_texels),
            "XCX model texture pixel budget");
    // Only top mip, layer0. The external base mip is twice the MIBL dimensions.
    const auto source = base_mip.empty() ? mibl.first(footer) : base_mip;
    if (format == 73 || format == 75)
        return ModelChannels(source, width, height, format == 75);
    return DecodeTexture(source, Texture{0, source.size(), width, height, format});
}
Image FrameTextureReader::Read(const EdenDsmodHostApi &host, const FrameGeometry &geometry,
                               unsigned index) {
    Require(index < geometry.texture_sources.size(), "XCX model texture index");
    const auto &source = geometry.texture_sources[index];
    Bytes middle, base;
    Span mibl;
    if (source.mip_path.empty()) {
        Require(source.base_mip_path.empty(), "XCX model base without mip source");
        mibl = Range(geometry.low_texture_data, source.low_offset, source.low_size);
    } else {
        Require(source.name.size() == 8 &&
                    source.name.find_first_not_of("0123456789abcdef") == std::string::npos,
                "XCX unsafe shared texture name");
        const auto stem = "/chr/cmntex/" + source.name.substr(0, 2) + "/" + source.name;
        Require(source.mip_path == stem + "_m.wismt" &&
                    (source.base_mip_path.empty() || source.base_mip_path == stem + "_h.wismt"),
                "XCX unsafe shared texture path");
        middle = ReadSource(host, source.mip_path);
        mibl = middle;
        if (!source.base_mip_path.empty())
            base = ReadSource(host, source.base_mip_path);
    }
    constexpr std::uint64_t BatchTexels = 24 * 1024 * 1024;
    auto image = DecodeModelTexture(mibl, base, BatchTexels - decoded_texels);
    decoded_texels += std::uint64_t(image.w) * image.h;
    return image;
}
namespace {
// Skeleton (model-owned bone table) and the weight-row buffers. Bone records are 184 bytes:
// name offset (relative to the table), parent, 3 descendant/unknown ints, translation, euler,
// scale, inverse bind (row-vector layout) and a transform. The bind world position comes from
// the inverse bind matrix: p_local = p_world * S + t, so p_world = -t * S^-1.
void ReadSkin(FrameGeometry &out, Span md, unsigned models, Span vd, Span vb, unsigned vcount) {
    constexpr unsigned BoneSize = 184;
    const auto bone_count = U32(md, models + 84);
    Require(bone_count <= 512, "XCX excessive bone count");
    const std::uint64_t base = std::uint64_t(models) + U32(md, models + 80);
    const auto records = Array(md, base, bone_count, BoneSize, 512);
    for (unsigned n = 0; n < bone_count; ++n) {
        const auto p = n * BoneSize;
        ModelBone bone;
        bone.name = Name(md, base + U32(records, p));
        const auto parent = std::int32_t(U32(records, p + 4));
        Require(parent >= -1 && parent < int(bone_count) && parent != int(n),
                "XCX bone parent out of range");
        bone.parent = parent;
        float s[4][4];
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c) {
                s[r][c] = At<float>(records, p + 56 + (r * 4 + c) * 4);
                Require(std::isfinite(s[r][c]) && std::abs(s[r][c]) < 1e6f,
                        "XCX invalid inverse bind matrix");
            }
        // inverse of the upper 3x3 (cofactors), then p_world = -(t * S^-1)
        const double a = s[0][0], b = s[0][1], c = s[0][2], d = s[1][0], e = s[1][1], f = s[1][2],
                     g = s[2][0], h = s[2][1], i = s[2][2];
        const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if (std::abs(det) > 1e-9) {
            const double inv[3][3] = {{(e * i - f * h) / det, (c * h - b * i) / det,
                                       (b * f - c * e) / det},
                                      {(f * g - d * i) / det, (a * i - c * g) / det,
                                       (c * d - a * f) / det},
                                      {(d * h - e * g) / det, (b * g - a * h) / det,
                                       (a * e - b * d) / det}};
            for (unsigned k = 0; k < 3; ++k)
                bone.bind_position[k] = float(-(s[3][0] * inv[0][k] + s[3][1] * inv[1][k] +
                                                s[3][2] * inv[2][k]));
            for (unsigned r = 0; r < 3; ++r)
                for (unsigned q = 0; q < 3; ++q)
                    bone.rotation[r * 3 + q] = float(inv[q][r]);
        }
        out.skin.bones.push_back(std::move(bone));
    }
    // A parent chain must terminate (no cycles) within the table.
    for (unsigned n = 0; n < bone_count; ++n) {
        int at = int(n), guard = 0;
        while (at >= 0 && guard++ <= int(bone_count))
            at = out.skin.bones[at].parent;
        Require(at < 0, "XCX bone hierarchy cycle");
    }
    const auto name_count = U32(md, models + 104);
    Require(name_count <= 512, "XCX excessive skin bone names");
    const std::uint64_t names = std::uint64_t(models) + U32(md, models + 100);
    const auto offsets = Array(md, names, name_count, 4, 512);
    for (unsigned n = 0; n < name_count; ++n)
        out.skin.skin_names.push_back(Name(md, names + U32(offsets, n * 4)));
    // Weight buffers: vertex buffers made of float3 SkinWeights2 (1) + uint8x4 BoneIndices2 (2).
    for (unsigned buffer = 0; buffer < vcount; ++buffer) {
        const auto p = buffer * 24;
        const auto attrs = Array(vd, U32(vb, p + 12), U32(vb, p + 16), 4, 64);
        unsigned offset = 0, weight_at = ~0u, bone_at = ~0u;
        for (std::size_t a = 0; a < attrs.size(); a += 4) {
            const auto type = U16(attrs, a), size = U16(attrs, a + 2);
            if (type == 1 && size == 12)
                weight_at = offset;
            else if (type == 2 && size == 4)
                bone_at = offset;
            offset += size;
        }
        if (weight_at == ~0u || bone_at == ~0u)
            continue;
        const auto count = U32(vb, p + 4), stride = U32(vb, p + 8);
        Require(count && count <= 65536 && stride && stride <= 256 && offset == stride &&
                    weight_at + 12 <= stride && bone_at + 4 <= stride,
                "XCX weight buffer layout");
        const auto data = Array(vd, U32(vb, p), count, stride, 65536);
        std::vector<WeightRow> rows(count);
        for (unsigned n = 0; n < count; ++n) {
            auto &row = rows[n];
            float sum = 0;
            for (unsigned k = 0; k < 3; ++k) {
                row.weight[k] = At<float>(data, std::size_t(n) * stride + weight_at + k * 4);
                Require(std::isfinite(row.weight[k]) && row.weight[k] > -0.01f &&
                            row.weight[k] < 1.01f,
                        "XCX invalid skin weight");
                sum += row.weight[k];
            }
            row.weight[3] = std::max(0.f, 1.f - sum);
            for (unsigned k = 0; k < 4; ++k) {
                row.bone[k] = At<std::uint8_t>(data, std::size_t(n) * stride + bone_at + k);
                Require(row.bone[k] < name_count || row.weight[k] <= 0,
                        "XCX skin bone index out of range");
            }
        }
        out.skin.weight_buffers.push_back(std::move(rows));
        out.skin.weight_buffer_vertex_buffer.push_back(buffer);
    }
}
} // namespace
FrameGeometry ReadWeaponGeometry(Archive &archive, const EdenDsmodHostApi &host,
                                 std::string_view name) {
    // wdNNNNNNN[l|r]: Skell weapon models (RSC_DlWpnList); digits and one side letter only.
    const bool ok = name.size() >= 9 && name.size() <= 11 && name.starts_with("wd") &&
                    (name.back() == 'l' || name.back() == 'r') &&
                    name.substr(2, name.size() - 3).find_first_not_of("0123456789") ==
                        std::string_view::npos;
    if (!ok)
        throw std::runtime_error("XCX unsafe weapon resource path");
    const auto stem = "/chr/wd/" + std::string(name);
    const auto wimdo = archive.Read(host, stem + ".wimdo");
    const auto wismt = archive.Read(host, stem + ".wismt");
    return DecodeFrameGeometry(wimdo, wismt);
}
FrameGeometry DecodeFrameGeometry(Span md, Span mt) {
    Require(U32(md, 0) == 0x4d584d44 && U32(md, 4) == 10040, "XCX model version");
    Require(U32(md, 20) == 0, "XCX embedded geometry unsupported");
    Require(U32(mt, 0) == 0x4d535244 && U32(mt, 4) == 10001, "XCX stream version");
    const auto s = U32(mt, 12);
    Require(U32(mt, s) == 4097, "XCX streaming schema");
    const auto ec = U32(mt, s + 8), sc = U32(mt, s + 16), ei = U32(mt, s + 24);
    const auto entries = Array(mt, std::uint64_t(s) + U32(mt, s + 12), ec, 20, 4096);
    const auto streams = Array(mt, std::uint64_t(s) + U32(mt, s + 20), sc, 12, 256);
    Require(sc && ei < ec && U16(entries, ei * 20 + 10) == 0, "XCX missing vertex entry");
    // The vertex entry belongs to stream0. The base-mip selector applies only to textures.
    const auto packed = Range(mt, U32(streams, 8), U32(streams, 0));
    const auto decompressed = DecodeModelStream(packed, U32(streams, 4));
    const auto vd = Range(decompressed, U32(entries, ei * 20), U32(entries, ei * 20 + 4));
    const auto vcount = U32(vd, 4), icount = U32(vd, 12);
    const auto vb = Array(vd, U32(vd, 0), vcount, 24, 256);
    const auto ib = Array(vd, U32(vd, 8), icount, 12, 4096);
    FrameGeometry out;
    Materials(out, md, mt, s, entries, decompressed);
    out.vertices.resize(vcount);
    out.indices.resize(icount);
    const auto models = U32(md, 8);
    const auto mc = U32(md, models + 28);
    const auto model_array = Array(md, std::uint64_t(models) + U32(md, models + 24), mc, 68, 16);
    Require(mc > 0, "XCX model has no meshes");
    for (unsigned m = 0; m < mc; ++m) {
        const auto meshes = Array(md, std::uint64_t(models) + U32(model_array, m * 68),
                                  U32(model_array, m * 68 + 4), 64, 4096);
        for (std::size_t p = 0; p < meshes.size(); p += 64) {
            ModelMesh mesh{U32(meshes, p + 8), U32(meshes, p + 12), U32(meshes, p + 20),
                           U32(meshes, p + 4)};
            Require(mesh.vertex_buffer < vcount && mesh.index_buffer < icount,
                    "XCX mesh references missing buffer");
            Require(mesh.material < out.materials.size(), "XCX mesh references missing material");
            out.meshes.push_back(mesh);
        }
    }
    Require(!out.meshes.empty(), "XCX model has no draw calls");
    std::size_t total_vertices = 0, total_indices = 0;
    for (const auto &mesh : out.meshes) {
        auto &vertices = out.vertices[mesh.vertex_buffer];
        if (vertices.empty()) {
            const auto p = mesh.vertex_buffer * 24;
            const auto count = U32(vb, p + 4), stride = U32(vb, p + 8);
            Require(count && stride && stride <= 256, "XCX model vertex stride");
            Require(count <= 1000000 - total_vertices, "XCX total vertex budget");
            total_vertices += count;
            const auto data = Array(vd, U32(vb, p), count, stride, 1000000);
            const auto attrs = Array(vd, U32(vb, p + 12), U32(vb, p + 16), 4, 64);
            unsigned offset = 0, required = 0;
            bool seen_weight_index = false;
            vertices.resize(count);
            for (std::size_t a = 0; a < attrs.size(); a += 4) {
                const auto type = U16(attrs, a), size = U16(attrs, a + 2);
                Require(offset + size <= stride, "XCX vertex attribute exceeds stride");
                if (type == 3) { // WeightIndex: uint16x2, column 0 selects the weight-buffer row
                    Require(size == 4 && !(seen_weight_index), "XCX weight index layout");
                    seen_weight_index = true;
                    for (unsigned n = 0; n < count; ++n)
                        vertices[n].weight_index = U16(data, std::size_t(n) * stride + offset);
                } else if (type == 0 || type == 5 || type == 28 || type == 29) {
                    Require(size == (type == 0   ? 12u
                                     : type == 5 ? 8u
                                                 : 4u),
                            "XCX vertex attribute layout");
                    const unsigned bit = type == 0 ? 1 : type == 5 ? 2 : type == 28 ? 4 : 8;
                    Require(!(required & bit), "XCX duplicate vertex attribute");
                    required |= bit;
                    for (unsigned n = 0; n < count; ++n) {
                        auto &v = vertices[n];
                        const auto at = std::size_t(n) * stride + offset;
                        if (type == 0 || type == 5) {
                            for (unsigned k = 0; k < (type == 0 ? 3u : 2u); ++k) {
                                const auto f = At<float>(data, at + k * 4);
                                Require(std::isfinite(f) && std::abs(f) <= 100000,
                                        "XCX invalid model coordinate");
                                if (type == 0)
                                    v.position[k] = f;
                                else
                                    v.uv[k] = f;
                            }
                        } else {
                            auto &target = type == 28 ? v.normal : v.tangent;
                            for (unsigned k = 0; k < 4; ++k)
                                target[k] = std::max(-1.f, At<std::int8_t>(data, at + k) / 127.f);
                        }
                    }
                }
                offset += size;
            }
            Require(offset == stride && required == 15, "XCX missing vertex inputs");
        }
        auto &indices = out.indices[mesh.index_buffer];
        if (indices.empty()) {
            const auto p = mesh.index_buffer * 12;
            const auto count = U32(ib, p + 4);
            Require(count && count % 3 == 0 && U16(ib, p + 8) == 0 && U16(ib, p + 10) == 0,
                    "XCX unsupported triangle index format");
            Require(count <= 3000000 - total_indices, "XCX total index budget");
            total_indices += count;
            const auto data = Array(vd, U32(ib, p), count, 2, 3000000);
            indices.resize(count);
            for (unsigned n = 0; n < count; ++n)
                indices[n] = U16(data, n * 2);
        }
        Require(std::all_of(indices.begin(), indices.end(),
                            [&](unsigned n) { return n < vertices.size(); }),
                "XCX triangle index exceeds vertices");
    }
    ReadSkin(out, md, models, vd, vb, vcount);
    return out;
}
} // namespace xcx
