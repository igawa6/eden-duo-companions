#!/usr/bin/env python3
"""Check a load plan the way Eden's loader does (mod_load_plan.cpp: DiscoverModLoadPlan + Apply),
against the game's own bytes, then show the patched code with a made-up mailbox address."""
import json, sys, os
plan_path, = sys.argv[1:2]
HERE = os.path.dirname(os.path.abspath(__file__))
p = json.load(open(plan_path))
nso = {"build": "F74B633E480E4E161F80A7C16A467F2A" + "0" * 32, "title": 0x010097F018538000,
       "segments": [(0x0, 0x7FD38DC), (0x7FD4000, 0x16AE810), (0x9683000, 0xBA13E8)], "bss": 0x3B0C18}
def need(c, msg):
    if not c: sys.exit("REJECTED: " + msg)
need(isinstance(p.get("module"), str) and p["module"] == "main", "module")
need(p.get("format", 0) == 1 and p.get("title_id") == f"{nso['title']:016X}" and p.get("build_id") == nso["build"], "identity")
segs = p["layout"]["segments"]
need(len(segs) == 3 and all(s["location"] == a and s["size"] == b for s, (a, b) in zip(segs, nso["segments"])), "segments")
need(p["layout"]["bss_size"] == nso["bss"], "bss")
mb = p["mailbox_size"]; need(isinstance(mb, int) and 0 < mb <= 16 << 20 and mb % 0x1000 == 0, "mailbox size")
writes = []
for key, payload in (("patches", False), ("payloads", True)):
    for w in p.get(key, []):
        exp = bytes.fromhex(w["expected"]); rep = bytes.fromhex(w["bytes" if payload else "replacement"])
        need(len(exp) == len(rep) and rep and len(w["expected"]) % 2 == 0, "sizes")
        rel = w.get("relocations", [])
        need(payload or not rel, "relocations on a patch")
        for r in rel:
            need(r["target"] == "mailbox" and r["kind"] in ("aarch64_adrp", "aarch64_add_lo12") and r["offset"] + 4 <= len(rep)
                 and r["offset"] % 4 == 0 and 0 <= r.get("addend", 0) < mb, "relocation")
        writes.append((w["offset"], exp, rep, rel))
need(writes, "empty")
# the game's bytes at those places (round 17)
orig = {0x4203910: bytes.fromhex(open(os.path.join(HERE, "orig_lateupdate.hex")).read().strip()),
        0x25CE2E0: bytes.fromhex(open(os.path.join(HERE, "orig_conesight.hex")).read().strip()),
        0x402EEF0: bytes.fromhex(open(os.path.join(HERE, "orig_sushibarupdate.hex")).read().strip())}
for off, exp, rep, rel in writes:
    need(off in orig and orig[off][:len(exp)] == exp, f"expected bytes at {off:#x}")
for i, a in enumerate(writes):
    for b in writes[i + 1:]:
        need(not (a[0] < b[0] + len(b[2]) and b[0] < a[0] + len(a[2])), "overlap")
# apply with a plausible load base / mailbox (mailbox right after the image + bss)
base = 0x8000000; mbox = base + 0x9683000 + 0xBA13E8 + 0x3B0C18 + 0x1000 & ~0xFFF
out = {}
for off, exp, rep, rel in writes:
    rep = bytearray(rep)
    for r in rel:
        ins = int.from_bytes(rep[r["offset"]:r["offset"] + 4], "little"); pc = base + off + r["offset"]; tgt = mbox + r.get("addend", 0)
        if r["kind"] == "aarch64_adrp":
            need(ins & 0x9F000000 == 0x90000000, "adrp opcode")
            pages = (tgt >> 12) - (pc >> 12); imm = pages & 0x1FFFFF
            ins = (ins & 0x9F00001F) | ((imm & 3) << 29) | ((imm >> 2) << 5)
        else:
            need(ins & 0xFFC00000 == 0x91000000, "add opcode")
            ins = (ins & ~(0xFFF << 10)) | ((tgt & 0xFFF) << 10)
        rep[r["offset"]:r["offset"] + 4] = ins.to_bytes(4, "little")
    out[off] = bytes(rep)
print(f"ACCEPTED. load base {base:#x}, mailbox {mbox:#x}")
# decode what the patched code now points at
def sx(v, bits): return v - (1 << bits) if v & (1 << (bits - 1)) else v
hook = int.from_bytes(out[0x4203910][:4], "little")
need(hook & 0xFC000000 == 0x14000000, "hook is not a B")
dst = base + 0x4203910 + sx(hook & 0x3FFFFFF, 26) * 4
print(f"LateUpdate+0 -> b {dst:#x} (cave {base + 0x25CE2E0:#x})"); need(dst == base + 0x25CE2E0, "hook target")
stub = out[0x25CE2E0]
adrp = int.from_bytes(stub[0x10:0x14], "little"); add = int.from_bytes(stub[0x14:0x18], "little")
pc = base + 0x25CE2E0 + 0x10
imm = ((adrp >> 29) & 3) | (((adrp >> 5) & 0x7FFFF) << 2)
x19 = (pc & ~0xFFF) + sx(imm, 21) * 4096 + ((add >> 10) & 0xFFF)
print(f"x19 = {x19:#x} (mailbox {mbox:#x})"); need(x19 == mbox, "mailbox address")
# 0.38.39: found by target, not at fixed offsets (the HideHUD loop moved the discard code). The LateUpdate part (up to
# its b LateUpdate+4) may only call: Component.get_transform, Transform.get_localScale / set_localScale (HideHUD) and
# LootBox.Remove_Impl (discard); it must end with the displaced stp and b LateUpdate+4.
ALLOWED = {0x7997BC0: "get_transform", 0x79C6380: "get_localScale", 0x79C6500: "set_localScale", 0x4571230: "Remove_Impl"}
words = [(k, int.from_bytes(stub[k:k + 4], "little")) for k in range(0, len(stub), 4)]
back = next(k for k, w in words if w & 0xFC000000 == 0x14000000 and 0x25CE2E0 + k + sx(w & 0x3FFFFFF, 26) * 4 == 0x4203914)
print(f"stub+{back:#x} -> b main+4203914 (LateUpdate+4)")
need(int.from_bytes(stub[back - 4:back], "little") == 0xa9bd7bfd, "displaced stp")
seen = set()
for k, w in words[:back // 4]:
    if w & 0xFC000000 == 0x94000000:
        t = 0x25CE2E0 + k + sx(w & 0x3FFFFFF, 26) * 4
        print(f"stub+{k:#x} -> bl main+{t:X} {ALLOWED.get(t, '??')}"); need(t in ALLOWED, f"bl at stub+{k:#x} to an unexpected place")
        seen.add(ALLOWED[t])
    elif w & 0xFC000000 == 0x14000000:
        t = 0x25CE2E0 + k + sx(w & 0x3FFFFFF, 26) * 4
        need(0x25CE2E0 <= t < 0x25CE2E0 + back + 4, f"b at stub+{k:#x} leaves the LateUpdate part")
need(seen == set(ALLOWED.values()), f"calls present: {sorted(seen)}")
# tonight's menu: SushiBarManager.Update -> menu stub (mailbox again), bl AddMenu, b back to Update's body
if 0x402EEF0 in out:
    h2 = int.from_bytes(out[0x402EEF0][:4], "little")
    need(h2 & 0xFC000000 == 0x14000000, "hook 2 is not a B")
    entry = 0x402EEF0 + sx(h2 & 0x3FFFFFF, 26) * 4
    e = entry - 0x25CE2E0
    print(f"SushiBarManager.Update+0 -> b main+{entry:X} (stub+{e:#x})"); need(0 < e < len(stub) and e % 4 == 0, "hook 2 target")
    words = [int.from_bytes(stub[k:k + 4], "little") for k in range(e, len(stub), 4)]
    k_adrp = next(k for k, w in enumerate(words) if w & 0x9F000000 == 0x90000000) * 4 + e
    adrp = int.from_bytes(stub[k_adrp:k_adrp + 4], "little"); add = int.from_bytes(stub[k_adrp + 4:k_adrp + 8], "little")
    pc = base + 0x25CE2E0 + k_adrp
    imm = ((adrp >> 29) & 3) | (((adrp >> 5) & 0x7FFFF) << 2)
    x19b = (pc & ~0xFFF) + sx(imm, 21) * 4096 + ((add >> 10) & 0xFFF)
    print(f"menu stub x19 = {x19b:#x}"); need(x19b == mbox, "mailbox address (menu)")
    bls = [(k, w) for k, w in ((k, int.from_bytes(stub[k:k + 4], "little")) for k in range(e, len(stub), 4)) if w & 0xFC000000 in (0x94000000, 0x14000000)]
    targets = [(k, w & 0xFC000000, 0x25CE2E0 + k + sx(w & 0x3FFFFFF, 26) * 4) for k, w in bls]
    for k, op, t in targets:
        print(f"stub+{k:#x} -> {'bl' if op == 0x94000000 else 'b'} main+{t:X}")
    need(any(op == 0x94000000 and t == 0x49D1960 for _, op, t in targets), "bl AddMenu")
    need(any(op == 0x94000000 and t == 0x49D2340 for _, op, t in targets), "bl RemoveMenu")
    need(targets[-1][1] == 0x14000000 and targets[-1][2] == 0x402EF00 and targets[-1][0] == len(stub) - 4, "b back to Update's body")
print("all checks passed")
