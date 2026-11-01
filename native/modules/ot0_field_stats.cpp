// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_field_stats.h"
#include <cstddef>
namespace ot0 {
namespace {
constexpr uint32_t Field(std::size_t offset) { return OT0_STATS_OFFSET + static_cast<uint32_t>(offset); }
} // namespace
bool NativeStats::Load(uint32_t offset, uint32_t &value) const {
  return configured_ && ext_.load_u32 && ext_.load_u32(ext_.userdata, offset, &value) == EDEN_DSMOD_TRUE;
}
bool NativeStats::Store(uint32_t offset, uint32_t value) const {
  return configured_ && ext_.store_u32 && ext_.store_u32(ext_.userdata, offset, value) == EDEN_DSMOD_TRUE;
}
void NativeStats::Configure(const EdenDsmodHostExtensions &ext) {
  std::lock_guard lock(mutex_);
  configured_ = ext.version == EDEN_DSMOD_EXT_VERSION && ext.struct_size == sizeof ext &&
                ext.abi_hash == EDEN_DSMOD_EXT_HASH && ext.mailbox_size >= 4096 && ext.load_u32 &&
                ext.store_u32;
  ext_ = ext;
  requested_ = 0;
}
MemberStats NativeStats::Poll(uint32_t member_id) {
  std::lock_guard lock(mutex_);
  return Fresh(member_id);
}
MemberStats NativeStats::Fresh(uint32_t member_id) {
  MemberStats out;
  if (!configured_ || !member_id)
    return out;
  if (requested_ != member_id) {
    bool ok = true;
    for (std::size_t i = 0; i < SelectorArgs.size(); ++i)
      ok = ok && Store(Field(offsetof(Ot0StatsMailbox, args) + 4 * i), static_cast<uint32_t>(SelectorArgs[i]));
    ok = ok && Store(Field(offsetof(Ot0StatsMailbox, exclude_weapon)), 1);
    if (ok && Store(Field(offsetof(Ot0StatsMailbox, request_id)), member_id))
      requested_ = member_id;
  }
  uint32_t version{}, result{}, stamp{}, heartbeat{};
  if (!Load(Field(offsetof(Ot0StatsMailbox, version)), version) || version != OT0_STATS_VERSION ||
      !Load(Field(offsetof(Ot0StatsMailbox, result_id)), result) || result != member_id ||
      !Load(Field(offsetof(Ot0StatsMailbox, stamp)), stamp) || !Load(0, heartbeat) ||
      uint32_t(heartbeat - stamp) > 120)
    return out;
  std::array<int32_t, 8> native{};
  for (std::size_t i = 0; i < 8; ++i) {
    uint32_t v{};
    if (!Load(Field(offsetof(Ot0StatsMailbox, values) + 4 * i), v))
      return out;
    native[i] = static_cast<int32_t>(v);
  }
  uint32_t again{}, stamp_again{};
  if (!Load(Field(offsetof(Ot0StatsMailbox, result_id)), again) || again != member_id ||
      !Load(Field(offsetof(Ot0StatsMailbox, stamp)), stamp_again) || stamp_again != stamp)
    return out; // retargeted while reading
  for (std::size_t i = 0; i < 8; ++i)
    out.values[i] = native[static_cast<std::size_t>(StatusOrder[i] - 9)];
  out.id = member_id;
  out.valid = true;
  return out;
}
} // namespace ot0
