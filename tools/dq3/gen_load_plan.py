#!/usr/bin/env python3
"""gen_load_plan.py [--verify <main NSO>] - Dragon Quest III HD-2D Remake guest load plan (Switch 1.1.0.0,
main build 4F41309B39EEBE5E).

Heal All and Handy Heal All run the game's own field-menu routine on the game thread, with no input and
no menu. The plan (format 1, runtime 18+) patches one `bl` in the engine main loop to a stub in the zero
padding after .text. Once per frame the stub bumps a heartbeat and serves at most one request from the
mailbox, then continues to the original callee with every argument register (x0-x8, q0-q7) restored:

  mailbox +0x00 u32 seq        (module) bumped after the request below is written
          +0x04 u32 taken      (stub)   seq the stub has started (a request never runs twice)
          +0x08 u32 heartbeat  (stub)   +1 every frame
          +0x0C u32 done       (stub)   seq whose result is stored
          +0x10 u32 mode       (module) 0 Heal All, 1 Handy Heal All; anything else = cancelled
          +0x18 u64 result     (stub)   the helper's return value (below)
          +0x20 ...            (helper) scratch: FName, map context, the two outcome bytes, SE FName

The helper repeats the Misc menu's own Heal All / Handy Heal All case (main+0xBA0130 / +0xBA0174 and
their continuations +0xBA04A8 / +0xBA0514), minus the message window:

  1. IsLuaFlag(FName "FE169") (+0x9CE070; the Romaria throne): set -> return 0x20 | royal kind << 8
     (+0xBA08A0, its low byte: 1 King, 2 Queen); the menu only shows the King / Queen line, heals nobody.
  2. +0xA058A0 -> context, +0x66A7B8(&context, 0) -> area info; info && byte +0x43 == 0 -> return 0x30
     (the menu's Txt_Magic_Battle_Invalid_Common: no spells here).
  3. +0x8B9A00(mode, &a, &b): the game's whole Heal All (Kazing / Tingle / Squelch passes, then
     Heal ... Omniheal by caster and MP, the MP cost, the Info statistics counter). Both bytes in 4..6 is
     the menu's failure (6 / 6 "Nothing happens", else "the magic couldn't be cast"); otherwise it
     healed and the helper plays SYSSE_HEAL (+0xA57328) as the menu does. Return 0x10000 | a | b << 8.

usage: gen_load_plan.py [--verify NSO]   (checks layout, the hook, the cave and every callee's
call site in the menu routine against the NSO or the decompressed image)
"""
import argparse
import json
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
PKG = ROOT / "packages/DragonQuest3HD2D/dualscreen"
TITLE = "01003E601E324000"
BUILD = "4F41309B39EEBE5E1B8F2729B8105C35" + "0" * 32
SEGMENTS = [(0x0, 0x44FE090), (0x44FF000, 0x7B5E48), (0x4CB5000, 0x92F4E8)]
BSS = 0x2450B18
# The engine main loop (GameThread, once per frame): `bl 0x33BE740` with x0 = &global+0x450, w1 = flag.
HOOK, CALLEE = 0xCF90, 0x33BE740
CAVE = 0x44FE090  # .text ends at 0x44FE090; .rodata starts at 0x44FF000 (zero padding, executable)
MAILBOX_SIZE = 0x1000

FNAME_CTOR = 0x1129B70     # FName(this, const TCHAR*, EFindName = 1)
IS_LUA_FLAG = 0x9CE070     # bool (FName)
ROYAL_KIND = 0xBA08A0      # 1 King, 2 Queen (Misc menu King/Queen message choice)
AREA_CONTEXT = 0xA058A0    # -> object whose first qword the area query takes
AREA_INFO = 0x66A7B8       # (&context, 0) -> info; byte +0x43 = spells usable
HEAL_ALL = 0x8B9A00        # (mode, u8* a, u8* b)
PLAY_SE = 0xA57328         # (FName*)
TXT_FE169 = 0x46FD2F6      # L"FE169"
TXT_SYSSE_HEAL = 0x48D0640  # L"SYSSE_HEAL"

# The Misc menu routine's own uses (call site -> callee; ADRP/ADD site -> string): --verify checks them,
# so the helper calls exactly what the menu calls.
MENU_CALLS = [(0xBA0140, FNAME_CTOR), (0xBA0148, IS_LUA_FLAG), (0xBA0150, ROYAL_KIND),
              (0xBA0174 + 0x10, FNAME_CTOR), (0xBA018C, IS_LUA_FLAG), (0xBA0194, ROYAL_KIND),
              (0xBA04A8, AREA_CONTEXT), (0xBA04C0, AREA_INFO), (0xBA04DC, HEAL_ALL),
              (0xBA0514, AREA_CONTEXT), (0xBA052C, AREA_INFO), (0xBA0548, HEAL_ALL),
              (0xBA0868, FNAME_CTOR), (0xBA0870, PLAY_SE)]
MENU_STRINGS = [(0xBA0130, TXT_FE169), (0xBA0858, TXT_SYSSE_HEAL)]

# llvm-mc -triple=aarch64 of the stub and the helper; zero words / placeholders are filled by fixups().
STUB = [
    0x90000009,  # 000 adrp x9, mailbox                 (loader relocation)
    0x91000129,  # 004 add  x9, x9, :lo12:mailbox       (loader relocation)
    0xB940092A,  # 008 ldr  w10, [x9, #8]
    0x1100054A,  # 00c add  w10, w10, #1
    0xB900092A,  # 010 str  w10, [x9, #8]               heartbeat
    0xB940012A,  # 014 ldr  w10, [x9]                   seq
    0xB940052B,  # 018 ldr  w11, [x9, #4]               taken
    0x6B0B015F,  # 01c cmp  w10, w11
    0x54000440,  # 020 b.eq tail
    0xD50339BF,  # 024 dmb  ishld                       read the request after seq
    0xA9B17BFD,  # 028 stp  x29, x30, [sp, #-0xf0]!
    0x910003FD,  # 02c mov  x29, sp
    0xA90107E0,  # 030 stp  x0, x1, [sp, #0x10]         the callee's arguments
    0xA9020FE2,  # 034 stp  x2, x3, [sp, #0x20]
    0xA90317E4,  # 038 stp  x4, x5, [sp, #0x30]
    0xA9041FE6,  # 03c stp  x6, x7, [sp, #0x40]
    0xA9054FE8,  # 040 stp  x8, x19, [sp, #0x50]
    0xF90033F4,  # 044 str  x20, [sp, #0x60]
    0xAD0387E0,  # 048 stp  q0, q1, [sp, #0x70]
    0xAD048FE2,  # 04c stp  q2, q3, [sp, #0x90]
    0xAD0597E4,  # 050 stp  q4, q5, [sp, #0xb0]
    0xAD069FE6,  # 054 stp  q6, q7, [sp, #0xd0]
    0xAA0903F3,  # 058 mov  x19, x9
    0x2A0A03F4,  # 05c mov  w20, w10
    0xB9000674,  # 060 str  w20, [x19, #4]              taken = seq
    0xB9401260,  # 064 ldr  w0, [x19, #0x10]            mode
    0x91008261,  # 068 add  x1, x19, #0x20              scratch
    0x94000010,  # 06c bl   helper
    0xF9000E60,  # 070 str  x0, [x19, #0x18]            result
    0xD5033BBF,  # 074 dmb  ish
    0xB9000E74,  # 078 str  w20, [x19, #0xc]            done = seq
    0xAD469FE6,  # 07c ldp  q6, q7, [sp, #0xd0]
    0xAD4597E4,  # 080 ldp  q4, q5, [sp, #0xb0]
    0xAD448FE2,  # 084 ldp  q2, q3, [sp, #0x90]
    0xAD4387E0,  # 088 ldp  q0, q1, [sp, #0x70]
    0xF94033F4,  # 08c ldr  x20, [sp, #0x60]
    0xA9454FE8,  # 090 ldp  x8, x19, [sp, #0x50]
    0xA9441FE6,  # 094 ldp  x6, x7, [sp, #0x40]
    0xA94317E4,  # 098 ldp  x4, x5, [sp, #0x30]
    0xA9420FE2,  # 09c ldp  x2, x3, [sp, #0x20]
    0xA94107E0,  # 0a0 ldp  x0, x1, [sp, #0x10]
    0xA8CF7BFD,  # 0a4 ldp  x29, x30, [sp], #0xf0
    0,           # 0a8 tail: b CALLEE
    # helper(w0 mode, x1 scratch) -> x0
    0xA9BE7BFD,  # 0ac stp  x29, x30, [sp, #-0x20]!
    0x910003FD,  # 0b0 mov  x29, sp
    0xA90153F3,  # 0b4 stp  x19, x20, [sp, #0x10]
    0x2A0003F3,  # 0b8 mov  w19, w0
    0xAA0103F4,  # 0bc mov  x20, x1
    0xD2801FE0,  # 0c0 mov  x0, #0xff                   unknown mode (a cancelled request)
    0x7100067F,  # 0c4 cmp  w19, #1
    0x540005C8,  # 0c8 b.hi out
    0xAA1403E0,  # 0cc mov  x0, x20
    0,           # 0d0 adrp x1, L"FE169"
    0,           # 0d4 add  x1, x1, :lo12:L"FE169"
    0x52800022,  # 0d8 mov  w2, #1
    0,           # 0dc bl   FName ctor
    0xF9400280,  # 0e0 ldr  x0, [x20]
    0,           # 0e4 bl   IsLuaFlag
    0x360000A0,  # 0e8 tbz  w0, #0, 1f
    0,           # 0ec bl   royal kind
    0x53181C00,  # 0f0 ubfiz w0, w0, #8, #8             royal kind << 8
    0x321B0000,  # 0f4 orr  w0, w0, #0x20
    0x14000022,  # 0f8 b    out
    0,           # 0fc 1: bl area context
    0xF9400008,  # 100 ldr  x8, [x0]
    0xF9000688,  # 104 str  x8, [x20, #8]
    0x91002280,  # 108 add  x0, x20, #8
    0x2A1F03E1,  # 10c mov  w1, wzr
    0,           # 110 bl   area info
    0xB40000A0,  # 114 cbz  x0, 2f
    0x39410C08,  # 118 ldrb w8, [x0, #0x43]
    0x35000068,  # 11c cbnz w8, 2f
    0xD2800600,  # 120 mov  x0, #0x30                   no spells here
    0x14000017,  # 124 b    out
    0x7900229F,  # 128 2: strh wzr, [x20, #0x10]
    0x2A1303E0,  # 12c mov  w0, w19
    0x91004281,  # 130 add  x1, x20, #0x10
    0x91004682,  # 134 add  x2, x20, #0x11
    0,           # 138 bl   Heal All
    0x39404288,  # 13c ldrb w8, [x20, #0x10]
    0x39404689,  # 140 ldrb w9, [x20, #0x11]
    0x51001108,  # 144 sub  w8, w8, #4
    0x7100091F,  # 148 cmp  w8, #2
    0x54000088,  # 14c b.hi 3f
    0x51001129,  # 150 sub  w9, w9, #4
    0x7100093F,  # 154 cmp  w9, #2
    0x54000109,  # 158 b.ls 4f                          both 4..6: the menu's failure, no sound
    0x91006280,  # 15c 3: add x0, x20, #0x18
    0,           # 160 adrp x1, L"SYSSE_HEAL"
    0,           # 164 add  x1, x1, :lo12:L"SYSSE_HEAL"
    0x52800022,  # 168 mov  w2, #1
    0,           # 16c bl   FName ctor
    0x91006280,  # 170 add  x0, x20, #0x18
    0,           # 174 bl   play SE
    0x79402280,  # 178 4: ldrh w0, [x20, #0x10]
    0x32100000,  # 17c orr  w0, w0, #0x10000
    0xA94153F3,  # 180 out: ldp x19, x20, [sp, #0x10]
    0xA8C27BFD,  # 184 ldp  x29, x30, [sp], #0x20
    0xD65F03C0,  # 188 ret
]


def check(cond, msg):
    """A failed check stops the script (not an assert, so python -O still checks)."""
    if not cond:
        raise SystemExit(f"verify failed: {msg}")


def branch(op, source, target):
    delta = target - source
    check(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), f"branch {source:#x} -> {target:#x} out of range")
    return op | ((delta // 4) & 0x3FFFFFF)


def adrp(reg, source, target):
    pages = (target >> 12) - (source >> 12)
    check(-(1 << 20) <= pages < (1 << 20), "adrp range")
    imm = pages & 0x1FFFFF
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg


def add_lo12(reg, target):
    return 0x91000000 | ((target & 0xFFF) << 10) | (reg << 5) | reg


def fixups():
    w = list(STUB)
    at = lambda off: CAVE + off
    w[0xA8 // 4] = branch(0x14000000, at(0xA8), CALLEE)
    for off, string in ((0xD0, TXT_FE169), (0x160, TXT_SYSSE_HEAL)):
        w[off // 4] = adrp(1, at(off), string)
        w[off // 4 + 1] = add_lo12(1, string)
    for off, fn in ((0xDC, FNAME_CTOR), (0xE4, IS_LUA_FLAG), (0xEC, ROYAL_KIND), (0xFC, AREA_CONTEXT),
                    (0x110, AREA_INFO), (0x138, HEAL_ALL), (0x16C, FNAME_CTOR), (0x174, PLAY_SE)):
        w[off // 4] = branch(0x94000000, at(off), fn)
    check(all(w), "an unfilled stub word")
    return struct.pack(f"<{len(w)}I", *w)


def make_plan():
    payload = fixups()
    check(CAVE + len(payload) <= SEGMENTS[1][0], "stub overruns the padding before .rodata")
    return {
        "format": 1, "title_id": TITLE, "build_id": BUILD, "module": "main",
        "layout": {"segments": [{"location": loc, "size": size} for loc, size in SEGMENTS], "bss_size": BSS},
        "mailbox_size": MAILBOX_SIZE,
        "patches": [{"offset": HOOK, "expected": struct.pack("<I", branch(0x94000000, HOOK, CALLEE)).hex(),
                     "replacement": struct.pack("<I", branch(0x94000000, HOOK, CAVE)).hex()}],
        "payloads": [{"offset": CAVE, "expected": bytes(len(payload)).hex(), "bytes": payload.hex(),
                      "relocations": [
                          {"offset": 0, "kind": "aarch64_adrp", "target": "mailbox", "addend": 0},
                          {"offset": 4, "kind": "aarch64_add_lo12", "target": "mailbox", "addend": 0}]}]}


def text_of(path):
    d = pathlib.Path(path).read_bytes()
    if d[:4] != b"NSO0":
        return d  # an already decompressed, relocated image (research fixtures/update-main.img)
    import lz4.block
    flags = struct.unpack_from("<I", d, 0xC)[0]
    segs = [struct.unpack_from("<III", d, 0x10 + i * 0x10) for i in range(3)]
    csz = struct.unpack_from("<III", d, 0x60)
    check(d[0x40:0x60].hex().upper() == BUILD, "build id")
    check([(s[1], s[2]) for s in segs] == SEGMENTS, "segments")
    check(struct.unpack_from("<I", d, 0x3C)[0] == BSS, "bss")
    text = d[segs[0][0]:segs[0][0] + csz[0]]
    if flags & 1:
        text = lz4.block.decompress(text, uncompressed_size=segs[0][2])
    rodata = d[segs[1][0]:segs[1][0] + csz[1]]
    if flags & 2:
        rodata = lz4.block.decompress(rodata, uncompressed_size=segs[1][2])
    return text + bytes(SEGMENTS[1][0] - len(text)) + rodata


def verify(path, plan):
    image = text_of(path)
    u32 = lambda off: struct.unpack_from("<I", image, off)[0]
    for w in plan["patches"] + plan["payloads"]:
        exp = bytes.fromhex(w["expected"])
        check(image[w["offset"]:w["offset"] + len(exp)] == exp, f"expected bytes at {w['offset']:#x}")
    for site, fn in MENU_CALLS:
        check(u32(site) == branch(0x94000000, site, fn), f"menu call {site:#x} -> {fn:#x}")
    for site, string in MENU_STRINGS:
        reg = u32(site) & 31
        check(u32(site) == adrp(reg, site, string) and u32(site + 4) == add_lo12(reg, string),
              f"menu string {site:#x} -> {string:#x}")
    for string, text in ((TXT_FE169, "FE169"), (TXT_SYSSE_HEAL, "SYSSE_HEAL")):
        raw = (text + "\0").encode("utf-16-le")
        check(image[string:string + len(raw)] == raw, f"string {text}")


def main():
    ap = argparse.ArgumentParser(description="Write the DQ3 guest load plan (and optionally check it).")
    ap.add_argument("--verify", metavar="NSO", help="main NSO (or decompressed image) to check the plan against")
    args = ap.parse_args()
    plan = make_plan()
    if args.verify:
        verify(args.verify, plan)
        print("verified against", args.verify)
    out = PKG / "guest" / BUILD / "load-plan.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(plan, indent=1) + "\n")
    print("wrote", out.relative_to(ROOT))


if __name__ == "__main__":
    main()
