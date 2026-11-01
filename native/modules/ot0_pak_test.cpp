// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_pak.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
void Put32(std::vector<ot0::u8> &b, std::size_t at, ot0::u32 value) {
  for (int i = 0; i < 4; ++i)
    b[at + i] = value >> (8 * i);
}
std::vector<ot0::u8> Texture(const ot0::TexturePin &pin, const char *format) {
  std::vector<ot0::u8> b(pin.data_at + pin.data_bytes + 12);
  Put32(b, pin.format_at - 16, pin.width);
  Put32(b, pin.format_at - 12, pin.height);
  Put32(b, pin.format_at - 8, 1);
  Put32(b, pin.format_at - 4, std::strlen(format) + 1);
  std::memcpy(b.data() + pin.format_at, format, std::strlen(format) + 1);
  Put32(b, pin.data_at - 8, 1);
  Put32(b, pin.data_at + pin.data_bytes, pin.width);
  Put32(b, pin.data_at + pin.data_bytes + 4, pin.height);
  Put32(b, pin.data_at + pin.data_bytes + 8, 1);
  return b;
}
void Synthetic() {
  const ot0::u8 abc[] = {'a', 'b', 'c'};
  const ot0::Sha1Digest expected{0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81,
                                 0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50,
                                 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
  Require(ot0::Sha1(abc) == expected, "SHA1 known vector");
  for (const auto f : {ot0::TextureFormat::Bc1, ot0::TextureFormat::Bc3}) {
    const ot0::u32 block = f == ot0::TextureFormat::Bc1 ? 8 : 16;
    const ot0::TexturePin pin{f, 5, 5, 16, 36, block * 4};
    auto b = Texture(pin, block == 8 ? "PF_DXT1" : "PF_DXT5");
    for (ot0::u32 i = 0; i < 4; ++i) {
      auto at = pin.data_at + i * block;
      if (block == 16) {
        b[at] = 255;
        b[at + 1] = 255;
        at += 8;
      }
      b[at + 1] = 0xf8; // RGB565 red, index zero in every pixel.
    }
    auto image = ot0::DecodeTextureRect(b, pin, {1, 1, 4, 4});
    Require(bool(image), "unaligned cropped BC decode");
    for (std::size_t i = 0; i < image->rgba.size(); i += 4)
      Require(image->rgba[i] == 255 && image->rgba[i + 1] == 0 &&
                  image->rgba[i + 2] == 0 && image->rgba[i + 3] == 255,
              "BC red texel");
    Require(!ot0::DecodeTextureRect(b, pin, {0xffffffff, 0, 2, 2}),
            "overflow rect rejected");
    Require(!ot0::DecodeTextureRect(b, pin, {0, 0, 0, 2}),
            "empty rect rejected");
    b[pin.format_at] = 'X';
    Require(!ot0::DecodeTexture(b, pin), "bad format rejected");
  }
  const ot0::TexturePin pin{ot0::TextureFormat::Bgra8, 2, 1, 16, 40, 8};
  auto b = Texture(pin, "PF_B8G8R8A8");
  b[40] = 12;
  b[41] = 23;
  b[42] = 34;
  b[43] = 45;
  auto image = ot0::DecodeTextureRect(b, pin, {0, 0, 1, 1});
  Require(image && image->rgba == std::vector<ot0::u8>({34, 23, 12, 45}),
          "BGRA conversion");
  b.pop_back();
  Require(!ot0::DecodeTexture(b, pin), "truncated trailer rejected");
}
} // namespace
int main() {
  try {
    Synthetic();
    const char *path = std::getenv("OT0_PAK");
    if (!path) {
      std::cout
          << "Synthetic checks passed; OT0_PAK unset (real archive skipped).\n";
      return 0;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(bool(file), "open real pak");
    Require(static_cast<ot0::u64>(file.tellg()) == ot0::PakSize,
            "archive size");
    const ot0::RangeReader read = [&file](ot0::u64 offset, void *out,
                                          std::size_t n) {
      file.clear();
      file.seekg(offset);
      file.read(static_cast<char *>(out), n);
      return static_cast<std::size_t>(file.gcount()) == n;
    };
    unsigned textures = 0;
    for (ot0::u32 i = 0; i < static_cast<ot0::u32>(ot0::MemberId::Count); ++i) {
      const auto id = static_cast<ot0::MemberId>(i);
      const auto &member = ot0::Member(id);
      std::string why;
      auto bytes = ot0::ReadMember(read, member, &why);
      Require(bool(bytes), std::string(member.path) + ": " + why);
      if (auto pin = ot0::TextureLayout(id)) {
        auto image = ot0::DecodeTexture(*bytes, *pin, &why);
        Require(bool(image), std::string(member.path) + ": " + why);
        ++textures;
        auto crop = ot0::DecodeTextureRect(
            *bytes, *pin, {pin->width - 1, pin->height - 1, 1, 1}, &why);
        Require(crop && std::equal(crop->rgba.begin(), crop->rgba.end(),
                                   image->rgba.end() - 4),
                "edge crop matches");
        if (id == ot0::MemberId::WorldMap)
          Require(image->width == 4096 && image->height == 2732,
                  "world map dimensions");
        if (id == ot0::MemberId::MinimapRevealMask) {
          Require(pin->format == ot0::TextureFormat::Bc3 &&
                      image->width == 256 && image->height == 256,
                  "native reveal mask layout");
          // Native BLEND_AlphaHoldout uses this soft alpha stamp; its RGB is
          // only a binary disc and must not replace the alpha falloff.
          const ot0::Sha1Digest rgba_hash{
              0x73, 0x18, 0xd1, 0x1e, 0x77, 0x7c, 0x5e, 0x86, 0xa5, 0x14,
              0xa4, 0x91, 0xae, 0x11, 0x0a, 0x26, 0x2c, 0x9f, 0xc3, 0xe4};
          Require(ot0::Sha1(image->rgba) == rgba_hash,
                  "native reveal mask RGBA bytes");
          Require(image->rgba[(128 * 256 + 32) * 4 + 3] == 84 &&
                      image->rgba[(128 * 256 + 64) * 4 + 3] == 242 &&
                      image->rgba[(128 * 256 + 128) * 4 + 3] == 255,
                  "native reveal mask alpha falloff");
        }
        auto bad = *bytes;
        bad[pin->format_at - 16] ^= 1;
        Require(!ot0::DecodeTexture(bad, *pin),
                "bad texture dimensions rejected");
      }
    }
    const auto &pin = ot0::Member(ot0::MemberId::InBox);
    const ot0::RangeReader corrupt = [&read, &pin](ot0::u64 offset, void *out,
                                                   std::size_t n) {
      if (!read(offset, out, n))
        return false;
      if (offset == pin.offset + pin.blocks.front().start && n)
        static_cast<ot0::u8 *>(out)[0] ^= 1;
      return true;
    };
    std::string why;
    Require(!ot0::ReadMember(corrupt, pin, &why) &&
                why == "stored payload hash mismatch",
            "stored rejection before ooz");
    auto wrong = pin;
    wrong.decoded_sha1[0] ^= 1;
    Require(!ot0::ReadMember(read, wrong, &why) &&
                why == "decoded hash mismatch",
            "decoded hash rejection");
    wrong = pin;
    wrong.offset = ot0::PakSize;
    Require(!ot0::ReadMember(read, wrong), "archive bounds rejection");
    std::cout << "All real members validated; " << textures
              << " textures decoded; synthetic and corruption checks passed.\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
