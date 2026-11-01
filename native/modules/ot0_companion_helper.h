// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../guest/ot0_companion_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "ot0_battle_stats.h"
#include "ot0_minimap.h"
#include <chrono>
#include <mutex>
namespace ot0 {
// All mailbox traffic uses the host's atomics; guest-private tail is untouched.
class NativeCompanionHelper {
public:
  struct State {
    uint64_t mailbox{}, mailbox_size{};
    uint32_t heartbeat{}, epoch{}, version{}, app_state{}, native_visibility{},
        query_result{};
    bool live{}, hidden{}, status_known{};
  };
  ~NativeCompanionHelper();
  void Configure(const EdenDsmodHostExtensions &);
  State Poll();
  void SetMapLease(const ReadMemory &, uint64_t main, int64_t delta,
                   const MinimapIcons *, bool hide);
  BattleStatsSnapshot Query(const ReadMemory &, uint64_t main, int64_t delta,
                            const BattleStatsSnapshot &,
                            uint32_t desiredPartyId, int desiredEnemyIndex);
  void Stop();

private:
  struct Request {
    uint32_t seq{}, epoch{}, heartbeat{}, mask{};
    uint64_t bm{};
    std::array<Ot0NativeStatRow, OT0_COMPANION_ROWS> rows{};
  };
  struct Cached {
    Ot0NativeStatRow row{};
    uint32_t epoch{}, heartbeat{};
    uint64_t bm{};
    bool valid{};
  };
  bool Load32(uint32_t, uint32_t &) const;
  bool Load64(uint32_t, uint64_t &) const;
  bool Store32(uint32_t, uint32_t) const;
  bool Store64(uint32_t, uint64_t) const;
  State PollLocked();
  void StopLocked();
  bool CommitLease(bool, uint64_t resource = 0, uint64_t section = 0,
                   uint32_t map = 0, uint64_t root = 0, uint32_t root_index = 0,
                   uint32_t root_serial = 0, uint64_t canvas = 0,
                   uint32_t canvas_index = 0, uint32_t canvas_serial = 0);
  std::mutex mutex_;
  EdenDsmodHostExtensions ext_{};
  State state_{};
  bool configured_{}, advanced_{}, have_sample_{};
  uint32_t last_heartbeat_{}, last_epoch_{}, lease_seq_{}, query_seq_{};
  std::chrono::steady_clock::time_point last_advance_{};
  Ot0CompanionMailbox status_{};
  Request pending_{};
  std::array<Cached, OT0_COMPANION_ROWS> cache_{};
};
} // namespace ot0
