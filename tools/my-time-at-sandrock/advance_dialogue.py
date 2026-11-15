#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Advance a native dialogue, waiting for each line's state.

Requires a running label with --gdb. Default return-page assertion is Inventory (4).
Uses the harness's rsp helper; no guest memory writes. Offsets target build b74dfd27.
Example: advance_dialogue.py --harness /path/to/harness --driver /path/to/driver --label probe
Install the adjacent item26-*.route files in the game's harness routes directory.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


GAME = "my-time-at-sandrock"


def snapshot(c, base):
    def ptr(addr):
        value = c.read_u64(addr)
        if not 0x1000000 <= value < 1 << 48 or value & 7:
            raise RuntimeError(f"Invalid pointer at {addr:#x}")
        return value

    def check_class(obj, expected):
        name = c.read(c.read_u64(ptr(obj) + 0x10), 64, on_error="raise").split(b"\0")[0]
        if name.decode("ascii") != expected:
            raise RuntimeError(f"Expected {expected}, got {name!r}")

    klass = ptr(base + 0x80B8A28)
    # UISystemMgr is non-generic: static Instance is at static_fields+0x28.
    name = c.read(c.read_u64(klass + 0x10), 64, on_error="raise").split(b"\0")[0]
    if name != b"UISystemMgr":
        raise RuntimeError("Unsupported game build: UISystemMgr mismatch")
    ui = ptr(ptr(klass + 0xB8) + 0x28)
    state = ptr(ptr(ui + 0xB0) + 0x70)
    kind = c.read_u32(state + 0x84)
    row = {"state": kind}
    if kind != 16:
        return row
    view = ptr(state + 0x50)
    check_class(view, "DialogUI")
    auto = ptr(view + 0x70)
    check_class(auto, "UITextAutoShow")
    control = ptr(state + 0x58)
    check_class(control, "DialogControl")
    text = ptr(ptr(auto + 0x18) + 0xD0)
    choices = ptr(view + 0x140)
    row.update(line=text, working=bool(c.read(auto + 0x20, 1, on_error="raise")[0]),
               can_click=bool(c.read(control + 0xAC, 1, on_error="raise")[0]),
               choice=bool(c.read(view + 0x148, 1, on_error="raise")[0])
               or c.read_u32(choices + 0x18) > 0)
    return row


def advance_dialogue(read, press, *, end_state=26, timeout=30, max_lines=64,
                     clock=time.monotonic, sleep=time.sleep):
    """One press per completed line; unchanged lines time out without another press."""
    previous = None
    count = 0
    deadline = clock() + timeout
    while True:
        row = read()
        print(json.dumps(dict(row, presses=count)), flush=True)
        state = row["state"]
        if state == end_state and count:
            return count
        if state not in (1, 16):  # Lock=1 is a transient dialogue-exit state.
            raise RuntimeError(f"Unexpected UI state {state}; no input sent")
        if state == 16:
            if row["choice"]:
                raise RuntimeError("Dialogue choice needs an explicit route; no input sent")
            if row["line"] != previous and not row["working"] and row["can_click"]:
                if count >= max_lines:
                    raise RuntimeError("Dialogue line limit reached")
                press(row["line"])
                previous = row["line"]
                count += 1
                deadline = clock() + timeout
        if clock() >= deadline:
            raise RuntimeError("Dialogue state did not progress; no repeated A press sent")
        sleep(0.2)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--harness", type=Path, required=True)
    ap.add_argument("--driver", type=Path, required=True, help="Harness route executable")
    ap.add_argument("--label", required=True)
    ap.add_argument("--end-state", type=int, choices=(0, 26), default=26)
    ap.add_argument("--return-page", type=int, default=4)
    ap.add_argument("--timeout", type=float, default=30)
    ap.add_argument("--max-lines", type=int, default=64)
    args = ap.parse_args()
    if args.timeout <= 0 or args.max_lines <= 0:
        ap.error("timeout and max-lines must be positive")
    session = args.harness / GAME / "session" / args.label
    run_id = (session / "run_id").read_text().strip()
    port = int((session / "port").read_text().strip())
    sys.path.insert(0, str(args.harness / "gdb"))
    import rsp

    def read():
        if (session / "run_id").read_text().strip() != run_id:
            raise RuntimeError("Harness session changed")
        c = rsp.RSP(port=port, timeout=5)
        try:
            info = c.info()
            if info["program_id"] != 0x0100B63016916000:
                raise RuntimeError("Wrong game on harness memory channel")
            return snapshot(c, info["main_base"])
        finally:
            c.cont_and_close()

    def route(name, *variables):
        command = [str(args.driver), GAME, args.label, "route", name]
        for variable in variables:
            command.extend(("--var", variable))
        subprocess.run(command, check=True)

    count = advance_dialogue(read, lambda line: route("item26-dialogue-step.route",
                             f"line={line:#x}"), end_state=args.end_state,
                             timeout=args.timeout, max_lines=args.max_lines)
    route("item26-dialogue-ended.route", f"end_state={args.end_state}",
          f"return_page={args.return_page}")
    print(f"Dialogue ended after {count} state-gated A presses", flush=True)


if __name__ == "__main__":
    main()
