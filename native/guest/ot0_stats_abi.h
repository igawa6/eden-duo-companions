// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
// Field status values (ABI3), companion mailbox+2304. The host writes request_id (a party member
// id); in field state 23 the helper fills the native Status values for that member through the
// game's own stat selector (main+0x3a7ede0, research analysis/20261031-field-stats) about every
// 15 frames, then publishes result_id last. Effect-free for the game's state.
enum { OT0_STATS_VERSION = 4, OT0_STATS_OFFSET = 2304 };
typedef struct Ot0StatsMailbox {
  uint32_t version, request_id, result_id, stamp;
  // Native stat ids 9..16 in this order.
  int32_t values[8];
  // Selector arguments after (member, stat), in call order: w2..w7 and the stacked byte
  // (main+0x3a7ede0 call sites, e.g. the level-up comparison at main+0x3c59a84).
  int32_t args[7];
  // Nonzero: subtract the selected weapon slot's own item bonus (main+0x3a76c64 slot item,
  // main+0x39be1a8 per-item bonus). The native Status page shows stats without weapons.
  uint32_t exclude_weapon;
} Ot0StatsMailbox;
#if defined(__cplusplus)
static_assert(sizeof(Ot0StatsMailbox) == 80);
#else
_Static_assert(sizeof(Ot0StatsMailbox) == 80, "stats ABI size");
#endif
