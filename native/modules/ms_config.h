// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Release modules omit hidden diagnostics and profiling. Enable explicitly for a local audit
// (-DEDEN_MS_DIAGNOSTICS=1): the reader's state line and map-symbol inventory, cache events,
// guest reads per refresh by state and module pictures rendered, the follower write watch, the
// helper's own counters every ~5 s, and the h_cache / h_detach actions (add those to a local
// manifest's actions).
#ifndef EDEN_MS_DIAGNOSTICS
#define EDEN_MS_DIAGNOSTICS 0
#endif

namespace ms {
inline constexpr bool Diagnostics = EDEN_MS_DIAGNOSTICS != 0;
} // namespace ms
