// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pinned pak members of Octopath Traveler 0 1.0.5. Format notes: ot0_pak.h.

#include "ot0_pak.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "bc_decoder.h"
#include "third_party/ooz/ooz.h"

namespace ot0 {
namespace {

#include "ot0_pak_pins.inc"

static_assert(std::size(Members) == static_cast<std::size_t>(MemberId::Count));
static_assert(std::size(TexturePins) == std::size(Members));

u32 Rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

} // namespace

Sha1Digest Sha1(std::span<const u8> bytes) {
  u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  const u64 bit_len = static_cast<u64>(bytes.size()) * 8;
  const auto block = [&h](const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = u32{p[4 * i]} << 24 | u32{p[4 * i + 1]} << 16 |
             u32{p[4 * i + 2]} << 8 | p[4 * i + 3];
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

const PakMember &Member(MemberId id) {
  return Members[static_cast<std::size_t>(id)];
}

std::string CheckEntryHeader(std::span<const u8> header, const PakMember &pin) {
  const std::size_t blocks = pin.compression ? pin.blocks.size() : 0;
  const std::size_t want =
      8 + 8 + 8 + 4 + 20 + (pin.compression ? 4 + 16 * blocks : 0) + 1 + 4;
  if (want != pin.header_bytes || header.size() != want)
    return "entry header length";
  const u8 *p = header.data();
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
    for (const Block &b : pin.blocks) {
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

std::optional<std::vector<u8>> DecodeStored(std::span<const u8> stored,
                                            const PakMember &pin) {
  if (Sha1(stored) != pin.stored_sha1)
    return std::nullopt;
  if (pin.compression == 0)
    return stored.size() == pin.size
               ? std::optional{std::vector<u8>(stored.begin(), stored.end())}
               : std::nullopt;
  if (pin.compression != 1 || pin.blocks.empty() ||
      (pin.block_size == 0 && pin.blocks.size() != 1) ||
      pin.size > 64 * 1024 * 1024)
    return std::nullopt;
  const u64 base = pin.blocks.front().start;
  std::vector<u8> out(pin.size + OOZ_SAFE_SPACE);
  std::vector<u8> src;
  u64 produced = 0;
  for (const Block &b : pin.blocks) {
    if (b.end <= b.start || b.start < base || b.end - base > stored.size())
      return std::nullopt;
    const u64 want = std::min<u64>(pin.block_size ? pin.block_size : pin.size,
                                   pin.size - produced);
    if (want == 0)
      return std::nullopt;
    // A padded private copy: ooz may read a little past the end of its input.
    src.assign(stored.begin() + (b.start - base),
               stored.begin() + (b.end - base));
    src.resize(src.size() + OOZ_SAFE_SPACE, 0);
    const int n =
        ooz_decompress(src.data(), static_cast<std::size_t>(b.end - b.start),
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

std::optional<std::vector<u8>>
ReadMember(const RangeReader &read, const PakMember &pin, std::string *why) {
  const auto fail =
      [why](const std::string &reason) -> std::optional<std::vector<u8>> {
    if (why)
      *why = reason;
    return std::nullopt;
  };
  if (!read)
    return fail("no reader");
  if (pin.header_bytes > 65536 || pin.offset > PakSize ||
      pin.header_bytes > PakSize - pin.offset || pin.compression > 1 ||
      (pin.compression && pin.blocks.empty()) ||
      pin.compressed_size > 64 * 1024 * 1024 || pin.size > 64 * 1024 * 1024)
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
    if (begin < pin.header_bytes || end < begin ||
        end - begin != pin.compressed_size)
      return fail("block total");
  }
  if (end > PakSize - pin.offset)
    return fail("payload archive bounds");
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

std::optional<std::vector<std::vector<u8>>>
ReadMembers(const RangeReader &read, std::span<const PakMember *const> pins,
            std::string *why) {
  std::vector<std::vector<u8>> out;
  out.reserve(pins.size());
  std::string reason;
  for (const PakMember *p : pins) {
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
  const auto i = static_cast<std::size_t>(id);
  return i < std::size(TexturePins) ? TexturePins[i] : std::nullopt;
}

void DecodeBc1Block(const u8 *block, u8 *dst, std::size_t x, std::size_t y,
                    std::size_t width, std::size_t height) {
  alignas(8) u8 aligned[8];
  std::memcpy(aligned, block, sizeof(aligned));
  bcn::DecodeBc1(aligned, dst, x, y, width, height);
}
void DecodeBc3Block(const u8 *block, u8 *dst, std::size_t x, std::size_t y,
                    std::size_t width, std::size_t height) {
  alignas(8) u8 aligned[16];
  std::memcpy(aligned, block, sizeof(aligned));
  bcn::DecodeBc3(aligned, dst, x, y, width, height);
}

namespace {
std::optional<Image> Decode(std::span<const u8> uexp, const TexturePin &pin,
                            const std::array<u32, 4> *rect, std::string *why) {
  if (why)
    why->clear();
  const auto fail = [why](const char *reason) -> std::optional<Image> {
    if (why)
      *why = reason;
    return std::nullopt;
  };
  std::string_view format;
  u32 block_bytes{};
  switch (pin.format) {
  case TextureFormat::Bc1:
    format = {"PF_DXT1", 8};
    block_bytes = 8;
    break;
  case TextureFormat::Bc3:
    format = {"PF_DXT5", 8};
    block_bytes = 16;
    break;
  case TextureFormat::Bgra8:
    format = {"PF_B8G8R8A8", 12};
    break;
  default:
    return fail("texture format pin");
  }
  const u32 limit = rect ? 16384 : 4096;
  const u64 want = block_bytes ? u64{(pin.width + 3) / 4} *
                                     ((pin.height + 3) / 4) * block_bytes
                               : u64{pin.width} * pin.height * 4;
  if (!pin.width || !pin.height || pin.width > limit || pin.height > limit ||
      want != pin.data_bytes || pin.data_bytes > 64 * 1024 * 1024 ||
      pin.format_at < 16 ||
      pin.data_at != u64{pin.format_at} + format.size() + 12)
    return fail("texture pin");
  const std::array<u32, 4> area =
      rect ? *rect : std::array<u32, 4>{0, 0, pin.width, pin.height};
  const auto [rx, ry, rw, rh] = area;
  if (!rw || !rh || rw > 4096 || rh > 4096 || rx > pin.width ||
      ry > pin.height || rw > pin.width - rx || rh > pin.height - ry)
    return fail("texture rect");
  if (u64{pin.data_at} + pin.data_bytes + 12 > uexp.size())
    return fail("texture too short");
  const u8 *p = uexp.data();
  if (Le32(p + pin.format_at - 4) != format.size() ||
      std::memcmp(p + pin.format_at, format.data(), format.size()) != 0)
    return fail("texture format");
  if (Le32(p + pin.format_at - 16) != pin.width ||
      Le32(p + pin.format_at - 12) != pin.height ||
      Le32(p + pin.format_at - 8) != 1)
    return fail("texture dimensions");
  const u8 *mip = p + pin.format_at + format.size();
  if (Le32(mip) != 0 || Le32(mip + 4) < 1 || Le32(mip + 4) > 32 ||
      Le32(mip + 8) != 0)
    return fail("texture mip header");
  const u8 *after = p + pin.data_at + pin.data_bytes;
  if (Le32(after) != pin.width || Le32(after + 4) != pin.height ||
      Le32(after + 8) != 1)
    return fail("texture trailer");
  Image image{rw, rh, std::vector<u8>(std::size_t{rw} * rh * 4)};
  if (!block_bytes) {
    for (u32 y = 0; y < rh; ++y) {
      const u8 *s =
          p + pin.data_at + ((std::size_t{ry} + y) * pin.width + rx) * 4;
      u8 *d = image.rgba.data() + std::size_t{y} * rw * 4;
      for (u32 x = 0; x < rw; ++x, s += 4, d += 4) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
      }
    }
  } else {
    const u32 columns = (pin.width + 3) / 4;
    for (u32 by = ry / 4; by <= (ry + rh - 1) / 4; ++by) {
      for (u32 bx = rx / 4; bx <= (rx + rw - 1) / 4; ++bx) {
        // Decode to a complete scratch tile, then clip. This handles
        // non-4-aligned rectangles and edge blocks without pointer arithmetic
        // outside the image.
        alignas(8) u8 tile[64];
        const u8 *s =
            p + pin.data_at + (std::size_t{by} * columns + bx) * block_bytes;
        if (pin.format == TextureFormat::Bc1)
          DecodeBc1Block(s, tile, 0, 0, 4, 4);
        else
          DecodeBc3Block(s, tile, 0, 0, 4, 4);
        for (u32 y = std::max(ry, by * 4); y < std::min(ry + rh, by * 4 + 4);
             ++y) {
          const u32 left = std::max(rx, bx * 4),
                    right = std::min(rx + rw, bx * 4 + 4);
          std::memcpy(image.rgba.data() +
                          (std::size_t{y - ry} * rw + left - rx) * 4,
                      tile + ((y - by * 4) * 4 + left - bx * 4) * 4,
                      (right - left) * 4);
        }
      }
    }
  }
  return image;
}
} // namespace

std::optional<Image> DecodeTexture(std::span<const u8> uexp,
                                   const TexturePin &pin, std::string *why) {
  return Decode(uexp, pin, nullptr, why);
}
std::optional<Image> DecodeTextureRect(std::span<const u8> uexp,
                                       const TexturePin &pin,
                                       const std::array<u32, 4> &rect,
                                       std::string *why) {
  return Decode(uexp, pin, &rect, why);
}
} // namespace ot0
