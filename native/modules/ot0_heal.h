// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../guest/ot0_heal_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include <chrono>
#include <mutex>

namespace ot0 {
enum class HealPhase { Idle, Checking, Ready, Applying, Done, Unavailable };
struct HealState {
  HealPhase phase{HealPhase::Idle};
  uint32_t target{}, caster{}, skill{}, text_id{}, result{};
  int32_t cost{}, hp{}, max_hp{}, sp{}, after_hp{}, after_sp{};
  bool live{}, ready{};
  bool operator==(const HealState &) const = default;
};
// Mailbox-only client. Native eligibility and effects run on the game's Tick.
// Apply is explicit and one-shot: timeouts never retry a resource transaction.
// Find refuses while a request is pending, and briefly after withdrawal until
// a later native heartbeat proves previous query output writes have finished.
class NativeHeal {
public:
  ~NativeHeal();
  void Configure(const EdenDsmodHostExtensions &);
  HealState Poll();
  bool Find(uint32_t target);
  bool Apply();
  void Cancel();
  void Stop();

private:
  struct Pending {
    uint32_t seq{}, epoch{}, heartbeat{}, target{}, caster{}, skill{}, op{};
  };
  bool Load32(uint32_t, uint32_t &) const;
  bool Load64(uint32_t, uint64_t &) const;
  bool Store32(uint32_t, uint32_t) const;
  bool Store64(uint32_t, uint64_t) const;
  bool ReadBox(Ot0HealMailbox &) const;
  bool Publish(uint32_t op, const Ot0HealMailbox &, uint32_t epoch,
               uint32_t heartbeat);
  HealState PollLocked();
  void CancelLocked();
  void BeginDrainLocked();
  void RefuseLocked(uint32_t result);
  uint32_t NextSeq();
  std::mutex mutex_;
  EdenDsmodHostExtensions ext_{};
  bool configured_{}, have_sample_{}, advanced_{};
  bool draining_{}, drain_sample_{};
  uint32_t drain_heartbeat_{};
  uint32_t sequence_{}, last_heartbeat_{}, last_epoch_{};
  std::chrono::steady_clock::time_point last_advance_{};
  HealState state_{};
  Pending pending_{};
  Ot0HealMailbox offer_{};
};
} // namespace ot0
