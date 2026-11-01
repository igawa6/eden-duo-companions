// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Real-archive checks run only with OT0_PAK=<owned merged pakchunk0-Switch.pak>.
#include "ot0_data.h"
#include "ot0_enemy_assets.h"
#include "ot0_pak.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
using namespace ot0;
int main() {
  assert(EnemyTextureCount() == 545);
  assert(!LoadEnemyTexture({}, "/Game/../x.x"));
  const char *pak = std::getenv("OT0_PAK");
  if (!pak) { std::cout << "ot0 enemy: synthetic checks passed (OT0_PAK unset)\n"; return 0; }
  FILE *f = std::fopen(pak, "rb"); assert(f);
  RangeReader read = [f](uint64_t off, void *out, std::size_t n) {
    return std::fseek(f, long(off), SEEK_SET) == 0 && std::fread(out, 1, n, f) == n;
  };
  auto bytes = [&](MemberId id) { auto b = ReadMember(read, Member(id)); assert(b); return *b; };
  const auto e = bytes(MemberId::EnemyDb), t = bytes(MemberId::EnemyTypeDb), a = bytes(MemberId::EnemyAnimDb),
             x = bytes(MemberId::EnemyTexDb), k = bytes(MemberId::EnemyWeakDb), g = bytes(MemberId::TextEnemy);
  const auto db = DecodeEnemies(e, t, a, x, k, g);
  assert(db.valid && db.rows.size() == 1386);
  const auto *ari = db.Find(32312);
  assert(ari && ari->name == "Thespian (Aristocrat)" && ari->level == "6" && ari->max_shields == 2);
  assert(ari->weapon_weak == ((1u << 2) | (1u << 3) | (1u << 7) | (1u << 8))); // Polearm Dagger Tome Fan
  assert(ari->magic_weak == ((1u << 1) | (1u << 4)));                          // Fire Wind
  assert(ari->texture == "/Game/Character/Resource/V1_0_0/Enm/Textures/Emd_Trouper_b_cl.Emd_Trouper_b_cl");
  const auto *dan = db.Find(32311);
  assert(dan && dan->name == "Thespian (Dancer)" && dan->texture.find("Emd_Trouper_a_cl") != std::string::npos);
  auto img = LoadEnemyTexture(read, ari->texture); assert(img && img->width == 128 && img->height == 220);
  img = LoadEnemyTexture(read, dan->texture); assert(img && img->width == 388 && img->height == 72);
  std::fclose(f);
  std::cout << "ot0 enemy: real-archive checks passed\n";
}
