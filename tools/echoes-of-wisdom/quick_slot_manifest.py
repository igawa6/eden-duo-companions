#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen-runtime22 flag transport. Import pieces(); merge into the manifest.

Module publishes echo.quickslot.restore.pending/slot, echo.quickslot.persist.N.pending,
echo.quickslot.N.packed, and echo.quickslot.enforce.any. Set the package-wide enforce_gate
to a derived OR of echo.drive.any and echo.quickslot.enforce.any. Never remove the
controller actions' individual gates. Each flag encodes native ID+1; zero is unset. All referenced flag values are integers
below2**16, exactly representable by runtime expressions' f64.

Module actions: quickslot_restore_packet(bit*2+value or -1), quickslot_persist_ack(slot).
Sample calls AdvanceRestore then QueueWrites; on_action only stages packets/ACK.
Persistence is host flag persistence, profile-global, outside game saves. ACK
means the runtime flag matched; the runtime owns/logs actual disk-save failure.
"""
CAPACITY = 3
BITS = 16


def pieces():
    flags = {f"eow_quickslot{i}": 0 for i in range(CAPACITY)}
    derived, actions, enforce = [], {}, []
    # Keep the same shallow per-slot picks as the proven stamp bridge.
    for slot in range(CAPACITY):
        derived.append({"name": f"echo.quickslot.restore.pick.{slot}",
                        "expr": f"echo.quickslot.restore.slot == {slot} ? @flag:eow_quickslot{slot} : 0"})
    derived += [
        {"name": "echo.quickslot.restore.value",
         "terms": [f"echo.quickslot.restore.pick.{i}" for i in range(CAPACITY)]},
        {"name": "echo.quickslot.restore.invalid", "expr":
         "echo.quickslot.restore.pending && (echo.quickslot.restore.value < 0 || "
         "echo.quickslot.restore.value >= 65536 || floor(echo.quickslot.restore.value) != echo.quickslot.restore.value)"},
    ]
    actions["quickslot_restore_reject"] = {
        "kind": "module", "action": "quickslot_restore_packet", "argument": -1,
        "enabled_bind": "echo.quickslot.restore.invalid", "haptic": "off"}
    enforce.append({"action": "quickslot_restore_reject", "every_ms": 0})
    for bit in range(BITS):
        for value in (0, 1):
            key = f"quickslot_restore_{bit}_{value}"
            gate = f"echo.quickslot.restore.bit.{bit}.{value}"
            derived.append({"name": gate, "expr":
                "echo.quickslot.restore.pending && !echo.quickslot.restore.invalid && "
                f"floor(echo.quickslot.restore.value / {1 << bit}) % 2 == {value}"})
            actions[key] = {"kind": "module", "action": "quickslot_restore_packet",
                            "argument": bit * 2 + value, "enabled_bind": gate, "haptic": "off"}
            enforce.append({"action": key, "every_ms": 0})
    for slot in range(CAPACITY):
        pending = f"echo.quickslot.persist.{slot}.pending"
        match = f"echo.quickslot.persist.{slot}.match"
        source = f"echo.quickslot.{slot}.packed"
        derived.append({"name": match, "expr":
                        f"{pending} && @flag:eow_quickslot{slot} == {source}"})
        actions[f"quickslot_persist_write_{slot}"] = {
            "kind": "set_value", "flag": f"eow_quickslot{slot}", "value": f"${source}",
            "enabled_bind": pending, "haptic": "off"}
        actions[f"quickslot_persist_ack_{slot}"] = {
            "kind": "module", "action": "quickslot_persist_ack", "argument": slot,
            "enabled_bind": match, "haptic": "off"}
        enforce += [{"action": f"quickslot_persist_write_{slot}", "every_ms": 0},
                    {"action": f"quickslot_persist_ack_{slot}", "every_ms": 0}]
    return {"flags": flags, "persist_flags": list(flags), "derived": derived,
            "actions": actions, "enforce": enforce}
