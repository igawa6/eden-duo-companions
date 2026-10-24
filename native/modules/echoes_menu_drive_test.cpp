// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_menu_drive.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace echoes;
namespace {
size_t checks{};
void Check(bool v, const char *message) {
  ++checks;
  if (!v) {
    std::cerr << "FAIL " << checks << ": " << message << '\n';
    std::abort();
  }
}
struct Fixture {
  MenuSnapshot menu;
  InventorySnapshot inventory;
  ContextSnapshot context;
  std::vector<MenuPulse> pulses;
  MenuKind desired{MenuKind::Equipment};
  size_t quantity_index{};
  bool deferred_outfit{true};
  uint16_t queued_outfit{};
  Fixture() {
    menu.compatible = true;
    inventory.compatible = inventory.inventory_valid = true;
    inventory.source = {0x1000, 0x2000};
    inventory.acquired_accessories = 15;
    inventory.acquired_outfits = 3;
    inventory.equipped_outfit = 1;
    inventory.accessory_capacity = 3;
    inventory.equipped_accessory_count = 1;
    inventory.equipped_accessories[0] = 1;
    inventory.ingredients[0] = 3;
    inventory.ingredients[1] = 2;
    context.compatible = context.valid = true;
    context.context = NativeContext::FieldHud;
  }
  void Equipment() {
    menu = {};
    menu.compatible = menu.valid = true;
    menu.kind = MenuKind::Equipment;
    menu.component = 0x3000;
    menu.outer_state = 2;
    menu.nested_state = 0;
    menu.row_starts = {1, 2};
    menu.row = 0;
    menu.column = 0;
    menu.selected_index = 4;
    menu.visible_cursor = 0;
    menu.entries.resize(12);
    for (size_t i = 0; i < 4; ++i)
      menu.entries[i].placeholder = true;
    for (size_t i = 0; i < 3; ++i) {
      auto &e = menu.entries[4 + i];
      e.kind = MenuEntryKind::Accessory;
      e.mask = 1u << i;
      e.owned = true;
      e.equipped = i == 0;
    }
    menu.entries[7] = {MenuEntryKind::Outfit, 2, 0, 0, true, false, false};
    menu.entries[8] = {MenuEntryKind::Accessory, 8, 0, 0, true, false, false};
  }
  void Items() {
    menu = {};
    menu.compatible = menu.valid = true;
    menu.kind = MenuKind::Items;
    menu.component = 0x4000;
    menu.outer_state = 2;
    menu.nested_state = 1;
    menu.selected_index = menu.visible_cursor = 0;
    menu.entries.resize(16);
    for (size_t i = 0; i < 16; ++i) {
      auto &e = menu.entries[i];
      e.kind = MenuEntryKind::Item;
      e.item_id = 212 + uint32_t(i);
      e.quantity = inventory.ingredients[i];
      e.owned = e.quantity > 0;
      e.raw_action2a = 1;
    }
  }
  void Open(MenuKind kind) {
    context.context = NativeContext::NativeMenu;
    if (kind == MenuKind::Equipment)
      Equipment();
    else
      Items();
  }
  void SetEquipped(uint32_t masks) {
    inventory.equipped_accessories = {};
    inventory.equipped_accessory_count = 0;
    for (uint32_t b = 1; b; b <<= 1)
      if (masks & b)
        inventory.equipped_accessories[inventory.equipped_accessory_count++] =
            b;
    for (auto &e : menu.entries)
      if (e.kind == MenuEntryKind::Accessory)
        e.equipped = (masks & e.mask) != 0;
  }
  void Apply(MenuPulse p) {
    if (p == MenuPulse::None)
      return;
    pulses.push_back(p);
    if (p == MenuPulse::Plus) {
      context.context = NativeContext::NativeMenu;
      menu.valid = false;
      menu.kind = MenuKind::None;
      return;
    }
    if (p == MenuPulse::R) {
      Open(desired);
      return;
    }
    if (p == MenuPulse::B) {
      if (queued_outfit)
        inventory.equipped_outfit = queued_outfit;
      context.context = NativeContext::FieldHud;
      menu.valid = false;
      return;
    }
    if (p == MenuPulse::A) {
      auto &e = menu.entries[size_t(menu.selected_index)];
      if (menu.kind == MenuKind::Items) {
        if (menu.nested_state == 1)
          menu.nested_state = 2;
        else {
          --inventory.ingredients[quantity_index];
          --e.quantity;
          e.owned = e.quantity > 0;
          menu.nested_state = 1;
        }
      } else if (e.kind == MenuEntryKind::Accessory) {
        uint32_t old{};
        for (auto v : inventory.equipped_accessories)
          old |= v;
        SetEquipped(old ^ e.mask);
      } else {
        if (deferred_outfit)
          queued_outfit = uint16_t(e.mask);
        else
          inventory.equipped_outfit = uint16_t(e.mask);
        for (auto &other : menu.entries)
          if (other.kind == MenuEntryKind::Outfit)
            other.equipped = other.mask == e.mask;
      }
      return;
    }
    if (menu.kind == MenuKind::Equipment) {
      if (p == MenuPulse::Down)
        ++menu.row;
      if (p == MenuPulse::Up)
        --menu.row;
      if (p == MenuPulse::Left)
        --menu.column;
      if (p == MenuPulse::Right)
        ++menu.column;
      menu.selected_index =
          int32_t(menu.row_starts[size_t(menu.row)] * 4) + menu.column;
      menu.visible_cursor = menu.row * 4 + menu.column;
    } else {
      menu.selected_index += (p == MenuPulse::Down   ? 8
                              : p == MenuPulse::Up   ? -8
                              : p == MenuPulse::Left ? -1
                                                     : 1);
      menu.visible_cursor = menu.selected_index;
    }
  }
  MenuPulse Step(MenuDrive &d, uint64_t tick) {
    const auto p = d.Advance(menu, inventory, context, tick);
    Apply(p);
    return p;
  }
  void Run(MenuDrive &d, uint64_t first = 1, uint64_t last = 300) {
    uint64_t prior{};
    MenuPulse prior_pulse{MenuPulse::None};
    for (uint64_t t = first;
         t <= last && d.status() == MenuDriveStatus::Pending; ++t) {
      const auto p = Step(d, t);
      if (p != MenuPulse::None) {
        if (prior)
          Check(t - prior >= (prior_pulse == MenuPulse::R
                                 ? MenuDrive::TabTicks
                                 : prior_pulse == MenuPulse::A
                                       ? MenuDrive::ActionTicks
                                 : (prior_pulse == MenuPulse::Up ||
                                    prior_pulse == MenuPulse::Down ||
                                    prior_pulse == MenuPulse::Left ||
                                    prior_pulse == MenuPulse::Right)
                                       ? MenuDrive::NavigationTicks
                                 : MenuDrive::StepTicks),
                "every emitted input has a neutral/readback interval");
        prior = t;
        prior_pulse = p;
      }
    }
  }
  size_t Count(MenuPulse p) const {
    return size_t(std::count(pulses.begin(), pulses.end(), p));
  }
};
} // namespace
int main() {
  {
    Fixture f;
    f.Open(MenuKind::Equipment);
    MenuDrive d;
    Check(!d.RequestEquipment(2, true, true, f.menu, f.inventory, f.context, 0) &&
              d.abort_reason() == MenuDriveAbort::Unsupported &&
              d.Advance(f.menu, f.inventory, f.context, 1) == MenuPulse::None,
          "outfit refuses user-owned pause before queuing a native change");
  }
  {
    Fixture f;f.Open(MenuKind::Equipment);MenuDrive d;
    Check(d.RequestEquipment(2,false,true,f.menu,f.inventory,f.context,0),"display-only guard starts from known native menu");
    f.context.context=NativeContext::Unknown;f.context.field_presentation=true;
    Check(d.Advance(f.menu,f.inventory,f.context,1)==MenuPulse::None && d.status()==MenuDriveStatus::Pending,
          "display-only permission waits silently for native action authority");
    Check(d.Advance(f.menu,f.inventory,f.context,122)==MenuPulse::None && d.status()==MenuDriveStatus::Aborted,
          "display-only permission cannot bypass bounded unknown-state timeout");
  }
  for (int fault = 0; fault < 5; ++fault) {
    Fixture f;
    MenuDrive d;
    Check(d.RequestEquipment(2, true, true, f.menu, f.inventory, f.context, 0),
          "deferred outfit commit fixture");
    uint64_t action{};
    for (uint64_t t = 1; t < 200 && !action; ++t)
      if (f.Step(d, t) == MenuPulse::A)
        action = t;
    Check(action && f.inventory.equipped_outfit == 1 &&
              f.menu.entries[7].equipped,
          "native outfit flag changes before authoritative inventory commit");
    for (uint64_t t = action + 1; t < action + MenuDrive::ActionTicks; ++t)
      Check(f.Step(d, t) == MenuPulse::None && f.Count(MenuPulse::B) == 0,
            "queued outfit cannot close before action animation settles");
    if (fault == 0)
      ++f.inventory.source.state;
    if (fault == 1)
      f.inventory.equipped_outfit = 2;
    if (fault == 2) {
      f.menu.entries[8].kind = MenuEntryKind::Outfit;
      f.menu.entries[8].mask = 1;
      f.menu.entries[8].equipped = true;
    }
    if (fault == 3)
      --f.menu.selected_index;
    if (fault == 4)
      f.context.context = NativeContext::FieldHud;
    const uint64_t settled = action + MenuDrive::ActionTicks;
    if (fault < 5) {
      Check(d.Advance(f.menu, f.inventory, f.context, settled) == MenuPulse::None &&
                d.Advance(f.menu, f.inventory, f.context, settled + 1) == MenuPulse::None &&
                d.status() == MenuDriveStatus::Aborted && f.Count(MenuPulse::B) == 0,
            "source/external inventory/multiple flags/selection/early close abort without B");
    } else {
      Check(d.Advance(f.menu, f.inventory, f.context, settled) == MenuPulse::None &&
                d.Advance(f.menu, f.inventory, f.context, settled + 1) == MenuPulse::B,
            "stable native selected outfit authorizes exactly one owned close");
      Check(d.Advance(f.menu, f.inventory, f.context, settled + 2) == MenuPulse::None &&
                d.status() == MenuDriveStatus::Pending,
            "close pulse alone cannot report outfit success");
      f.context.context = NativeContext::FieldHud;
      Check(d.Advance(f.menu, f.inventory, f.context, settled + 3) == MenuPulse::None &&
                d.status() == MenuDriveStatus::Pending,
            "FieldHUD with old inventory cannot report outfit success");
      f.inventory.equipped_outfit = 2;
      Check(d.Advance(f.menu, f.inventory, f.context, settled + 4) == MenuPulse::None &&
                d.status() == MenuDriveStatus::Complete,
            "FieldHUD and exact authoritative outfit commit complete action");
    }
  }
  {
    Fixture f;
    MenuDrive d;
    Check(d.RequestEquipment(2, true, true, f.menu, f.inventory, f.context, 0),
          "outfit cursor animation request");
    Check(f.Step(d, 1) == MenuPulse::Plus &&
              f.Step(d, 7) == MenuPulse::R,
          "outfit owns the pause opened from field");
    Check(f.Step(d, 37) == MenuPulse::Right && f.menu.selected_index == 5,
          "native cursor becomes readable immediately after movement");
    for (uint64_t t = 38; t < 37 + MenuDrive::NavigationTicks; ++t)
      Check(f.Step(d, t) == MenuPulse::None,
            "immediate cursor readback cannot bypass navigation settle");
    f.Run(d, 37 + MenuDrive::NavigationTicks);
    Check(d.status() == MenuDriveStatus::Complete &&
              f.inventory.equipped_outfit == 2 &&
              f.Count(MenuPulse::Right) == 3 && f.Count(MenuPulse::A) == 1,
          "settled outfit navigation confirms once without movement retries");
  }
  {
    Fixture f;
    MenuDrive d;
    Check(d.RequestEquipment(2, true, true, f.menu, f.inventory, f.context, 0),
          "outfit race request accepted while still on field hud");
    // Simulate the native menu opening by something other than this drive
    // in the gap between RequestEquipment() and the first Advance(): on the
    // very first tick, context already reports NativeMenu with the target
    // outfit ready to select, yet opened_ was never set.
    f.Open(MenuKind::Equipment);
    Check(d.Advance(f.menu, f.inventory, f.context, 1) == MenuPulse::None &&
              d.status() == MenuDriveStatus::Aborted &&
              d.abort_reason() == MenuDriveAbort::ContextChanged &&
              f.Count(MenuPulse::A) == 0 && f.pulses.empty(),
          "menu already open before the first Advance aborts the outfit "
          "request without ever pressing A");
  }
  {
    Fixture f;
    f.Open(MenuKind::Equipment);
    MenuDrive d;
    Check(d.RequestConsume(212, 0, f.menu, f.inventory, f.context, 0),
          "delayed header transition request");
    Check(d.Advance(f.menu, f.inventory, f.context, 1) == MenuPulse::R,
          "first unmatched header advances once");
    for (uint64_t t = 2; t <= MenuDrive::TabTicks; ++t)
      Check(d.Advance(f.menu, f.inventory, f.context, t) == MenuPulse::None,
            "header animation cannot cause another input before thirty ticks");
    f.Open(MenuKind::Items);
    f.desired = MenuKind::Items;
    f.Run(d, MenuDrive::TabTicks + 1);
    Check(d.status() == MenuDriveStatus::Complete &&
              f.Count(MenuPulse::R) == 0 && f.Count(MenuPulse::A) == 2 &&
              f.inventory.ingredients[0] == 2,
          "settled target tab consumes once without another header advance");
  }
  {
    Fixture f;
    MenuDrive d;
    Check(d.RequestEquipment(2, false, true, f.menu, f.inventory, f.context, 0),
          "equipment request from field");
    f.Run(d);
    Check(d.status() == MenuDriveStatus::Complete &&
              f.Count(MenuPulse::Plus) == 1 && f.Count(MenuPulse::R) == 1 &&
              f.Count(MenuPulse::Right) == 1 && f.Count(MenuPulse::A) == 1 &&
              f.Count(MenuPulse::B) == 1,
          "field equip navigates, confirms once and closes owned pause");
    Check(f.inventory.equipped_accessory_count == 2, "equip readback observed");
  }
  {
    Fixture f;
    f.Open(MenuKind::Equipment);
    f.inventory.accessory_capacity = 1;
    MenuDrive d;
    Check(
        d.RequestEquipment(1, false, false, f.menu, f.inventory, f.context, 0),
        "remove is permitted at full capacity");
    f.Run(d);
    Check(d.status() == MenuDriveStatus::Complete &&
              f.inventory.equipped_accessory_count == 0 &&
              f.Count(MenuPulse::A) == 1 && f.Count(MenuPulse::Plus) == 0 &&
              f.Count(MenuPulse::B) == 0,
          "already-open menu remains open after remove");
  }
  {
    Fixture f;
    MenuDrive d;
    Check(d.RequestEquipment(2, true, true, f.menu, f.inventory, f.context, 0),
          "outfit request");
    f.Run(d);
    Check(d.status() == MenuDriveStatus::Complete &&
              f.inventory.equipped_outfit == 2 && f.Count(MenuPulse::A) == 1,
          "outfit uses native equip and readback");
  }
  {
    Fixture f;
    f.Open(MenuKind::Equipment);
    MenuDrive d;
    Check(d.RequestEquipment(8, false, true, f.menu, f.inventory, f.context, 0),
          "equipment in another native row");
    f.Run(d);
    Check(d.status() == MenuDriveStatus::Complete &&
              f.Count(MenuPulse::Down) == 1,
          "native row mapping drives vertical readback");
  }
  for (bool last_item : {false, true}) {
    Fixture f;
    f.desired = MenuKind::Items;
    if (last_item)
      f.inventory.ingredients[0] = 1;
    f.Open(MenuKind::Items);
    const auto count = f.inventory.ingredients[0];
    MenuDrive d;
    Check(d.RequestConsume(212, 0, f.menu, f.inventory, f.context, 0),
          "edible item request");
    f.Run(d);
    Check(
        d.status() == MenuDriveStatus::Complete && f.Count(MenuPulse::A) == 2 &&
            f.inventory.ingredients[0] + 1 == count &&
            f.Count(MenuPulse::B) == 0,
        "consume opens dialog then confirms exactly once including final item");
  }
  {
    Fixture f;
    f.desired = MenuKind::Items;
    f.quantity_index = 6;
    f.inventory.ingredients[6] = 39;
    MenuDrive d;
    Check(d.RequestConsume(218, 6, f.menu, f.inventory, f.context, 0),
          "Milk Drink request from field");
    uint64_t opened_dialog{}, confirmed{}, closed{};
    for (uint64_t t = 1;
         t <= 300 && d.status() == MenuDriveStatus::Pending; ++t) {
      const auto p = f.Step(d, t);
      if (p == MenuPulse::A) {
        if (!opened_dialog)
          opened_dialog = t;
        else
          confirmed = t;
      }
      if (p == MenuPulse::B)
        closed = t;
      if (opened_dialog && t < opened_dialog + MenuDrive::ActionTicks)
        Check(!confirmed && p != MenuPulse::B,
              "rapid nested2 readback cannot bypass dialog animation settle");
      if (confirmed && t < confirmed + MenuDrive::ActionTicks)
        Check(!closed,
              "immediate quantity decrement cannot bypass commit settle");
    }
    Check(d.status() == MenuDriveStatus::Complete && opened_dialog &&
              confirmed >= opened_dialog + MenuDrive::ActionTicks &&
              closed >= confirmed + MenuDrive::ActionTicks &&
              f.inventory.ingredients[6] == 38 && f.Count(MenuPulse::A) == 2,
          "Drink settles dialog and commit before closing its owned pause");
  }
  {
    Fixture f;
    f.desired = MenuKind::Items;
    f.inventory.ingredients[11] = 2;
    f.quantity_index = 11;
    f.Open(MenuKind::Items);
    MenuDrive d;
    Check(d.RequestConsume(223, 11, f.menu, f.inventory, f.context, 0),
          "second ingredient row request");
    f.Run(d);
    Check(d.status() == MenuDriveStatus::Complete &&
              f.Count(MenuPulse::Down) == 1 && f.Count(MenuPulse::Right) == 3,
          "ingredient navigation uses eight columns");
  }
  {
    Fixture f;
    f.desired = MenuKind::Items;
    MenuDrive d;
    Check(d.RequestConsume(212, 0, f.menu, f.inventory, f.context, 0),
          "consumption from field");
    f.Run(d);
    Check(d.status() == MenuDriveStatus::Complete &&
              f.Count(MenuPulse::A) == 2 && f.Count(MenuPulse::B) == 1,
          "consume closes only planner-opened pause after decrement");
  }
  {
    Fixture f;
    MenuDrive d;
    f.inventory.accessory_capacity = 1;
    Check(
        !d.RequestEquipment(2, false, true, f.menu, f.inventory, f.context, 0),
        "full capacity refuses new equip");
    Check(
        !d.RequestEquipment(3, false, true, f.menu, f.inventory, f.context, 0),
        "multi-bit gear target refused");
    Check(
        !d.RequestEquipment(1, true, false, f.menu, f.inventory, f.context, 0),
        "outfit remove refused");
    Check(!d.RequestConsume(212, 16, f.menu, f.inventory, f.context, 0),
          "out-of-range quantity index refused");
  }
  {
    Fixture f;
    f.Open(MenuKind::Items);
    f.menu.entries[0].raw_action2a = 0;
    MenuDrive d;
    Check(!d.RequestConsume(212, 0, f.menu, f.inventory, f.context, 0) &&
              f.pulses.empty(),
          "native ineligible ingredient never gains consume confirmation");
  }
  {
    Fixture f;
    f.Open(MenuKind::Equipment);
    MenuDrive d;
    Check(
        d.RequestEquipment(1, false, true, f.menu, f.inventory, f.context, 0) &&
            d.status() == MenuDriveStatus::Complete,
        "already equipped no-op needs no input");
  }
  {
    Fixture f;
    MenuDrive d;
    Check(d.RequestEquipment(2, false, true, f.menu, f.inventory, f.context, 0),
          "unknown-transition fixture request");
    f.context.context = NativeContext::Unknown;
    for (uint64_t t = 1; t < 125; ++t)
      Check(f.Step(d, t) == MenuPulse::None, "unknown context emits no input");
    Check(d.status() == MenuDriveStatus::Aborted &&
              d.abort_reason() == MenuDriveAbort::Timeout,
          "unknown wait is bounded");
  }
  {
    Fixture f;
    f.Open(MenuKind::Equipment);
    MenuDrive d;
    Check(d.RequestEquipment(2, false, true, f.menu, f.inventory, f.context, 0),
          "cursor fault fixture request");
    Check(d.Advance(f.menu, f.inventory, f.context, 1) == MenuPulse::Right,
          "first navigation input");
    f.menu.selected_index = 6;
    f.menu.column = 2;
    Check(d.Advance(f.menu, f.inventory, f.context, 31) == MenuPulse::None &&
              d.abort_reason() == MenuDriveAbort::UnexpectedCursor,
          "unexpected cursor readback cancels without confirm");
  }
  {
    Fixture f;
    f.Open(MenuKind::Items);
    MenuDrive d;
    Check(d.RequestConsume(212, 0, f.menu, f.inventory, f.context, 0),
          "dialog safety fixture request");
    f.Step(d, 1);
    Check(f.Step(d, 2) == MenuPulse::A, "single open-dialog pulse");
    size_t confirms = 0;
    for (uint64_t t = 3; t <= 200; ++t)
      confirms += d.Advance(f.menu, f.inventory, f.context, t) == MenuPulse::A;
    Check(d.status() == MenuDriveStatus::Aborted &&
              f.Count(MenuPulse::A) == 1 && confirms == 1,
          "missing inventory decrement never repeats consume confirmation");
  }
  for (int fault = 0; fault < 5; ++fault) {
    Fixture f;
    f.Open(MenuKind::Equipment);
    MenuDrive d;
    Check(d.RequestEquipment(2, false, true, f.menu, f.inventory, f.context, 0),
          "bounded source fixture request");
    if (fault == 0)
      ++f.inventory.source.state;
    if (fault == 1)
      f.context.context = NativeContext::Title;
    if (fault == 2)
      f.inventory.ingredients[0] = 2;
    if (fault == 3)
      f.menu.compatible = false;
    if (fault == 4) {
      d.Cancel();
    }
    Check(d.Advance(f.menu, f.inventory, f.context, 1) == MenuPulse::None &&
              d.status() == MenuDriveStatus::Aborted,
          "source/context/inventory/compatibility/cancel never emits input");
  }
  {
    Fixture f;
    MenuDrive d;
    Check(
        d.RequestEquipment(2, false, true, f.menu, f.inventory, f.context, 10),
        "clock fixture request");
    Check(d.Advance(f.menu, f.inventory, f.context, 9) == MenuPulse::None &&
              d.abort_reason() == MenuDriveAbort::Clock,
          "backwards clock abort");
  }
  {
    Fixture f;
    MenuDrive d;
    Check(d.RequestEquipment(2, false, true, f.menu, f.inventory, f.context, 0),
          "clock-gap fixture request");
    Check(d.Advance(f.menu, f.inventory, f.context, 121) == MenuPulse::None &&
              d.abort_reason() == MenuDriveAbort::Clock,
          "unobserved clock gap abort");
  }
  {
    Fixture f;
    MenuDrive d;
    Check(d.RequestConsume(212, 0, f.menu, f.inventory, f.context, 0),
          "bounded tab fixture request");
    f.Run(d);
    Check(d.status() == MenuDriveStatus::Aborted &&
              f.Count(MenuPulse::R) == 7 && f.Count(MenuPulse::A) == 0,
          "seven unmatched tabs abort without confirmation");
  }
  std::cout << checks << " menu action planner checks passed\n";
}
