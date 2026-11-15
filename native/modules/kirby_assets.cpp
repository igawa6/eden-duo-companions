// SPDX-License-Identifier: GPL-3.0-or-later
// SARC, BNTX, deswizzle and BC decode adapted from eden-duo-companions
// native/modules/wonder_assets.cpp (Eden DSMod fork); .cmp (zstd / LZ4) is Kirby's own wrapper.

#include "kirby_assets.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

#include <bc_decoder.h>
#include <lz4frame.h>
#include <zstd.h>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace KirbyAssets {
namespace {
using namespace dsmod_sdk::int_types;
constexpr std::size_t MaxRaw = 96 * 1024 * 1024;
constexpr u32 MaxDimension = 2048;
constexpr u64 MaxPixels = 4 * 1024 * 1024;
constexpr std::size_t CachedArchives = 3;

bool Fits(std::span<const u8> s, u64 o, u64 n) {
    return o <= s.size() && n <= s.size() - o;
}
u16 U16(std::span<const u8> s, u64 o) {
    return Fits(s, o, 2) ? dsmod_sdk::Le16(s.data() + o) : 0;
}
u32 U32(std::span<const u8> s, u64 o) {
    return Fits(s, o, 4) ? dsmod_sdk::Le32(s.data() + o) : 0;
}
u64 U64(std::span<const u8> s, u64 o) {
    return Fits(s, o, 8) ? dsmod_sdk::Le64(s.data() + o) : 0;
}
bool Magic(std::span<const u8> s, std::string_view m) {
    return s.size() >= m.size() && std::memcmp(s.data(), m.data(), m.size()) == 0;
}
u16 EU16(std::span<const u8> s, u64 o, bool be) {
    if (!Fits(s, o, 2))
        return 0;
    return be ? static_cast<u16>(s[o] << 8 | s[o + 1]) : U16(s, o);
}
u32 EU32(std::span<const u8> s, u64 o, bool be) {
    if (!Fits(s, o, 4))
        return 0;
    return be ? dsmod_sdk::Be32(s.data() + o) : U32(s, o);
}

std::optional<Bytes> Lz4Frame(std::span<const u8> in, std::size_t raw_size) {
    LZ4F_dctx* ctx{};
    if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION)))
        return std::nullopt;
    Bytes out(raw_size);
    std::size_t in_at = 0, out_at = 0, hint = 1;
    while (hint != 0 && in_at < in.size() && out_at <= out.size()) {
        std::size_t src = in.size() - in_at, dst = out.size() - out_at;
        hint = LZ4F_decompress(ctx, out.data() + out_at, &dst, in.data() + in_at, &src, nullptr);
        if (LZ4F_isError(hint) || (src == 0 && dst == 0))
            break;
        in_at += src;
        out_at += dst;
    }
    LZ4F_freeDecompressionContext(ctx);
    if (hint != 0 || out_at != out.size())
        return std::nullopt;
    return out;
}

struct Format {
    u32 bw, bh, bpb, channels;
};
std::optional<Format> GetFormat(u32 f) {
    switch (f >> 8) {
    case 2:
        return Format{1, 1, 1, 1};
    case 9:
        return Format{1, 1, 2, 2};
    case 11:
    case 12:
        return Format{1, 1, 4, 4};
    case 26:
        return Format{4, 4, 8, 4};
    case 27:
    case 28:
        return Format{4, 4, 16, 4};
    case 29:
        return Format{4, 4, 8, 1};
    case 30:
        return Format{4, 4, 16, 2};
    case 32:
        return Format{4, 4, 16, 4};
    default:
        return std::nullopt;
    }
}

u32 Pdep(u32 value, u32 mask) {
    u32 result = 0;
    for (u32 bit = 1; mask; bit += bit) {
        if (value & bit)
            result |= mask & (~mask + 1);
        mask &= mask - 1;
    }
    return result;
}

std::optional<Bytes> Unswizzle2d(std::span<const u8> source, u32 bpp, u32 width, u32 height,
                                 u32 block_height) {
    if (!bpp || block_height > 5)
        return std::nullopt;
    const u64 row_bytes = static_cast<u64>(width) * bpp;
    const u64 aligned_width = (row_bytes + 63) & ~UINT64_C(63);
    const u64 aligned_height = (static_cast<u64>(height) + (UINT64_C(8) << block_height) - 1) &
                               ~((UINT64_C(8) << block_height) - 1);
    if (aligned_width * aligned_height > source.size() || row_bytes * height > MaxRaw)
        return std::nullopt;
    Bytes output(static_cast<std::size_t>(row_bytes * height));
    const u32 stride = static_cast<u32>(aligned_width);
    const u32 gobs_x = stride >> 6;
    const u32 block_size = gobs_x << (9 + block_height);
    const u32 height_mask = (1u << block_height) - 1;
    for (u32 y = 0; y < height; ++y) {
        const u32 swizzled_y = Pdep(y, 0b011010000);
        const u32 block_y = y >> 3;
        const u32 offset_y =
            (block_y >> block_height) * block_size + ((block_y & height_mask) << 9);
        for (u32 x = 0; x < width; ++x) {
            const u32 byte_x = x * bpp;
            const u64 offset = static_cast<u64>(offset_y) +
                               (static_cast<u64>(byte_x >> 6) << (9 + block_height)) +
                               (Pdep(byte_x, 0b100101111) | swizzled_y);
            if (offset + bpp > source.size())
                return std::nullopt;
            std::memcpy(output.data() + static_cast<std::size_t>(y) * row_bytes + byte_x,
                        source.data() + offset, bpp);
        }
    }
    return output;
}

struct Texture {
    u32 w, h, format, size, arrays, layout, cmap;
    u16 tile;
    u64 image;
    std::string name;
};

std::span<const u8> BntxView(std::span<const u8> source) {
    if (!Magic(source, "BNTX") || source.size() < 0x30)
        return {};
    const u32 declared = U32(source, 0x1c);
    return declared >= 0x30 && declared <= source.size() ? source.first(declared) : source;
}

std::vector<Texture> Textures(std::span<const u8> b) {
    std::vector<Texture> out;
    if (b.empty())
        return out;
    const u32 count = U32(b, 0x24);
    const u64 array = U64(b, 0x28);
    if (!count || count > 4096 || !Fits(b, array, u64{count} * 8))
        return out;
    for (u32 i = 0; i < count; ++i) {
        const u64 o = U64(b, array + u64{i} * 8);
        if (!Fits(b, o, 0xa0) || !Magic(b.subspan(o), "BRTI"))
            continue;
        Texture t{U32(b, o + 0x24), U32(b, o + 0x28), U32(b, o + 0x1c), U32(b, o + 0x50),
                  U32(b, o + 0x30), U32(b, o + 0x34), U32(b, o + 0x58), U16(b, o + 0x12),
                  0,                {}};
        const u64 mip = U64(b, o + 0x70);
        if (!Fits(b, mip, 8))
            continue;
        t.image = U64(b, mip);
        const u64 no = U64(b, o + 0x60);
        if (Fits(b, no, 2)) {
            const u16 nl = U16(b, no);
            if (Fits(b, no + 2, nl))
                t.name.assign(reinterpret_cast<const char*>(b.data() + no + 2), nl);
        }
        if (!t.w || !t.h || t.w > MaxDimension || t.h > MaxDimension ||
            static_cast<u64>(t.w) * t.h > MaxPixels || !t.arrays || t.image >= b.size())
            continue;
        out.push_back(std::move(t));
    }
    return out;
}

std::optional<Image> DecodeTexture(std::span<const u8> b, const Texture& t, u32 layer = 0) {
    const auto f = GetFormat(t.format);
    if (!f)
        return std::nullopt;
    const u32 bx = (t.w + f->bw - 1) / f->bw, by = (t.h + f->bh - 1) / f->bh;
    const std::size_t linear_size = static_cast<std::size_t>(bx) * by * f->bpb;
    const std::size_t array_size = t.size / t.arrays;
    if (layer >= t.arrays || !Fits(b, t.image + array_size * layer, array_size))
        return std::nullopt;
    const auto src = b.subspan(t.image + array_size * layer, array_size);
    Bytes blocks(linear_size);
    if (t.tile == 0) {
        auto linear = Unswizzle2d(src, f->bpb, bx, by, t.layout & 7);
        if (!linear)
            return std::nullopt;
        blocks = std::move(*linear);
    } else if (t.tile == 1) {
        const std::size_t row = static_cast<std::size_t>(bx) * f->bpb;
        std::size_t pitch = (row + 31) / 32 * 32;
        if (pitch * by > src.size())
            pitch = row;
        if (pitch * by > src.size())
            return std::nullopt;
        for (u32 y = 0; y < by; ++y)
            std::memcpy(blocks.data() + y * row, src.data() + y * pitch, row);
    } else {
        return std::nullopt;
    }
    Bytes channels(static_cast<std::size_t>(t.w) * t.h * f->channels);
    const u32 fc = t.format >> 8;
    if (fc == 2 || fc == 9 || fc == 11) {
        if (blocks.size() < channels.size())
            return std::nullopt;
        std::copy_n(blocks.begin(), channels.size(), channels.begin());
    } else if (fc == 12) {
        if (blocks.size() < channels.size())
            return std::nullopt;
        for (std::size_t p = 0; p < channels.size(); p += 4) {
            channels[p] = blocks[p + 2];
            channels[p + 1] = blocks[p + 1];
            channels[p + 2] = blocks[p];
            channels[p + 3] = blocks[p + 3];
        }
    } else {
        for (u32 y = 0; y < by; ++y)
            for (u32 x = 0; x < bx; ++x) {
                const u8* s = blocks.data() + (static_cast<std::size_t>(y) * bx + x) * f->bpb;
                u8* d = channels.data() +
                        (static_cast<std::size_t>(y) * 4 * t.w + x * 4) * f->channels;
                switch (fc) {
                case 26:
                    bcn::DecodeBc1(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                case 27:
                    bcn::DecodeBc2(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                case 28:
                    bcn::DecodeBc3(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                case 29:
                    bcn::DecodeBc4(s, d, x * 4, y * 4, t.w, t.h, (t.format & 255) == 2);
                    break;
                case 30:
                    bcn::DecodeBc5(s, d, x * 4, y * 4, t.w, t.h, (t.format & 255) == 2);
                    break;
                case 32:
                    bcn::DecodeBc7(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                default:
                    return std::nullopt;
                }
            }
    }
    Image out{t.w, t.h, Bytes(static_cast<std::size_t>(t.w) * t.h * 4)};
    for (std::size_t p = 0; p < static_cast<std::size_t>(t.w) * t.h; ++p)
        for (u32 c = 0; c < 4; ++c) {
            const u8 sel = static_cast<u8>(t.cmap >> (c * 8));
            out.rgba[p * 4 + c] =
                sel == 0   ? 0
                : sel == 1 ? 255
                           : (sel - 2u < f->channels ? channels[p * f->channels + sel - 2] : 0);
        }
    return out;
}

bool SafeName(std::string_view s) {
    return !s.empty() && s.size() <= 96 && s.front() != '.' && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '^' || c == '-' || c == '.';
    });
}
} // namespace

std::optional<Bytes> DecompressCmp(std::span<const u8> in) {
    if (in.size() < 8)
        return std::nullopt;
    const u32 raw_size = U32(in, 0);
    const auto frame = in.subspan(4);
    if (raw_size == 0 || raw_size > MaxRaw)
        return std::nullopt;
    if (U32(frame, 0) == 0xFD2FB528u) {
        Bytes out(raw_size);
        const std::size_t got = ZSTD_decompress(out.data(), out.size(), frame.data(), frame.size());
        if (ZSTD_isError(got) || got != out.size())
            return std::nullopt;
        return out;
    }
    if (U32(frame, 0) == 0x184D2204u)
        return Lz4Frame(frame, raw_size);
    return std::nullopt;
}

std::optional<Bytes> SarcMember(std::span<const u8> s, std::string_view wanted) {
    if (s.size() < 0x20 || !Magic(s, "SARC"))
        return std::nullopt;
    const bool be = s[6] == 0xfe && s[7] == 0xff;
    const u32 hs = EU16(s, 4, be);
    const u32 data = EU32(s, 0xc, be);
    if (hs + 12 > s.size() || data > s.size() || !Magic(s.subspan(hs), "SFAT"))
        return std::nullopt;
    const u32 count = EU16(s, hs + 6, be);
    const u32 nodes = hs + 12;
    if (count > 8192 || nodes + count * 16 > s.size())
        return std::nullopt;
    const u32 sfnt = nodes + count * 16;
    if (sfnt + 8 > s.size() || !Magic(s.subspan(sfnt), "SFNT"))
        return std::nullopt;
    const u32 names = sfnt + EU16(s, sfnt + 4, be);
    for (u32 i = 0; i < count; ++i) {
        const u32 n = nodes + i * 16;
        const u32 attr = EU32(s, n + 4, be);
        const u32 begin = EU32(s, n + 8, be), end = EU32(s, n + 12, be);
        if (!(attr & 0x01000000))
            continue;
        const u64 no = names + u64{attr & 0xffffff} * 4;
        if (no >= s.size())
            continue;
        const auto zero = std::find(s.begin() + no, s.end(), 0);
        if (zero == s.end())
            continue;
        const std::string_view name(reinterpret_cast<const char*>(s.data() + no),
                                    static_cast<std::size_t>(zero - s.begin()) - no);
        if (name == wanted && begin <= end && u64{data} + end <= s.size())
            return Bytes(s.begin() + data + begin, s.begin() + data + end);
    }
    return std::nullopt;
}

std::optional<std::vector<MsbtRun>> MsbtRuns(std::span<const u8> s, std::string_view label, bool glyphs) {
    if (!Magic(s, "MsgStdBn") || s.size() < 0x20 || U16(s, 8) != 0xFEFF)
        return std::nullopt;
    std::span<const u8> lbl, txt;
    for (u64 at = 0x20, n = U16(s, 0xE); n > 0 && Fits(s, at, 0x10); --n) {
        const u64 size = U32(s, at + 4);
        if (!Fits(s, at + 0x10, size))
            return std::nullopt;
        const auto body = s.subspan(at + 0x10, size);
        if (Magic(s.subspan(at), "LBL1"))
            lbl = body;
        else if (Magic(s.subspan(at), "TXT2"))
            txt = body;
        at = (at + 0x10 + size + 15) & ~u64{15};
    }
    std::optional<u32> index;
    for (u32 g = 0, groups = U32(lbl, 0); g < groups && !index; ++g) {
        u64 off = U32(lbl, 4 + g * 8 + 4);
        for (u32 k = U32(lbl, 4 + g * 8); k > 0 && Fits(lbl, off, 1); --k) {
            const u8 len = lbl[off];
            if (!Fits(lbl, off + 1, len + 4u))
                return std::nullopt;
            if (std::string_view{reinterpret_cast<const char*>(lbl.data() + off + 1), len} == label) {
                index = U32(lbl, off + 1 + len);
                break;
            }
            off += 1 + len + 4;
        }
    }
    if (!index || *index >= U32(txt, 0))
        return std::nullopt;
    std::vector<MsbtRun> out(1);
    for (u64 at = U32(txt, 4 + u64{*index} * 4); Fits(txt, at, 2);) {
        const u16 c = U16(txt, at);
        at += 2;
        if (c == 0)
            break;
        if (c == 0x0E) { // control tag: group, type, argument bytes; 0.3 sets the colour (RGBA)
            const u16 group = U16(txt, at), type = U16(txt, at + 2), size = Fits(txt, at, 6) ? U16(txt, at + 4) : 0;
            if (group == 0 && type == 3 && size >= 4 && Fits(txt, at + 6, 4)) {
                const u32 rgb = u32{txt[at + 6]} << 16 | u32{txt[at + 7]} << 8 | txt[at + 8];
                out.push_back({{}, rgb, out.back().scale});
            } else if (glyphs && size >= 2 && Fits(txt, at + 6, 2)) {
                if (group == 0 && type == 2)
                    out.push_back({{}, out.back().colour, static_cast<float>(U16(txt, at + 6)) / 100});
                else if (group == 4)
                    out.back().text += static_cast<char32_t>(0xF000 | txt[at + 6]);
                else if (group == 7)
                    out.back().text += static_cast<char32_t>(0xF000 + (txt[at + 6] & 3));
            }
            at += 6 + size;
        } else if (c == 0x0F)
            at += 4;
        else if (c >= 0xD800 && c < 0xDC00 && Fits(txt, at, 2))
            out.back().text += static_cast<char32_t>(0x10000 + ((c - 0xD800) << 10) + (U16(txt, at) - 0xDC00)), at += 2;
        else
            out.back().text += static_cast<char32_t>(c);
    }
    std::erase_if(out, [](const MsbtRun& r) { return r.text.empty(); });
    return out;
}

namespace {
std::optional<YamlNode> YamlAt(std::span<const u8> s, u64 at, int depth) {
    const auto rel = [&](u64 field) -> std::optional<u64> {
        if (!Fits(s, field, 4))
            return std::nullopt;
        const auto to = static_cast<std::int64_t>(field) + static_cast<std::int32_t>(U32(s, field));
        return to >= 0 && static_cast<u64>(to) < s.size() ? std::optional<u64>{static_cast<u64>(to)} : std::nullopt;
    };
    const auto string = [&](u64 field) -> std::optional<std::string> {
        const auto to = rel(field);
        if (!to || !Fits(s, *to + 4, U32(s, *to)))
            return std::nullopt;
        return std::string{reinterpret_cast<const char*>(s.data() + *to + 4), U32(s, *to)};
    };
    if (depth > 32 || !Fits(s, at, 8))
        return std::nullopt;
    YamlNode n;
    switch (U32(s, at)) {
    case 1:
        n.kind = YamlNode::Kind::Int, n.number = static_cast<std::int32_t>(U32(s, at + 4));
        return n;
    case 2:
        n.kind = YamlNode::Kind::Float, n.real = std::bit_cast<float>(U32(s, at + 4));
        return n;
    case 3:
        n.kind = YamlNode::Kind::Bool, n.number = U32(s, at + 4) != 0;
        return n;
    case 4:
        if (auto t = string(at + 4))
            n.kind = YamlNode::Kind::String, n.text = std::move(*t);
        return n;
    case 5:
        n.kind = YamlNode::Kind::Map;
        for (u32 i = 0, count = U32(s, at + 4); i < count && i < 4096; ++i) {
            auto key = string(at + 8 + 8 * u64{i});
            const auto to = rel(at + 12 + 8 * u64{i});
            auto value = to ? YamlAt(s, *to, depth + 1) : std::nullopt;
            if (!key || !value)
                return std::nullopt;
            n.map.emplace_back(std::move(*key), std::move(*value));
        }
        return n;
    case 6:
        n.kind = YamlNode::Kind::List;
        for (u32 i = 0, count = U32(s, at + 4); i < count && i < 4096; ++i) {
            const auto to = rel(at + 8 + 4 * u64{i});
            auto value = to ? YamlAt(s, *to, depth + 1) : std::nullopt;
            if (!value)
                return std::nullopt;
            n.list.push_back(std::move(*value));
        }
        return n;
    default:
        return n;
    }
}
} // namespace

const YamlNode* YamlNode::Find(std::string_view key) const {
    for (const auto& [k, v] : map)
        if (k == key)
            return &v;
    return nullptr;
}

std::optional<YamlNode> ReadXbinYaml(std::span<const u8> s) {
    // "XBIN", byte order, version, size, encoding, (RLOC offset), then "YAML", its version and the root
    if (!Magic(s, "XBIN") || !Magic(s.subspan(std::min<std::size_t>(0x14, s.size())), "YAML"))
        return std::nullopt;
    return YamlAt(s, 0x1C, 0);
}

std::optional<std::array<float, 8>> BflanMaterialColour(std::span<const u8> s, std::string_view material) {
    if (!Magic(s, "FLAN"))
        return std::nullopt;
    std::array<float, 8> out;
    out.fill(-1);
    bool found = false;
    for (u64 at = U16(s, 6), n = U16(s, 0x10); n > 0 && Fits(s, at, 8); --n) {
        const u64 size = U32(s, at + 4);
        if (Magic(s.subspan(at), "pai1")) {
            const u64 entries = at + U32(s, at + 0x10);
            for (u32 e = 0, count = U16(s, at + 0xE); e < count; ++e) {
                const u64 eo = at + U32(s, entries + 4 * u64{e});
                if (!Fits(s, eo, 32))
                    break;
                const std::string_view name{reinterpret_cast<const char*>(s.data() + eo), strnlen(reinterpret_cast<const char*>(s.data() + eo), 28)};
                if (name != material)
                    continue;
                for (u32 t = 0, tags = s[eo + 28]; t < tags; ++t) {
                    const u64 to = eo + U32(s, eo + 32 + 4 * u64{t});
                    if (!Fits(s, to, 8) || !Magic(s.subspan(to), "FLMC"))
                        continue;
                    for (u32 c = 0, curves = s[to + 4]; c < curves; ++c) {
                        const u64 co = to + U32(s, to + 8 + 4 * u64{c});
                        if (!Fits(s, co, 0x10) || U16(s, co + 4) == 0)
                            continue;
                        const u32 target = s[co + 1];
                        if (target < out.size())
                            out[target] = std::bit_cast<float>(U32(s, co + 0x10)), found = true;
                    }
                }
            }
        }
        if (size == 0)
            break;
        at += size;
    }
    return found ? std::optional{out} : std::nullopt;
}

std::vector<Image> DecodeBntxAll(std::span<const u8> source) {
    std::vector<Image> out;
    const auto b = BntxView(source);
    for (const auto& t : Textures(b))
        for (u32 layer = 0; layer < t.arrays && layer < 64; ++layer)
            if (auto img = DecodeTexture(b, t, layer))
                out.push_back(std::move(*img));
    return out;
}

std::optional<std::u32string> MsbtFilled(std::span<const u8> s, std::string_view label,
                                         const std::function<std::u32string(std::uint16_t, std::uint16_t)>& fill) {
    // (the label's text found as MsbtRuns finds it)
    if (!Magic(s, "MsgStdBn") || s.size() < 0x20 || U16(s, 8) != 0xFEFF)
        return std::nullopt;
    std::span<const u8> lbl, txt;
    for (u64 at = 0x20, n = U16(s, 0xE); n > 0 && Fits(s, at, 0x10); --n) {
        const u64 size = U32(s, at + 4);
        if (!Fits(s, at + 0x10, size))
            return std::nullopt;
        const auto body = s.subspan(at + 0x10, size);
        if (Magic(s.subspan(at), "LBL1"))
            lbl = body;
        else if (Magic(s.subspan(at), "TXT2"))
            txt = body;
        at = (at + 0x10 + size + 15) & ~u64{15};
    }
    std::optional<u32> index;
    for (u32 g = 0, groups = U32(lbl, 0); g < groups && !index; ++g) {
        u64 off = U32(lbl, 4 + g * 8 + 4);
        for (u32 k = U32(lbl, 4 + g * 8); k > 0 && Fits(lbl, off, 1); --k) {
            const u8 len = lbl[off];
            if (!Fits(lbl, off + 1, len + 4u))
                return std::nullopt;
            if (std::string_view{reinterpret_cast<const char*>(lbl.data() + off + 1), len} == label) {
                index = U32(lbl, off + 1 + len);
                break;
            }
            off += 1 + len + 4;
        }
    }
    if (!index || *index >= U32(txt, 0))
        return std::nullopt;
    std::u32string out;
    for (u64 at = U32(txt, 4 + u64{*index} * 4); Fits(txt, at, 2);) {
        const u16 c = U16(txt, at);
        at += 2;
        if (c == 0)
            break;
        if (c == 0x0E) {
            const u16 group = U16(txt, at), type = U16(txt, at + 2), size = Fits(txt, at, 6) ? U16(txt, at + 4) : 0;
            out += fill(group, type);
            at += 6 + size;
        } else if (c == 0x0F)
            at += 4;
        else if (c >= 0xD800 && c < 0xDC00 && Fits(txt, at, 2))
            out += static_cast<char32_t>(0x10000 + ((c - 0xD800) << 10) + (U16(txt, at) - 0xDC00)), at += 2;
        else
            out += static_cast<char32_t>(c);
    }
    return out;
}

std::optional<std::u32string> MsbtText(std::span<const u8> s, std::string_view label) {
    const auto runs = MsbtRuns(s, label);
    if (!runs)
        return std::nullopt;
    std::u32string out;
    for (const auto& r : *runs)
        out += r.text;
    return out;
}

std::vector<std::string> MsbtLabels(std::span<const u8> s) {
    // (the label table walked as MsbtRuns walks it)
    std::vector<std::string> out;
    if (!Magic(s, "MsgStdBn") || s.size() < 0x20 || U16(s, 8) != 0xFEFF)
        return out;
    std::span<const u8> lbl;
    for (u64 at = 0x20, n = U16(s, 0xE); n > 0 && Fits(s, at, 0x10); --n) {
        const u64 size = U32(s, at + 4);
        if (!Fits(s, at + 0x10, size))
            return out;
        if (Magic(s.subspan(at), "LBL1"))
            lbl = s.subspan(at + 0x10, size);
        at = (at + 0x10 + size + 15) & ~u64{15};
    }
    for (u32 g = 0, groups = U32(lbl, 0); g < groups && Fits(lbl, 4 + u64{g} * 8, 8); ++g) {
        u64 off = U32(lbl, 4 + u64{g} * 8 + 4);
        for (u32 k = U32(lbl, 4 + u64{g} * 8); k > 0 && Fits(lbl, off, 1); --k) {
            const u8 len = lbl[off];
            if (!Fits(lbl, off + 1, len + 4u))
                return out;
            out.emplace_back(reinterpret_cast<const char*>(lbl.data() + off + 1), len);
            off += 1 + len + 4;
        }
    }
    return out;
}

std::optional<Image> DecodeBntx(std::span<const u8> source, std::string_view texture) {
    const auto b = BntxView(source);
    for (const auto& t : Textures(b))
        if (texture.empty() || t.name == texture)
            return DecodeTexture(b, t);
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> Decoder::Resolve(std::string_view key) {
    const bool model = key.starts_with("gfx/");
    if (!key.starts_with("lyt/") && !model)
        return std::nullopt;
    const auto rest = key.substr(4);
    const auto last = rest.rfind('/');
    if (last == std::string_view::npos)
        return std::nullopt;
    const auto archive = rest.substr(0, last);
    const auto texture = rest.substr(last + 1);
    if (!SafeName(texture))
        return std::nullopt;
    std::size_t at = 0;
    while (at <= archive.size()) {
        const auto slash = std::min(archive.find('/', at), archive.size());
        if (!SafeName(archive.substr(at, slash - at)))
            return std::nullopt;
        at = slash + 1;
    }
    return model ? std::pair{"/gfx/" + std::string{archive} + ".bfres.cmp", std::string{texture}}
                 : std::pair{"/lyt/" + std::string{archive} + ".arc.cmp", std::string{texture}};
}

struct Decoder::Impl {
    explicit Impl(std::size_t b) : budget(b) {}
    std::size_t budget, used{};
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const Image>> images;
    std::list<std::string> lru;
    std::list<std::pair<std::string, std::shared_ptr<const Bytes>>> archives;

    std::shared_ptr<const Bytes> Archive(const RomfsReader& read, const std::string& path) {
        {
            std::scoped_lock lock{mutex};
            for (auto it = archives.begin(); it != archives.end(); ++it)
                if (it->first == path) {
                    archives.splice(archives.begin(), archives, it);
                    return it->second;
                }
        }
        const auto raw = read(path);
        if (!raw)
            return nullptr;
        auto bytes = DecompressCmp(*raw);
        if (!bytes)
            return nullptr;
        auto shared = std::make_shared<const Bytes>(std::move(*bytes));
        std::scoped_lock lock{mutex};
        archives.emplace_front(path, shared);
        if (archives.size() > CachedArchives)
            archives.pop_back();
        return shared;
    }
};

Decoder::Decoder(std::size_t b) : impl(std::make_unique<Impl>(b)) {}
Decoder::~Decoder() = default;

std::shared_ptr<const Image> Decoder::Load(const RomfsReader& read, std::string_view key) {
    const std::string k{key};
    {
        std::scoped_lock lock{impl->mutex};
        if (const auto it = impl->images.find(k); it != impl->images.end()) {
            impl->lru.remove(k);
            impl->lru.push_front(k);
            return it->second;
        }
    }
    const auto where = Resolve(key);
    if (!where)
        return nullptr;
    const auto archive = impl->Archive(read, where->first);
    if (!archive)
        return nullptr;
    // a layout archive keeps its textures in timg/, a model (bfres) embeds one BNTX
    std::optional<Bytes> member;
    std::optional<std::span<const u8>> bntx;
    if (key.starts_with("gfx/")) {
        const std::string_view all{reinterpret_cast<const char*>(archive->data()), archive->size()};
        if (const auto at = all.find("BNTX"); at != std::string_view::npos)
            bntx = std::span<const u8>{*archive}.subspan(at);
    } else if ((member = SarcMember(*archive, "timg/__Combined.bntx"))) {
        bntx = std::span<const u8>{*member};
    }
    if (!bntx)
        return nullptr;
    auto image = DecodeBntx(*bntx, where->second);
    if (!image || image->rgba.empty())
        return nullptr;
    auto shared = std::make_shared<const Image>(std::move(*image));
    std::scoped_lock lock{impl->mutex};
    if (const auto it = impl->images.find(k); it != impl->images.end())
        return it->second;
    impl->images.emplace(k, shared);
    impl->lru.push_front(k);
    impl->used += shared->rgba.size();
    while (impl->used > impl->budget && impl->lru.size() > 1) {
        const auto old = impl->lru.back();
        impl->lru.pop_back();
        if (const auto it = impl->images.find(old); it != impl->images.end()) {
            impl->used -= it->second->rgba.size();
            impl->images.erase(it);
        }
    }
    return shared;
}

} // namespace KirbyAssets
