// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pla_packed_ui.h"
#include "re4_sha256.h"
#include "third_party/ooz/ooz.h"
#include <algorithm>
#include <array>
#include <string_view>

namespace pla {
namespace {
struct Pin {
  const char *path;
  size_t archive_size, offset, compressed_size, decoded_size;
  std::string_view archive_sha, compressed_sha, decoded_sha;
};
// Measured from the player's AEE8F150 1.1.1 ROMFS. Index/name provenance:
// battle_menu member2=battle_button_waza_03; poke_dex
// member0=common_button_LR_00, member6=pokedex_detail_task_00,
// member8=pokedex_detail_btn_lr_00; map member18=
// map_deco_frame_00; resident_hud_arc01 member0=common_as_00. Each decoded member
// battle_resident member2=battle_timeline_icon_00, member4=battle_top_02. Each
// is a plain SARC containing timg/__Combined.bntx. These constants describe
// assets; they contain no asset bytes.
constexpr std::array<Pin, 8> Pins{{
    {"bin/archive/appli/battle_menu.gfpak", 419792, 23792, 67368, 408924,
     "1f63d3cb41cf1b51cf249e8ed92b2a29e089700c007707ef67893c8c75ec3d0a",
     "72d8eb80531f28f93322c24be49832cb19de195885cecfac742c4df38f373db7",
     "f220fd6a747e74f5b8c26ffac6f66eaee8f3689cc8511472b26b9614a899cf79"},
    {"bin/archive/appli/poke_dex.gfpak", 1811472, 1392, 125754, 1896288,
     "16d91ea97f62bd69e8ba44ae41280b71ce6ae7b94affec0b914ccadeff690d3f",
     "592f17e067e482331ef3b661bb9d8150bbc763b9b6b7d3fd7748c583fcddb9e0",
     "dc9f0d5034b80663cedc54b912d8fb6e2e5f29fc7c7486ee32e50016c8ce68cc"},
    {"bin/archive/appli/poke_dex.gfpak", 1811472, 605104, 18990, 219540,
     "16d91ea97f62bd69e8ba44ae41280b71ce6ae7b94affec0b914ccadeff690d3f",
     "7e221ec40db02f889e467fa745bb8fded8d633d74624da96807270cf633d6133",
     "2327bc48a47427efa19949c6583c85eb03fbb4ccf4e6deee3dbe20cee7cd9643"},
    {"bin/archive/appli/map.gfpak", 1609296, 462624, 15966, 377780,
     "6d60d7b25804a2dd8f67203b20fea4e34c10d8b51f3ad7b2541073f604449fd6",
     "5140eba4edf95f920df5e997aa3c72a0e68ba4a81145d12e518d8ea2800e1cb5",
     "c367b4b8f88b12cb47643c8e67c65190b6634e3482d18c09b77bb56fee7cf78b"},
    {"bin/archive/appli/resident_hud_arc01.gfpak", 397168, 400, 97309, 857080,
     "9ba571960af64a24d2646d01dfa74ce2e9453bd578bf1e112481c44affc3ebc7",
     "6fc2494a4205f4a69f447d44e13c392e3927960fbe4a888a662732095b9f0249",
     "c5e01aac86b5c367a66fa2ba32b8572f8cf9aa364424f740de40d46c2d04e96e"},
    {"bin/archive/appli/poke_dex.gfpak", 1811472, 629872, 12141, 99200,
     "16d91ea97f62bd69e8ba44ae41280b71ce6ae7b94affec0b914ccadeff690d3f",
     "d76b9928323cef3b6b731d29d29cad0c2788223347a9153c9f0d78c550c0edfd",
     "0a1f953ac578ecc5b8553bcbe796752576d36e77dfa1b40b584ac9eee42d62f2"},
    {"bin/archive/appli/battle_resident.gfpak", 174576, 2208, 15702, 132004,
     "0c09db5b77cc8eb04cf9f5180c3a52cf768a60e74e4303cc7b20f42068024ec2",
     "e7ba37dfdc283fb67c84b72bb9f071d6f1eac520f04145c1504a057a35f2f463",
     "ce0ab3cc74b6dd2916e29038536f3996948743a3e8827b9c5a0c6c6d04d7e755"},
    {"bin/archive/appli/battle_resident.gfpak", 174576, 19776, 91896, 966960,
     "0c09db5b77cc8eb04cf9f5180c3a52cf768a60e74e4303cc7b20f42068024ec2",
     "3f32227df3159f7a5abde00a9d852eb8a52a9c2d2651512d16cf734d2574c7c7",
     "06bd2f279e414bbcc6e5588871d8250c10797b4085e887ce5780bb33f0d361c9"},
}};
const Pin *Find(PackedUiMember member) {
  const auto index = static_cast<unsigned>(member);
  return index < Pins.size() ? &Pins[index] : nullptr;
}
} // namespace
const char *PackedUiPath(PackedUiMember member) {
  const auto *pin = Find(member);
  return pin ? pin->path : nullptr;
}
size_t PackedUiSize(PackedUiMember member) {
  const auto *pin = Find(member);
  return pin ? pin->archive_size : 0;
}
std::vector<uint8_t> DecodePackedUi(PackedUiMember member,
                                    std::span<const uint8_t> archive) {
  const auto *pin = Find(member);
  if (!pin || archive.size() != pin->archive_size ||
      re4::Sha256(archive) != pin->archive_sha ||
      pin->offset > archive.size() ||
      pin->compressed_size > archive.size() - pin->offset)
    return {};
  const auto compressed = archive.subspan(pin->offset, pin->compressed_size);
  if (re4::Sha256(compressed) != pin->compressed_sha)
    return {};
  // ooz is not fuzz safe. Only exact known compressed input reaches it, and
  // both source and destination have the required readable/writable slack.
  std::vector<uint8_t> source(pin->compressed_size + OOZ_SAFE_SPACE);
  std::copy(compressed.begin(), compressed.end(), source.begin());
  std::vector<uint8_t> decoded(pin->decoded_size + OOZ_SAFE_SPACE);
  if (ooz_decompress(source.data(), pin->compressed_size, decoded.data(),
                     pin->decoded_size) !=
          static_cast<int>(pin->decoded_size) ||
      re4::Sha256(std::span(decoded).first(pin->decoded_size)) !=
          pin->decoded_sha)
    return {};
  decoded.resize(pin->decoded_size);
  return decoded;
}
} // namespace pla
