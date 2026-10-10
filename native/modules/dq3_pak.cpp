// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pinned pak members of Dragon Quest III HD-2D Remake 1.1.0.0. Format notes: dq3_pak.h.

#include "dq3_pak.h"

#include <cstring>
#include <limits>

#include "bc_decoder.h"
#include "third_party/ooz/ooz.h"

namespace dq3 {
namespace {

// Values from the merged archive's FPakEntry records (research fixtures/base-pak-members.json,
// directory SHA-1 599ebe03cb27dedd14c4756976823da6ad43d90b; stored hashes re-read from the
// archive at member offset + 28). decoded_sha1 = SHA-1 of the reference Oodle library's output.
constexpr Block MapWindowBaseBlocks[] = {{185, 39903}, {39903, 79660}, {79660, 121656}, {121656, 164958}, {164958, 206997}, {206997, 249845}, {249845, 291144}, {291144, 327418}};
constexpr Block WindowBaseBlocks[] = {{73, 1498}};
constexpr Block StatusHpBlocks[] = {{73, 400}};
constexpr Block StatusMpBlocks[] = {{73, 404}};
constexpr Block StatusGaugeBgBlocks[] = {{73, 489}};
constexpr Block StatusIconBlocks[] = {{73, 9504}};
constexpr Block FontSemiboldBlocks[] = {{73, 42073}};
constexpr Block FontBoldBlocks[] = {{73, 42341}};
constexpr Block FontAvenirBlocks[] = {{73, 58218}};
constexpr PakMember Members[] = {
    {"Nicola/Content/Nicola/UI/Map2/Texture/T_UI_Map_Window_Base_00.uexp",
     26725330, 327233, 2073949, 1, 185, 262144, MapWindowBaseBlocks,
     {0x80, 0x90, 0x19, 0xfc, 0x3d, 0x95, 0xe3, 0x52, 0x3e, 0xd5, 0x33, 0x13, 0xe9, 0xda, 0x5d, 0xee, 0x01, 0x56, 0x6e, 0x40},
     {0xf2, 0x12, 0xac, 0x17, 0x0e, 0x07, 0xf9, 0x2d, 0x5b, 0x46, 0x6d, 0x09, 0x3d, 0x6f, 0x52, 0x44, 0xd5, 0x34, 0x08, 0x81}},
    {"Nicola/Content/Nicola/UI/Common/Texture/T_UI_Common_Window_Base_00.uexp",
     27297850, 1425, 14749, 1, 73, 14749, WindowBaseBlocks,
     {0x8a, 0xfb, 0x44, 0xe3, 0x54, 0x65, 0x2c, 0x7c, 0x9d, 0x25, 0x8e, 0x57, 0x72, 0x60, 0xc3, 0xf4, 0x64, 0xa0, 0x17, 0x33},
     {0x67, 0x5d, 0x68, 0xdf, 0xf9, 0x6a, 0xef, 0x21, 0xd7, 0x49, 0xa9, 0x2a, 0xc8, 0x2c, 0xc1, 0x6f, 0x07, 0xb6, 0x45, 0xbe}},
    {"Nicola/Content/Nicola/UI/Common/Texture/T_UI_Common_Status_HP_00.uexp",
     27321606, 327, 4189, 1, 73, 4189, StatusHpBlocks,
     {0x00, 0x15, 0x44, 0xc5, 0x39, 0x2c, 0xd6, 0xef, 0x18, 0x23, 0xeb, 0xc9, 0x6a, 0xd2, 0xc9, 0x3f, 0x94, 0x3c, 0xd0, 0x25},
     {0xfc, 0x81, 0xb1, 0x17, 0xd0, 0x5b, 0x36, 0x2d, 0xbf, 0xe0, 0x10, 0x6a, 0x60, 0x11, 0xd8, 0xeb, 0x63, 0xb7, 0x0d, 0xf7}},
    {"Nicola/Content/Nicola/UI/Common/Texture/T_UI_Common_Status_MP_00.uexp",
     27313542, 331, 4189, 1, 73, 4189, StatusMpBlocks,
     {0x4f, 0x80, 0x9a, 0xf3, 0xa0, 0xf8, 0x8f, 0xcb, 0xb0, 0x78, 0xb1, 0xe8, 0xd0, 0x1d, 0x4a, 0xbb, 0x75, 0x16, 0x5f, 0xba},
     {0xa4, 0xb6, 0xd2, 0x6f, 0xad, 0xdc, 0x0e, 0xfe, 0x18, 0xab, 0xcd, 0x51, 0xfb, 0x94, 0x37, 0x59, 0xd6, 0x94, 0x6b, 0xa2}},
    {"Nicola/Content/Nicola/UI/Common/Texture/T_UI_Common_Status_HP_01.uexp",
     27314950, 416, 4189, 1, 73, 4189, StatusGaugeBgBlocks,
     {0x8d, 0xd2, 0x3e, 0x77, 0x14, 0x85, 0xbe, 0x4c, 0x82, 0x5e, 0x32, 0x56, 0xb3, 0xae, 0xc0, 0xe9, 0xc1, 0x58, 0x4f, 0x39},
     {0x81, 0x9e, 0x95, 0xfb, 0x06, 0x0d, 0xe2, 0xc8, 0x82, 0x5c, 0x42, 0x5e, 0x72, 0x15, 0xdd, 0xe6, 0xde, 0x66, 0x8a, 0xb0}},
    {"Nicola/Content/Nicola/UI/Common/Texture/T_UI_Common_Status_StatusIcon_00.uexp",
     23306454, 9431, 55645, 1, 73, 55645, StatusIconBlocks,
     {0x07, 0x5b, 0xfb, 0x5a, 0x48, 0x6d, 0x53, 0xcb, 0x10, 0x0f, 0x6d, 0x7e, 0x04, 0x52, 0xec, 0x73, 0x21, 0x23, 0x5b, 0x0e},
     {0xab, 0xaa, 0x7c, 0x27, 0x5b, 0xf4, 0x5f, 0x63, 0x99, 0x85, 0x6d, 0xd5, 0x70, 0xf1, 0x21, 0x4a, 0xe9, 0x98, 0x69, 0xdf}},
    {"Nicola/Content/Nicola/UI/Font/PlantinMTPro-Semibold.ufont",
     8672992, 42000, 77012, 1, 73, 77012, FontSemiboldBlocks,
     {0xd4, 0x33, 0x51, 0xc9, 0x8e, 0x55, 0x28, 0x0e, 0x98, 0xb6, 0x80, 0xbe, 0xa4, 0x93, 0x0e, 0x93, 0x22, 0xe9, 0xb5, 0x70},
     {0x17, 0xdf, 0xde, 0x63, 0xce, 0xb6, 0xff, 0x01, 0x7c, 0x0b, 0xcf, 0x8e, 0xce, 0x80, 0x02, 0xd0, 0xc9, 0xed, 0xf7, 0x42}},
    {"Nicola/Content/Nicola/UI/Font/PlantinMTPro-Bold.ufont",
     11290608, 42268, 76768, 1, 73, 76768, FontBoldBlocks,
     {0xd8, 0x65, 0xc4, 0xd0, 0x80, 0x34, 0x1d, 0xbc, 0xbb, 0x38, 0x36, 0x86, 0x10, 0x5d, 0xbe, 0x9d, 0x67, 0xee, 0xea, 0x18},
     {0x94, 0xd1, 0xc5, 0x46, 0xda, 0xf9, 0xd8, 0xec, 0xab, 0xf5, 0xd1, 0xd0, 0x04, 0x61, 0x8a, 0x06, 0x11, 0x7a, 0x9d, 0x5e}},
    {"Nicola/Content/Nicola/UI/Font/AvenirNextW1G-Demi.ufont",
     25265008, 58145, 122132, 1, 73, 122132, FontAvenirBlocks,
     {0xf8, 0x8c, 0xc8, 0xe0, 0x6c, 0x7e, 0xf9, 0x72, 0x88, 0x0b, 0xf3, 0x5f, 0xcc, 0x08, 0xae, 0x5c, 0x7a, 0x6b, 0x0b, 0xbe},
     {0x42, 0x68, 0x01, 0x59, 0x75, 0x4a, 0xd4, 0xa1, 0xec, 0x6a, 0xec, 0x6d, 0x7c, 0xee, 0x18, 0x33, 0xbd, 0xee, 0x84, 0x62}},
};

static_assert(std::size(Members) == static_cast<std::size_t>(MemberId::Count));

u32 Rol(u32 v, int n) {
    return (v << n) | (v >> (32 - n));
}

} // namespace

Sha1Digest Sha1(std::span<const u8> bytes) {
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const u64 bit_len = static_cast<u64>(bytes.size()) * 8;
    const auto block = [&h](const u8* p) {
        u32 w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = u32{p[4 * i]} << 24 | u32{p[4 * i + 1]} << 16 | u32{p[4 * i + 2]} << 8 |
                   p[4 * i + 3];
        for (int i = 16; i < 80; ++i)
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const u32 t = Rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    };
    std::size_t i = 0;
    for (; i + 64 <= bytes.size(); i += 64)
        block(bytes.data() + i);
    u8 tail[128]{};
    const std::size_t rest = bytes.size() - i;
    if (rest)
        std::memcpy(tail, bytes.data() + i, rest);
    tail[rest] = 0x80;
    const std::size_t total = rest + 9 <= 64 ? 64 : 128;
    for (int k = 0; k < 8; ++k)
        tail[total - 1 - k] = static_cast<u8>(bit_len >> (8 * k));
    block(tail);
    if (total == 128)
        block(tail + 64);
    Sha1Digest out{};
    for (int k = 0; k < 5; ++k)
        for (int j = 0; j < 4; ++j)
            out[4 * k + j] = static_cast<u8>(h[k] >> (24 - 8 * j));
    return out;
}

const PakMember& Member(MemberId id) {
    return Members[static_cast<std::size_t>(id)];
}

std::string CheckEntryHeader(std::span<const u8> header, const PakMember& pin) {
    const std::size_t blocks = pin.compression ? pin.blocks.size() : 0;
    const std::size_t want = 8 + 8 + 8 + 4 + 20 + (pin.compression ? 4 + 16 * blocks : 0) + 1 + 4;
    if (want != pin.header_bytes || header.size() != want)
        return "entry header length";
    const u8* p = header.data();
    if (Le64(p) != 0)
        return "entry offset field";
    if (Le64(p + 8) != pin.compressed_size || Le64(p + 16) != pin.size)
        return "entry sizes";
    if (Le32(p + 24) != pin.compression)
        return "entry compression";
    if (std::memcmp(p + 28, pin.stored_sha1.data(), 20) != 0)
        return "entry hash";
    std::size_t at = 48;
    if (pin.compression) {
        if (Le32(p + at) != blocks)
            return "entry block count";
        at += 4;
        for (const Block& b : pin.blocks) {
            if (Le64(p + at) != b.start || Le64(p + at + 8) != b.end)
                return "entry block range";
            at += 16;
        }
    }
    if (p[at] != 0)
        return "entry flags (encrypted?)";
    if (Le32(p + at + 1) != pin.block_size)
        return "entry block size";
    return {};
}

std::optional<std::vector<u8>> DecodeStored(std::span<const u8> stored, const PakMember& pin) {
    if (pin.compression == 0)
        return stored.size() == pin.size ? std::optional{std::vector<u8>(stored.begin(), stored.end())}
                                         : std::nullopt;
    if (pin.compression != 1 || pin.blocks.empty() || pin.block_size == 0 ||
        pin.size > 64 * 1024 * 1024)
        return std::nullopt;
    const u64 base = pin.blocks.front().start;
    std::vector<u8> out(pin.size + OOZ_SAFE_SPACE);
    std::vector<u8> src;
    u64 produced = 0;
    for (const Block& b : pin.blocks) {
        if (b.end <= b.start || b.start < base || b.end - base > stored.size())
            return std::nullopt;
        const u64 want = std::min<u64>(pin.block_size, pin.size - produced);
        if (want == 0)
            return std::nullopt;
        // A padded private copy: ooz may read a little past the end of its input.
        src.assign(stored.begin() + (b.start - base), stored.begin() + (b.end - base));
        src.resize(src.size() + OOZ_SAFE_SPACE, 0);
        const int n = ooz_decompress(src.data(), static_cast<std::size_t>(b.end - b.start),
                                     out.data() + produced, static_cast<std::size_t>(want));
        if (n < 0 || static_cast<u64>(n) != want)
            return std::nullopt;
        produced += want;
    }
    if (produced != pin.size)
        return std::nullopt;
    out.resize(pin.size);
    return out;
}

std::optional<std::vector<u8>> ReadMember(const RangeReader& read, const PakMember& pin,
                                          std::string* why) {
    const auto fail = [why](const std::string& reason) -> std::optional<std::vector<u8>> {
        if (why)
            *why = reason;
        return std::nullopt;
    };
    if (!read)
        return fail("no reader");
    if (pin.compressed_size > 64 * 1024 * 1024 || pin.size > 64 * 1024 * 1024)
        return fail("member too large");
    std::vector<u8> header(pin.header_bytes);
    if (!read(pin.offset, header.data(), header.size()))
        return fail("header read");
    if (auto bad = CheckEntryHeader(header, pin); !bad.empty())
        return fail(bad);
    u64 begin = pin.header_bytes, end = pin.header_bytes + pin.compressed_size;
    if (pin.compression) {
        begin = pin.blocks.front().start;
        end = pin.blocks.back().end;
        for (std::size_t i = 1; i < pin.blocks.size(); ++i)
            if (pin.blocks[i].start != pin.blocks[i - 1].end)
                return fail("blocks not contiguous");
        if (end - begin != pin.compressed_size)
            return fail("block total");
    }
    std::vector<u8> stored(end - begin);
    if (!read(pin.offset + begin, stored.data(), stored.size()))
        return fail("payload read");
    if (Sha1(stored) != pin.stored_sha1)
        return fail("stored payload hash mismatch");
    auto decoded = DecodeStored(stored, pin);
    if (!decoded)
        return fail("decode failed");
    if (Sha1(*decoded) != pin.decoded_sha1)
        return fail("decoded hash mismatch");
    return decoded;
}

std::optional<std::vector<std::vector<u8>>> ReadMembers(const RangeReader& read,
                                                        std::span<const PakMember* const> pins,
                                                        std::string* why) {
    std::vector<std::vector<u8>> out;
    out.reserve(pins.size());
    std::string reason;
    for (const PakMember* p : pins) {
        auto b = ReadMember(read, *p, &reason);
        if (!b) {
            if (why)
                *why = std::string{p->path} + ": " + reason;
            return std::nullopt;
        }
        out.push_back(std::move(*b));
    }
    return out;
}

std::optional<TexturePin> TextureLayout(MemberId id) {
    switch (id) {
    case MemberId::MapWindowBase:
        return TexturePin{1920, 1080, 2073600};
    case MemberId::WindowBase:
        return TexturePin{120, 120, 14400};
    case MemberId::StatusHp:
    case MemberId::StatusMp:
    case MemberId::StatusGaugeBg:
        return TexturePin{160, 24, 3840};
    case MemberId::StatusIcon:
        return TexturePin{192, 288, 55296};
    default:
        return std::nullopt;
    }
}

namespace {
std::optional<Image> Decode(std::span<const u8> uexp, const TexturePin& pin, const std::array<u32, 4>* rect,
                            std::string* why) {
    const auto fail = [why](const char* reason) -> std::optional<Image> {
        if (why)
            *why = reason;
        return std::nullopt;
    };
    const bool bc7 = pin.format == TextureFormat::Bc7;
    constexpr std::size_t DimsAt = 266, FormatLenAt = 278, FormatAt = 282;
    const std::string_view format = bc7 ? std::string_view{"PF_BC7", 7}
                                        : std::string_view{"PF_B8G8R8A8", 12}; // incl. the NUL
    const std::size_t MipAt = FormatAt + format.size(), DataAt = MipAt + 32;
    const u64 want = bc7 ? u64{(pin.width + 3) / 4} * ((pin.height + 3) / 4) * 16
                         : u64{pin.width} * pin.height * 4;
    const u32 limit = rect && !bc7 ? 16384 : 4096;
    if (pin.width == 0 || pin.height == 0 || pin.width > limit || pin.height > limit ||
        pin.data_bytes != want)
        return fail("texture pin");
    if (rect && (bc7 || (*rect)[2] == 0 || (*rect)[3] == 0 || (*rect)[2] > 4096 || (*rect)[3] > 4096 ||
                 (*rect)[0] + (*rect)[2] > pin.width || (*rect)[1] + (*rect)[3] > pin.height))
        return fail("texture rect");
    if (uexp.size() < DataAt + std::size_t{pin.data_bytes} + 12)
        return fail("texture too short");
    const u8* p = uexp.data();
    if (Le32(p + FormatLenAt) != format.size() ||
        std::memcmp(p + FormatAt, format.data(), format.size()) != 0)
        return fail("texture format");
    if (Le32(p + DimsAt) != pin.width || Le32(p + DimsAt + 4) != pin.height ||
        Le32(p + DimsAt + 8) != 1)
        return fail("texture dimensions");
    const u32 mip[6] = {Le32(p + MipAt), Le32(p + MipAt + 4), Le32(p + MipAt + 8),
                        Le32(p + MipAt + 12), Le32(p + MipAt + 16), Le32(p + MipAt + 20)};
    if (mip[0] != 0 || mip[1] != 1 || mip[2] != 1 || mip[3] != 72 || mip[4] != pin.data_bytes ||
        mip[5] != pin.data_bytes)
        return fail("texture mip header");
    const u8* after = p + DataAt + pin.data_bytes;
    if (Le32(after) != pin.width || Le32(after + 4) != pin.height || Le32(after + 8) != 1)
        return fail("texture trailer");
    if (rect) {
        const auto [rx, ry, rw, rh] = *rect;
        Image img{rw, rh, std::vector<u8>(std::size_t{rw} * rh * 4)};
        for (u32 y = 0; y < rh; ++y) {
            const u8* s = p + DataAt + ((std::size_t{ry} + y) * pin.width + rx) * 4;
            u8* d = img.rgba.data() + std::size_t{y} * rw * 4;
            for (u32 x = 0; x < rw; ++x, s += 4, d += 4) { // B G R A -> R G B A
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = s[3];
            }
        }
        return img;
    }
    Image img{pin.width, pin.height, std::vector<u8>(std::size_t{pin.width} * pin.height * 4)};
    if (!bc7) {
        const u8* s = p + DataAt;
        for (std::size_t i = 0; i < img.rgba.size(); i += 4) { // B G R A -> R G B A
            img.rgba[i] = s[i + 2];
            img.rgba[i + 1] = s[i + 1];
            img.rgba[i + 2] = s[i];
            img.rgba[i + 3] = s[i + 3];
        }
        return img;
    }
    const u32 bx = (pin.width + 3) / 4, by = (pin.height + 3) / 4;
    for (u32 y = 0; y < by; ++y)
        for (u32 x = 0; x < bx; ++x) {
            const u8* s = p + DataAt + (std::size_t{y} * bx + x) * 16;
            // The decoder writes one 4x4 block at (x*4, y*4) relative to dst's block corner.
            u8* d = img.rgba.data() + (std::size_t{y} * 4 * pin.width + x * 4) * 4;
            DecodeBc7Block(s, d, x * 4, y * 4, pin.width, pin.height);
        }
    return img;
}

} // namespace

void DecodeBc7Block(const u8* block, u8* dst, std::size_t x, std::size_t y, std::size_t width, std::size_t height) {
    alignas(8) u8 aligned[16];
    std::memcpy(aligned, block, sizeof(aligned));
    bcn::DecodeBc7(aligned, dst, x, y, width, height);
}

void DecodeBc4Block(const u8* block, u8* dst, std::size_t x, std::size_t y, std::size_t width, std::size_t height,
                    bool is_signed) {
    alignas(8) u8 aligned[8];
    std::memcpy(aligned, block, sizeof(aligned));
    bcn::DecodeBc4(aligned, dst, x, y, width, height, is_signed);
}

std::optional<Image> DecodeTexture(std::span<const u8> uexp, const TexturePin& pin, std::string* why) {
    return Decode(uexp, pin, nullptr, why);
}

std::optional<Image> DecodeTextureRect(std::span<const u8> uexp, const TexturePin& pin,
                                       const std::array<u32, 4>& rect, std::string* why) {
    return Decode(uexp, pin, &rect, why);
}

} // namespace dq3
