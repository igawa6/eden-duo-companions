// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_appearance.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace ot0 {
namespace {
struct AppearancePin {
  std::string_view name;
  PakMember member;
  TexturePin texture;
};
#include "ot0_appearance_pins.inc"
constexpr u64 Limit = u64{1} << 39;
bool Pointer(u64 p) { return p && p < Limit && p % 8 == 0; }
const AppearancePin *Find(std::string_view name) {
  const auto it =
      std::lower_bound(std::begin(AppearancePins), std::end(AppearancePins),
                       name, [](const AppearancePin &a, std::string_view b) {
                         return a.member.path < b;
                       });
  return it != std::end(AppearancePins) && it->member.path == name ? it
                                                                   : nullptr;
}
// MessagePack CharacterCreateBodyList: male current-job parts 1,3,...,17;
// female 2,4,...,18. The live resource pool corroborates job4->B0007 and
// job9->B0017.
u32 Body(u32 job, u32 gender) { return 2 * (job - 1) + gender; }
bool Key(const u8 *rgba) {
  return rgba[0] >= 251 && rgba[1] <= 4 && rgba[2] < 5;
}
} // namespace

std::optional<ProtagonistAppearance>
ReadProtagonistAppearance(const ReadMemory &read, u64 main,
                          int64_t relocation_delta) {
  if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8)
    return std::nullopt;
  const auto raw = [&](u64 p, void *out, std::size_t n) {
    return p && p < Limit && n <= Limit - p && read(p, out, n);
  };
  const auto pointer = [&](u64 at, u64 &p) {
    return raw(at, &p, 8) && Pointer(p);
  };
  u64 state_root{}, vtable{}, manager{}, save{}, controller{}, actor{},
      actor_vtable{};
  std::array<u8, 3> state{};
  if (!pointer(main + 0x7cc8cc0, state_root) || !raw(state_root, &vtable, 8) ||
      !MatchesVtable(vtable, main + 0x6207610, relocation_delta) ||
      !raw(state_root + 0x2f0, state.data(), state.size()) ||
      (state[0] != 23 && state[0] != 24 && state[0] != 25 && state[0] != 26) ||
      !pointer(main + 0x7cc8540, manager) || !pointer(manager + 0x2c8, save) ||
      !pointer(main + 0x7cc6288, controller) ||
      !pointer(controller + 0x290, actor) || !raw(actor, &actor_vtable, 8) ||
      !MatchesVtable(actor_vtable, main + 0x6416cf0, relocation_delta))
    return std::nullopt;
  std::array<u8, 0xa0> record{}, again{};
  if (!raw(save + 0xa10, record.data(), record.size()))
    return std::nullopt;
  const auto get = [&](std::size_t offset) {
    return Le32(record.data() + offset);
  };
  ProtagonistAppearance a{get(0),    get(4),    get(8),    get(0xc), get(0x10),
                          get(0x34), get(0x38), get(0x3c), get(0x50)};
  if (!raw(actor + 0x67c, &a.actor_chara_id, 4))
    return std::nullopt;
  a.child = a.actor_chara_id == 91116;
  a.childhood_body = get(0x68);
  a.childhood_hair = get(0x6c);
  a.childhood_hair_color = get(0x70);
  a.childhood_face = get(0x74);
  a.childhood_skin = get(0x78);
  if (a.created != 1 || (!a.child && a.job == 0) || a.job > 9 ||
      a.gender == 0 || a.gender > 2 || a.hair == 0 || a.hair > 99 ||
      a.face == 0 || a.face > 99 || a.eye > 99 || a.skin > 99 ||
      a.hair_color > 99 || a.personality > 99)
    return std::nullopt;
  u64 root2{}, manager2{}, save2{}, controller2{}, actor2{}, root_vtable2{},
      actor_vtable2{};
  u32 actor_chara_id2{};
  std::array<u8, 3> state2{};
  if (!pointer(main + 0x7cc8cc0, root2) || root2 != state_root ||
      !raw(root2, &root_vtable2, 8) || root_vtable2 != vtable ||
      !pointer(main + 0x7cc8540, manager2) || manager2 != manager ||
      !pointer(manager + 0x2c8, save2) || save2 != save ||
      !pointer(main + 0x7cc6288, controller2) || controller2 != controller ||
      !pointer(controller + 0x290, actor2) || actor2 != actor ||
      !raw(actor2, &actor_vtable2, 8) || actor_vtable2 != actor_vtable ||
      !raw(actor + 0x67c, &actor_chara_id2, 4) ||
      actor_chara_id2 != a.actor_chara_id ||
      !raw(save + 0xa10, again.data(), again.size()) || again != record ||
      !raw(state_root + 0x2f0, state2.data(), state2.size()) || state2 != state)
    return std::nullopt;
  return a;
}

std::optional<Image> LoadProtagonistAtlas(const RangeReader &read,
                                          const ProtagonistAppearance &a,
                                          std::string *why) {
  if (why)
    why->clear();
  const auto fail = [why](std::string message) -> std::optional<Image> {
    if (why)
      *why = std::move(message);
    return std::nullopt;
  };
  if (a.created != 1 || (!a.child && a.job == 0) || a.job > 9 ||
      (a.gender != 1 && a.gender != 2) || !a.hair || a.hair > 99 || !a.face ||
      a.face > 99)
    return fail("unsupported protagonist appearance");
  if (a.eye > 9 || a.skin > 5 || a.hair_color > 9)
    return fail("protagonist palette out of range");
  // Exact CharacterCreateHairList and FaceList rows: odd hair parts are male,
  // even are female; face1/2 are male and face3/4 are female.
  if (a.hair > 20 || a.hair % 2 != a.gender % 2 ||
      (a.gender == 1 ? (a.face > 2) : (a.face < 3 || a.face > 4)))
    return fail("protagonist face or hairstyle unavailable for gender");
  // Native 38bb07c: ordinary [0,2,4,6,5,1,3,7], childhood [2,4,5,1].
  // 38ba338..360 passes named MainCharacterPartsInfo fields; 38bb2f0 adds
  // 100*palette to each part ID. These are original prepainted variants.
  // 38b9c84..ca8 binds CharaID91116's J00 texture to the childhood constructor.
  struct Layer {
    char prefix;
    u32 part, job;
    const char *suffix;
  };
  std::vector<Layer> layers;
  if (a.child) {
    if (a.actor_chara_id != 91116 || a.childhood_body != 18 + a.gender ||
        a.childhood_face != 4 + a.gender || a.childhood_skin > 1 ||
        a.childhood_hair_color > 9 ||
        (a.gender == 1 ? (a.childhood_hair < 21 || a.childhood_hair > 30)
                       : (a.childhood_hair < 31 || a.childhood_hair > 40)))
      return fail("unsupported childhood protagonist appearance");
    layers = {
        {'B', a.childhood_body + 100 * a.childhood_skin, 0, "Far_"},
        {'F', a.childhood_face + 100 * a.childhood_skin, 0, ""},
        {'S', a.childhood_hair + 100 * a.childhood_skin, 0, ""},
        {'H', a.childhood_hair + 100 * a.childhood_hair_color, 0, "Near_"}};
  } else {
    layers = {{'H', a.hair + 100 * a.hair_color, 1, "Far_"},
              {'B', Body(a.job, a.gender) + 100 * a.skin, a.job, "Far_"},
              {'F', a.face + 100 * a.skin, 1, ""},
              {'E', a.face + 100 * a.eye, 1, ""},
              {'S', a.hair + 100 * a.skin, 1, ""},
              {'H', a.hair + 100 * a.hair_color, 1, "Near_"},
              {'B', Body(a.job, a.gender) + 100 * a.skin, a.job, "Near_"},
              {'H', a.hair + 100 * a.hair_color, 1, "Addition_"}};
  }
  Image out;
  for (std::size_t i = 0; i < layers.size(); ++i) {
    const auto &layer = layers[i];
    char name[128];
    const int n = std::snprintf(
        name, sizeof name, "ChrTX_Chd_Prt_%c%04u_J%02u_%s1_%sN", layer.prefix,
        layer.part, layer.job, a.gender == 1 ? "Male" : "Female", layer.suffix);
    if (n <= 0 || std::size_t(n) >= sizeof name)
      return fail("protagonist part name length");
    char folder[128];
    const int folder_n =
        std::snprintf(folder, sizeof folder,
                      "Character/Resource/V1_0_0/Prt/ChrTX_Chd_Prt_%c%04u/",
                      layer.prefix, layer.part);
    if (folder_n <= 0 || std::size_t(folder_n) >= sizeof folder)
      return fail("protagonist folder length");
    const auto *pin = Find(std::string(folder) + name + ".uexp");
    if (!pin)
      return fail(std::string("protagonist part unavailable: ") + name);
    std::string reason;
    auto bytes = ReadMember(read, pin->member, &reason);
    if (!bytes)
      return fail(std::string(name) + ": " + reason);
    auto image = DecodeTexture(*bytes, pin->texture, &reason);
    if (!image)
      return fail(std::string(name) + ": " + reason);
    if (i == 0)
      out = std::move(*image);
    else {
      if (image->width != out.width || image->height != out.height ||
          image->rgba.size() != out.rgba.size())
        return fail("protagonist layer dimensions disagree");
      for (std::size_t at = 0; at < out.rgba.size(); at += 4)
        if (!Key(image->rgba.data() + at))
          std::memcpy(out.rgba.data() + at, image->rgba.data() + at, 4);
    }
  }
  return out;
}
} // namespace ot0
