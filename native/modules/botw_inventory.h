// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "botw_pouch.h"
#include <string_view>
namespace botw {
inline int inventory_category(int type) {
  constexpr int categories[]={0,1,1,2,3,3,3,4,5,6};
  return type>=0 && type<10 ? categories[type] : -1;
}
inline std::vector<const Item*> inventory_items(const Snapshot& pouch,int category) {
  std::vector<const Item*> out;
  if (pouch.valid && category>=0 && category<7)
    for (const auto& item:pouch.items)
      if (inventory_category(item.type)==category) out.push_back(&item);
  return out;
}
inline int item_quantity(const Item& item) {
  return (item.type==2 || item.type==7 || item.type==8) && item.value>=0 && item.value<=999999 ? item.value : -1;
}
inline int equipped_arrows(const Snapshot& pouch) {
  if (!pouch.valid) return -1;
  const Item* selected{};
  for (const auto& item:pouch.items) if (item.type==2 && item.equipped) {
    if (selected) return -1;
    selected=&item;
  }
  return selected ? item_quantity(*selected) : -1;
}
// The game's own display order (CRITIQUE-3, 20261018 fix round B), live-confirmed on save A
// (debug instrumentation, 320/320 pouch instances recovered from a single column's chain, see
// milestone REVIEW "Fix round B"). pouch() in botw_pouch.h already validates this is one shared
// intrusive doubly-linked list (sentinel manager+0x68, "+8"-biased next at node+8/prev at
// node+16) and walks it forward (next) from the sentinel. manager+0x44230 holds up to 50 entry
// points into that SAME list (column[1] live-measured as an alias back into column[0]'s already
// fully-walked chain, not a disjoint category list -- the 50-column removal-lookup search in
// botw_inventory_actions.h only ever needed to find one of several aliasing entry points, never
// proving them disjoint). Walking one non-empty entry point through "prev" instead of "next"
// recovers every pouch instance exactly once in the game's own order (the exact reverse of
// pouch()'s own "next" order on every category CRITIQUE-3 checked). This is a reader only, never
// writes memory, and fails closed (empty/invalid) on any inconsistency so callers fall back to
// the cached pouch order instead of showing a partially-reordered or duplicated grid.
inline Snapshot native_order(const EdenDsmodHostApi &h, const Snapshot &pouch) {
  Snapshot out;
  if (!inventory_supported(h) || !pouch.valid || pouch.items.empty() || pouch.items.size() > 420)
    return out;
  uint64_t manager{}, vt{};
  if (!read(h, h.main_base + inventory_offsets(h).pouch_root, manager) || manager % 8 ||
      manager > UINT64_MAX - 0x44230 - 50 * 8 || !read(h, manager, vt) ||
      vt != h.main_base + inventory_offsets(h).pouch_vtable)
    return out;
  const uint64_t sentinel = manager + 0x68;
  std::vector<Item> ordered;
  std::vector<bool> used(pouch.items.size(), false);
  auto index_of = [&](uint64_t addr) -> int {
    for (size_t i = 0; i < pouch.items.size(); ++i)
      if (pouch.items[i].address == addr)
        return int(i);
    return -1;
  };
  // Try entry points in order and stop at the first one whose walk recovers the whole pouch; a
  // later entry is only reached if an earlier one is empty or does not cover everything (a
  // scheme change on a different build), never to append to an already-complete result -- the
  // live case showed later entries can alias back into the same chain and must not be re-walked.
  for (unsigned col = 0; col < 50 && ordered.size() < pouch.items.size(); ++col) {
    uint64_t node{};
    if (!read(h, manager + 0x44230 + col * 8, node))
      return {};
    if (!node)
      continue;
    std::vector<Item> chain;
    std::vector<bool> chain_used(pouch.items.size(), false);
    for (unsigned steps = 0; node && steps <= pouch.items.size(); ++steps) {
      const int i = index_of(node);
      if (i < 0 || chain_used[size_t(i)])
        return {}; // unknown instance, or a cycle within this entry point's own chain
      chain_used[size_t(i)] = true;
      chain.push_back(pouch.items[size_t(i)]);
      uint64_t prev{};
      if (!read(h, node + 16, prev))
        return {};
      node = prev == sentinel ? 0 : prev >= 8 ? prev - 8 : 0;
    }
    if (node)
      return {}; // walked more nodes than the pouch has instances without reaching the sentinel
    if (chain.size() == pouch.items.size()) {
      ordered = std::move(chain);
      break;
    }
    // A short chain from an empty or partial entry point: keep looking, but only if none of its
    // nodes were already placed by a previous (also partial) entry point -- true disjoint
    // category lists would merge cleanly; an alias back into prior nodes fails closed instead of
    // silently duplicating or dropping items.
    for (size_t i = 0; i < pouch.items.size(); ++i)
      if (chain_used[i] && used[i])
        return {};
    for (size_t i = 0; i < pouch.items.size(); ++i)
      if (chain_used[i])
        used[i] = true;
    ordered.insert(ordered.end(), chain.begin(), chain.end());
  }
  if (ordered.size() != pouch.items.size())
    return {}; // every cached pouch instance must appear exactly once
  out.valid = true;
  out.items = std::move(ordered);
  return out;
}
inline int64_t actor_key(std::string_view actor) {
  if (actor.empty()) return -1;
  uint32_t key=2166136261u;
  for (const auto c:actor) key=(key^uint8_t(c))*16777619u;
  return key;
}
// Same FNV-1a as actor_key(), but hashes "a" + sep + "b" without heap-allocating the
// concatenated std::string first (cleanup: this used to be actor_key(a+"_"+b) on quest
// journal rows, which allocated once per row per sample while the quest page or ready
// pane quest card was shown). -1 only when both parts are empty, matching actor_key("").
inline int64_t actor_key2(std::string_view a,char sep,std::string_view b) {
  if (a.empty() && b.empty()) return -1;
  uint32_t key=2166136261u;
  for (const auto c:a) key=(key^uint8_t(c))*16777619u;
  key=(key^uint8_t(sep))*16777619u;
  for (const auto c:b) key=(key^uint8_t(c))*16777619u;
  return key;
}
// Selection follows instance identity, including duplicate actors. Never carries
// a deleted instance into a replacement slot and never mutates guest inventory.
struct InventoryView {
  int category{};uint64_t selected{};
  void update(const Snapshot& pouch) {
    const auto items=inventory_items(pouch,category);
    for (const auto* item:items) if (item->address==selected && selected) return;
    selected=items.empty() ? 0 : items.front()->address;
  }
  bool choose(const Snapshot& pouch,int64_t index) {
    const auto items=inventory_items(pouch,category);
    if(index<0 || uint64_t(index)>=items.size()) return false;
    selected=items[size_t(index)]->address;return selected!=0;
  }
};
} // namespace botw
