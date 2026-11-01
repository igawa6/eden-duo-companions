// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
// Native SP-skill helper ABI2, separate area at companion mailbox+2048.
// Host writes request fields first and publishes seq last. Native ack is last.
// Loader owns epoch. Requests expire in game frames and bind to that epoch.
enum { OT0_HEAL_QUERY = 1, OT0_HEAL_APPLY = 2, OT0_HEAL_FIND = 3 };
enum {
  OT0_HEAL_QUERY_OK = 1,
  OT0_HEAL_BAD_REQUEST,
  OT0_HEAL_EXPIRED,
  OT0_HEAL_NOT_FIELD,
  OT0_HEAL_MENU_OPEN,
  OT0_HEAL_EVENT_PLAYING,
  OT0_HEAL_NOT_IN_PARTY,
  OT0_HEAL_NOT_LEARNED,
  OT0_HEAL_NOT_RECOVERY,
  OT0_HEAL_UNAFFORDABLE,
  OT0_HEAL_NO_TARGET,
  OT0_HEAL_NATIVE_UNAVAILABLE,
  OT0_HEAL_CONTROL_LOCKED,
  OT0_HEAL_MODAL_OPEN,
  OT0_HEAL_STALE,
  OT0_HEAL_APPLIED,
  OT0_HEAL_UNSUPPORTED
};
typedef struct Ot0HealMailbox {
  uint32_t heartbeat, epoch, op, seq, ack, result;
  uint32_t caster, skill, target;
  int32_t cost, target_hp, caster_sp, target_max_hp;
  uint32_t native_eligible, app_state, version;
  uint32_t expected_epoch, deadline;
  int32_t expected_hp, expected_sp, expected_cost, expected_max_hp;
  uint64_t expected_save, expected_avatar, expected_party_hash;
  int32_t result_hp, result_sp;
  uint32_t executed_seq, result_heartbeat;
  uint32_t skill_text_id, padding[3];
} Ot0HealMailbox;
#if defined(__cplusplus)
static_assert(sizeof(Ot0HealMailbox) == 144);
#else
_Static_assert(sizeof(Ot0HealMailbox) == 144, "helper ABI size");
#endif
