#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only, build-specific loading investigation via a harness gdb port.

Put the existing rsp and live_reads helpers on PYTHONPATH. Writes timed JSON lines. Every
connection resumes the guest, including failed reads. No guest memory is written.
"""
import argparse
import json
import struct
import time

import rsp
from live_reads import Reader, SLOT_PLAYER, SLOT_TIME_MODULE, SLOT_UI_SYSTEM_MGR


def snapshot(c, base):
    r = Reader(c)
    log = []

    def root(slot, name, hops):
        p = r.resolve_root(base, slot, name, hops, log)
        return r.u64(p) if p else 0

    def count(lock):
        rows = r.u64(lock + 0x10) if r.ok(lock) else 0
        return r.i32(rows + 0x18) if r.ok(rows) else None

    sm = root(134966304, 'ScenarioModule', 2)
    tm = root(SLOT_TIME_MODULE, 'TimeModule', 3)
    player = root(SLOT_PLAYER, 'Player', 3)
    klass = r.u64(base + SLOT_UI_SYSTEM_MGR)
    if r.class_name(klass)[0] != 'UISystemMgr':
        raise ValueError('UISystemMgr class mismatch')
    ui = r.u64(r.u64(klass + 0xB8) + 0x28)
    out = {'roots': log, 'scenario': hex(sm), 'player': hex(player)}
    if r.ok(sm):
        raw = c.read(sm + 0x30, 0xB0, on_error='raise')
        out.update(unload_complete=raw[0], load_complete=raw[1],
                   loading=raw[0x80], posting_loading=raw[0x81],
                   scenes=list(struct.unpack_from('<iii', raw, 0x84)),
                   operations=[hex(struct.unpack_from('<Q', raw, i)[0]) for i in (8, 16)])
    if r.ok(tm):
        out.update(pause_count=count(r.u64(tm + 0x28)), time_scale=r.f32(tm + 0x78))
    if r.ok(player):
        bag = r.u64(player + 0x1B0)
        out.update(bag=hex(bag), gold=r.i32(bag + 0xA8) if r.ok(bag) else None)
        actor = r.u64(player + 0x38)
        transform = r.u64(actor + 0x28) if r.ok(actor) else 0
        if r.ok(transform):
            out.update(position=list(struct.unpack('<fff', c.read(transform + 0x10, 12))),
                       actor_scene=r.i32(transform + 0x74))
    if r.ok(ui):
        layer = r.u64(ui + 0xB0)
        state = r.u64(layer + 0x70) if r.ok(layer) else 0
        out.update(ui_state=r.i32(state + 0x84) if r.ok(state) else None,
                   load_lock_count=count(r.u64(ui + 0x30)))
        shared = r.u64(ui + 0x18)
        entries = r.dict_entries_int_key(r.u64(shared + 0x28)) if r.ok(shared) else []
        for key, asset in entries:
            if key != 8:
                continue
            control = r.u64(asset + 0x58)
            view = r.u64(asset + 0x50)
            out.update(mask_asset=hex(asset), mask_control=hex(control), mask_view=hex(view))
            if r.ok(control):
                out['control_class'] = r.class_name(r.u64(control))[0]
                if out['control_class'] == 'LoadingMaskControl':
                    raw = c.read(control + 0x2C, 0x38, on_error='raise')
                    out.update(mask_requests=struct.unpack_from('<i', raw)[0], mask_display=raw[0x1C])
            if r.ok(view):
                out['view_class'] = r.class_name(r.u64(view))[0]
                if out['view_class'] == 'LoadingMaskUI':
                    raw = c.read(view + 0x100, 0x24, on_error='raise')
                    out.update(mask_can_exit=raw[0], mask_wait_exit=raw[1], mask_play_end=raw[0x20])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--interval', type=float, default=0.05)
    args = ap.parse_args()
    start = time.monotonic()
    base = None
    while time.monotonic() - start < args.seconds:
        tick = time.monotonic()
        c = None
        try:
            c = rsp.RSP(port=args.port, timeout=5)
            if base is None:
                base = c.info()['main_base']
            row = snapshot(c, base)
        except Exception as e:
            row = {'error': str(e)}
        finally:
            if c:
                try:
                    c.cont()
                    time.sleep(0.02)
                finally:
                    c.close()
        row.update(t=round(tick-start, 6), wall=time.time(), probe_ms=round((time.monotonic()-tick)*1000, 3))
        print(json.dumps(row), flush=True)
        time.sleep(max(0.01, args.interval - (time.monotonic()-tick)))


if __name__ == '__main__':
    main()
