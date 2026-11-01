// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_appearance.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
void Require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
ot0::Sha1Digest Digest(std::string_view hex) {
  ot0::Sha1Digest out{};
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = std::stoul(std::string(hex.substr(i * 2, 2)), nullptr, 16);
  return out;
}
struct Memory {
  std::map<ot0::u64, ot0::u8> bytes;
  void Write(ot0::u64 at, ot0::u64 value, std::size_t size = 8) {
    for (std::size_t i = 0; i < size; ++i)
      bytes[at + i] = value >> (i * 8);
  }
  bool Read(ot0::u64 at, void *out, std::size_t size) {
    auto *dest = static_cast<ot0::u8 *>(out);
    for (std::size_t i = 0; i < size; ++i) {
      const auto it = bytes.find(at + i);
      if (it == bytes.end())
        return false;
      dest[i] = it->second;
    }
    return true;
  }
};
void ReaderChecks() {
  constexpr ot0::u64 main = 0x80000000, root = 0x1000000000,
                     manager = root + 0x1000, save = root + 0x2000,
                     controller = root + 0x3000, actor = root + 0x4000;
  Memory m;
  m.Write(main + 0x7cc8cc0, root);
  m.Write(root, main + 0x6207610);
  m.Write(root + 0x2f0, 25, 3);
  m.Write(main + 0x7cc8540, manager);
  m.Write(manager + 0x2c8, save);
  m.Write(main + 0x7cc6288, controller);
  m.Write(controller + 0x290, actor);
  m.Write(actor, main + 0x6416cf0);
  m.Write(actor + 0x67c, 9004, 4);
  for (std::size_t i = 0; i < 0xa0; ++i)
    m.bytes[save + 0xa10 + i] = 0;
  for (const auto &[offset, value] :
       std::array<std::pair<ot0::u32, ot0::u32>, 6>{
           {{0, 1}, {4, 4}, {8, 1}, {12, 13}, {16, 1}, {0x50, 1}}})
    m.Write(save + 0xa10 + offset, value, 4);
  ot0::ReadMemory read = [&](auto at, auto *out, auto n) {
    return m.Read(at, out, n);
  };
  const auto a = ot0::ReadProtagonistAppearance(read, main);
  Require(a && a->job == 4 && a->hair == 13 && a->personality == 1,
          "appearance exact fields");
  for (int64_t delta : {int64_t(0x10000000000), int64_t(-0x1000)}) {
    m.Write(root, main + 0x6207610 + static_cast<ot0::u64>(delta));
    m.Write(actor, main + 0x6416cf0 + static_cast<ot0::u64>(delta));
    Require(!ot0::ReadProtagonistAppearance(read, main),
            "shift requires host delta");
    Require(ot0::ReadProtagonistAppearance(read, main, delta) == a,
            "relocated appearance");
    m.Write(actor, main + 0x6416cf8 + static_cast<ot0::u64>(delta));
    Require(!ot0::ReadProtagonistAppearance(read, main, delta),
            "wrong actor rejected");
    m.Write(root, main + 0x6207610);
    m.Write(actor, main + 0x6416cf0);
    Require(ot0::ReadProtagonistAppearance(read, main, delta) == a,
            "original appearance with delta");
  }
  int record_reads = 0;
  ot0::ReadMemory mutate = [&](auto at, auto *out, auto n) {
    if (at == save + 0xa10 && ++record_reads == 2)
      m.Write(save + 0xa10 + 4, 9, 4);
    return m.Read(at, out, n);
  };
  Require(!ot0::ReadProtagonistAppearance(mutate, main),
          "inconsistent appearance rejected");
  m.Write(actor + 0x67c, 91116, 4);
  for (const auto &[offset, value] :
       std::array<std::pair<ot0::u32, ot0::u32>, 3>{
           {{0x68, 19}, {0x6c, 21}, {0x74, 5}}})
    m.Write(save + 0xa10 + offset, value, 4);
  const auto child = ot0::ReadProtagonistAppearance(read, main);
  Require(child && child->child && child->actor_chara_id == 91116 &&
              child->childhood_body == 19 && child->childhood_hair == 21 &&
              child->childhood_face == 5,
          "childhood record and native actor gate");
  m.Write(root + 0x2f0, 10, 1);
  Require(!ot0::ReadProtagonistAppearance(read, main),
          "title hides appearance");
  Require(!ot0::ReadProtagonistAppearance(read, ~ot0::u64{}),
          "overflow root rejected");
  bool touched = false;
  ot0::RangeReader never = [&](auto, auto *, auto) {
    touched = true;
    return false;
  };
  auto unsupported = *a;
  unsupported.hair_color = 10;
  std::string why;
  Require(!ot0::LoadProtagonistAtlas(never, unsupported, &why) && !touched &&
              why == "protagonist palette out of range",
          "palette fails before asset reads");
}
} // namespace
int main() {
  try {
    ReaderChecks();
    const char *path = std::getenv("OT0_PAK");
    if (!path) {
      std::cout << "Appearance reader/rejection checks passed; real archive "
                   "skipped.\n";
      return 0;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(bool(file) && static_cast<ot0::u64>(file.tellg()) == ot0::PakSize,
            "owned pak size");
    ot0::RangeReader read = [&](auto at, void *out, auto n) {
      file.clear();
      file.seekg(at);
      file.read(static_cast<char *>(out), n);
      return std::size_t(file.gcount()) == n;
    };
    std::string why;
    ot0::ProtagonistAppearance a{1, 4, 1, 13, 1, 0, 0, 0, 1};
    auto atlas = ot0::LoadProtagonistAtlas(read, a, &why);
    Require(atlas && atlas->width == 440 && atlas->height == 528,
            "job4 atlas: " + why);
    Require(ot0::Sha1(atlas->rgba) ==
                Digest("29433f8585474a836e416777344e7a31bbc1e562"),
            "job4 independent composition digest");
    a.job = 9;
    atlas = ot0::LoadProtagonistAtlas(read, a, &why);
    Require(atlas && ot0::Sha1(atlas->rgba) ==
                         Digest("8704666dec310d14f54acec08848f6218c4a376b"),
            "job9 independent composition digest: " + why);
    // A distinct valid hairstyle verifies that the selector uses generic pins
    // rather than the current fixture's eight exact assets. A native live check
    // remains separate.
    a.job = 1;
    a.hair = 11;
    atlas = ot0::LoadProtagonistAtlas(read, a, &why);
    Require(bool(atlas), "alternate normal hair selector: " + why);
    a = {1, 4, 1, 13, 1, 2, 5, 9, 1};
    atlas = ot0::LoadProtagonistAtlas(read, a, &why);
    Require(atlas && ot0::Sha1(atlas->rgba) ==
                         Digest("2b37e632825b6a7e0362e6ae932c9b6f8af64f11"),
            "male nonzero palettes: " + why);
    a = {1, 1, 2, 14, 3, 7, 3, 8, 1};
    atlas = ot0::LoadProtagonistAtlas(read, a, &why);
    Require(atlas && ot0::Sha1(atlas->rgba) ==
                         Digest("c0c729dacc60645a6e386debb169568ffdb8d1fd"),
            "female nonzero palettes: " + why);
    a = {1, 9, 1, 13, 1, 0, 0, 0, 1};
    a.childhood_body = 19;
    a.childhood_hair = 21;
    a.childhood_face = 5;
    a.actor_chara_id = 91116;
    a.child = true;
    atlas = ot0::LoadProtagonistAtlas(read, a, &why);
    Require(atlas && atlas->height == 220 &&
                ot0::Sha1(atlas->rgba) ==
                    Digest("9e107e13059fda732ce145104ee2128d165d7c16"),
            "native four-layer childhood atlas: " + why);
    a.actor_chara_id = 9000;
    Require(!ot0::LoadProtagonistAtlas(read, a, &why),
            "invented child actor ID rejected");
    std::cout << "Appearance reader, consistency, rejection and 6 real "
                 "normal-atlas compositions passed.\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
