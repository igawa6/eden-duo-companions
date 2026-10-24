// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_inventory.h"
#include "echoes_menu.h"
namespace echoes {
enum class MenuPulse { None, Plus, R, Up, Down, Left, Right, A, B };
enum class MenuDriveStatus { Idle, Pending, Complete, Aborted };
enum class MenuDriveAbort {
  None,
  Cancelled,
  InvalidRequest,
  Unsupported,
  SourceChanged,
  ContextChanged,
  Clock,
  Timeout,
  UnexpectedCursor,
  Postcondition
};
// Pure planner. Each non-None result is ONE pulse of exactly2 host frames;
// the caller must neutralize its owned input, including on cancel/abort. The
// caller must cancel on observed physical-controller interference. Identical
// native changes made externally cannot be distinguished from our input.
class MenuDrive {
public:
  static constexpr uint64_t PulseTicks = 2, StepTicks = 6, TabTicks = 30,
                            ActionTicks = 30, NavigationTicks = 30,
                            ReadbackTimeout = 120, RequestTimeout = 1024,
                            MaxClockGap = 120;
  bool RequestEquipment(uint32_t mask, bool outfit, bool desired_equipped,
                        const MenuSnapshot &, const InventorySnapshot &,
                        const ContextSnapshot &, uint64_t tick);
  bool RequestConsume(uint32_t native_item_id, size_t quantity_index,
                      const MenuSnapshot &, const InventorySnapshot &,
                      const ContextSnapshot &, uint64_t tick);
  MenuPulse Advance(const MenuSnapshot &, const InventorySnapshot &,
                    const ContextSnapshot &, uint64_t tick);
  void Cancel();
  MenuDriveStatus status() const { return status_; }
  MenuDriveAbort abort_reason() const { return abort_; }

private:
  enum class Phase {
    FindMenu,
    WaitMenu,
    Navigate,
    WaitDialog,
    ConfirmConsume,
    WaitCommit,
    WaitOutfitCommit,
    CloseMenu
  };
  enum class Kind { Equipment, Consume };
  bool Start(Kind, const MenuSnapshot &, const InventorySnapshot &,
             const ContextSnapshot &, uint64_t);
  void Abort(MenuDriveAbort);
  MenuPulse Pulse(MenuPulse, uint64_t);
  bool OwnershipSame(const InventorySnapshot &) const;
  bool InitialSame(const InventorySnapshot &) const;
  bool Committed(const InventorySnapshot &) const;
  int Target(const MenuSnapshot &) const;
  bool TargetReady(const MenuSnapshot &, int) const;
  bool Stable(uint64_t);
  void Wait(uint64_t);
  MenuDriveStatus status_{MenuDriveStatus::Idle};
  MenuDriveAbort abort_{MenuDriveAbort::None};
  Phase phase_{Phase::FindMenu};
  Kind kind_{Kind::Equipment};
  InventorySnapshot initial_{};
  uint32_t mask_{}, item_id_{};
  size_t quantity_index_{};
  bool outfit_{}, desired_{}, opened_{}, close_sent_{}, nav_seen_{}, moving_{};
  uint8_t tabs_{};
  uint64_t component_{}, requested_{}, last_tick_{}, next_pulse_{};
  uint64_t wait_started_{}, unknown_since_{}, stable_tick_{};
  bool unknown_wait_{};
  uint8_t stable_count_{};
  int32_t nav_index_{-1}, origin_row_{-1}, origin_col_{-1}, expected_index_{-1};
  MenuPulse movement_{MenuPulse::None};
};
} // namespace echoes
