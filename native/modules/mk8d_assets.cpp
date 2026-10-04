// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Mario Kart 8 Deluxe romfs decoders (Yaz0, SARC, BNTX, MSBT, course_mapcamera.bin) and the
// per-instance Library cache. All parsing is
// bounds-checked and fails closed (nullopt / null image).

#include "mk8d_assets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>

#include <bc_decoder.h>
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace Mk8dAssets {
namespace {
using namespace dsmod_sdk::int_types;
using Bytes = std::vector<u8>;
using Span = std::span<const u8>;

constexpr std::size_t MaxSource = 64 * 1024 * 1024;
constexpr std::size_t MaxLooseBntx = 8 * 1024 * 1024;
constexpr std::size_t MaxSarcHeader = 4 * 1024 * 1024;
constexpr u32 MaxDimension = 2048;
constexpr u64 MaxPixels = 4 * 1024 * 1024;
/// The inner layout archives (*.szs) carry no name table; their texture archive is the member
/// whose SFAT hash is that of this name (checked for every archive the keys use).
constexpr std::string_view CombinedBntx = "timg/__Combined.bntx";

/// [o, o + n) lies inside a buffer of `size` bytes. Offsets come from file data (any u64), so the
/// test is written so that nothing can wrap.
constexpr bool Fits(u64 size, u64 o, u64 n) {
    return o <= size && n <= size - o;
}
// Bounded little-endian reads: 0 when the value does not fit inside the span.
u16 U16(Span s, u64 o) {
    return Fits(s.size(), o, 2) ? dsmod_sdk::Le16(s.data() + o) : 0;
}
u32 U32(Span s, u64 o) {
    return Fits(s.size(), o, 4) ? dsmod_sdk::Le32(s.data() + o) : 0;
}
u64 U64(Span s, u64 o) {
    return Fits(s.size(), o, 8) ? dsmod_sdk::Le64(s.data() + o) : 0;
}
u16 EU16(Span s, u64 o, bool be) {
    if (!Fits(s.size(), o, 2))
        return 0;
    return be ? dsmod_sdk::Be16(s.data() + o) : dsmod_sdk::Le16(s.data() + o);
}
u32 EU32(Span s, u64 o, bool be) {
    if (!Fits(s.size(), o, 4))
        return 0;
    return be ? dsmod_sdk::Be32(s.data() + o) : dsmod_sdk::Le32(s.data() + o);
}
bool Magic(Span s, std::string_view m) {
    return s.size() >= m.size() && std::memcmp(s.data(), m.data(), m.size()) == 0;
}
/// One path/name component: no separators, no "..", bounded.
bool Safe(std::string_view s) {
    return !s.empty() && s.size() <= 128 && s != "." && s != ".." &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '^' || c == '+';
           });
}
bool EqualNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// ---- host reads ------------------------------------------------------------------------------

std::size_t FileSize(const EdenDsmodHostApi& host, const std::string& path) {
    return host.read_romfs ? host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0) : 0;
}
std::optional<Bytes> ReadRange(const EdenDsmodHostApi& host, const std::string& path, u64 offset,
                               std::size_t size) {
    if (!host.read_romfs || size == 0 || size > MaxSource)
        return std::nullopt;
    Bytes out(size);
    if (host.read_romfs(host.userdata, path.c_str(), offset, out.data(), size) != size)
        return std::nullopt;
    return out;
}
std::optional<Bytes> ReadWhole(const EdenDsmodHostApi& host, const std::string& path,
                               std::size_t limit) {
    const auto size = FileSize(host, path);
    if (!size || size > limit)
        return std::nullopt;
    return ReadRange(host, path, 0, size);
}

// ---- SARC ------------------------------------------------------------------------------------

struct SarcEntry {
    u64 begin{}, end{}; ///< absolute file offsets
};
/// Look a member up from the archive's header region (SARC + SFAT + SFNT, i.e. everything
/// before the data offset). `total` is the archive size, for the range check.
std::optional<SarcEntry> SarcFind(Span head, u64 total, std::string_view wanted) {
    if (head.size() < 0x14 || !Magic(head, "SARC"))
        return std::nullopt;
    const bool be = head[6] == 0xfe && head[7] == 0xff;
    const u16 hs = EU16(head, 4, be);
    const u32 data = EU32(head, 0xc, be);
    if (hs < 0x14 || u64{hs} + 12 > head.size() || !Magic(head.subspan(hs), "SFAT") || data > total)
        return std::nullopt;
    const u16 count = EU16(head, hs + 6, be);
    const u32 multiplier = EU32(head, hs + 8, be);
    const u64 nodes = u64{hs} + 12;
    if (count > 16384 || nodes + u64{count} * 16 + 8 > head.size())
        return std::nullopt;
    const u64 sfnt = nodes + u64{count} * 16;
    if (!Magic(head.subspan(sfnt), "SFNT"))
        return std::nullopt;
    const u64 names = sfnt + EU16(head, sfnt + 4, be);
    const u32 hash = SarcHash(wanted, multiplier);
    for (u32 i = 0; i < count; ++i) {
        const u64 n = nodes + u64{i} * 16;
        const u32 node_hash = EU32(head, n, be);
        const u32 attr = EU32(head, n + 4, be);
        const u64 begin = EU32(head, n + 8, be), end = EU32(head, n + 12, be);
        bool match;
        if (attr & 0x01000000) {
            const u64 no = names + u64{attr & 0xffffff} * 4;
            if (no >= head.size())
                continue;
            const auto* first = head.data() + no;
            const auto* zero = std::find(first, head.data() + head.size(), u8{0});
            if (zero == head.data() + head.size())
                continue;
            match = std::string_view(reinterpret_cast<const char*>(first),
                                     static_cast<std::size_t>(zero - first)) == wanted;
        } else {
            match = node_hash == hash; // hash-only archive: the SFAT hash identifies the name
        }
        if (!match)
            continue;
        if (begin > end || data + end > total)
            return std::nullopt;
        return SarcEntry{data + begin, data + end};
    }
    return std::nullopt;
}

/// A member of a romfs SARC, read by range (the outer UI archives are up to 5 MB and are never
/// read whole).
std::optional<Bytes> ReadSarcMember(const EdenDsmodHostApi& host, const std::string& path,
                                    std::string_view member) {
    const auto total = FileSize(host, path);
    auto header = ReadRange(host, path, 0, 0x14);
    if (!total || !header || !Magic(*header, "SARC"))
        return std::nullopt;
    const bool be = (*header)[6] == 0xfe && (*header)[7] == 0xff;
    const u32 data = EU32(*header, 0xc, be);
    if (data < 0x14 || data > total || data > MaxSarcHeader)
        return std::nullopt;
    auto head = ReadRange(host, path, 0, data);
    if (!head)
        return std::nullopt;
    const auto entry = SarcFind(*head, total, member);
    if (!entry || entry->end == entry->begin)
        return std::nullopt;
    return ReadRange(host, path, entry->begin, static_cast<std::size_t>(entry->end - entry->begin));
}

// ---- BNTX pixel decode -----------------------------------------------------------------------

struct Format {
    u32 bw, bh, bpb, channels;
};
std::optional<Format> GetFormat(u32 f) {
    switch (f >> 8) {
    case 0x02: // R8
        return Format{1, 1, 1, 1};
    case 0x07: // R5G6B5 (red in the low bits, as the runtime decodes it)
    case 0x0f: // B5G6R5
        return Format{1, 1, 2, 4};
    case 0x09: // R8G8
        return Format{1, 1, 2, 2};
    case 0x0b: // R8G8B8A8
    case 0x0c: // B8G8R8A8
        return Format{1, 1, 4, 4};
    case 0x1a: // BC1
        return Format{4, 4, 8, 4};
    case 0x1b: // BC2
    case 0x1c: // BC3
        return Format{4, 4, 16, 4};
    case 0x1d: // BC4
        return Format{4, 4, 8, 1};
    case 0x1e: // BC5
        return Format{4, 4, 16, 2};
    case 0x20: // BC7
        return Format{4, 4, 16, 4};
    default:
        break;
    }
    static constexpr std::array<std::array<u8, 3>, 14> Astc{{{0x2d, 4, 4},
                                                             {0x2e, 5, 4},
                                                             {0x2f, 5, 5},
                                                             {0x30, 6, 5},
                                                             {0x31, 6, 6},
                                                             {0x32, 8, 5},
                                                             {0x33, 8, 6},
                                                             {0x34, 8, 8},
                                                             {0x35, 10, 5},
                                                             {0x36, 10, 6},
                                                             {0x37, 10, 8},
                                                             {0x38, 10, 10},
                                                             {0x39, 12, 10},
                                                             {0x3a, 12, 12}}};
    for (const auto& a : Astc)
        if ((f >> 8) == a[0])
            return Format{a[1], a[2], 16, 4};
    return std::nullopt;
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
/// Tegra block-linear -> row order, on the block grid (width/height in blocks).
std::optional<Bytes> Unswizzle2d(Span source, u32 bpp, u32 width, u32 height, u32 block_height) {
    if (!bpp || block_height > 5)
        return std::nullopt;
    const u64 row_bytes = static_cast<u64>(width) * bpp;
    const u64 aligned_width = (row_bytes + 63) & ~UINT64_C(63);
    const u64 aligned_height = (static_cast<u64>(height) + (UINT64_C(8) << block_height) - 1) &
                               ~((UINT64_C(8) << block_height) - 1);
    const u64 required = aligned_width * aligned_height;
    if (required > source.size() || row_bytes * height > MaxSource)
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

// ---- MSBT ------------------------------------------------------------------------------------

void AppendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
}

/// Image-key parsing: "<kind>/<rest>" with an optional "module:mk8d:" prefix.
constexpr std::string_view KeyPrefix = "module:mk8d:";
std::string_view StripPrefix(std::string_view key) {
    return key.starts_with(KeyPrefix) ? key.substr(KeyPrefix.size()) : key;
}
std::vector<std::string_view> Split(std::string_view s) {
    std::vector<std::string_view> parts;
    while (true) {
        const auto slash = s.find('/');
        parts.push_back(s.substr(0, slash));
        if (slash == std::string_view::npos || parts.size() > 8)
            break;
        s = s.substr(slash + 1);
    }
    return parts;
}

/// Where one image key's pixels live.
struct Source {
    std::string file;    ///< romfs path
    std::string member;  ///< SARC member (layout archive), empty for a loose BNTX
    std::string texture; ///< exact texture name, or the base (before '^'/'+') when by_base
    bool by_base{};
    /// The file is a Yaz0 BFRES whose textures are its embedded external file "textures.bntx"
    /// (Kart/Emblem/*.szs); member is unused.
    bool fres{};
    /// A loose one-texture BNTX (course map, course picture, cup icon): when no texture carries
    /// the expected name and the file holds exactly one texture, that texture is the image. The
    /// game loads these files by path; CTGP-DX's CT_xx files keep vanilla internal names
    /// (CT_00's map is "ym_Map_Gu_City^q") and still show in-game.
    bool sole{};
};
/// Cup icon variants and their texture suffixes, from the romfs file list of UI/cmn/race
/// (every cup ships exactly these four files). Line = white emblem with alpha (the default key),
/// 130x130 = full-colour square badge (R5G6B5, no alpha), Line128x128 = small emblem,
/// Nml = the emblem's normal map (shading input, not for display).
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> CupVariants{
    {{"Nml", "^l"}, {"Line", "^s"}, {"Line128x128", "^s"}, {"130x130", "^h"}}};

std::optional<Source> Resolve(std::string_view key) {
    const auto p = Split(StripPrefix(key));
    const auto kind = p[0];
    const auto safe_rest = [&p](std::size_t from) {
        for (std::size_t i = from; i < p.size(); ++i)
            if (!Safe(p[i]))
                return false;
        return p.size() > from;
    };
    if (kind == "map" && p.size() == 2 && safe_rest(1)) {
        const std::string c(p[1]);
        return Source{
            "/Course/" + c + "/course_maptexture.bntx", {}, "ym_Map_" + c, true, false, true};
    }
    if (kind == "chara" && p.size() == 2 && safe_rest(1))
        return Source{"/UI/cmn/common.sarc", "cm_L_CharaIcon_00.szs",
                      "tc_MapChara_" + std::string(p[1]), true};
    if (kind == "item" && p.size() == 2 && safe_rest(1))
        return Source{"/UI/cmn/race.sarc", "rc_L_ItemBox_00.szs", "tc_Item_" + std::string(p[1]),
                      true};
    if (kind == "cup" && (p.size() == 2 || p.size() == 3) && safe_rest(1)) {
        const std::string_view variant = p.size() == 3 ? p[2] : std::string_view{"Line"};
        for (const auto& [name, suffix] : CupVariants) {
            if (name != variant)
                continue;
            std::string stem =
                "ym_CupIcon" + std::string(p[1]) + "_" + std::string(name) + std::string(suffix);
            return Source{"/UI/cmn/race/" + stem + ".bntx", {}, stem, false, false, true};
        }
        return std::nullopt;
    }
    if (kind == "coursepict" && p.size() == 2 && safe_rest(1)) {
        std::string stem = "ym_CoursePict_" + std::string(p[1]) + "_00^u";
        return Source{"/UI/cmn/menu/" + stem + ".bntx", {}, stem, false, false, true};
    }
    if (kind == "emblem" && p.size() == 2 && safe_rest(1)) {
        // Kart/Emblem/Emblem_<code>.szs: 75 files in 4.0.0, each a Yaz0 BFRES (v0.5.0.3) with one
        // external file "textures.bntx" holding one texture named Emblem_<code>.
        const std::string stem = "Emblem_" + std::string(p[1]);
        Source src{"/Kart/Emblem/" + stem + ".szs", {}, stem, false};
        src.fres = true;
        return src;
    }
    if (kind == "lyt" && p.size() == 5 && safe_rest(1))
        return Source{"/UI/" + std::string(p[1]) + "/" + std::string(p[2]) + ".sarc",
                      std::string(p[3]) + ".szs", std::string(p[4]), false};
    return std::nullopt;
}

/// The base of a texture name: the part before the "^x" / "+x" format suffix.
std::string_view BaseName(std::string_view name) {
    const auto at = name.find_first_of("^+");
    return at == std::string_view::npos ? name : name.substr(0, at);
}

} // namespace

// ---- public stateless decoders ---------------------------------------------------------------

std::uint32_t SarcHash(std::string_view name, std::uint32_t multiplier) {
    u32 h = 0;
    for (const char c : name)
        h = h * multiplier + static_cast<u8>(c);
    return h;
}

std::optional<std::span<const std::uint8_t>> SarcMember(std::span<const std::uint8_t> sarc,
                                                        std::string_view name) {
    const auto entry = SarcFind(sarc, sarc.size(), name);
    if (!entry)
        return std::nullopt;
    return sarc.subspan(static_cast<std::size_t>(entry->begin),
                        static_cast<std::size_t>(entry->end - entry->begin));
}

std::optional<std::vector<std::uint8_t>> Yaz0Decompress(std::span<const std::uint8_t> in) {
    if (!Magic(in, "Yaz0"))
        return in.size() <= MaxSource ? std::optional{Bytes(in.begin(), in.end())} : std::nullopt;
    if (in.size() < 16)
        return std::nullopt;
    const u32 size = dsmod_sdk::Be32(in.data() + 4);
    if (!size || size > MaxSource)
        return std::nullopt;
    Bytes out(size);
    std::size_t si = 16, di = 0;
    u8 code = 0, mask = 0;
    while (di < out.size()) {
        if (!mask) {
            if (si >= in.size())
                return std::nullopt;
            code = in[si++];
            mask = 0x80;
        }
        if (code & mask) {
            if (si >= in.size())
                return std::nullopt;
            out[di++] = in[si++];
        } else {
            if (si + 1 >= in.size())
                return std::nullopt;
            const u8 a = in[si++], b = in[si++];
            const std::size_t dist = ((a & 15) << 8) | b;
            if (dist + 1 > di)
                return std::nullopt;
            std::size_t len = a >> 4;
            if (!len) {
                if (si >= in.size())
                    return std::nullopt;
                len = in[si++] + 0x12;
            } else
                len += 2;
            std::size_t copy = di - dist - 1;
            while (len-- && di < out.size())
                out[di++] = out[copy++];
        }
        mask >>= 1;
    }
    return out;
}

std::optional<std::vector<TextureInfo>> ParseBntx(std::span<const std::uint8_t> b) {
    if (b.size() < 0x30 || !Magic(b, "BNTX"))
        return std::nullopt;
    const u32 declared = U32(b, 0x1c);
    if (declared >= 0x30 && declared <= b.size())
        b = b.first(declared);
    const u32 count = U32(b, 0x24);
    const u64 array = U64(b, 0x28);
    if (!count || count > 4096 || !Fits(b.size(), array, u64{count} * 8))
        return std::nullopt;
    std::vector<TextureInfo> out;
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const u64 o = U64(b, array + u64{i} * 8);
        if (!Fits(b.size(), o, 0xa0) || !Magic(b.subspan(o), "BRTI"))
            return std::nullopt;
        TextureInfo t;
        t.tile_mode = U16(b, o + 0x12);
        t.format = U32(b, o + 0x1c);
        t.width = U32(b, o + 0x24);
        t.height = U32(b, o + 0x28);
        t.arrays = U32(b, o + 0x30);
        t.block_height_log2 = U32(b, o + 0x34) & 7;
        t.image_size = U32(b, o + 0x50);
        t.channel_map = U32(b, o + 0x58);
        const u64 name_at = U64(b, o + 0x60);
        const u64 mips = U64(b, o + 0x70);
        if (!Fits(b.size(), name_at, 2) || !Fits(b.size(), mips, 8))
            return std::nullopt;
        const u16 length = U16(b, name_at);
        if (!Fits(b.size(), name_at + 2, length))
            return std::nullopt;
        t.name.assign(reinterpret_cast<const char*>(b.data() + name_at + 2), length);
        t.image_offset = U64(b, mips);
        out.push_back(std::move(t));
    }
    return out;
}

std::optional<Image> DecodeTexture(std::span<const std::uint8_t> b, const TextureInfo& t,
                                   const AstcDecoder& astc) {
    const u32 declared = U32(b, 0x1c);
    if (declared >= 0x30 && declared <= b.size())
        b = b.first(declared);
    if (!t.width || !t.height || t.width > MaxDimension || t.height > MaxDimension ||
        static_cast<u64>(t.width) * t.height > MaxPixels || !t.arrays || t.image_offset >= b.size())
        return std::nullopt;
    const auto f = GetFormat(t.format);
    if (!f)
        return std::nullopt;
    const u32 bx = (t.width + f->bw - 1) / f->bw, by = (t.height + f->bh - 1) / f->bh;
    const std::size_t linear_size = static_cast<std::size_t>(bx) * by * f->bpb;
    const std::size_t array_size = t.image_size / t.arrays;
    if (t.image_offset + array_size > b.size())
        return std::nullopt;
    const auto src = b.subspan(static_cast<std::size_t>(t.image_offset), array_size);
    Bytes blocks(linear_size);
    if (t.tile_mode == 0) {
        auto linear = Unswizzle2d(src, f->bpb, bx, by, t.block_height_log2);
        if (!linear)
            return std::nullopt;
        blocks = std::move(*linear);
    } else if (t.tile_mode == 1) {
        const std::size_t row = static_cast<std::size_t>(bx) * f->bpb;
        std::size_t pitch = (row + 31) / 32 * 32;
        if (pitch * by > src.size())
            pitch = row;
        if (pitch * by > src.size())
            return std::nullopt;
        for (u32 y = 0; y < by; ++y)
            std::memcpy(blocks.data() + y * row, src.data() + y * pitch, row);
    } else
        return std::nullopt;
    const u32 w = t.width, h = t.height;
    Bytes channels(static_cast<std::size_t>(w) * h * f->channels);
    const u32 fc = t.format >> 8;
    if (fc == 0x02 || fc == 0x09 || fc == 0x0b) {
        if (blocks.size() < channels.size())
            return std::nullopt;
        std::copy_n(blocks.begin(), channels.size(), channels.begin());
    } else if (fc == 0x0c) {
        if (blocks.size() < channels.size())
            return std::nullopt;
        for (std::size_t p = 0; p < channels.size(); p += 4) {
            channels[p] = blocks[p + 2];
            channels[p + 1] = blocks[p + 1];
            channels[p + 2] = blocks[p];
            channels[p + 3] = blocks[p + 3];
        }
    } else if (fc == 0x07 || fc == 0x0f) {
        if (blocks.size() < static_cast<std::size_t>(w) * h * 2)
            return std::nullopt;
        for (std::size_t p = 0; p < static_cast<std::size_t>(w) * h; ++p) {
            const u32 v = u32{blocks[p * 2]} | (u32{blocks[p * 2 + 1]} << 8);
            const u32 lo = v & 31, mid = (v >> 5) & 63, hi = (v >> 11) & 31;
            const u32 r = fc == 0x07 ? lo : hi, bl = fc == 0x07 ? hi : lo;
            channels[p * 4] = static_cast<u8>(r * 255 / 31);
            channels[p * 4 + 1] = static_cast<u8>(mid * 255 / 63);
            channels[p * 4 + 2] = static_cast<u8>(bl * 255 / 31);
            channels[p * 4 + 3] = 255;
        }
    } else if (fc >= 0x2d && fc <= 0x3a) {
        if (!astc.decode || !astc.decode(astc.userdata, w, h, f->bw, f->bh, blocks.data(),
                                         blocks.size(), channels.data(), channels.size()))
            return std::nullopt;
    } else {
        const bool snorm = (t.format & 255) == 2;
        for (u32 y = 0; y < by; ++y)
            for (u32 x = 0; x < bx; ++x) {
                const u8* s = blocks.data() + (static_cast<std::size_t>(y) * bx + x) * f->bpb;
                u8* d =
                    channels.data() + (static_cast<std::size_t>(y) * 4 * w + x * 4) * f->channels;
                switch (fc) {
                case 0x1a:
                    bcn::DecodeBc1(s, d, x * 4, y * 4, w, h);
                    break;
                case 0x1b:
                    bcn::DecodeBc2(s, d, x * 4, y * 4, w, h);
                    break;
                case 0x1c:
                    bcn::DecodeBc3(s, d, x * 4, y * 4, w, h);
                    break;
                case 0x1d:
                    bcn::DecodeBc4(s, d, x * 4, y * 4, w, h, snorm);
                    break;
                case 0x1e:
                    bcn::DecodeBc5(s, d, x * 4, y * 4, w, h, snorm);
                    break;
                case 0x20:
                    bcn::DecodeBc7(s, d, x * 4, y * 4, w, h);
                    break;
                default:
                    return std::nullopt;
                }
            }
    }
    // The texture's channel map (0 = zero, 1 = one, 2..5 = R, G, B, A of the decoded texel).
    Image out{w, h, Bytes(static_cast<std::size_t>(w) * h * 4)};
    for (std::size_t p = 0; p < static_cast<std::size_t>(w) * h; ++p)
        for (u32 c = 0; c < 4; ++c) {
            const u8 sel = static_cast<u8>(t.channel_map >> (c * 8));
            out.rgba[p * 4 + c] = sel == 0   ? 0
                                  : sel == 1 ? 255
                                             : (sel >= 2 && sel - 2u < f->channels
                                                    ? channels[p * f->channels + sel - 2]
                                                    : (sel == 5 ? 255 : 0));
        }
    return out;
}

std::optional<MessageTable> ParseMsbt(std::span<const std::uint8_t> d) {
    if (d.size() < 0x20 || !Magic(d, "MsgStdBn"))
        return std::nullopt;
    const bool be = d[8] == 0xfe && d[9] == 0xff;
    const u8 encoding = d[0xc]; // 0 UTF-8, 1 UTF-16, 2 UTF-32
    const u16 sections = EU16(d, 0xe, be);
    if (encoding > 2 || sections > 64)
        return std::nullopt;
    std::vector<std::pair<std::string, u32>> labels;
    Span txt;
    u64 at = 0x20;
    for (u16 i = 0; i < sections; ++i) {
        if (at + 16 > d.size())
            return std::nullopt;
        const u32 size = EU32(d, at + 4, be);
        if (at + 16 + size > d.size())
            return std::nullopt;
        const Span body = d.subspan(static_cast<std::size_t>(at + 16), size);
        if (Magic(d.subspan(at), "LBL1")) {
            const u32 slots = EU32(body, 0, be);
            if (u64{slots} * 8 + 4 > body.size())
                return std::nullopt;
            for (u32 s = 0; s < slots; ++s) {
                const u32 n = EU32(body, 4 + u64{s} * 8, be);
                u64 o = EU32(body, 8 + u64{s} * 8, be);
                for (u32 k = 0; k < n; ++k) {
                    if (o + 1 > body.size())
                        return std::nullopt;
                    const u8 len = body[o];
                    if (o + 1 + len + 4 > body.size())
                        return std::nullopt;
                    labels.emplace_back(
                        std::string(reinterpret_cast<const char*>(body.data() + o + 1), len),
                        EU32(body, o + 1 + len, be));
                    o += 1 + len + 4;
                }
            }
        } else if (Magic(d.subspan(at), "TXT2")) {
            txt = body;
        }
        at = (at + 16 + size + 15) & ~u64{15};
    }
    if (txt.size() < 4)
        return std::nullopt;
    const u32 count = EU32(txt, 0, be);
    if (u64{count} * 4 + 4 > txt.size())
        return std::nullopt;
    const u32 unit = encoding == 0 ? 1 : encoding == 1 ? 2 : 4;
    MessageTable out;
    out.texts.reserve(labels.size());
    for (const auto& [label, index] : labels) {
        if (index >= count)
            return std::nullopt;
        const u64 begin = EU32(txt, 4 + u64{index} * 4, be);
        const u64 end = index + 1 < count ? EU32(txt, 4 + u64{index + 1} * 4, be) : txt.size();
        if (begin > end || end > txt.size())
            return std::nullopt;
        std::string text;
        u64 p = begin;
        const auto unit_at = [&](u64 o) -> u32 {
            return unit == 1 ? txt[o] : unit == 2 ? EU16(txt, o, be) : EU32(txt, o, be);
        };
        while (p + unit <= end) {
            const u32 c = unit_at(p);
            p += unit;
            if (c == 0)
                break;
            if (c == 0x0e) { // tag: group, type, parameter size, parameters
                if (p + unit * 3 > end)
                    return std::nullopt;
                const u32 param = unit_at(p + unit * 2);
                p += unit * 3 + param;
                continue;
            }
            if (c == 0x0f) { // closing tag: group, type
                p += unit * 2;
                continue;
            }
            char32_t cp = c;
            if (unit == 2 && c >= 0xd800 && c < 0xdc00 && p + 2 <= end) {
                const u32 lo = unit_at(p);
                if (lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                    p += 2;
                }
            }
            AppendUtf8(text, cp);
        }
        out.texts.emplace(label, std::move(text));
    }
    return out;
}

const std::string* MessageTable::Find(std::string_view label) const {
    const auto it = texts.find(std::string(label));
    return it == texts.end() ? nullptr : &it->second;
}

// ---- map camera ------------------------------------------------------------------------------

std::optional<MapCamera> MapCamera::Parse(std::span<const std::uint8_t> file) {
    // 11 floats; the 4.0.0 files are 45 bytes (one is 46: "\r\n"). The loader reads floats
    // 0x00..0x28 of the file and nothing past them.
    if (file.size() < 44)
        return std::nullopt;
    float f[11];
    std::memcpy(f, file.data(), sizeof(f));
    for (const float v : f)
        if (!std::isfinite(v))
            return std::nullopt;
    MapCamera c;
    std::memcpy(c.position, f, 12);
    std::memcpy(c.look_at, f + 3, 12);
    // The loader normalises up (len > 0 only), stores it and LookAtCamera's constructor
    // normalises it again; both times as up * (1 / len).
    for (int pass = 0; pass < 2; ++pass) {
        const float* src = pass == 0 ? f + 6 : c.up;
        const float len = std::sqrt(src[0] * src[0] + src[1] * src[1] + src[2] * src[2]);
        float n[3]{src[0], src[1], src[2]};
        if (len > 0.0f) {
            const float inv = 1.0f / len;
            n[0] *= inv;
            n[1] *= inv;
            n[2] *= inv;
        }
        std::memcpy(c.up, n, 12);
    }
    c.width = f[9];
    c.height = f[10];
    if (!(c.width > 0.0f) || !(c.height > 0.0f))
        return std::nullopt;
    // sead LookAtCamera::doUpdateMatrix (4.0.0 main+0x62a094): d = normalize(pos - at),
    // r = normalize(up x d), u = d x r; rows (r, -r.pos), (u, -u.pos), (d, -d.pos).
    const float* p = c.position;
    const float* a = c.look_at;
    if (p[0] == a[0] && p[1] == a[1] && p[2] == a[2])
        return std::nullopt; // the game leaves the matrix untouched here
    float d[3]{p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len > 0.0f) {
        const float inv = 1.0f / len;
        d[0] *= inv;
        d[1] *= inv;
        d[2] *= inv;
    }
    const float* u = c.up;
    float r[3]{d[2] * u[1] - d[1] * u[2], d[0] * u[2] - d[2] * u[0], d[1] * u[0] - d[0] * u[1]};
    len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!(len > 0.0f))
        return std::nullopt; // up parallel to the view direction: no usable right axis
    {
        const float inv = 1.0f / len;
        r[0] *= inv;
        r[1] *= inv;
        r[2] *= inv;
    }
    const float v[3]{d[1] * r[2] - d[2] * r[1], d[2] * r[0] - d[0] * r[2],
                     d[0] * r[1] - d[1] * r[0]};
    const float* axes[3]{r, v, d};
    for (int i = 0; i < 3; ++i) {
        const float* x = axes[i];
        c.row[i][0] = x[0];
        c.row[i][1] = x[1];
        c.row[i][2] = x[2];
        c.row[i][3] = -((x[0] * p[0] + x[1] * p[1]) + x[2] * p[2]);
    }
    return c;
}

bool MapCamera::Project(float x, float y, float z, float& u, float& v, float world_y_scale) const {
    y *= world_y_scale;
    // Camera::worldPosToCameraPosByMatrix (main+0x629dd4), then OrthoProjection (setTBLR with
    // +/-h/2, -/+w/2; main+0x62c9e0) and the viewport scale of 240 on a 480-pixel pane.
    const float vx = row[0][3] + ((x * row[0][0] + y * row[0][1]) + z * row[0][2]);
    const float vy = row[1][3] + ((x * row[1][0] + y * row[1][1]) + z * row[1][2]);
    const float nx = vx / (width * 0.5f);
    const float ny = vy / (height * 0.5f);
    if (!std::isfinite(nx) || !std::isfinite(ny))
        return false;
    u = 0.5f + nx * 0.5f;
    v = 0.5f - ny * 0.5f;
    return true;
}

// ---- BFRES external files ----------------------------------------------------------------------

namespace {
/// The byte range of a BFRES (Switch, v0.5) external file by exact name. Header +0x98 = u64
/// offset of the external-file array (16-byte entries: u64 data offset, u32 size, pad), +0xA0 =
/// u64 offset of its dictionary (u32 magic (0 in v0.5), u32 count, then count+1 16-byte nodes:
/// u32 reference bit, u16 left, u16 right, u64 name offset; node 0 is the root). Names are
/// u16 length + bytes. Everything bounds-checked; nullopt on any mismatch.
std::optional<Span> FresExternalFile(Span fres, std::string_view name) {
    if (!Magic(fres, "FRES    "))
        return std::nullopt;
    const u64 array = U64(fres, 0x98), dict = U64(fres, 0xA0);
    if (array == 0 || dict == 0 || !Fits(fres.size(), dict, 8))
        return std::nullopt;
    const u32 count = U32(fres, dict + 4);
    if (count == 0 || count > 64)
        return std::nullopt;
    for (u32 i = 1; i <= count; ++i) {
        const u64 node = dict + 8 + 16ull * i;
        if (!Fits(fres.size(), node, 16))
            return std::nullopt;
        const u64 name_at = U64(fres, node + 8);
        const u16 len = U16(fres, name_at);
        if (!Fits(fres.size(), name_at, 2) || !Fits(fres.size(), name_at + 2, len))
            return std::nullopt;
        const std::string_view got(reinterpret_cast<const char*>(fres.data() + name_at + 2), len);
        if (got != name)
            continue;
        if (array > fres.size())
            return std::nullopt;
        const u64 entry = array + 16ull * (i - 1);
        if (!Fits(fres.size(), entry, 12))
            return std::nullopt;
        const u64 off = U64(fres, entry);
        const u32 size = U32(fres, entry + 8);
        if (off >= fres.size() || size > fres.size() - off)
            return std::nullopt;
        return fres.subspan(off, size);
    }
    return std::nullopt;
}
/// One layout material (BFLYT v8 "mat1" entry): the first texture map and the black/white
/// colours the layout interpolates the texture between. Material layout (checked on 4.0.0 data,
/// every UI layout): name[0x1C], u32 flags (bits 0-1 = texture-map count),
/// u32 at +0x20, black RGBA8 at +0x24, white RGBA8 at +0x28, texture maps at +0x2C
/// (u16 txl1 index, u8 wrap s, u8 wrap t). "txl1": u16 count at +8, u32 name offsets relative
/// to +0xC. Section offsets of "mat1" entries are relative to the section.
struct LayoutMaterial {
    std::string texture;
    std::array<u8, 4> black{}, white{};
};
std::optional<LayoutMaterial> FindLayoutMaterial(Span flyt, std::string_view material) {
    if (!Magic(flyt, "FLYT") || U16(flyt, 4) != 0xFEFF)
        return std::nullopt;
    const u16 header = U16(flyt, 6);
    std::vector<std::string> textures;
    Span mat1{};
    for (u64 at = header; at + 8 <= flyt.size();) {
        const u32 size = U32(flyt, at + 4);
        if (size < 8 || size > flyt.size() - at)
            return std::nullopt;
        const Span sec = flyt.subspan(at, size);
        if (Magic(sec, "txl1")) {
            const u16 n = U16(sec, 8);
            for (u16 i = 0; i < n; ++i) {
                const u64 name_at = 0xC + static_cast<u64>(U32(sec, 0xC + 4ull * i));
                if (name_at >= sec.size())
                    return std::nullopt;
                const auto* b = reinterpret_cast<const char*>(sec.data() + name_at);
                textures.emplace_back(b, strnlen(b, sec.size() - name_at));
            }
        } else if (Magic(sec, "mat1")) {
            mat1 = sec;
        }
        at += size;
    }
    if (mat1.empty())
        return std::nullopt;
    const u16 n = U16(mat1, 8);
    for (u16 i = 0; i < n; ++i) {
        const u64 m = U32(mat1, 0xC + 4ull * i);
        if (m + 0x30 > mat1.size())
            return std::nullopt;
        const auto* b = reinterpret_cast<const char*>(mat1.data() + m);
        if (std::string_view(b, strnlen(b, 0x1C)) != material)
            continue;
        const u32 flags = U32(mat1, m + 0x1C);
        if ((flags & 3) == 0)
            return std::nullopt; // no texture map
        const u16 index = U16(mat1, m + 0x2C);
        if (index >= textures.size())
            return std::nullopt;
        LayoutMaterial out;
        out.texture = textures[index];
        std::memcpy(out.black.data(), mat1.data() + m + 0x24, 4);
        std::memcpy(out.white.data(), mat1.data() + m + 0x28, 4);
        return out;
    }
    return std::nullopt;
}
} // namespace

std::optional<std::string> LayoutMaterialTexture(std::span<const std::uint8_t> flyt,
                                                 std::string_view material,
                                                 std::array<std::uint8_t, 4>* black,
                                                 std::array<std::uint8_t, 4>* white) {
    const auto m = FindLayoutMaterial(flyt, material);
    if (!m)
        return std::nullopt;
    if (black)
        *black = m->black;
    if (white)
        *white = m->white;
    return m->texture;
}

// ---- Library ---------------------------------------------------------------------------------

namespace {
/// A texture archive: the BNTX bytes (owned) and its parsed descriptors.
struct Container {
    Bytes bytes;
    std::size_t bntx_offset{};
    std::vector<TextureInfo> textures;
    Span Bntx() const {
        return Span(bytes).subspan(bntx_offset);
    }
};

template <typename T>
class Lru {
public:
    explicit Lru(std::size_t budget) : budget{budget} {}
    std::shared_ptr<const T> Find(const std::string& key) {
        const auto it = map.find(key);
        if (it == map.end())
            return {};
        order.splice(order.end(), order, it->second.second);
        return it->second.first;
    }
    void Put(const std::string& key, std::shared_ptr<const T> value, std::size_t bytes) {
        if (bytes > budget || map.contains(key))
            return;
        while (used + bytes > budget && !order.empty()) {
            const auto old = map.find(order.front());
            order.pop_front();
            if (old != map.end()) {
                used -= sizes[old->first];
                sizes.erase(old->first);
                map.erase(old);
            }
        }
        order.push_back(key);
        map.emplace(key, std::make_pair(std::move(value), std::prev(order.end())));
        sizes[key] = bytes;
        used += bytes;
    }
    void Clear() {
        map.clear();
        order.clear();
        sizes.clear();
        used = 0;
    }

private:
    std::size_t budget, used{};
    std::list<std::string> order;
    std::unordered_map<std::string,
                       std::pair<std::shared_ptr<const T>, std::list<std::string>::iterator>>
        map;
    std::unordered_map<std::string, std::size_t> sizes;
};
} // namespace

struct Library::Impl {
    Impl(std::size_t images_budget, std::size_t archives_budget)
        : images{images_budget}, archives{archives_budget} {}
    std::mutex image_mutex; ///< images, archives, astc (asset worker; tick-thread lists)
    std::mutex data_mutex;  ///< cameras, messages, fonts (worker threads)
    AstcDecoder astc{};
    Lru<Image> images;
    Lru<Container> archives;
    std::unordered_map<std::string, std::shared_ptr<const MapCamera>> cameras;
    std::unordered_map<std::string, std::shared_ptr<const MessageTable>> messages;
    std::unordered_map<std::string, std::shared_ptr<const FontAdvances>> fonts;

    /// Called with image_mutex held.
    std::shared_ptr<const Container> Open(const EdenDsmodHostApi& host, const Source& s) {
        const std::string key = s.file + "#" + s.member;
        if (auto hit = archives.Find(key))
            return hit;
        auto c = std::make_shared<Container>();
        if (s.fres) {
            auto packed = ReadWhole(host, s.file, MaxLooseBntx);
            if (!packed)
                return {};
            auto fres = Yaz0Decompress(*packed);
            if (!fres)
                return {};
            const auto bntx = FresExternalFile(*fres, "textures.bntx");
            if (!bntx)
                return {};
            c->bytes.assign(bntx->begin(), bntx->end());
        } else if (s.member.empty()) {
            auto raw = ReadWhole(host, s.file, MaxLooseBntx);
            if (!raw)
                return {};
            c->bytes = std::move(*raw);
        } else {
            auto packed = ReadSarcMember(host, s.file, s.member);
            if (!packed)
                return {};
            auto inner = Yaz0Decompress(*packed);
            if (!inner)
                return {};
            const auto bntx = SarcMember(*inner, CombinedBntx);
            if (!bntx)
                return {};
            c->bntx_offset = static_cast<std::size_t>(bntx->data() - inner->data());
            c->bytes = std::move(*inner);
            // Keep only what the textures need: drop the layouts/animations before the BNTX.
            Bytes trimmed(c->bytes.begin() + static_cast<std::ptrdiff_t>(c->bntx_offset),
                          c->bytes.begin() +
                              static_cast<std::ptrdiff_t>(c->bntx_offset + bntx->size()));
            c->bytes = std::move(trimmed);
            c->bntx_offset = 0;
        }
        auto textures = ParseBntx(c->Bntx());
        if (!textures)
            return {};
        c->textures = std::move(*textures);
        archives.Put(key, c, c->bytes.size());
        return c;
    }
};

Library::Library(std::size_t image_budget, std::size_t archive_budget)
    : impl{std::make_unique<Impl>(image_budget, archive_budget)} {}
Library::~Library() = default;

void Library::SetAstcDecoder(AstcDecoder decoder) {
    std::scoped_lock lock{impl->image_mutex};
    impl->astc = decoder;
    impl->images.Clear();
}
void Library::Clear() {
    {
        std::scoped_lock lock{impl->image_mutex};
        impl->images.Clear();
        impl->archives.Clear();
    }
    std::scoped_lock lock{impl->data_mutex};
    impl->cameras.clear();
    impl->messages.clear();
    impl->fonts.clear();
}

std::shared_ptr<const Image> Library::LoadImage(const EdenDsmodHostApi& host,
                                                std::string_view key) {
    // lytmat/<dir>/<sarc>/<layout>/<material>: the material's first texture, interpolated between
    // the material's black and white colours per channel (how the layout draws it), e.g. the map
    // crown lytmat/cmn/race/rc_L_DRC_MapIconChara_00/P_MapIconCrown_00 (yellow fill, dark red
    // outline). The layout file is blyt/<layout>.bflyt (hash-only SARC member).
    if (const auto p = Split(StripPrefix(key)); p[0] == "mapfit") {
        if (p.size() != 2 || !Safe(p[1]))
            return {};
        const std::string cache_key = "mapfit#" + std::string(p[1]);
        {
            std::scoped_lock lock{impl->image_mutex};
            if (auto hit = impl->images.Find(cache_key))
                return hit;
        }
        const auto map = LoadImage(host, MapKey(p[1]));
        const auto box = MapFitBox(host, p[1]);
        if (!map || !box)
            return {};
        const long mw = map->width, mh = map->height;
        const long x0 = std::lround(box->u0 * mw), y0 = std::lround(box->v0 * mh);
        const long w = std::lround((box->u1 - box->u0) * mw);
        const long h = std::lround((box->v1 - box->v0) * mh);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
            return {};
        auto out = std::make_shared<Image>();
        out->width = static_cast<std::uint32_t>(w);
        out->height = static_cast<std::uint32_t>(h);
        out->rgba.assign(static_cast<std::size_t>(w * h * 4), 0);
        for (long y = 0; y < h; ++y) {
            const long sy = y0 + y;
            if (sy < 0 || sy >= mh)
                continue;
            for (long x = 0; x < w; ++x) {
                const long sx = x0 + x;
                if (sx < 0 || sx >= mw)
                    continue;
                std::memcpy(out->rgba.data() + (y * w + x) * 4,
                            map->rgba.data() + (sy * mw + sx) * 4, 4);
            }
        }
        std::shared_ptr<const Image> image = std::move(out);
        std::scoped_lock lock{impl->image_mutex};
        impl->images.Put(cache_key, image, image->rgba.size());
        return image;
    }
    if (const auto p = Split(StripPrefix(key)); p[0] == "lytmat") {
        if (p.size() != 5 || !Safe(p[1]) || !Safe(p[2]) || !Safe(p[3]) || !Safe(p[4]))
            return {};
        const std::string cache_key = "lytmat#" + std::string(StripPrefix(key));
        {
            std::scoped_lock lock{impl->image_mutex};
            if (auto hit = impl->images.Find(cache_key))
                return hit;
        }
        const std::string dir(p[1]), sarc(p[2]), layout(p[3]), material(p[4]);
        auto packed = ReadSarcMember(host, "/UI/" + dir + "/" + sarc + ".sarc", layout + ".szs");
        if (!packed)
            return {};
        auto inner = Yaz0Decompress(*packed);
        if (!inner)
            return {};
        const auto flyt = SarcMember(*inner, "blyt/" + layout + ".bflyt");
        if (!flyt)
            return {};
        const auto mat = FindLayoutMaterial(*flyt, material);
        if (!mat || !Safe(mat->texture))
            return {};
        const auto base =
            LoadImage(host, "lyt/" + dir + "/" + sarc + "/" + layout + "/" + mat->texture);
        if (!base)
            return {};
        auto out = std::make_shared<Image>(*base);
        for (std::size_t i = 0; i + 3 < out->rgba.size(); i += 4)
            for (std::size_t c = 0; c < 4; ++c) {
                const int b = mat->black[c], w = mat->white[c], t = out->rgba[i + c];
                out->rgba[i + c] = static_cast<u8>(b + ((w - b) * t + (w >= b ? 127 : -127)) / 255);
            }
        std::shared_ptr<const Image> image = std::move(out);
        std::scoped_lock lock{impl->image_mutex};
        impl->images.Put(cache_key, image, image->rgba.size());
        return image;
    }
    const auto source = Resolve(key);
    if (!source)
        return {};
    // Keyed by what is decoded, so aliases ("cup/X" and "cup/X/Nml") share one image.
    const std::string cache_key =
        source->file + "#" + source->member + "#" + source->texture + (source->by_base ? "^*" : "");
    std::scoped_lock lock{impl->image_mutex};
    if (auto hit = impl->images.Find(cache_key))
        return hit;
    const auto container = impl->Open(host, *source);
    if (!container)
        return {};
    const TextureInfo* chosen = nullptr;
    for (const auto& t : container->textures) {
        const bool match = source->by_base ? EqualNoCase(BaseName(t.name), source->texture)
                                           : t.name == source->texture;
        if (!match)
            continue;
        if (chosen)
            return {}; // two textures with the same base: ambiguous, fail closed
        chosen = &t;
    }
    if (!chosen && source->sole && container->textures.size() == 1)
        chosen = &container->textures.front();
    if (!chosen)
        return {};
    auto decoded = DecodeTexture(container->Bntx(), *chosen, impl->astc);
    if (!decoded)
        return {};
    auto image = std::make_shared<const Image>(std::move(*decoded));
    impl->images.Put(cache_key, image, image->rgba.size());
    return image;
}

std::vector<std::string> Library::ListTextures(const EdenDsmodHostApi& host, std::string_view key) {
    const auto source = Resolve(key);
    if (!source)
        return {};
    std::scoped_lock lock{impl->image_mutex};
    const auto container = impl->Open(host, *source);
    std::vector<std::string> names;
    if (container)
        for (const auto& t : container->textures)
            names.push_back(t.name);
    return names;
}

std::shared_ptr<const MapCamera> Library::LoadMapCamera(const EdenDsmodHostApi& host,
                                                        std::string_view course) {
    if (!Safe(course))
        return {};
    std::scoped_lock lock{impl->data_mutex};
    const std::string key(course);
    if (const auto it = impl->cameras.find(key); it != impl->cameras.end())
        return it->second;
    auto file = ReadWhole(host, "/Course/" + key + "/course_mapcamera.bin", 4096);
    if (!file)
        return {};
    auto camera = MapCamera::Parse(*file);
    if (!camera)
        return {};
    auto shared = std::make_shared<const MapCamera>(*camera);
    impl->cameras.emplace(key, shared);
    return shared;
}

std::shared_ptr<const MessageTable> Library::LoadMessages(const EdenDsmodHostApi& host,
                                                          std::string_view lang,
                                                          std::string_view file) {
    if (!Safe(lang) || !Safe(file))
        return {};
    std::scoped_lock lock{impl->data_mutex};
    const std::string key = std::string(lang) + "/" + std::string(file);
    if (const auto it = impl->messages.find(key); it != impl->messages.end())
        return it->second;
    auto bytes = ReadSarcMember(host, "/UI/" + std::string(lang) + "/message.sarc",
                                std::string(file) + ".msbt");
    if (!bytes)
        return {};
    auto table = ParseMsbt(*bytes);
    if (!table)
        return {};
    auto shared = std::make_shared<const MessageTable>(std::move(*table));
    impl->messages.emplace(key, shared);
    return shared;
}

std::uint32_t FontAdvances::Measure(std::string_view s) const {
    std::uint32_t total = 0;
    for (std::size_t i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = c;
        std::size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            cp = ((c & 7u) << 18) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<u8>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<u8>(s[i + 3]) & 0x3Fu);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0xFu) << 12) | ((static_cast<u8>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<u8>(s[i + 2]) & 0x3Fu);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<u8>(s[i + 1]) & 0x3Fu);
            n = 2;
        }
        const auto it = advance.find(cp);
        total += it != advance.end() ? it->second : fallback;
        i += n;
    }
    return total;
}

std::optional<FontAdvances> ParseBffntAdvances(std::span<const std::uint8_t> b) {
    if (!Magic(b, "FFNT") || b.size() < 0x14)
        return std::nullopt;
    const bool be = b[4] == 0xFE && b[5] == 0xFF;
    const u16 header = EU16(b, 6, be);
    std::unordered_map<u32, u16> index_of; // code point -> glyph index
    std::unordered_map<u32, u16> width_of; // glyph index -> advance
    for (u64 at = header; at + 8 <= b.size();) {
        const u32 size = EU32(b, at + 4, be);
        if (size < 8 || size > b.size() - at)
            break;
        const Span sec = Span(b).subspan(at, size);
        if (Magic(sec, "CWDH") && sec.size() >= 0x10) {
            const u16 first = EU16(sec, 8, be), last = EU16(sec, 10, be);
            for (u32 g = first; g <= last; ++g) {
                const u64 e = 0x10 + 3ull * (g - first);
                if (e + 3 > sec.size())
                    break;
                width_of[g] = sec[e + 2]; // left, glyph width, char width (advance)
            }
        } else if (Magic(sec, "CMAP") && sec.size() >= 0x18) {
            const u32 cb = EU32(sec, 8, be), ce = EU32(sec, 12, be);
            const u16 method = EU16(sec, 16, be);
            const u64 q = 0x18; // after method u16, reserved u16, next-CMAP offset u32
            if (ce >= cb && ce - cb < 0x110000) {
                if (method == 0) {
                    const u16 base = EU16(sec, q, be);
                    for (u32 c = cb; c <= ce; ++c)
                        index_of[c] = static_cast<u16>(base + (c - cb));
                } else if (method == 1) {
                    for (u32 c = cb; c <= ce; ++c) {
                        const u64 e = q + 2ull * (c - cb);
                        if (e + 2 > sec.size())
                            break;
                        const u16 g = EU16(sec, e, be);
                        if (g != 0xFFFF)
                            index_of[c] = g;
                    }
                } else if (method == 2) {
                    const u16 n = EU16(sec, q, be);
                    for (u32 k = 0; k < n; ++k) {
                        const u64 e = q + 4 + 8ull * k; // u32 code, u16 index, pad
                        if (e + 6 > sec.size())
                            break;
                        index_of[EU32(sec, e, be)] = EU16(sec, e + 4, be);
                    }
                }
            }
        }
        at += size;
    }
    if (index_of.empty() || width_of.empty())
        return std::nullopt;
    FontAdvances out;
    for (const auto& [cp, g] : index_of)
        if (const auto w = width_of.find(g); w != width_of.end())
            out.advance[cp] = w->second;
    if (const auto sp = out.advance.find(u32{' '}); sp != out.advance.end())
        out.fallback = sp->second;
    return out;
}

std::shared_ptr<const FontAdvances> Library::LoadFontAdvances(const EdenDsmodHostApi& host,
                                                              std::string_view lang,
                                                              std::string_view member) {
    if (!Safe(lang) || !Safe(member))
        return {};
    const std::string key = std::string(lang) + "#" + std::string(member);
    {
        std::scoped_lock lock{impl->data_mutex};
        if (const auto it = impl->fonts.find(key); it != impl->fonts.end())
            return it->second;
    }
    auto bytes = ReadSarcMember(host, "/UI/" + std::string(lang) + "/font.sarc", member);
    std::shared_ptr<const FontAdvances> result;
    if (bytes)
        if (auto parsed = ParseBffntAdvances(*bytes))
            result = std::make_shared<const FontAdvances>(std::move(*parsed));
    std::scoped_lock lock{impl->data_mutex};
    impl->fonts.emplace(key, result); // a failure is cached too: not retried every sample
    return result;
}

std::string Library::Text(const EdenDsmodHostApi& host, std::string_view lang,
                          std::string_view file, std::string_view label) {
    const auto table = LoadMessages(host, lang, file);
    const std::string* text = table ? table->Find(label) : nullptr;
    return text ? *text : std::string{};
}

namespace {
std::string MakeKey(std::string_view kind, std::string_view name) {
    return Safe(name) ? std::string(KeyPrefix) + std::string(kind) + "/" + std::string(name)
                      : std::string{};
}
} // namespace
std::string Library::MapKey(std::string_view course) {
    return MakeKey("map", course);
}
std::string Library::CharaKey(std::string_view name) {
    return MakeKey("chara", name);
}
std::string Library::ItemKey(std::string_view name) {
    return MakeKey("item", name);
}
std::string Library::CupKey(std::string_view name) {
    return MakeKey("cup", name);
}
std::string Library::CoursePictKey(std::string_view name) {
    return MakeKey("coursepict", name);
}
std::string Library::MapFitKey(std::string_view course) {
    return MakeKey("mapfit", course);
}
std::optional<MapFit> Library::MapFitBox(const EdenDsmodHostApi& host, std::string_view course) {
    const auto map = LoadImage(host, MapKey(course));
    if (!map || map->width == 0 || map->height == 0)
        return std::nullopt;
    const long mw = map->width, mh = map->height;
    long x0 = mw, y0 = mh, x1 = -1, y1 = -1;
    for (long y = 0; y < mh; ++y)
        for (long x = 0; x < mw; ++x)
            if (map->rgba[static_cast<std::size_t>((y * mw + x) * 4 + 3)] != 0) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    if (x1 < x0 || y1 < y0)
        return std::nullopt;
    // Texel box [x0, x1+1) x [y0, y1+1), margin, then the page aspect about the centre.
    double bx0 = x0, by0 = y0, bx1 = x1 + 1.0, by1 = y1 + 1.0;
    const double m = MapFitMargin * std::max(bx1 - bx0, by1 - by0);
    bx0 -= m, by0 -= m, bx1 += m, by1 += m;
    const double aspect = static_cast<double>(MapFitAspectW) / MapFitAspectH;
    double w = bx1 - bx0, h = by1 - by0;
    if (w / h < aspect)
        w = h * aspect;
    else
        h = w / aspect;
    const double cx = (bx0 + bx1) / 2, cy = (by0 + by1) / 2;
    // Whole texels, so the image key and the coordinates agree exactly.
    const double px0 = std::round(cx - w / 2), py0 = std::round(cy - h / 2);
    const double pw = std::round(w), ph = std::round(h);
    MapFit fit;
    fit.u0 = static_cast<float>(px0 / mw);
    fit.v0 = static_cast<float>(py0 / mh);
    fit.u1 = static_cast<float>((px0 + pw) / mw);
    fit.v1 = static_cast<float>((py0 + ph) / mh);
    return fit;
}
std::string Library::EmblemKey(std::string_view code) {
    return MakeKey("emblem", code);
}

} // namespace Mk8dAssets
