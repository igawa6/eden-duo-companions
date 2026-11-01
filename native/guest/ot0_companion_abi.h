// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
enum { OT0_COMPANION_ABI = 1, OT0_COMPANION_ROWS = 14 };
enum { OT0_NATIVE_OK=1, OT0_NATIVE_BAD_REQUEST, OT0_NATIVE_EXPIRED,
       OT0_NATIVE_NOT_BATTLE, OT0_NATIVE_UNAVAILABLE, OT0_NATIVE_STALE,
       OT0_NATIVE_UNSUPPORTED };
typedef struct Ot0NativeStatRow {
  uint64_t actor;
  uint32_t id, team, slot, valid_mask;
  int32_t values[8]; // Native parameter IDs9..16, never database estimates.
  uint32_t error, serial;
  uint64_t ailment_fingerprint;
  uint32_t weapon_selector, native_weapon;
} Ot0NativeStatRow;
typedef struct Ot0CompanionMailbox {
  uint32_t heartbeat, epoch; // Loader owns epoch; wrapper owns heartbeat.
  uint32_t desired_hide, lease_deadline, lease_epoch, lease_seq;
  uint32_t status_seq, hidden;
  uint64_t owned_ui, owned_slate, owned_controller;
  uint32_t owned_index, owned_serial, original_visibility, helper_version;
  uint32_t query_seq, query_ack, query_epoch, query_deadline;
  uint32_t query_result, query_mask, app_state, native_visibility;
  uint32_t filter_magnification_bits, reserved;
  int64_t relocation_delta;
  uint64_t query_bm;
  uint64_t expected_resource, expected_section;
  uint32_t expected_map_id, lease_reserved;
  uint64_t owned_root, owned_root_slate, owned_root_controller, owned_slot;
  uint32_t owned_root_index, owned_root_serial;
  uint64_t expected_canvas;
  uint32_t expected_canvas_serial, expected_canvas_index;
  uint64_t expected_root;
  uint32_t expected_root_serial, expected_root_index;
  uint8_t reserved_header[32];
  Ot0NativeStatRow rows[OT0_COMPANION_ROWS];
  uint8_t reserved_tail[672];
} Ot0CompanionMailbox;
#if defined(__cplusplus)
static_assert(sizeof(Ot0CompanionMailbox)==2048);
static_assert(offsetof(Ot0CompanionMailbox,rows)==256);
#else
_Static_assert(sizeof(Ot0CompanionMailbox)==2048,"mailbox ABI");
_Static_assert(offsetof(Ot0CompanionMailbox,rows)==256,"row ABI");
#endif
