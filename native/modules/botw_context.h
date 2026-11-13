// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_pouch.h"
namespace botw {
// Observed UI signatures, not simulation/pause or action-readiness guarantees.
enum class UiContext { Unknown, Title, Transition, World, Menu };
struct UiSample {
  uint64_t manager{}, screens{}, manager_vtable{}, screens_vtable{}, flags{}, array{};
  int32_t count{};
  std::array<uint64_t, 3> slots{};
  bool operator==(const UiSample &) const = default;
};
inline bool ui_sample(const EdenDsmodHostApi &h, UiSample &s) {
  if (!inventory_supported(h))return false;
  const uint64_t End = inventory_offsets(h)->screen_root+8;
  if (!inventory_supported(h) || h.main_size < End || h.main_base > UINT64_MAX - End)
    return false;
  if (!read(h, h.main_base + inventory_offsets(h)->ui_root, s.manager) ||
      !read(h, h.main_base + inventory_offsets(h)->screen_root, s.screens) ||
      s.manager % 8 || s.screens % 8 || s.manager > UINT64_MAX - 0x64C38 ||
      s.screens > UINT64_MAX - 0x38 ||
      !read(h, s.manager, s.manager_vtable) ||
      !read(h, s.screens, s.screens_vtable) ||
      s.manager_vtable != h.main_base + inventory_offsets(h)->ui_vtable ||
      s.screens_vtable != h.main_base + inventory_offsets(h)->screens_vtable ||
      !read(h, s.manager + 0x64C30, s.flags) ||
      !read(h, s.screens + 0x28, s.count) || s.count != 99 ||
      !read(h, s.screens + 0x30, s.array) || s.array % 8 ||
      !read(h, s.array, s.slots))
    return false;
  return true;
}
inline UiContext context(const EdenDsmodHostApi &h) {
  UiSample a{}, b{};
  if (!ui_sample(h, a) || !ui_sample(h, b) || a != b)
    return UiContext::Unknown;
  // Whole-signature allowlist deliberately fails closed for unobserved states.
  if (a.flags == 0x1849000000ULL && a.slots[1] && !a.slots[2])
    return UiContext::Title;
  if (!a.slots[1] && a.slots[2]) {
    if (a.flags == 0x0E09E00000ULL)
      return UiContext::Transition;
    if (a.flags == 0x1E09000000ULL)
      return UiContext::World;
    if (a.flags == 0x1E09000004ULL)
      return UiContext::Menu;
  }
  return UiContext::Unknown;
}
inline bool inventory_context(UiContext c) {
  return c == UiContext::World || c == UiContext::Menu;
}
inline const char *context_text(UiContext c) {
  switch (c) {
  case UiContext::Title: return "Title / save selection";
  case UiContext::Transition: return "Loading";
  case UiContext::World: return "World view";
  case UiContext::Menu: return "Game menu open";
  default: return "Waiting for game context";
  }
}
} // namespace botw
