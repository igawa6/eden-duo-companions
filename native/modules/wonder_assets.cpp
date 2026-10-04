// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Asset-free art for the Wonder companion: zstd -> SARC -> BNTX -> RGBA from the player's own
// romfs. The SARC/BNTX/deswizzle/BC code follows mk8d_assets.cpp (kept separate so the two
// companions can change independently).

#include "wonder_assets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

#include <bc_decoder.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "core/mods/modules/dsmod_module_sdk.h"

namespace WonderAssets {
namespace {
using namespace dsmod_sdk::int_types;
constexpr std::size_t MaxSource = 32 * 1024 * 1024;
constexpr u32 MaxDimension = 2048;
constexpr u64 MaxPixels = 4 * 1024 * 1024;

// [o, o + n) lies inside s (no wrap-around for offsets read from the file).
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
    case 45:
        return Format{4, 4, 16, 4};
    case 46:
        return Format{5, 4, 16, 4};
    case 47:
        return Format{5, 5, 16, 4};
    case 48:
        return Format{6, 5, 16, 4};
    case 49:
        return Format{6, 6, 16, 4};
    case 50:
        return Format{8, 5, 16, 4};
    case 51:
        return Format{8, 6, 16, 4};
    case 52:
        return Format{8, 8, 16, 4};
    case 53:
        return Format{10, 5, 16, 4};
    case 54:
        return Format{10, 6, 16, 4};
    case 55:
        return Format{10, 8, 16, 4};
    case 56:
        return Format{10, 10, 16, 4};
    case 57:
        return Format{12, 10, 16, 4};
    case 58:
        return Format{12, 12, 16, 4};
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
std::optional<std::vector<u8>> Unswizzle2d(std::span<const u8> source, u32 bpp, u32 width,
                                           u32 height, u32 block_height) {
    if (!bpp || block_height > 5)
        return std::nullopt;
    const u64 row_bytes = static_cast<u64>(width) * bpp;
    const u64 aligned_width = (row_bytes + 63) & ~UINT64_C(63);
    const u64 aligned_height = (static_cast<u64>(height) + (UINT64_C(8) << block_height) - 1) &
                               ~((UINT64_C(8) << block_height) - 1);
    if (aligned_width * aligned_height > source.size() || row_bytes * height > MaxSource)
        return std::nullopt;
    std::vector<u8> output(static_cast<std::size_t>(row_bytes * height));
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
    const std::array<u8, 4> bm{'B', 'N', 'T', 'X'};
    auto it = std::search(source.begin(), source.end(), bm.begin(), bm.end());
    if (it == source.end())
        return {};
    auto b = source.subspan(static_cast<std::size_t>(it - source.begin()));
    if (b.size() < 0x30)
        return {};
    const u32 declared = U32(b, 0x1c);
    if (declared >= 0x30 && declared <= b.size())
        b = b.first(declared);
    return b;
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

std::optional<Image> DecodeTexture(std::span<const u8> b, const Texture& t, AstcDecoder astc) {
    auto f = GetFormat(t.format);
    if (!f)
        return std::nullopt;
    const u32 bx = (t.w + f->bw - 1) / f->bw, by = (t.h + f->bh - 1) / f->bh;
    const std::size_t linear_size = static_cast<std::size_t>(bx) * by * f->bpb;
    const std::size_t array_size = t.size / t.arrays;
    if (!Fits(b, t.image, array_size))
        return std::nullopt;
    auto src = b.subspan(t.image, array_size);
    std::vector<u8> blocks(linear_size);
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
    std::vector<u8> channels(static_cast<std::size_t>(t.w) * t.h * f->channels);
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
    } else if (fc >= 45 && fc <= 58) {
        if (!astc.decode || !astc.decode(astc.userdata, t.w, t.h, f->bw, f->bh, blocks.data(),
                                         blocks.size(), channels.data(), channels.size()))
            return std::nullopt;
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
    Image out{t.w, t.h, std::vector<u8>(static_cast<std::size_t>(t.w) * t.h * 4)};
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
    return !s.empty() && s.size() <= 96 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '^' || c == '-';
    });
}

constexpr std::array<std::string_view, 12> CharacterIcons{
    "IconPlayer",           "IconPlayerLuigi",      "IconPlayerPeach",
    "IconPlayerDaisy",      "IconPlayerKinopioYellow", "IconPlayerKinopioBlue",
    "IconPlayerKinopico",   "IconPlayerYoshiGreen", "IconPlayerYoshiRed",
    "IconPlayerYoshiYellow", "IconPlayerYoshiBlue", "IconPlayerTotten"};

std::optional<std::string_view> PowerIcon(int id) {
    switch (id) {
    case 1:
        return "IconItemKinoko";
    case 2:
        return "IconItemFireFlower";
    case 3:
        return "IconItemElephantSuit";
    case 6:
        return "IconItemDrillSuit";
    case 9:
        return "IconItemAwaFlower";
    default:
        return std::nullopt;
    }
}

std::optional<int> Number(std::string_view s) {
    int v{};
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size())
        return std::nullopt;
    return v;
}
} // namespace

std::optional<std::vector<u8>> Zstd(std::span<const u8> in) {
    if (in.size() < 4 || dsmod_sdk::Le32(in.data()) != 0xFD2FB528u)
        return in.size() <= MaxSource ? std::optional{std::vector<u8>(in.begin(), in.end())}
                                      : std::nullopt;
    const unsigned long long size = ZSTD_getFrameContentSize(in.data(), in.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size == 0 ||
        size > MaxSource)
        return std::nullopt;
    std::vector<u8> out(static_cast<std::size_t>(size));
    const std::size_t got = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(got) || got != out.size())
        return std::nullopt;
    return out;
}

namespace {
template <typename F>
void SarcWalk(std::span<const u8> s, F&& visit) {
    if (s.size() < 0x20 || !Magic(s, "SARC"))
        return;
    const bool be = s[6] == 0xfe && s[7] == 0xff;
    const u32 hs = EU16(s, 4, be);
    const u32 data = EU32(s, 0xc, be);
    if (hs + 12 > s.size() || data > s.size() || !Magic(s.subspan(hs), "SFAT"))
        return;
    const u32 count = EU16(s, hs + 6, be);
    const u32 nodes = hs + 12;
    if (count > 8192 || nodes + count * 16 > s.size())
        return;
    const u32 sfnt = nodes + count * 16;
    if (sfnt + 8 > s.size() || !Magic(s.subspan(sfnt), "SFNT"))
        return;
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
        if (begin <= end && u64{data} + end <= s.size())
            if (visit(name, s.subspan(data + begin, end - begin)))
                return;
    }
}
} // namespace

std::optional<std::vector<u8>> SarcMember(std::span<const u8> s, std::string_view wanted) {
    std::optional<std::vector<u8>> out;
    SarcWalk(s, [&](std::string_view name, std::span<const u8> bytes) {
        if (name != wanted)
            return false;
        out.emplace(bytes.begin(), bytes.end());
        return true;
    });
    return out;
}

std::vector<std::string> SarcNames(std::span<const u8> s) {
    std::vector<std::string> out;
    SarcWalk(s, [&](std::string_view name, std::span<const u8>) {
        out.emplace_back(name);
        return false;
    });
    return out;
}

std::vector<std::string> BntxNames(std::span<const u8> source) {
    std::vector<std::string> out;
    for (auto& t : Textures(BntxView(source)))
        out.push_back(std::move(t.name));
    return out;
}

std::optional<Image> DecodeBntx(std::span<const u8> source, std::string_view texture,
                                AstcDecoder astc) {
    const auto b = BntxView(source);
    for (const auto& t : Textures(b))
        if (texture.empty() || t.name == texture)
            return DecodeTexture(b, t, astc);
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> Decoder::Resolve(std::string_view key) {
    const auto slash = key.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    const auto kind = key.substr(0, slash);
    const auto rest = key.substr(slash + 1);
    if (kind == "chara") {
        const auto id = Number(rest);
        if (!id || *id < 0 || *id >= static_cast<int>(CharacterIcons.size()))
            return std::nullopt;
        return std::pair{"/UI/Tex/Icon/" + std::string{CharacterIcons[*id]} + ".bntx.zs",
                         std::string{}};
    }
    if (kind == "power") {
        const auto id = Number(rest);
        const auto icon = id ? PowerIcon(*id) : std::nullopt;
        if (!icon)
            return std::nullopt;
        return std::pair{"/UI/Tex/Icon/" + std::string{*icon} + ".bntx.zs", std::string{}};
    }
    if (kind == "course") {
        const auto id = Number(rest);
        if (!id || *id < 1 || *id > 999 || rest.size() != 3)
            return std::nullopt;
        return std::pair{"/UI/Tex/Thumbnail/Course" + std::string{rest} + "_Course.bntx.zs",
                         std::string{}};
    }
    if (kind == "lyt") {
        const auto s2 = rest.find('/');
        if (s2 == std::string_view::npos)
            return std::nullopt;
        const auto layout = rest.substr(0, s2);
        const auto tex = rest.substr(s2 + 1);
        if (!SafeName(layout) || !SafeName(tex))
            return std::nullopt;
        return std::pair{"/Layout/" + std::string{layout} + ".Nin_NX_NVN.blarc.zs",
                         std::string{tex}};
    }
    if (kind == "icon") {
        if (!SafeName(rest))
            return std::nullopt;
        return std::pair{"/UI/Tex/Icon/" + std::string{rest} + ".bntx.zs", std::string{}};
    }
    return std::nullopt;
}

struct Decoder::Impl {
    explicit Impl(std::size_t b) : budget(b) {}
    std::size_t budget, used{};
    AstcDecoder astc{};
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const Image>> images;
    std::list<std::string> lru;
};

Decoder::Decoder(std::size_t b) : impl(std::make_unique<Impl>(b)) {}
Decoder::~Decoder() = default;

void Decoder::SetAstcDecoder(AstcDecoder decoder) {
    std::scoped_lock lock{impl->mutex};
    impl->astc = decoder;
}

std::shared_ptr<const Image> Decoder::Load(const RomfsReader& read, std::string_view key) {
    const std::string k{key};
    AstcDecoder astc;
    {
        std::scoped_lock lock{impl->mutex};
        if (const auto it = impl->images.find(k); it != impl->images.end()) {
            impl->lru.remove(k);
            impl->lru.push_front(k);
            return it->second;
        }
        astc = impl->astc;
    }
    const auto where = Resolve(key);
    if (!where)
        return nullptr;
    const auto raw = read(where->first);
    if (!raw)
        return nullptr;
    auto bytes = Zstd(*raw);
    if (!bytes)
        return nullptr;
    std::optional<Image> image;
    if (key.starts_with("lyt/")) {
        const auto bntx = SarcMember(*bytes, "timg/__Combined.bntx");
        if (bntx)
            image = DecodeBntx(*bntx, where->second, astc);
    } else {
        image = DecodeBntx(*bytes, where->second, astc);
    }
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

} // namespace WonderAssets
