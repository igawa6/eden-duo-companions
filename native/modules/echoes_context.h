// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_reader.h"
#include <string>
namespace echoes {
// Observations of native UI only. FieldHud is not gameplay or input permission.
// Dialogue/pause/world transitions without a verified marker remain unresolved.
enum class NativeContext { Unknown, Title, NativeMenu, FieldHud };
struct ContextSnapshot {
  bool compatible{}, valid{};
  NativeContext context{NativeContext::Unknown};
  uint64_t hud_owner{};
  size_t registry_count{};
  // Passive opening-scene presentation only. Must also join authenticated
  // OP_Dungeon resource scope; never authorizes ordinary pages or input.
  bool prologue_hud{};
  // Read-only presentation with an authenticated settled Field HUD. Unknown
  // extra registry components still keep context Unknown and cannot authorize
  // input/equip/travel; they must not replace the entire companion with a logo.
  bool field_presentation{};
};
// No guest writes/calls. The callback must reject unreadable ranges. Two
// complete registry/state samples must agree; no collection ownership
// establishes context.
std::string DescribeContext(const ReadFn&, uint64_t main_base);
ContextSnapshot ReadContext(const ReadFn &, uint64_t main_base,
                            bool build_matches);
} // namespace echoes
