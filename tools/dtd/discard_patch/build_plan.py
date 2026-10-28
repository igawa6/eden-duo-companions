#!/usr/bin/env python3
"""Build the discard load plan (Eden's per-build code patch) for Dave the Diver 1.0.6.1267.

    python3 tools/dtd/discard_patch/build_plan.py <out.json>

Needs clang + ld.lld (as build_module.sh). The "expected" bytes are the game's own bytes at the
two patched places (from round 17 on the player's dump): Eden refuses the patch unless they match.
"""
import json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD_ID = "F74B633E480E4E161F80A7C16A467F2A" + "0" * 32
LAYOUT = {"segments": [{"location": 0x0, "size": 0x7FD38DC},
                       {"location": 0x7FD4000, "size": 0x16AE810},
                       {"location": 0x9683000, "size": 0xBA13E8}],
          "bss_size": 0x3B0C18}
HOOK, CAVE, REMOVE_IMPL = 0x4203910, 0x25CE2E0, 0x4571230
# tonight's menu (round 22): SushiBarManager.Update is a single "b main+402EF00"
HOOK2, UPDATE2_BODY, ADD_MENU, REMOVE_MENU = 0x402EEF0, 0x402EF00, 0x49D1960, 0x49D2340
MAILBOX = 0x1000
# HideHUD (round 63): Component.get_transform, Transform.get_localScale / set_localScale
GET_TRANSFORM, GET_LOCALSCALE, SET_LOCALSCALE = 0x7997BC0, 0x79C6380, 0x79C6500
# DTD_MENU_HOOK=0 leaves SushiBarManager.Update untouched (test build: is the sushi bar hook what
# stops the game loading the sushi bar?). The menu code stays in the cave, unreachable.
MENU_HOOK = os.environ.get("DTD_MENU_HOOK", "1") != "0"

def assemble():
    with tempfile.TemporaryDirectory() as d:
        o, elf, b = (os.path.join(d, n) for n in ("s.o", "s.elf", "s.bin"))
        subprocess.check_call(["clang", "--target=aarch64-none-elf", "-c", os.path.join(HERE, "stub.S"), "-o", o])
        subprocess.check_call(["ld.lld", "-T", os.path.join(HERE, "link.ld"), f"--defsym=REMOVE_IMPL={REMOVE_IMPL:#x}",
                               f"--defsym=LATEUPDATE_CONT={HOOK + 4:#x}", f"--defsym=ADD_MENU={ADD_MENU:#x}", f"--defsym=REMOVE_MENU={REMOVE_MENU:#x}",
                               f"--defsym=SUSHIBAR_UPDATE_BODY={UPDATE2_BODY:#x}", f"--defsym=GET_TRANSFORM={GET_TRANSFORM:#x}",
                               f"--defsym=GET_LOCALSCALE={GET_LOCALSCALE:#x}", f"--defsym=SET_LOCALSCALE={SET_LOCALSCALE:#x}", o, "-o", elf])
        subprocess.check_call(["llvm-objcopy", "-O", "binary", "--only-section=.text", elf, b])
        syms = subprocess.check_output(["llvm-nm", elf], text=True)
        at = {l.split()[2]: int(l.split()[0], 16) for l in syms.splitlines() if len(l.split()) == 3}
        return (open(b, "rb").read(), at["mb_adrp"] - CAVE, at["mb_add"] - CAVE,
                at["mb2_adrp"] - CAVE, at["mb2_add"] - CAVE, at["menu_entry"])

def branch(src, dst):
    imm = (dst - src) // 4
    assert -(1 << 25) <= imm < (1 << 25)
    return (0x14000000 | (imm & 0x3FFFFFF)).to_bytes(4, "little")

stub, r_adrp, r_add, r2_adrp, r2_add, menu_entry = assemble()
orig_hook2 = bytes.fromhex(open(os.path.join(HERE, "orig_sushibarupdate.hex")).read().strip())[:4]
assert orig_hook2 == branch(HOOK2, UPDATE2_BODY), "SushiBarManager.Update is not b main+402EF00"
orig_hook = bytes.fromhex(open(os.path.join(HERE, "orig_lateupdate.hex")).read().strip())[:4]
orig_cave = bytes.fromhex(open(os.path.join(HERE, "orig_conesight.hex")).read().strip())
assert orig_hook == bytes.fromhex("fd7bbda9"), "LateUpdate does not start with stp x29, x30, [sp, #-0x30]!"
assert len(stub) <= len(orig_cave) and len(stub) % 4 == 0
plan = {
    "format": 1, "module": "main", "title_id": "010097F018538000", "build_id": BUILD_ID,
    "layout": LAYOUT, "mailbox_size": MAILBOX,
    "patches": [{"offset": HOOK, "expected": orig_hook.hex(), "replacement": branch(HOOK, CAVE).hex()}] +
               ([{"offset": HOOK2, "expected": orig_hook2.hex(), "replacement": branch(HOOK2, menu_entry).hex()}]
                if MENU_HOOK else []),
    "payloads": [{"offset": CAVE, "expected": orig_cave[:len(stub)].hex(), "bytes": stub.hex(),
                  "relocations": [{"offset": r_adrp, "target": "mailbox", "kind": "aarch64_adrp", "addend": 0},
                                  {"offset": r_add, "target": "mailbox", "kind": "aarch64_add_lo12", "addend": 0},
                                  {"offset": r2_adrp, "target": "mailbox", "kind": "aarch64_adrp", "addend": 0},
                                  {"offset": r2_add, "target": "mailbox", "kind": "aarch64_add_lo12", "addend": 0}]}],
}
json.dump(plan, open(sys.argv[1], "w"), indent=1)
print(f"menu hook {'on' if MENU_HOOK else 'OFF'}, stub {len(stub)} bytes, menu entry main+{menu_entry:X}, relocations at +{r_adrp:#x} +{r_add:#x} +{r2_adrp:#x} +{r2_add:#x} -> {sys.argv[1]}")
