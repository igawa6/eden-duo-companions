#!/usr/bin/env python3
"""test_load_plan.py - runs the DQ3 load plan's stub and helper (gen_load_plan.py) in an AArch64 emulator
(unicorn) against synthetic guest memory: fake game functions at their real main offsets record their
calls and answer like the game, the patched main-loop `bl` is executed frame by frame.

Checks: an idle frame only bumps the heartbeat and reaches the original callee with x0..x8 / q0..q7
untouched; a request runs exactly once (taken / done / result), in the Misc menu's order (FE169 flag,
area query, Heal All with the mode and the two outcome bytes, SYSSE_HEAL only on success); the Romaria
throne and a no-spells area stop before Heal All; a cancelled mode calls nothing; the stack is balanced.

Needs the unicorn Python package; exits 0 with a note when it is missing (a development check, not part
of the package build).
"""
import pathlib
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gen_load_plan as plan  # noqa: E402

try:
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn import arm64_const as A
except ImportError:
    print("unicorn not installed: load plan emulation skipped")
    sys.exit(0)

MAIN = 0x80000000          # synthetic main base (the plan is position independent apart from the mailbox)
MAILBOX = 0x90000000
STACK = 0x91000000
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


class Guest:
    def __init__(self):
        self.uc = uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(MAIN, 0x4A00000)
        uc.mem_map(MAILBOX, 0x1000)
        uc.mem_map(STACK - 0x10000, 0x20000)
        p = plan.make_plan()
        hook = p["patches"][0]
        uc.mem_write(MAIN + hook["offset"], bytes.fromhex(hook["replacement"]))
        uc.mem_write(MAIN + hook["offset"] + 4, struct.pack("<I", 0xD65F03C0))  # stop marker: ret
        pay = p["payloads"][0]
        code = bytearray(bytes.fromhex(pay["bytes"]))
        # the loader's mailbox relocations (adrp / add lo12)
        at = MAIN + pay["offset"]
        struct.pack_into("<I", code, 0, plan.adrp(9, at, MAILBOX))
        struct.pack_into("<I", code, 4, plan.add_lo12(9, MAILBOX))
        uc.mem_write(at, bytes(code))
        for string, text in ((plan.TXT_FE169, "FE169"), (plan.TXT_SYSSE_HEAL, "SYSSE_HEAL")):
            uc.mem_write(MAIN + string, (text + "\0").encode("utf-16-le"))
        self.fns = {plan.CALLEE: "callee", plan.FNAME_CTOR: "fname", plan.IS_LUA_FLAG: "flag",
                    plan.ROYAL_KIND: "royal", plan.AREA_CONTEXT: "context", plan.AREA_INFO: "info",
                    plan.HEAL_ALL: "heal", plan.PLAY_SE: "se"}
        for off in self.fns:
            uc.mem_write(MAIN + off, struct.pack("<I", 0xD65F03C0))  # ret; the hook answers first
        uc.mem_write(MAIN + 0x100000, b"\0" * 0x100)             # the area info object
        uc.mem_write(MAIN + 0x100100, struct.pack("<Q", 0x1234))  # the context object's first qword
        uc.hook_add(UC_HOOK_CODE, self.on_code, begin=MAIN, end=MAIN + 0x4A00000)
        # the synthetic game's answers
        self.throne, self.royal_kind, self.spells_ok, self.info_null = False, 1, True, False
        self.outcome = (1, 6)
        self.calls = []
        self.names = {}

    def reg(self, r):
        return self.uc.reg_read(r)

    def on_code(self, uc, address, size, _):
        name = self.fns.get(address - MAIN)
        if not name:
            return
        x = [self.reg(getattr(A, f"UC_ARM64_REG_X{i}")) for i in range(9)]
        if name == "callee":
            self.callee_regs = (x, [self.reg(getattr(A, f"UC_ARM64_REG_Q{i}")) for i in range(8)],
                                self.reg(A.UC_ARM64_REG_SP))
            self.calls.append(("callee",))
            return
        if name == "fname":
            raw = uc.mem_read(x[1], 32)
            text = raw.decode("utf-16-le", "ignore").split("\0")[0]
            index = self.names.setdefault(text, 0x100 + len(self.names))
            uc.mem_write(x[0], struct.pack("<Q", index))
            self.calls.append(("fname", text, x[2]))
            ret = x[0]
        elif name == "flag":
            self.calls.append(("flag", x[0]))
            ret = int(self.throne and x[0] == self.names.get("FE169"))
        elif name == "royal":
            self.calls.append(("royal",))
            ret = 0xABCD00 | self.royal_kind  # only the low byte counts
        elif name == "context":
            self.calls.append(("context",))
            ret = MAIN + 0x100100
        elif name == "info":
            self.calls.append(("info", struct.unpack("<Q", uc.mem_read(x[0], 8))[0], x[1]))
            uc.mem_write(MAIN + 0x100000 + 0x43, bytes([1 if self.spells_ok else 0]))
            ret = 0 if self.info_null else MAIN + 0x100000
        elif name == "heal":
            self.calls.append(("heal", x[0] & 0xFFFFFFFF, x[2] - x[1]))
            uc.mem_write(x[1], bytes([self.outcome[0]]))
            uc.mem_write(x[2], bytes([self.outcome[1]]))
            ret = 0xDEAD
        elif name == "se":
            self.calls.append(("se", struct.unpack("<Q", uc.mem_read(x[0], 8))[0]))
            ret = 0
        uc.reg_write(A.UC_ARM64_REG_X0, ret)
        # scratch the caller-saved registers a real callee may clobber
        for r in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16, 17):
            uc.reg_write(getattr(A, f"UC_ARM64_REG_X{r}"), 0x5A5A0000 + r)
        for r in range(8):
            uc.reg_write(getattr(A, f"UC_ARM64_REG_Q{r}"), 0xA5A5 << 64 | r)

    def mb(self, off, size=4, value=None):
        if value is None:
            return int.from_bytes(self.uc.mem_read(MAILBOX + off, size), "little")
        self.uc.mem_write(MAILBOX + off, value.to_bytes(size, "little"))

    def frame(self):
        """One main-loop pass: the patched `bl` with argument registers the stub must preserve."""
        uc = self.uc
        self.calls = []
        x = [0x1000 + i * 0x111 for i in range(9)]
        q = [(0x0123456789ABCDEF << 64 | i * 0x1111) for i in range(8)]
        for i in range(9):
            uc.reg_write(getattr(A, f"UC_ARM64_REG_X{i}"), x[i])
        for i in range(8):
            uc.reg_write(getattr(A, f"UC_ARM64_REG_Q{i}"), q[i])
        for r in (19, 20, 29):
            uc.reg_write(getattr(A, f"UC_ARM64_REG_X{r}"), 0x7700 + r)
        uc.reg_write(A.UC_ARM64_REG_SP, STACK)
        uc.reg_write(A.UC_ARM64_REG_X30, MAIN + plan.HOOK + 4)
        self.callee_regs = None
        uc.emu_start(MAIN + plan.HOOK, MAIN + plan.HOOK + 4, count=10000)
        check(self.callee_regs is not None, "the original callee is reached")
        if self.callee_regs:
            cx, cq, csp = self.callee_regs
            check(cx == x, "x0..x8 reach the callee unchanged")
            check(cq == q, "q0..q7 reach the callee unchanged")
            check(csp == STACK, "the stack is balanced at the callee")
        for r in (19, 20, 29):
            check(self.reg(getattr(A, f"UC_ARM64_REG_X{r}")) == 0x7700 + r, f"x{r} preserved")
        check(self.reg(A.UC_ARM64_REG_X30) == MAIN + plan.HOOK + 4, "the callee returns to the main loop")
        return [c for c in self.calls if c[0] != "callee"]

    def request(self, mode):
        seq = self.mb(0)
        self.mb(0x10, 4, mode)
        self.mb(0, 4, seq + 1)
        return seq + 1


def main():
    g = Guest()
    # idle frames: heartbeat only, nothing called
    for k in range(3):
        check(g.frame() == [], "an idle frame calls nothing")
    check(g.mb(8) == 3 and g.mb(4) == 0 and g.mb(0xC) == 0, "heartbeat 3, nothing taken")

    def run(mode, **state):
        for k, v in state.items():
            setattr(g, k, v)
        seq = g.request(mode)
        calls = g.frame()
        check(g.mb(4) == seq and g.mb(0xC) == seq, "taken = done = seq after one frame")
        check(g.frame() == [], "a request never runs twice")
        return calls, g.mb(0x18, 8)

    fe = lambda: g.names["FE169"]
    base = dict(throne=False, spells_ok=True, info_null=False, outcome=(1, 6))
    # Heal All, healed (1 / 6): the menu's order, SYSSE_HEAL, result 0x10000 | a | b << 8
    calls, res = run(0, **base)
    check([c[0] for c in calls] == ["fname", "flag", "context", "info", "heal", "fname", "se"], f"order {calls}")
    check(calls[0][1:] == ("FE169", 1) and calls[1][1] == fe(), "FName(FE169, FNAME_Add) passed by value")
    check(calls[3][1:] == (0x1234, 0), "the area query gets the context's first qword and 0")
    check(calls[4][1:] == (0, 1), "Heal All gets mode 0 and two adjacent outcome bytes")
    check(calls[5][1] == "SYSSE_HEAL" and calls[6][1] == g.names["SYSSE_HEAL"], "SYSSE_HEAL is played")
    check(res == 0x10601, f"result {res:#x}")
    # Handy Heal All: mode 1 reaches the routine
    calls, res = run(1, **base)
    check(calls[4] == ("heal", 1, 1) and res == 0x10601, "Handy passes mode 1")
    # area info absent (most places): still heals
    calls, res = run(0, **{**base, "info_null": True})
    check("heal" in [c[0] for c in calls] and res == 0x10601, "a null area info allows spells")
    # nothing to heal (6 / 6) and the magic failure (4..6 / 4..6): no sound, the bytes returned
    for outcome in ((6, 6), (5, 6), (4, 4), (6, 5)):
        calls, res = run(0, **{**base, "outcome": outcome})
        check([c[0] for c in calls] == ["fname", "flag", "context", "info", "heal"], f"no SE for {outcome}")
        check(res == 0x10000 | outcome[0] | outcome[1] << 8, f"result for {outcome}")
    # healed with a in 4..6 but b outside (and the reverse): the menu's success, with the sound
    for outcome in ((4, 1), (1, 4), (3, 6)):
        calls, res = run(0, **{**base, "outcome": outcome})
        check(calls[-1][0] == "se", f"SE for {outcome}")
    # the Romaria throne: King / Queen kind << 8 | 0x20, no area query, no Heal All
    for kind in (1, 2, 7):
        calls, res = run(0, **{**base, "throne": True, "royal_kind": kind})
        check([c[0] for c in calls] == ["fname", "flag", "royal"], f"throne stops before Heal All ({kind})")
        check(res == 0x20 | kind << 8, f"throne result {res:#x}")
    # spells forbidden here: 0x30, no Heal All
    calls, res = run(1, **{**base, "spells_ok": False})
    check([c[0] for c in calls] == ["fname", "flag", "context", "info"] and res == 0x30, "no-spells area")
    # cancelled / unknown modes: 0xFF, nothing called
    for mode in (2, 0xFFFFFFFF):
        calls, res = run(mode, **base)
        check(calls == [] and res == 0xFF, f"mode {mode:#x} calls nothing")
    print(f"load plan emulation: {'ok' if not failures else f'{failures} failure(s)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
