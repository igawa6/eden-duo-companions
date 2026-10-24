// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "echoes_live.h"

namespace echoes {
enum class Direction { None, Left, Right };
enum class DriveStatus { Idle, Pending, Complete, Aborted };
enum class DriveAbort {
  None, Cancelled, InvalidSnapshot, ContextChanged, Released, Clock,
  UnexpectedCursor, Timeout
};

// Pure planner: no input dispatch, guest calls, or guest memory writes.
// Feed every host tick, including closed/unavailable picker observations. A
// first hold on a new instance needs two stable host intervals before a
// Request can be accepted. Thereafter a known closed observation is required
// before each new held generation. Missing a close/reopen entirely between
// samples is unobservable; the bridge must independently gate physical DRight.
// Direction is a ONE-SHOT pulse request, not a per-tick held axis value. The
// bridge must execute exactly PulseTicks host ticks, then neutralize. None
// means no new pulse. Cancel/release/abort must also clear bridge-owned input.
// Physical-controller arbitration is outside this API. A bridge that can
// observe physical input must Cancel it; otherwise only unexpected native
// cursor movement is detected. Same-expected controller steps and sort/filter
// actions that leave every published field unchanged are unobservable.
// Snapshots must come from the build-guarded ReadLive reader; its selector
// enforces the native DRight action mask, which is not a snapshot field.
class SelectorDrive {
public:
  // Host ticks can outpace the 30fps guest. An eight-tick pulse spans
  // multiple guest input samples; an equally long neutral window prevents
  // adjacent pulses merging. A delayed ACK is observed, never blindly retried.
  static constexpr uint64_t PulseTicks = 8;
  static constexpr uint64_t NeutralTicks = 8;
  static constexpr uint64_t SettleTicks = 2;
  static constexpr uint64_t StepTimeoutTicks = 90;
  static constexpr uint64_t RequestTimeoutTicks = 2048;

  // Accepts an intention only. Completion requires subsequent stable native
  // observations; neither Request nor Advance releases DRight or equips/casts.
  bool Request(uint16_t target_native_id, const LiveSnapshot&, uint64_t tick);
  Direction Advance(const LiveSnapshot&, uint64_t tick);
  // Disarms this hold. A fresh closed -> held lifecycle is required to rearm.
  void Cancel();
  DriveStatus status() const { return status_; }
  DriveAbort abort_reason() const { return abort_; }

private:
  bool Observe(const LiveSnapshot&, uint64_t tick);
  void Abort(DriveAbort);
  LiveSnapshot context_{};
  bool clock_seen_{}, armed_{}, in_hold_{}, step_active_{}, settling_{};
  bool destination_seen_{}, arrival_seen_{};
  bool bootstrap_allowed_{true}, bootstrap_seen_{};
  uint64_t bootstrap_at_{};
  int32_t bootstrap_cursor_{-1};
  uint64_t last_tick_{}, requested_at_{}, pulse_at_{}, settled_at_{};
  int32_t target_{-1}, origin_{-1}, expected_{-1};
  DriveStatus status_{DriveStatus::Idle};
  DriveAbort abort_{DriveAbort::None};
};
} // namespace echoes
