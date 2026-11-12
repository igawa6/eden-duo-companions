#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen-runtime22 flag transport for the per-save circle-assignment table.

Module publishes echo.saveslot.restore.pending/slot, echo.saveslot.persist.N.pending,
echo.saveslot.N.packed, and echo.saveslot.enforce.any. Set the package-wide enforce_gate
to a derived OR including echo.saveslot.enforce.any. Never remove the controller actions'
individual gates. Each flag encodes an unsigned16 (native ID+1 for a circle content value;
raw for an identity half or the age/generation counters); zero means unset. All referenced
flag values are integers below 2**16, exactly representable by runtime expressions' f64.

Module actions: saveslot_restore_packet(bit*2+value or -1), saveslot_persist_ack(slot).
Sample calls AdvanceRestore then QueueWrites; on_action only stages packets/ACK.
Persistence is host flag persistence, profile-global, outside game saves -- the module
interprets the table's Count values as 8 per-save buckets (id_lo, id_hi, age, c0, c1, c2)
plus one shared generation counter, and is solely responsible for matching a save's own
identity against the table; this transport moves the raw values only.
"""
BUCKETS = 8
FIELDS_PER_BUCKET = 6  # id_lo, id_hi, age, c0, c1, c2
CAPACITY = BUCKETS * FIELDS_PER_BUCKET + 1  # +1 shared generation counter
BITS = 16


def pieces():
    flags = {f"eow_ss_{i}": 0 for i in range(CAPACITY)}
    derived, actions, enforce = [], {}, []
    # Keep the same shallow per-slot picks as the proven stamp bridge.
    for slot in range(CAPACITY):
        derived.append({"name": f"echo.saveslot.restore.pick.{slot}",
                        "expr": f"echo.saveslot.restore.slot == {slot} ? @flag:eow_ss_{slot} : 0"})
    derived += [
        {"name": "echo.saveslot.restore.value",
         "terms": [f"echo.saveslot.restore.pick.{i}" for i in range(CAPACITY)]},
        {"name": "echo.saveslot.restore.invalid", "expr":
         "echo.saveslot.restore.pending && (echo.saveslot.restore.value < 0 || "
         "echo.saveslot.restore.value >= 65536 || floor(echo.saveslot.restore.value) != echo.saveslot.restore.value)"},
    ]
    actions["saveslot_restore_reject"] = {
        "kind": "module", "action": "saveslot_restore_packet", "argument": -1,
        "enabled_bind": "echo.saveslot.restore.invalid", "haptic": "off"}
    enforce.append({"action": "saveslot_restore_reject", "every_ms": 0})
    for bit in range(BITS):
        for value in (0, 1):
            key = f"saveslot_restore_{bit}_{value}"
            gate = f"echo.saveslot.restore.bit.{bit}.{value}"
            derived.append({"name": gate, "expr":
                "echo.saveslot.restore.pending && !echo.saveslot.restore.invalid && "
                f"floor(echo.saveslot.restore.value / {1 << bit}) % 2 == {value}"})
            actions[key] = {"kind": "module", "action": "saveslot_restore_packet",
                            "argument": bit * 2 + value, "enabled_bind": gate, "haptic": "off"}
            enforce.append({"action": key, "every_ms": 0})
    for slot in range(CAPACITY):
        pending = f"echo.saveslot.persist.{slot}.pending"
        match = f"echo.saveslot.persist.{slot}.match"
        source = f"echo.saveslot.{slot}.packed"
        derived.append({"name": match, "expr":
                        f"{pending} && @flag:eow_ss_{slot} == {source}"})
        actions[f"saveslot_persist_write_{slot}"] = {
            "kind": "set_value", "flag": f"eow_ss_{slot}", "value": f"${source}",
            "enabled_bind": pending, "haptic": "off"}
        actions[f"saveslot_persist_ack_{slot}"] = {
            "kind": "module", "action": "saveslot_persist_ack", "argument": slot,
            "enabled_bind": match, "haptic": "off"}
        enforce += [{"action": f"saveslot_persist_write_{slot}", "every_ms": 0},
                    {"action": f"saveslot_persist_ack_{slot}", "every_ms": 0}]
    return {"flags": flags, "persist_flags": list(flags), "derived": derived,
            "actions": actions, "enforce": enforce}
