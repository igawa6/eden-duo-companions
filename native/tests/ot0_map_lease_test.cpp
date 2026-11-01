// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_battle_signature.h"
#include "ot0_companion_helper.h"
#include <cassert>
#include <cstring>
#include <map>
#include <vector>
using namespace ot0;
static int hidden_lookups = 0;
namespace ot0 {
std::optional<HudRow> FindMinimapHud(const ReadMemory &, uint64_t, int64_t,
                                     bool) {
  return HudRow{0x13000000, 0x13100000, 0x13200000};
}
std::optional<HudRow> FindMinimapCanvas(const ReadMemory &, uint64_t, int64_t,
                                        bool allow_hidden) {
  hidden_lookups += allow_hidden;
  if (allow_hidden)
    return HudRow{0x10000000, 0x11000000, 0x12000000};
  return {};
}
} // namespace ot0
struct Mock {
  Ot0CompanionMailbox box{};
  std::vector<std::pair<uint32_t, uint64_t>> writes;
  bool changing{};
};
EdenDsmodBool l32(void *p, uint32_t o, uint32_t *v) {
  auto &m = *static_cast<Mock *>(p);
  if (o == offsetof(Ot0CompanionMailbox, status_seq) && m.changing)
    m.box.status_seq += 2;
  std::memcpy(v, (char *)&m.box + o, 4);
  return EDEN_DSMOD_TRUE;
}
EdenDsmodBool l64(void *p, uint32_t o, uint64_t *v) {
  std::memcpy(v, (char *)&static_cast<Mock *>(p)->box + o, 8);
  return EDEN_DSMOD_TRUE;
}
EdenDsmodBool s32(void *p, uint32_t o, uint32_t v) {
  auto &m = *static_cast<Mock *>(p);
  m.writes.emplace_back(o, v);
  std::memcpy((char *)&m.box + o, &v, 4);
  return EDEN_DSMOD_TRUE;
}
EdenDsmodBool s64(void *p, uint32_t o, uint64_t v) {
  auto &m = *static_cast<Mock *>(p);
  m.writes.emplace_back(o, v);
  std::memcpy((char *)&m.box + o, &v, 8);
  return EDEN_DSMOD_TRUE;
}

#include <cstdio>
int main() {
  Mock m;
  m.box.epoch = 3;
  m.box.helper_version = OT0_COMPANION_ABI;
  m.box.heartbeat = 100;
  m.box.status_seq = 2;
  m.box.hidden = 1;
  m.box.owned_ui = 0x10000000;
  m.box.owned_slate = 0x11000000;
  m.box.owned_controller = 0x12000000;
  m.box.owned_root = 0x13000000;
  m.box.owned_root_serial = 1;
  m.box.owned_serial = 1;
  m.box.owned_slot = 0x14000000;
  m.box.owned_root_index = 1;
  m.box.owned_root_slate = 0x13100000;
  m.box.owned_root_controller = 0x13200000;
  EdenDsmodHostExtensions ext{};
  ext.version = EDEN_DSMOD_EXT_VERSION;
  ext.struct_size = sizeof ext;
  ext.abi_hash = EDEN_DSMOD_EXT_HASH;
  ext.userdata = &m;
  ext.mailbox_address = 0x100000;
  ext.mailbox_size = sizeof m.box;
  ext.load_u32 = l32;
  ext.load_u64 = l64;
  ext.store_u32 = s32;
  ext.store_u64 = s64;
  NativeCompanionHelper helper;
  helper.Configure(ext);
  helper.Poll();
  ++m.box.heartbeat;
  auto stable = helper.Poll();
  assert(stable.live && stable.hidden);
  MinimapIcons icons{};
  icons.valid = icons.complete = true;
  icons.native_resource = 0x10000000;
  icons.native_section = 0x11000000;
  std::map<uint64_t, uint8_t> memory;
  auto put = [&](uint64_t a, auto v) {
    for (size_t i = 0; i < sizeof v; ++i)
      memory[a + i] = ((uint8_t *)&v)[i];
  };
  const uint64_t main = 0x80000000, chunks = 0x15000000, chunk = 0x16000000;
  put(main + 0x6ce97e8 + 0x24, int32_t(2));
  put(main + 0x6ce97e8 + 0x10, chunks);
  put(chunks, chunk);
  for (int i = 0; i < 2; ++i) {
    uint64_t object = i ? 0x13000000 : 0x10000000;
    put(object + 0xc, int32_t(i));
    std::array<uint8_t, 20> item{};
    std::memcpy(item.data(), &object, 8);
    item[16] = 1;
    put(chunk + 20 * i, item);
  }
  ReadMemory read = [&](uint64_t a, void *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      auto it = memory.find(a + i);
      if (it == memory.end())
        return false;
      ((uint8_t *)out)[i] = it->second;
    }
    return true;
  };
  helper.SetMapLease(read, main, 0, &icons, true);
  assert(hidden_lookups == 1 && m.box.desired_hide == 1);

  const auto deadline = m.box.lease_deadline;
  const auto lease = m.box.lease_seq;
  m.writes.clear();
  m.box.status_seq = 3;
  auto torn = helper.Poll();
  assert(torn.live && torn.hidden && !torn.status_known);
  helper.SetMapLease(read, main, 0, &icons, true);
  assert(m.writes.empty() && m.box.desired_hide == 1 &&
         m.box.lease_seq == lease && m.box.lease_deadline == deadline);
  // Even but continuously changing publication is also unknown.
  m.box.status_seq = 4;
  m.changing = true;
  helper.SetMapLease(read, main, 0, &icons, true);
  assert(m.writes.empty() && m.box.lease_deadline == deadline);
  // Retry recovers and renews only after full widget/serial validation.
  m.changing = false;
  ++m.box.heartbeat;
  helper.SetMapLease(read, main, 0, &icons, true);
  assert(m.box.desired_hide == 1 && m.box.lease_seq != lease &&
         m.box.lease_deadline == deadline + 1);
  // A long unknown run must not extend the guest deadline (guest owns expiry).
  const auto renewed = m.box.lease_deadline;
  m.writes.clear();
  m.box.status_seq |= 1;
  m.box.heartbeat += 121;
  helper.SetMapLease(read, main, 0, &icons, true);
  assert(m.writes.empty() && m.box.lease_deadline == renewed &&
         m.box.heartbeat > renewed);
  // Explicit page/settings exits still request show while status is unknown.
  helper.SetMapLease(read, main, 0, &icons, false);
  assert(m.box.desired_hide == 0);
  m.box.status_seq = 20;
  helper.SetMapLease(read, main, 0, &icons, true);
  assert(m.box.desired_hide == 1);
  // A coherent new epoch invalidates previous ownership and is fail-open.
  ++m.box.epoch;
  m.box.status_seq = 21;
  helper.SetMapLease(read, main, 0, &icons, true);
  assert(!helper.Poll().hidden && m.box.desired_hide == 0);
  ++m.box.heartbeat;
  m.box.status_seq = 22;
  helper.SetMapLease(read, main, 0, &icons, true);
  assert(m.box.desired_hide == 1);
  // Recycled object serial must still revoke a known lease.
  put(chunk + 16, uint32_t(2));
  helper.SetMapLease(read, main, 0, &icons, true);
  assert(m.box.desired_hide == 0);
  std::puts("PASS: odd/changing status preserves lease; coherent recovery, "
            "expiry deadline, explicit exit, epoch and serial guards");
}
