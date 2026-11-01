#!/usr/bin/env python3
"""Generate Fire Emblem Engage's guest load plan (Ver. 2.0.0 main, build 8C08B971...).

Purpose: hide the native top-screen HUD that the companion shows better (terrain window, unit
window strip, battle minimap, Somniel minimap), using the game's own show/hide code only.

1. Call stub. One `bl GameTime.Update` in App.Framework.Update (runs every frame on the game's
   main thread) goes through a stub in the zero padding after .text that serves one call request
   from the mailbox, then continues to GameTime.Update (same protocol as Luminescent Platinum):

     +0x00 u32 seq   (module)  +0x04 u32 taken (stub)  +0x08 u32 heartbeat (stub, +1 per frame)
     +0x0C u32 done  (stub)    +0x10 u64 fn (module, 0 = cancelled)  +0x18..+0x30 x0..x3
     +0x38 u64 result (stub)

2. Gates. The game's own visibility setters get a check in front of them. While the element's
   hide word is set, a request to show becomes a request to hide; the game's requested value is
   kept in a "want" word so the module can restore exactly what the game asked for:

     +0x40 u32 hide terrain   +0x44 hide unit window   +0x48 hide battle minimap  +0x4C hide hub map
     +0x50 u64 terrain Left   +0x58 terrain Right   (MapTerrainInfoSingle instances, module)
     +0x60 u64 unit Left      +0x68 unit Right      (UnitInfoWindow instances, module)
     +0x70 u32 want terrain L +0x74 want terrain R +0x78 want unit L +0x7C want unit R
     +0x80 u32 want hub map   +0x84 u32 forced-hide counter (stub, diagnostics)

   - MapTerrainInfoSingle.SetVisible(bool)  0x1E439C0 (entry; only for the two target instances)
   - UnitInfoWindow.SetActive(bool)         0x1FA5570 (entry; only for the two target instances)
   - MiniMapController.SetMode(Mode)        0x25E2E00 (entry; Show(1) -> Hide(0), Menu untouched;
     all 8 callers are MapMiniMap). The controller only stores the mode and a dirty flag, its own
     Update applies it, so MapMiniMap.SetDirty() re-applies the game's m_MiniMapMode either way.
   - HubMiniMap.Show()/Hide()               0x28BA270/0x28BA290 (the `mov w1, #1/#0` handed to
     GameObject.SetActive).

   Raw entry points (no gate, no want update) for the module's own hide/restore calls:
   terrain_raw(this, bool), unit_raw(this, bool), hub_hide_raw(this) (a copy of HubMiniMap.Hide).

usage: gen_load_plan.py [--verify <main NSO>] [--print-offsets]
"""
import argparse
import json
import pathlib
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
PKG = ROOT / "packages/FireEmblemEngage/dualscreen"
TITLE = "0100A6301214E000"
BUILD = "8C08B9719E085F91847B5E0F935D9488" + "0" * 32
SEGMENTS = [(0x0, 0x3F47038), (0x3F48000, 0x1E3321C), (0x5D7C000, 0x597080)]
BSS = 0x39BF80
CAVE = 0x3F47040  # .text ends at 0x3F47038; .rodata starts at 0x3F48000 (zero padding)

HOOK, CALLEE = 0x261BF74, 0x250D8E0  # Framework.Update: `mov x0, xzr; bl GameTime.Update`
TERRAIN_SET_VISIBLE = 0x1E439C0      # entry `sub sp, sp, #64`
UNIT_SET_ACTIVE = 0x1FA5570          # entry `sub sp, sp, #160`
MINIMAP_SET_MODE = 0x25E2E00         # entry `mov w8, #1`
HUB_SHOW_ARG, HUB_HIDE_ARG = 0x28BA280, 0x28BA2A0  # `mov w1, #1` / `mov w1, wzr`
HUB_GET_GO, GO_SET_ACTIVE = 0x2C46490, 0x2C4EA10   # Component.get_gameObject, GameObject.SetActive
# forecast helper (game's own battle calculator)
SCOPE_CTOR, SCOPE_DISPOSE = 0x19BBCF0, 0x19BBEC0       # BattleUtil.CalcScope .ctor(Unit, flags) / Dispose
INFO_GET_SIDE, SIDE_GET_ITEM = 0x1E7F210, 0x1E74CD0    # BattleInfo.GetSide(Type), BattleInfoSide.get_UnitItem
INFO_CALC_SIMPLE = 0x1E78E10   # BattleInfo.CalcSimple(Unit, UnitItem, Unit, SkillData, Flags, int)
CALC_SIMULATION = 0x246D610    # BattleCalculator.CalcSimulation()
GET_BATTLE_ATK_PARAM = 0x2914EA0  # InfoUtil.GetBattleAtkParam(Type, BattleSceneList, out int, out List)
DETAIL_SIMPLE_HIT, DETAIL_SIMPLE_CRIT = 0x1E76860, 0x1E76890  # BattleDetail.get_SimpleHit/Critical
DETAIL_EFFICACY = 0x1E762F0    # BattleDetail.get_WeaponEfficacy
DETAIL_HIT, DETAIL_CRIT = 0x1E76740, 0x1E767A0  # BattleDetail.get_Hit / get_Critical (vs the foe)
PARAM_GET_RESULT = 0x1E8DA30   # BattleParam.GetResult(BattleInfoSide) -> float

ORIG = {
    HOOK: 0x94000000 | (((CALLEE - HOOK) // 4) & 0x3FFFFFF),
    TERRAIN_SET_VISIBLE: 0xD10103FF,  # sub sp, sp, #0x40
    UNIT_SET_ACTIVE: 0xD10283FF,      # sub sp, sp, #0xa0
    MINIMAP_SET_MODE: 0x52800028,     # mov w8, #1
    HUB_SHOW_ARG: 0x52800021,         # mov w1, #1
    HUB_HIDE_ARG: 0x2A1F03E1,         # mov w1, wzr
}

# (label, asm). `@X` = a branch to main RVA X, resolved relative to the instruction's own address.
# `MB` lines are the adrp/add pair the loader relocates to the mailbox.
BLOCKS = [
    ("call", """
        MB x9
        ldr  w10, [x9, #8]
        add  w10, w10, #1
        str  w10, [x9, #8]
        ldr  w10, [x9]
        ldr  w11, [x9, #4]
        cmp  w10, w11
        b.eq 2f
        dmb  ishld
        stp  x29, x30, [sp, #-0x20]!
        mov  x29, sp
        stp  x19, x20, [sp, #0x10]
        mov  x19, x9
        mov  w20, w10
        str  w20, [x19, #4]
        ldr  x16, [x19, #0x10]
        cbz  x16, 1f
        ldp  x0, x1, [x19, #0x18]
        ldp  x2, x3, [x19, #0x28]
        blr  x16
        str  x0, [x19, #0x38]
    1:  dmb  ish
        str  w20, [x19, #0xc]
        ldp  x19, x20, [sp, #0x10]
        ldp  x29, x30, [sp], #0x20
    2:  mov  x0, xzr
        b    @%#x
    """ % CALLEE),
    ("terrain_gate", """
        MB x16
        ldr  x17, [x16, #0x50]
        cmp  x0, x17
        b.eq 1f
        ldr  x17, [x16, #0x58]
        cmp  x0, x17
        b.eq 2f
        b    4f
    1:  and  w17, w1, #0xff
        str  w17, [x16, #0x70]
        b    3f
    2:  and  w17, w1, #0xff
        str  w17, [x16, #0x74]
    3:  ldr  w17, [x16, #0x40]
        cbz  w17, 4f
        mov  w1, wzr
        ldr  w17, [x16, #0x84]
        add  w17, w17, #1
        str  w17, [x16, #0x84]
    4:  sub  sp, sp, #0x40
        b    @%#x
    """ % (TERRAIN_SET_VISIBLE + 4)),
    ("terrain_raw", """
        sub  sp, sp, #0x40
        b    @%#x
    """ % (TERRAIN_SET_VISIBLE + 4)),
    ("unit_gate", """
        MB x16
        ldr  x17, [x16, #0x60]
        cmp  x0, x17
        b.eq 1f
        ldr  x17, [x16, #0x68]
        cmp  x0, x17
        b.eq 2f
        b    4f
    1:  and  w17, w1, #0xff
        str  w17, [x16, #0x78]
        b    3f
    2:  and  w17, w1, #0xff
        str  w17, [x16, #0x7c]
    3:  ldr  w17, [x16, #0x44]
        cbz  w17, 4f
        mov  w1, wzr
        ldr  w17, [x16, #0x84]
        add  w17, w17, #1
        str  w17, [x16, #0x84]
    4:  sub  sp, sp, #0xa0
        b    @%#x
    """ % (UNIT_SET_ACTIVE + 4)),
    ("unit_raw", """
        sub  sp, sp, #0xa0
        b    @%#x
    """ % (UNIT_SET_ACTIVE + 4)),
    ("minimap_gate", """
        MB x16
        ldr  w17, [x16, #0x48]
        cbz  w17, 1f
        cmp  w1, #1
        b.ne 1f
        mov  w1, wzr
        ldr  w17, [x16, #0x84]
        add  w17, w17, #1
        str  w17, [x16, #0x84]
    1:  mov  w8, #1
        b    @%#x
    """ % (MINIMAP_SET_MODE + 4)),
    ("hub_show_gate", """
        MB x16
        mov  w17, #1
        str  w17, [x16, #0x80]
        mov  w1, #1
        ldr  w17, [x16, #0x4c]
        cbz  w17, 1f
        mov  w1, wzr
        ldr  w17, [x16, #0x84]
        add  w17, w17, #1
        str  w17, [x16, #0x84]
    1:  b    @%#x
    """ % (HUB_SHOW_ARG + 4)),
    ("hub_hide_gate", """
        MB x16
        str  wzr, [x16, #0x80]
        mov  w1, wzr
        b    @%#x
    """ % (HUB_HIDE_ARG + 4)),
    ("hub_hide_raw", """
        stp  x29, x30, [sp, #-16]!
        mov  x29, sp
        mov  x1, xzr
        bl   @%#x
        mov  w1, wzr
        mov  x2, xzr
        ldp  x29, x30, [sp], #16
        b    @%#x
    """ % (HUB_GET_GO, GO_SET_ACTIVE)),
    # Combat forecast for any pair, the way the native forecast panel computes it (read-only;
    # appended last so every earlier entry keeps its RVA). x0 offense Unit, x1 defense Unit,
    # x2 out (mailbox scratch), x3 the offense's weapon UnitItem (the native forecast's L/R pick,
    # BattleInfoSide.m_UnitItem; 0 = the scope's own pick); BattleInfo.Flags is 1 (Simulation, as the native panel). BattleUtil.CalcScope borrows a pooled BattleInfo/Calculator
    # (single-unit calc picks the offense's weapon), BattleInfo.CalcSimple(offense, that weapon,
    # defense, null, 1, 0) as MapSequenceTargetSelect.UpdateInfo does, BattleCalculator
    # .CalcSimulation (pushes/pops the battle RNG seed), then per side (0 offense, 1 defense) at
    # out + side*0x40: +0 InfoUtil.GetBattleAtkParam return (attacks), +4 total damage (native
    # "Dmg"), +8 damage-string count, +0xC side Hp, +0x10 status bits, +0x14 TotalResult,
    # +0x18 BattleTimes, +0x1C TotalAttack, +0x20 SimpleHit and +0x24 SimpleCritical
    # (BattleParam.GetResult, float), +0x28 WeaponEfficacy, +0x34 TotalOrder (live: rounds), +0x18 BattleTimes = attacks; TotalAttack / BattleTimes = hits per attack (2 for a brave weapon), +0x38 the damage-string list's items array (string[]), +0x2C Hit and +0x30 Critical (vs the
    # other side: what the native panel prints -- live: Crit 19 = 25 crit - 6 dodge); out+0x80 BattleInfo flags, +0x84 Range,
    # +0x88 BattleCount, +0x8C scene count (-1 = no list).
    # Values are copied before CalcScope.Dispose resets and returns the pooled objects.
    ("forecast", """
        stp  x29, x30, [sp, #-0x70]!
        mov  x29, sp
        stp  x19, x20, [sp, #0x10]
        stp  x21, x22, [sp, #0x20]
        stp  x23, x24, [sp, #0x30]
        mov  x19, x0
        mov  x20, x1
        mov  x21, x2
        str  x3, [sp, #0x60]
        stp  xzr, xzr, [sp, #0x40]
        add  x0, sp, #0x40
        mov  x1, x19
        mov  w2, wzr
        mov  x3, xzr
        bl   @%#x
        ldr  x0, [sp, #0x40]
        mov  w1, wzr
        mov  x2, xzr
        bl   @%#x
        mov  x1, xzr
        bl   @%#x
        mov  x22, x0
        ldr  x8, [sp, #0x60]
        cbz  x8, 7f
        mov  x22, x8
    7:  ldr  x0, [sp, #0x40]
        mov  x1, x19
        mov  x2, x22
        mov  x3, x20
        mov  x4, xzr
        mov  x5, #1
        mov  w6, wzr
        mov  x7, xzr
        bl   @%#x
        ldr  x0, [sp, #0x48]
        mov  x1, xzr
        bl   @%#x
        mov  w23, wzr
    3:  str  wzr, [sp, #0x50]
        str  xzr, [sp, #0x58]
        add  x24, x21, x23, lsl #6
        mov  w0, w23
        ldr  x8, [sp, #0x48]
        ldr  x1, [x8, #0x28]
        add  x2, sp, #0x50
        add  x3, sp, #0x58
        mov  x4, xzr
        bl   @%#x
        str  w0, [x24]
        str  xzr, [x24, #0x38]
        ldr  w8, [sp, #0x50]
        str  w8, [x24, #4]
        ldr  x8, [sp, #0x58]
        cbz  x8, 4f
        ldr  x9, [x8, #0x10]
        str  x9, [x24, #0x38]
        ldr  w8, [x8, #0x18]
    4:  str  w8, [x24, #8]
        ldr  x0, [sp, #0x40]
        mov  w1, w23
        mov  x2, xzr
        bl   @%#x
        mov  x20, x0
        ldr  w8, [x20, #0x94]
        str  w8, [x24, #0xc]
        ldr  w8, [x20, #0x50]
        str  w8, [x24, #0x10]
        ldr  w8, [x20, #0xc4]
        str  w8, [x24, #0x14]
        ldr  w8, [x20, #0xb0]
        str  w8, [x24, #0x18]
        ldr  w8, [x20, #0xbc]
        str  w8, [x24, #0x1c]
        ldr  w8, [x20, #0xb4]
        str  w8, [x24, #0x34]
        ldr  x0, [x20, #0x58]
        mov  x1, xzr
        bl   @%#x
        mov  x1, x20
        mov  x2, xzr
        bl   @%#x
        str  s0, [x24, #0x20]
        ldr  x0, [x20, #0x58]
        mov  x1, xzr
        bl   @%#x
        mov  x1, x20
        mov  x2, xzr
        bl   @%#x
        str  s0, [x24, #0x24]
        ldr  x0, [x20, #0x58]
        mov  x1, xzr
        bl   @%#x
        str  w0, [x24, #0x28]
        ldr  x0, [x20, #0x58]
        mov  x1, xzr
        bl   @%#x
        mov  x1, x20
        mov  x2, xzr
        bl   @%#x
        str  s0, [x24, #0x2c]
        ldr  x0, [x20, #0x58]
        mov  x1, xzr
        bl   @%#x
        mov  x1, x20
        mov  x2, xzr
        bl   @%#x
        str  s0, [x24, #0x30]
        add  w23, w23, #1
        cmp  w23, #2
        b.lt 3b
        ldr  x8, [sp, #0x40]
        ldr  x8, [x8, #0x10]
        cbz  x8, 5f
        ldr  w8, [x8, #0x10]
    5:  str  w8, [x21, #0x80]
        ldr  x8, [sp, #0x40]
        ldr  w9, [x8, #0xa8]
        str  w9, [x21, #0x84]
        ldr  w9, [x8, #0xac]
        str  w9, [x21, #0x88]
        mov  w9, #-1
        ldr  x8, [sp, #0x48]
        ldr  x8, [x8, #0x28]
        cbz  x8, 6f
        ldr  x8, [x8, #0x10]
        cbz  x8, 6f
        ldr  w9, [x8, #0x18]
    6:  str  w9, [x21, #0x8c]
        add  x0, sp, #0x40
        mov  x1, xzr
        bl   @%#x
        mov  x0, #1
        ldp  x23, x24, [sp, #0x30]
        ldp  x21, x22, [sp, #0x20]
        ldp  x19, x20, [sp, #0x10]
        ldp  x29, x30, [sp], #0x70
        ret
    """ % (SCOPE_CTOR, INFO_GET_SIDE, SIDE_GET_ITEM, INFO_CALC_SIMPLE, CALC_SIMULATION,
           GET_BATTLE_ATK_PARAM, INFO_GET_SIDE, DETAIL_SIMPLE_HIT, PARAM_GET_RESULT,
           DETAIL_SIMPLE_CRIT, PARAM_GET_RESULT, DETAIL_EFFICACY, DETAIL_HIT, PARAM_GET_RESULT,
           DETAIL_CRIT, PARAM_GET_RESULT, SCOPE_DISPOSE)),
]

# Patched instructions: site -> (block label it branches to, branch kind)
PATCHES = [
    (HOOK, "call", 0x94000000),  # bl (the stub tail-branches to the original callee)
    (TERRAIN_SET_VISIBLE, "terrain_gate", 0x14000000),
    (UNIT_SET_ACTIVE, "unit_gate", 0x14000000),
    (MINIMAP_SET_MODE, "minimap_gate", 0x14000000),
    (HUB_SHOW_ARG, "hub_show_gate", 0x14000000),
    (HUB_HIDE_ARG, "hub_hide_gate", 0x14000000),
]


def check(cond, msg):
    if not cond:
        raise SystemExit(f"verify failed: {msg}")


def branch(op, source, target):
    delta = target - source
    check(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), f"branch {source:#x} -> {target:#x}")
    return op | ((delta // 4) & 0x3FFFFFF)


def assemble(base, asm):
    """Assemble one block placed at main RVA `base`. Returns (bytes, mailbox relocation offsets)."""
    lines, relocs = [], []
    for raw in asm.strip().splitlines():
        line = raw.strip()
        if not line:
            continue
        label = ""
        if ":" in line.split()[0]:
            label, line = line.split(":", 1)
            label += ":"
            line = line.strip()
        if line.startswith("MB "):
            reg = line.split()[1]
            relocs.append(4 * len(lines))
            lines.append(f"{label} adrp {reg}, #0")
            lines.append(f"add {reg}, {reg}, #0")
            continue
        if "@" in line:
            op, target = line.split("@")
            target = int(target, 16)
            lines.append(f"{label} {op.strip()} #{target - (base + 4 * len(lines))}")
            continue
        lines.append(f"{label} {line}")
    with tempfile.TemporaryDirectory() as tmp:
        src, obj, out = (pathlib.Path(tmp) / n for n in ("a.s", "a.o", "a.bin"))
        src.write_text("\n".join(lines) + "\n")
        subprocess.run(["llvm-mc", "-triple=aarch64", "-filetype=obj", str(src), "-o", str(obj)], check=True)
        subprocess.run(["llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(out)], check=True)
        data = out.read_bytes()
    check(len(data) == 4 * len(lines), "assembled size")
    return data, relocs


def build():
    blocks, at = {}, CAVE
    payload, relocations = b"", []
    for label, asm in BLOCKS:
        data, rel = assemble(at, asm)
        blocks[label] = at
        for off in rel:
            relocations.append({"offset": at - CAVE + off, "kind": "aarch64_adrp", "target": "mailbox", "addend": 0})
            relocations.append({"offset": at - CAVE + off + 4, "kind": "aarch64_add_lo12", "target": "mailbox", "addend": 0})
        payload += data
        at += len(data)
    check(at <= SEGMENTS[1][0], "stub overruns the padding before .rodata")
    patches = []
    for site, label, op in PATCHES:
        patches.append({"offset": site, "expected": struct.pack("<I", ORIG[site]).hex(),
                        "replacement": struct.pack("<I", branch(op, site, blocks[label])).hex()})
    plan = {
        "format": 1, "title_id": TITLE, "build_id": BUILD, "module": "main",
        "layout": {"segments": [{"location": l, "size": s} for l, s in SEGMENTS], "bss_size": BSS},
        "mailbox_size": 0x1000,
        "patches": patches,
        "payloads": [{"offset": CAVE, "expected": bytes(len(payload)).hex(), "bytes": payload.hex(),
                      "relocations": relocations}],
    }
    return plan, blocks


def verify(nso_path, plan):
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
    text += bytes(SEGMENTS[1][0] - len(text))
    for w in plan["patches"] + plan["payloads"]:
        exp = bytes.fromhex(w["expected"])
        check(text[w["offset"]:w["offset"] + len(exp)] == exp, hex(w["offset"]))
    # the copied Hide() must match the game's
    check(text[0x28BA290:0x28BA298] == bytes.fromhex("fd7bbfa9fd030091"), "HubMiniMap.Hide prologue")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verify", metavar="NSO")
    ap.add_argument("--print-offsets", action="store_true", help="print the raw entry RVAs for fee_hud.cpp")
    args = ap.parse_args()
    plan, blocks = build()
    if args.verify:
        verify(args.verify, plan)
        print("verified against", args.verify)
    if args.print_offsets:
        for k, v in blocks.items():
            print(f"{k:16s} {v:#x}")
    out = PKG / "guest" / BUILD / "load-plan.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(plan, indent=1) + "\n")
    print("wrote", out.relative_to(ROOT))


if __name__ == "__main__":
    main()
