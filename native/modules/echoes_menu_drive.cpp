// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_menu_drive.h"
#include <algorithm>
#include <bit>
namespace echoes {
namespace {
uint32_t Equipped(const InventorySnapshot &s) {
  uint32_t value{};
  for (auto mask : s.equipped_accessories)
    value |= mask;
  return value;
}
bool PermittedContext(const ContextSnapshot &c) {
  return c.compatible && c.valid &&
         (c.context == NativeContext::FieldHud ||
          c.context == NativeContext::NativeMenu);
}
} // namespace
void MenuDrive::Abort(MenuDriveAbort reason) {
  status_ = MenuDriveStatus::Aborted;
  abort_ = reason;
  moving_ = false;
  stable_count_ = 0;
}
void MenuDrive::Cancel() { Abort(MenuDriveAbort::Cancelled); }
bool MenuDrive::Start(Kind kind, const MenuSnapshot &menu,
                      const InventorySnapshot &inventory,
                      const ContextSnapshot &context, uint64_t tick) {
  if (status_ == MenuDriveStatus::Pending)
    return false;
  if (!menu.compatible || !inventory.compatible || !inventory.inventory_valid ||
      !inventory.source.owner || !inventory.source.state ||
      !PermittedContext(context) || tick > UINT64_MAX - RequestTimeout) {
    Abort(MenuDriveAbort::InvalidRequest);
    return false;
  }
  if (context.context == NativeContext::NativeMenu && menu.valid &&
      ((menu.kind == MenuKind::Equipment && menu.nested_state != 0) ||
       (menu.kind == MenuKind::Items && menu.nested_state != 1))) {
    Abort(MenuDriveAbort::Unsupported);
    return false;
  }
  kind_ = kind;
  initial_ = inventory;
  phase_ = Phase::FindMenu;
  status_ = MenuDriveStatus::Pending;
  abort_ = MenuDriveAbort::None;
  requested_ = last_tick_ = next_pulse_ = wait_started_ = tick;
  unknown_wait_ = false;
  stable_count_ = 0;
  stable_tick_ = tick;
  opened_ = close_sent_ = nav_seen_ = moving_ = false;
  tabs_ = 0;
  component_ = 0;
  nav_index_ = -1;
  movement_ = MenuPulse::None;
  return true;
}
bool MenuDrive::RequestEquipment(uint32_t mask, bool outfit, bool desired,
                                 const MenuSnapshot &menu,
                                 const InventorySnapshot &inventory,
                                 const ContextSnapshot &context,
                                 uint64_t tick) {
  if (status_ == MenuDriveStatus::Pending)
    return false;
  if (outfit && context.context == NativeContext::NativeMenu) {
    Abort(MenuDriveAbort::Unsupported);
    return false;
  }
  if (!std::has_single_bit(mask) ||
      (outfit ? (mask & ~KnownOutfitMask) : (mask & ~KnownAccessoryMask)) ||
      (outfit && !desired) ||
      !(mask & (outfit ? uint32_t(inventory.acquired_outfits)
                       : inventory.acquired_accessories)) ||
      (!outfit && desired && !(Equipped(inventory) & mask) &&
       inventory.equipped_accessory_count >= inventory.accessory_capacity)) {
    Abort(MenuDriveAbort::InvalidRequest);
    return false;
  }
  if (!Start(Kind::Equipment, menu, inventory, context, tick))
    return false;
  mask_ = mask;
  outfit_ = outfit;
  desired_ = desired;
  if ((outfit ? inventory.equipped_outfit == mask
              : bool(Equipped(inventory) & mask)) == desired) {
    status_ = MenuDriveStatus::Complete;
    return true;
  }
  return true;
}
bool MenuDrive::RequestConsume(uint32_t item, size_t index,
                               const MenuSnapshot &menu,
                               const InventorySnapshot &inventory,
                               const ContextSnapshot &context, uint64_t tick) {
  if (status_ == MenuDriveStatus::Pending)
    return false;
  if (!item || item > 65535 || index >= IngredientKinds ||
      !inventory.ingredients[index]) {
    Abort(MenuDriveAbort::InvalidRequest);
    return false;
  }
  if (!Start(Kind::Consume, menu, inventory, context, tick))
    return false;
  item_id_ = item;
  quantity_index_ = index;
  if (menu.valid && menu.kind == MenuKind::Items && Target(menu) < 0) {
    Abort(MenuDriveAbort::Unsupported);
    return false;
  }
  return true;
}
bool MenuDrive::OwnershipSame(const InventorySnapshot &s) const {
  return s.compatible && s.inventory_valid && s.source == initial_.source &&
         s.acquired_accessories == initial_.acquired_accessories &&
         s.acquired_outfits == initial_.acquired_outfits &&
         s.accessory_capacity == initial_.accessory_capacity;
}
bool MenuDrive::InitialSame(const InventorySnapshot &s) const {
  return OwnershipSame(s) && s.equipped_outfit == initial_.equipped_outfit &&
         s.equipped_accessories == initial_.equipped_accessories &&
         s.equipped_accessory_count == initial_.equipped_accessory_count &&
         s.ingredients == initial_.ingredients && s.coupons == initial_.coupons;
}
bool MenuDrive::Committed(const InventorySnapshot &s) const {
  if (!OwnershipSame(s) || s.coupons != initial_.coupons)
    return false;
  if (kind_ == Kind::Consume) {
    if (s.equipped_outfit != initial_.equipped_outfit ||
        s.equipped_accessories != initial_.equipped_accessories ||
        s.equipped_accessory_count != initial_.equipped_accessory_count)
      return false;
    for (size_t i = 0; i < IngredientKinds; ++i)
      if (uint32_t(s.ingredients[i]) + (i == quantity_index_ ? 1u : 0u) !=
          initial_.ingredients[i])
        return false;
    return true;
  }
  if (s.ingredients != initial_.ingredients)
    return false;
  if (outfit_)
    return s.equipped_outfit == mask_ &&
           s.equipped_accessories == initial_.equipped_accessories &&
           s.equipped_accessory_count == initial_.equipped_accessory_count;
  const auto expected =
      desired_ ? (Equipped(initial_) | mask_) : (Equipped(initial_) & ~mask_);
  return s.equipped_outfit == initial_.equipped_outfit &&
         Equipped(s) == expected &&
         s.equipped_accessory_count == std::popcount(expected);
}
int MenuDrive::Target(const MenuSnapshot &menu) const {
  int found = -1;
  if (kind_ == Kind::Consume) {
    for (size_t i = 0; i < menu.entries.size(); ++i) {
      const auto &e = menu.entries[i];
      if (e.kind == MenuEntryKind::Item && e.item_id == item_id_) {
        const bool consumed =
            (phase_ == Phase::WaitCommit || phase_ == Phase::CloseMenu) &&
            e.quantity + 1u == initial_.ingredients[quantity_index_];
        if (found >= 0 || i >= IngredientKinds ||
            (!consumed && (!e.owned || !e.quantity || e.raw_action2a != 1)))
          return -1;
        found = int(i);
      }
    }
  } else {
    for (size_t row = 0; row < menu.row_starts.size(); ++row)
      for (size_t col = 0; col < 4; ++col) {
        const uint64_t i = uint64_t(menu.row_starts[row]) * 4 + col;
        if (i >= menu.entries.size())
          continue;
        const auto &e = menu.entries[size_t(i)];
        if (e.kind ==
                (outfit_ ? MenuEntryKind::Outfit : MenuEntryKind::Accessory) &&
            e.mask == mask_ && e.owned && !e.placeholder) {
          if (found >= 0)
            return -1;
          found = int(i);
        }
      }
  }
  return found;
}
bool MenuDrive::TargetReady(const MenuSnapshot &menu, int target) const {
  if (menu.selected_index != target || target < 0 ||
      size_t(target) >= menu.entries.size())
    return false;
  const auto &e = menu.entries[size_t(target)];
  if (kind_ == Kind::Consume)
    return e.item_id == item_id_ &&
           e.quantity == initial_.ingredients[quantity_index_] &&
           e.raw_action2a == 1 && e.owned;
  return e.mask == mask_ && e.equipped != desired_ && e.owned && !e.placeholder;
}
bool MenuDrive::Stable(uint64_t tick) {
  if (stable_tick_ != tick) {
    stable_tick_ = tick;
    if (stable_count_ < 2)
      ++stable_count_;
  }
  return stable_count_ >= 2;
}
void MenuDrive::Wait(uint64_t tick) {
  if (tick - wait_started_ > ReadbackTimeout)
    Abort(MenuDriveAbort::Timeout);
}
MenuPulse MenuDrive::Pulse(MenuPulse pulse, uint64_t tick) {
  // Native dialog state and inventory may update before their input-blocking
  // animations finish. A readback alone does not make the next pulse ready.
  const bool navigation = pulse == MenuPulse::Up || pulse == MenuPulse::Down ||
                          pulse == MenuPulse::Left || pulse == MenuPulse::Right;
  next_pulse_ = tick + (pulse == MenuPulse::R   ? TabTicks
                        : pulse == MenuPulse::A ? ActionTicks
                        : navigation            ? NavigationTicks
                                                : StepTicks);
  wait_started_ = tick;
  stable_count_ = 0;
  return pulse;
}
MenuPulse MenuDrive::Advance(const MenuSnapshot &menu,
                             const InventorySnapshot &inventory,
                             const ContextSnapshot &context, uint64_t tick) {
  if (status_ != MenuDriveStatus::Pending)
    return MenuPulse::None;
  if (tick < last_tick_ || tick - last_tick_ > MaxClockGap) {
    Abort(MenuDriveAbort::Clock);
    return MenuPulse::None;
  }
  if (tick == last_tick_)
    return MenuPulse::None;
  last_tick_ = tick;
  if (tick - requested_ > RequestTimeout) {
    Abort(MenuDriveAbort::Timeout);
    return MenuPulse::None;
  }
  if (inventory.inventory_valid && inventory.source != initial_.source) {
    Abort(MenuDriveAbort::SourceChanged);
    return MenuPulse::None;
  }
  if (!menu.compatible || !context.compatible || !inventory.compatible) {
    Abort(MenuDriveAbort::Unsupported);
    return MenuPulse::None;
  }
  if (!inventory.inventory_valid || !context.valid ||
      context.context == NativeContext::Unknown) {
    stable_count_ = 0;
    if (!unknown_wait_) {
      unknown_wait_ = true;
      unknown_since_ = tick;
    }
    if (tick - unknown_since_ > ReadbackTimeout)
      Abort(MenuDriveAbort::Timeout);
    return MenuPulse::None;
  }
  unknown_wait_ = false;
  if (!PermittedContext(context) || !OwnershipSame(inventory)) {
    Abort(MenuDriveAbort::ContextChanged);
    return MenuPulse::None;
  }
  const bool commit_phase =
      phase_ == Phase::WaitCommit || phase_ == Phase::WaitOutfitCommit ||
      phase_ == Phase::CloseMenu;
  if (!InitialSame(inventory) && !(commit_phase && Committed(inventory))) {
    Abort(MenuDriveAbort::Postcondition);
    return MenuPulse::None;
  }
  if (phase_ == Phase::WaitOutfitCommit) {
    if (context.context == NativeContext::FieldHud && Committed(inventory))
      status_ = MenuDriveStatus::Complete;
    else
      Wait(tick);
    return MenuPulse::None;
  }
  if (phase_ == Phase::CloseMenu) {
    if (!Committed(inventory)) {
      Abort(MenuDriveAbort::Postcondition);
      return MenuPulse::None;
    }
    if (!opened_ || context.context == NativeContext::FieldHud) {
      status_ = MenuDriveStatus::Complete;
      return MenuPulse::None;
    }
    if (tick < next_pulse_)
      return MenuPulse::None;
    if (!close_sent_) {
      close_sent_ = true;
      return Pulse(MenuPulse::B, tick);
    }
    Wait(tick);
    return MenuPulse::None;
  }
  if (phase_ == Phase::WaitMenu) {
    if (context.context == NativeContext::FieldHud) {
      Wait(tick);
      return MenuPulse::None;
    }
    phase_ = Phase::FindMenu;
  }
  if (phase_ == Phase::FindMenu && context.context == NativeContext::FieldHud) {
    if (tick < next_pulse_)
      return MenuPulse::None;
    if (opened_) {
      Abort(MenuDriveAbort::ContextChanged);
      return MenuPulse::None;
    }
    opened_ = true;
    phase_ = Phase::WaitMenu;
    return Pulse(MenuPulse::Plus, tick);
  }
  if (phase_ == Phase::FindMenu && kind_ == Kind::Equipment && outfit_ &&
      !opened_) {
    // An outfit swap may only ever be committed inside a pause this drive
    // opened itself (RequestEquipment refuses an outfit request unless the
    // caller was on FieldHud). If the native menu is already open here on
    // our very first FindMenu tick, it opened externally between Request
    // and this Advance() - abort before any navigation/A press rather than
    // risk pressing A on a menu state we never authenticated opened_ for.
    Abort(MenuDriveAbort::ContextChanged);
    return MenuPulse::None;
  }
  if (context.context != NativeContext::NativeMenu) {
    Abort(MenuDriveAbort::ContextChanged);
    return MenuPulse::None;
  }
  if (tick < next_pulse_)
    return MenuPulse::None;
  const auto wanted =
      kind_ == Kind::Equipment ? MenuKind::Equipment : MenuKind::Items;
  if (phase_ == Phase::FindMenu) {
    if (!menu.valid || menu.kind != wanted) {
      if (tabs_ >= 7) {
        Abort(MenuDriveAbort::Unsupported);
        return MenuPulse::None;
      }
      ++tabs_;
      return Pulse(MenuPulse::R, tick);
    }
    component_ = menu.component;
    phase_ = Phase::Navigate;
    wait_started_ = tick;
  }
  if (!menu.valid) {
    stable_count_ = 0;
    Wait(tick);
    return MenuPulse::None;
  }
  if (menu.kind != wanted || menu.component != component_) {
    Abort(MenuDriveAbort::ContextChanged);
    return MenuPulse::None;
  }
  const int target = Target(menu);
  if (target < 0) {
    Abort(MenuDriveAbort::Unsupported);
    return MenuPulse::None;
  }
  if (phase_ == Phase::WaitCommit) {
    if (kind_ == Kind::Equipment && outfit_) {
      if (!opened_ || !InitialSame(inventory)) {
        Abort(MenuDriveAbort::Postcondition);
        return MenuPulse::None;
      }
      if (menu.selected_index != target || menu.nested_state != 0) {
        Abort(MenuDriveAbort::UnexpectedCursor);
        return MenuPulse::None;
      }
      size_t equipped{};
      for (const auto &e : menu.entries)
        if (e.kind == MenuEntryKind::Outfit && e.equipped) {
          if (e.mask != mask_ || !e.owned || e.placeholder) {
            Abort(MenuDriveAbort::Postcondition);
            return MenuPulse::None;
          }
          ++equipped;
        }
      if (equipped > 1) {
        Abort(MenuDriveAbort::Postcondition);
        return MenuPulse::None;
      }
      if (equipped != 1 || !menu.entries[size_t(target)].equipped) {
        stable_count_ = 0;
        Wait(tick);
        return MenuPulse::None;
      }
      if (!Stable(tick))
        return MenuPulse::None;
      phase_ = Phase::WaitOutfitCommit;
      return Pulse(MenuPulse::B, tick);
    }
    if (Committed(inventory)) {
      const auto &e = menu.entries[size_t(target)];
      const bool ui =
          menu.selected_index == target &&
          (kind_ == Kind::Consume
               ? (menu.nested_state == 1 &&
                  e.quantity + 1u == initial_.ingredients[quantity_index_])
               : e.equipped == desired_);
      if (ui) {
        phase_ = Phase::CloseMenu;
        wait_started_ = tick;
        if (!opened_)
          status_ = MenuDriveStatus::Complete;
      } else
        Wait(tick);
    } else
      Wait(tick);
    return MenuPulse::None;
  }
  if (phase_ == Phase::WaitDialog || phase_ == Phase::ConfirmConsume) {
    if (!TargetReady(menu, target)) {
      Abort(MenuDriveAbort::UnexpectedCursor);
      return MenuPulse::None;
    }
    if (menu.nested_state == 1 && phase_ == Phase::WaitDialog) {
      Wait(tick);
      return MenuPulse::None;
    }
    if (menu.nested_state != 2) {
      Abort(MenuDriveAbort::ContextChanged);
      return MenuPulse::None;
    }
    phase_ = Phase::ConfirmConsume;
    if (!Stable(tick))
      return MenuPulse::None;
    phase_ = Phase::WaitCommit;
    return Pulse(MenuPulse::A, tick);
  }
  if (menu.nested_state != (kind_ == Kind::Consume ? 1 : 0)) {
    stable_count_ = 0;
    Wait(tick);
    return MenuPulse::None;
  }
  if (moving_) {
    const bool unchanged = menu.selected_index == nav_index_;
    if (unchanged) {
      Wait(tick);
      return MenuPulse::None;
    }
    bool expected = false;
    if (kind_ == Kind::Consume)
      expected = menu.selected_index == expected_index_;
    else if (movement_ == MenuPulse::Up || movement_ == MenuPulse::Down)
      expected =
          menu.row == origin_row_ + (movement_ == MenuPulse::Up ? -1 : 1) &&
          menu.column >= 0 && menu.column < 4;
    else
      expected =
          menu.row == origin_row_ &&
          menu.column == origin_col_ + (movement_ == MenuPulse::Left ? -1 : 1);
    if (!expected) {
      Abort(MenuDriveAbort::UnexpectedCursor);
      return MenuPulse::None;
    }
    moving_ = false;
    nav_index_ = menu.selected_index;
  } else if (nav_seen_ && menu.selected_index != nav_index_) {
    Abort(MenuDriveAbort::UnexpectedCursor);
    return MenuPulse::None;
  }
  nav_seen_ = true;
  nav_index_ = menu.selected_index;
  if (menu.selected_index == target) {
    if (!TargetReady(menu, target)) {
      Abort(MenuDriveAbort::Postcondition);
      return MenuPulse::None;
    }
    if (!Stable(tick))
      return MenuPulse::None;
    phase_ = kind_ == Kind::Consume ? Phase::WaitDialog : Phase::WaitCommit;
    return Pulse(MenuPulse::A, tick);
  }
  stable_count_ = 0;
  int row{}, col{}, target_row{}, target_col{};
  if (kind_ == Kind::Consume) {
    if (menu.selected_index < 0 ||
        menu.selected_index >= int(IngredientKinds)) {
      Abort(MenuDriveAbort::Unsupported);
      return MenuPulse::None;
    }
    row = menu.selected_index / 8;
    col = menu.selected_index % 8;
    target_row = target / 8;
    target_col = target % 8;
  } else {
    row = menu.row;
    col = menu.column;
    bool found = false;
    for (size_t i = 0; i < menu.row_starts.size(); ++i) {
      const uint64_t first = uint64_t(menu.row_starts[i]) * 4;
      if (uint64_t(target) >= first && uint64_t(target) < first + 4) {
        target_row = int(i);
        target_col = int(uint64_t(target) - first);
        found = true;
        break;
      }
    }
    if (!found || row < 0 || col < 0 || col >= 4) {
      Abort(MenuDriveAbort::Unsupported);
      return MenuPulse::None;
    }
  }
  movement_ = row > target_row   ? MenuPulse::Up
              : row < target_row ? MenuPulse::Down
              : col > target_col ? MenuPulse::Left
                                 : MenuPulse::Right;
  origin_row_ = row;
  origin_col_ = col;
  moving_ = true;
  expected_index_ = menu.selected_index + (movement_ == MenuPulse::Up     ? -8
                                           : movement_ == MenuPulse::Down ? 8
                                           : movement_ == MenuPulse::Left ? -1
                                                                          : 1);
  return Pulse(movement_, tick);
}
} // namespace echoes
