#!/usr/bin/env python3
"""Generate Luminescent Platinum's guest load plan (BD 1.3.0 main, build 94CEAE32...).

The plan patches one `bl` in FieldManager.fdUpdate to a small stub in the zero padding after
.text. Once per field frame the stub serves one call request from the mailbox on the game's own
thread, then continues to the original callee:

  mailbox +0x00 u32 seq        (module) bumped after the request below is written
          +0x04 u32 taken      (stub)   seq the stub has started
          +0x08 u32 heartbeat  (stub)   +1 every field frame
          +0x0C u32 done       (stub)   seq whose result is stored
          +0x10 u64 fn         (module) guest address to call, 0 = cancelled
          +0x18 u64 x0..x3     (module) arguments
          +0x38 u64 result     (stub)   x0 after the call
          +0x80 ...            (module) scratch (fake managed objects passed as arguments)

The module checks every precondition itself (one call per frame), see lp_guest.h.

usage: gen_load_plan.py [--verify <main NSO>]   (the NSO is checked for layout, hook and cave bytes)
"""
import argparse
import json
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
PKG = ROOT / "packages/LuminescentPlatinum/dualscreen"
TITLE = "0100000011D90000"
BUILD = "94CEAE325C205C4B9D6F7235552F28FD" + "0" * 32
SEGMENTS = [(0x0, 0x2CF6A30), (0x2CF7000, 0x19E2DE0), (0x46DA000, 0x578420)]
BSS = 0x40FBE0
# FieldManager.fdUpdate 0x179A970 (every field frame, unconditionally on the way to the event manager
# update): `bl EvDataManager.get_Instanse` with x0 = 0 just before. LP's exlaunch hooks only the entry of
# fdUpdate and its IPS leaves the body alone. (fdLateUpdate looked right but never runs in LP's field.)
HOOK, CALLEE = 0x179AA48, 0x2C3D4D0
CAVE = 0x2CF6A40  # .text ends at 0x2CF6A30; .rodata starts at 0x2CF7000 (zero padding, executable)

# llvm-mc -triple=aarch64 of the stub below; the last word is the tail branch, filled in.
STUB = [
    0x90000009,  # adrp x9, mailbox                 (loader relocation)
    0x91000129,  # add  x9, x9, :lo12:mailbox       (loader relocation)
    0xB940092A,  # ldr  w10, [x9, #8]
    0x1100054A,  # add  w10, w10, #1
    0xB900092A,  # str  w10, [x9, #8]               heartbeat
    0xB940012A,  # ldr  w10, [x9]                   seq
    0xB940052B,  # ldr  w11, [x9, #4]               taken
    0x6B0B015F,  # cmp  w10, w11
    0x54000240,  # b.eq tail
    0xD50339BF,  # dmb  ishld                       read the request after seq
    0xA9BE7BFD,  # stp  x29, x30, [sp, #-0x20]!
    0x910003FD,  # mov  x29, sp
    0xA90153F3,  # stp  x19, x20, [sp, #0x10]
    0xAA0903F3,  # mov  x19, x9
    0x2A0A03F4,  # mov  w20, w10
    0xB9000674,  # str  w20, [x19, #4]              taken = seq (never runs a request twice)
    0xF9400A70,  # ldr  x16, [x19, #0x10]           fn
    0xB40000B0,  # cbz  x16, 1f
    0xA9418660,  # ldp  x0, x1, [x19, #0x18]
    0xA9428E62,  # ldp  x2, x3, [x19, #0x28]
    0xD63F0200,  # blr  x16
    0xF9001E60,  # str  x0, [x19, #0x38]            result
    0xD5033BBF,  # 1: dmb ish
    0xB9000E74,  # str  w20, [x19, #0xc]            done = seq
    0xA94153F3,  # ldp  x19, x20, [sp, #0x10]
    0xA8C27BFD,  # ldp  x29, x30, [sp], #0x20
    0xAA1F03E0,  # tail: mov x0, xzr
    0,           # b    EvDataManager.get_Instanse
]


def check(cond, msg):
    """A failed check stops the script (not an assert, so python -O still checks)."""
    if not cond:
        raise SystemExit(f"verify failed: {msg}")


def branch(op, source, target):
    delta = target - source
    check(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), f"branch {source:#x} -> {target:#x} out of range")
    return op | ((delta // 4) & 0x3FFFFFF)


def stub_bytes():
    w = list(STUB)
    w[-1] = branch(0x14000000, CAVE + 4 * (len(w) - 1), CALLEE)
    return struct.pack(f"<{len(w)}I", *w)


def make_plan():
    payload = stub_bytes()
    check(CAVE + len(payload) <= SEGMENTS[1][0], "stub overruns the padding before .rodata")
    return {
        "format": 1, "title_id": TITLE, "build_id": BUILD, "module": "main",
        "layout": {"segments": [{"location": loc, "size": size} for loc, size in SEGMENTS], "bss_size": BSS},
        "mailbox_size": 0x1000,
        "patches": [{"offset": HOOK, "expected": struct.pack("<I", branch(0x94000000, HOOK, CALLEE)).hex(),
                     "replacement": struct.pack("<I", branch(0x94000000, HOOK, CAVE)).hex()}],
        "payloads": [{"offset": CAVE, "expected": bytes(len(payload)).hex(), "bytes": payload.hex(),
                      "relocations": [
                          {"offset": 0, "kind": "aarch64_adrp", "target": "mailbox", "addend": 0},
                          {"offset": 4, "kind": "aarch64_add_lo12", "target": "mailbox", "addend": 0}]}]}


def verify(nso_path, plan, ips_path=None):
    import lz4.block
    d = pathlib.Path(nso_path).read_bytes()
    check(d[:4] == b"NSO0", "not an NSO")
    flags = struct.unpack_from("<I", d, 0xC)[0]
    segs = [struct.unpack_from("<III", d, 0x10 + i * 0x10) for i in range(3)]
    csz = struct.unpack_from("<III", d, 0x60)
    check(d[0x40:0x60].hex().upper() == BUILD, "build id")
    check([(s[1], s[2]) for s in segs] == SEGMENTS, "segments")
    check(struct.unpack_from("<I", d, 0x3C)[0] == BSS, "bss")
    text = d[segs[0][0]:segs[0][0] + csz[0]]
    if flags & 1:
        text = lz4.block.decompress(text, uncompressed_size=segs[0][2])
    text += bytes(SEGMENTS[1][0] - len(text))  # page padding up to .rodata
    if ips_path:
        data = pathlib.Path(ips_path).read_bytes()
        check(data[:5] == b"IPS32", "expected IPS32")
        text = bytearray(text)
        at = 5
        while data[at:at + 4] != b"EEOF":
            offset = int.from_bytes(data[at:at + 4], "big") - 0x100
            size = int.from_bytes(data[at + 4:at + 6], "big")
            at += 6
            if size:
                payload = data[at:at + size]; at += size
            else:
                count = int.from_bytes(data[at:at + 2], "big")
                payload = data[at + 2:at + 3] * count; at += 3
            check(offset >= 0, "IPS32 offset")
            if offset < len(text):
                end = min(offset + len(payload), len(text))
                text[offset:end] = payload[:end - offset]
    for w in plan["patches"] + plan["payloads"]:
        exp = bytes.fromhex(w["expected"])
        check(text[w["offset"]:w["offset"] + len(exp)] == exp, hex(w["offset"]))


def main():
    global TITLE, BUILD, SEGMENTS, BSS, HOOK, CALLEE, CAVE, PKG
    ap = argparse.ArgumentParser(description="Write the guest load plan (and optionally check it against main).")
    ap.add_argument("--verify", metavar="NSO", help="BD 1.3.0 main NSO to check layout, hook and cave bytes against")
    ap.add_argument("--edition", choices=("diamond", "pearl"), default="diamond")
    ap.add_argument("--ips", help="also verify hook and cave after applying the Luminescent IPS32")
    args = ap.parse_args()
    if args.edition == "pearl":
        TITLE = "010018E011D92000"
        BUILD = "38F59CBDA2EB9C44B72F94C4D25935A2" + "0" * 32
        SEGMENTS = [(0x0, 0x2CF6A50), (0x2CF7000, 0x19E2DD0), (0x46DA000, 0x578420)]
        HOOK, CALLEE, CAVE = 0x1BCDCD8, 0x1B000C0, 0x2CF6A60
        PKG = ROOT / "packages/LuminescentPlatinumPearl/dualscreen"
    check(not args.ips or args.verify, "--ips requires --verify")
    plan = make_plan()
    if args.verify:
        verify(args.verify, plan)
        if args.ips:
            verify(args.verify, plan, args.ips)
            print("verified with Luminescent IPS", args.ips)
        print("verified against", args.verify)
    out = PKG / "guest" / BUILD / "load-plan.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(plan, indent=1) + "\n")
    print("wrote", out.relative_to(ROOT))


if __name__ == "__main__":
    main()
