// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ot0_memory.h"
#include "ot0_reader.h"
#include <cstring>
#include <vector>
namespace ot0 {
namespace {
using mem::Array;
using mem::Limit;
using mem::Pointer;
using Reader = mem::Reader;
template <class T> T At(const uint8_t *p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
} // namespace
Snapshot ReadSnapshot(const ReadMemory &read, uint64_t main,
                      int64_t relocation_delta) {
  Snapshot out;
  if (!read || !Pointer(main) || main > Limit - 0x7cc8cc8)
    return out;
  Reader r{read};
  uint64_t root{}, vtable{};
  std::array<uint8_t, 3> state{};
  if (!r.ptr(main + 0x7cc8cc0, root) || !r.get(root, vtable) ||
      !MatchesVtable(vtable, main + 0x6207610, relocation_delta) ||
      !r.get(root + 0x2f0, state))
    return out;
  out.state = state[0];
  auto fail = [&] {
    Snapshot empty;
    empty.state = out.state;
    return empty;
  };
  // These states may still precede a scene's visual readiness. Only expose
  // party data.
  if (out.state != 23 && out.state != 24 && out.state != 25 && out.state != 26)
    return fail();
  uint64_t manager{}, save{};
  Array parties{}, records{};
  uint32_t active{};
  if (!r.ptr(main + 0x7cc8540, manager) ||
      !r.ptr(manager + 0x2c8, save) || !r.get(save + 0x44, active) ||
      active > 11 || !r.array(save + 0x128, parties, 24) ||
      !r.array(save + 0x138, records, 256))
    return fail();
  std::vector<uint8_t> ps(size_t(parties.count) * 0x20),
      rs(size_t(records.count) * 0x6c);
  if (ps.empty() || rs.empty() || !r.raw(parties.data, ps.data(), ps.size()) ||
      !r.raw(records.data, rs.data(), rs.size()))
    return fail();
  const uint8_t *chosen = nullptr;
  unsigned matches = 0;
  size_t selected = 0;
  for (int i = 0; i < parties.count; ++i)
    if (At<uint32_t>(&ps[i * 0x20]) == active) {
      chosen = &ps[i * 0x20];
      selected = size_t(i);
      ++matches;
    }
  if (matches != 1 || At<uint32_t>(chosen + 0x18) != 0)
    return fail(); // 36-member mode has not been validated.
  Array ids;
  std::memcpy(&ids, chosen + 8, sizeof ids);
  if (ids.count < 8 || ids.count > 36 || ids.capacity < ids.count ||
      ids.capacity > 128 || !Pointer(ids.data, 4))
    return fail();
  std::array<uint32_t, 8> member_ids{};
  if (!r.get(ids.data, member_ids))
    return fail();
  for (size_t slot = 0; slot < 8; ++slot) {
    uint32_t id = member_ids[slot];
    if (!id)
      continue;
    uint32_t key = mem::Alias(id);
    const uint8_t *found = nullptr;
    unsigned n = 0;
    for (int i = 0; i < records.count; ++i)
      if (At<uint32_t>(&rs[i * 0x6c]) == key) {
        found = &rs[i * 0x6c];
        ++n;
      }
    if (n != 1)
      return fail();
    PartyMember m{id, At<uint32_t>(found + 0xc), At<uint32_t>(found + 0x10),
                  At<uint32_t>(found + 0x14), true};
    m.exp = At<uint32_t>(found + 0x8);
    if (m.level < 1 || m.level > 99 || m.hp > 9999 || m.sp > 9999)
      return fail();
    out.members[slot] = m;
  }
  // Verify roots, headers and the selected record again; never publish
  // mixed-load snapshots.
  uint64_t check{};
  uint32_t active_check{};
  Array p2{}, r2{};
  std::array<uint8_t, 3> s2{};
  std::array<uint8_t, 0x20> selected2{};
  std::array<uint32_t, 8> ids2{};
  if (!r.ptr(main + 0x7cc8cc0, check) || check != root ||
      !r.get(root, vtable) ||
      !MatchesVtable(vtable, main + 0x6207610, relocation_delta) ||
      !r.get(root + 0x2f0, s2) || s2 != state ||
      !r.ptr(main + 0x7cc8540, check) || check != manager ||
      !r.ptr(manager + 0x2c8, check) || check != save ||
      !r.get(save + 0x44, active_check) || active_check != active ||
      !r.get(save + 0x128, p2) || p2 != parties || !r.get(save + 0x138, r2) ||
      r2 != records || !r.get(parties.data + selected * 0x20, selected2) ||
      std::memcmp(selected2.data(), chosen, 0x20) != 0 ||
      !r.get(ids.data, ids2) || ids2 != member_ids)
    return fail();
  out.active_party = active;
  // Optional (results summary only): an unreadable value never invalidates the party.
  if (!r.get(save + 0x40, out.money))
    out.money = 0;
  out.valid = true;
  return out;
}
} // namespace ot0
