#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen-runtime22 flag transport. Import pieces(); merge into the manifest.

Module publishes echo.stamp.restore.pending/slot, echo.stamp.persist.N.pending,
echo.stamp.N.packed, and echo.stamp.enforce.any. Set the package-wide enforce_gate
to a derived OR of echo.drive.any and echo.stamp.enforce.any. Never remove the
controller actions' individual gates. All referenced flag values are integers
below2**40, exactly representable by runtime expressions' f64.

Module actions: stamp_restore_packet(bit*2+value or -1), stamp_persist_ack(slot).
Sample calls AdvanceRestore then QueueWrites; on_action only stages packets/ACK.
Persistence is host flag persistence, profile-global, outside game saves. ACK
means the runtime flag matched; the runtime owns/logs actual disk-save failure.
"""
CAPACITY = 24
BITS = 40


def pieces():
    flags = {f"eow_stamp{i}": 0 for i in range(CAPACITY)}
    derived, actions, enforce = [], {}, []
    # One giant24-way ternary exceeds frozen ExprMaxDepth64. Shallow picks plus
    # terms sum are equivalent without parser recursion or integer precision loss.
    for slot in range(CAPACITY):
        derived.append({"name": f"echo.stamp.restore.pick.{slot}",
                        "expr": f"echo.stamp.restore.slot == {slot} ? @flag:eow_stamp{slot} : 0"})
    derived += [
        {"name": "echo.stamp.restore.value",
         "terms": [f"echo.stamp.restore.pick.{i}" for i in range(CAPACITY)]},
        {"name": "echo.stamp.restore.invalid", "expr":
         "echo.stamp.restore.pending && (echo.stamp.restore.value < 0 || "
         "echo.stamp.restore.value >= 1099511627776)"},
    ]
    actions["stamp_restore_reject"] = {
        "kind": "module", "action": "stamp_restore_packet", "argument": -1,
        "enabled_bind": "echo.stamp.restore.invalid", "haptic": "off"}
    enforce.append({"action": "stamp_restore_reject", "every_ms": 0})
    for bit in range(BITS):
        for value in (0, 1):
            key = f"stamp_restore_{bit}_{value}"
            gate = f"echo.stamp.restore.bit.{bit}.{value}"
            derived.append({"name": gate, "expr":
                "echo.stamp.restore.pending && !echo.stamp.restore.invalid && "
                f"floor(echo.stamp.restore.value / {1 << bit}) % 2 == {value}"})
            actions[key] = {"kind": "module", "action": "stamp_restore_packet",
                            "argument": bit * 2 + value, "enabled_bind": gate, "haptic": "off"}
            enforce.append({"action": key, "every_ms": 0})
    for slot in range(CAPACITY):
        pending = f"echo.stamp.persist.{slot}.pending"
        match = f"echo.stamp.persist.{slot}.match"
        source = f"echo.stamp.{slot}.packed"
        derived.append({"name": match, "expr":
                        f"{pending} && @flag:eow_stamp{slot} == {source}"})
        actions[f"stamp_persist_write_{slot}"] = {
            "kind": "set_value", "flag": f"eow_stamp{slot}", "value": f"${source}",
            "enabled_bind": pending, "haptic": "off"}
        actions[f"stamp_persist_ack_{slot}"] = {
            "kind": "module", "action": "stamp_persist_ack", "argument": slot,
            "enabled_bind": match, "haptic": "off"}
        enforce += [{"action": f"stamp_persist_write_{slot}", "every_ms": 0},
                    {"action": f"stamp_persist_ack_{slot}", "every_ms": 0}]
    return {"flags": flags, "persist_flags": list(flags), "derived": derived,
            "actions": actions, "enforce": enforce}
