<!-- SPDX-FileCopyrightText: Copyright 2026 igawa6 -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Echoes of Wisdom format-2 guest helper -- toolchain recipe

Status: **toolchain preparation, now with TWO real hooks on two different HUD
Update functions and one real feature route served by both -- still nothing
installed, packaged, or run on an emulator/device.** `hook.S` contains three
guest routines: `echoes_dsmod_ping_hook` (the original toolchain-proof skeleton,
a synthetic `bl`-replacement hook, request kind 1 "ping" only), `echoes_helper`
(the DoAction-HUD hook at `main+0x24ff4d0`, an unconditional tail-branch
replacement -- found not to fire during ordinary Field play, since DoAction only
runs when an interact prompt is showing), and `echoes_helper_entry` (the
HeartGauge-HUD hook at `main+0x2595d60`, a function's-first-instruction
replacement, added as the follow-up hook once `echoes_helper` turned out not to
run reliably enough). Both real hooks serve the same request kind 1 "ping" + kind
2 "equip_y" ("set the Y-button Echo/Automaton directly") protocol.
`build_echoes_helper.py` picks which routine a given `plan.json` patches via
`--hook-kind {bl,tail-b,entry}`. Owner-approved scope: use the same format-2
load-plan guest helper mechanism the Breath of the Wild (BOTW) companion ships,
for the title/build below.

- Title: `01008CF01BAAC000` (The Legend of Zelda: Echoes of Wisdom)
- Build: `9F221E1AFA65BA5443E955158AB97DA871E5E41F` (verified 2026-10-23 against this
  research tree's own fixture, `fixtures/package-v196608/main`'s NSO0 header bytes
  `[0x40:0x60]` -- a plain fixed-offset header read, not RE)
- Reference implementation: BOTW's format-2 helper family under a sibling
  Breath of the Wild project's `{tools,prototype}` directories, and that
  project's own companion-module sources for title `01007EF00011E000`
- Runtime reference: the Eden Duo runtime's `src/core/mods/mod_load_plan.{h,cpp}`
  (the authoritative parser -- there is no separate load-plan schema document; the
  companions repo's `docs/PACKAGE_FORMAT.md` only covers `manifest.json`/`package.json`
  and defers to this parser for the plan file itself)

Files in this directory:

| File | What it is |
|---|---|
| `HELPER-TOOLCHAIN.md` | this document |
| `hook.S` | the EoW ping skeleton's guest code, format-2 (separate RX allocation) |
| `build_echoes_helper.py` | builder: assembles `hook.S`, decodes the hook site, writes `plan.json` |
| `echoes_helper_mailbox.h` | module-side mailbox struct and submit/poll/cancel API |
| `tests/ping_hook_test.py` | offline AArch64 execution test (Unicorn) for `--hook-kind bl` / `echoes_dsmod_ping_hook` |
| `tests/equip_y_hook_test.py` | offline AArch64 execution test (Unicorn) for `--hook-kind tail-b` / `echoes_helper` -- idle passthrough, every equip_y refuse code, already-equipped, success, ping-via-echoes_helper, stale epoch, cancelled permit |
| `tests/equip_y_entry_hook_test.py` | offline AArch64 execution test (Unicorn) for `--hook-kind entry` / `echoes_helper_entry` -- entry passthrough (sp = entry_sp - 0x40, x0-x7/x30/x29/callee-saved untouched), every equip_y refuse code, already-equipped, success, ping-via-echoes_helper_entry, stale epoch, cancelled permit |
| `runs/ping-hook/` | build output for the `bl` toolchain-proof hook (gitignored-style scratch; regenerate, do not hand-edit) |
| `runs/equip-y-hook/` | build output for the real `tail-b` DoAction-HUD EoW hook (same regenerate-don't-hand-edit rule) |
| `runs/equip-y-entry/` | build output for the real `entry` HeartGauge-HUD EoW hook (same rule) |

---

## 1. The load-plan JSON schema (authoritative: `mod_load_plan.cpp`)

A load plan is a separate JSON file the manifest points at (`manifest.json`'s
`load_plan` key), not part of `manifest.json` itself. Top level, as read by
`ReadModLoadPlan`:

| Key | Type | Rule |
|---|---|---|
| `module` | string | Must equal the module this discovery call is for (`"main"` unless `load_plan_module` says otherwise) |
| `format` | uint | `1` or `2`. `2` additionally requires `required_runtime >= 19` **and** the host runtime `>= 19` (`IsLoadPlanRuntimeCompatible`, `mod_load_plan.h`) |
| `title_id` | string | 16 upper-case hex chars; must equal the running title |
| `build_id` | string | Exactly the running build id, upper-case hex, **full 32 bytes (64 hex chars)**, zero-padded -- compared for exact string equality, not a prefix match (unlike `module.build_ids` in the package, which is a prefix match) |
| `layout.segments` | array of 3 `{location, size}` | Must byte-for-byte match the running NSO's own `.text`/`.rodata`/`.data` segment headers. A mismatch (different build/update) reports `Unsupported`, not `Invalid` -- the game still starts, without the plan |
| `layout.bss_size` | uint | Must match the running NSO's bss size |
| `mailbox_size` | uint | 1..16 MiB (`MaxMailboxSize`), **must be page-aligned** (`Core::Memory::YUZU_PAGEMASK`, i.e. a multiple of 4096) |
| `mailbox_epoch_offset` | uint, optional | Multiple of 4, `<= mailbox_size - 4`. If present, the **host** (not guest code, not the module) owns that one word: it is the session-epoch counter bumped on pause/resume (section 5) |
| `patches` | array | `expected`/`replacement` hex-byte writes into the module image, `expected.size()==replacement.size()`, non-overlapping, applied only if every `expected` still matches live bytes |
| `payloads` | array | Format-1 only (padding-based hooks: a write into loader-zeroed RX padding bytes, instead of a separately allocated RX page). Format-2 helpers use `guest_code`, not `payloads` |
| `guest_code` | object, format-2 only | `{"bytes": "<hex>", "relocations": [...]}`. Required iff `format==2`; refused otherwise (`"guest code requires format 2"`) |

Each **relocation** (inside a `patches[]`/`guest_code` entry's `relocations[]`, or the
standalone `guest_code.relocations`):

| Key | Values |
|---|---|
| `offset` | byte offset into that write's `replacement`/`bytes`, 4-aligned, `+4 <= len` |
| `kind` | `aarch64_adrp`, `aarch64_add_lo12`, `aarch64_branch26`, `aarch64_ldr32_lo12`, `aarch64_ldr64_lo12`. The last three are **format-2 only** (`ParseWrites(..., extended=format==2)`) |
| `target` | `mailbox`, `main`, `code`. `main`/`code` are **format-2 only** |
| `addend` | non-negative integer added to the target base |

`target` semantics (`ModLoadPlan::Apply`):

- `mailbox`: `base = mailbox_address`, `limit = mailbox_size`
- `main`: `base = load_base` (the module's own base), `limit = main_size` (text+rodata+data+bss) -- **only valid when `guest_code` is present**, i.e. format-2
- `code`: `base = code_address` (the separate RX allocation), `limit = guest_code.bytes.size()` -- **only valid when `guest_code` is present**

`GuestCodeSize()` additionally: non-empty, a multiple of 4 bytes, `<= 64 KiB`
(`MaxGuestCodeBytes`), and **rejects every SVC and every HVC/non-`nop`-SMC
instruction byte-for-byte** (`(word & 0xFF000000) == 0xD4000000`, or `0xD5000000` and
not the `nop`-class encoding `0xD503201F`) -- this allocation is never fed to NCE's
SVC/TLS rewriter, so a format-2 helper must never contain a syscall. Returns 0 (which
`ReadModLoadPlan` then rejects as `"unsupported guest code instructions"`) if any
check fails. The actually-reserved size is this value padded up to the next 4096.

Global limits (`mod_load_plan.cpp`): `MaxMailboxSize = 16 MiB`, `MaxWrites = 4096`,
`MaxWriteBytes = 4 MiB` per write, `MaxRelocations = 4096` total across every write in
the plan, `MaxGuestCodeBytes = 64 KiB`. Metadata (the plan file itself) is bounded at
1 MiB on runtime 19 (`docs/PACKAGE_FORMAT.md` section "Runtime 19 helper and metadata
compatibility"); `package.json`/`manifest.json` bound at 4 MiB.

---

## 2. How the hook branch is installed, and how the overwritten instruction is replayed

A format-2 plan's `patches[0]` (BOTW and this skeleton both use exactly one) writes
a single 4-byte instruction at the hook site: `bl #0` (opcode word `0x94000000`, all
26 displacement bits zero), plus one relocation `{"offset":0,"kind":"aarch64_branch26","target":"code","addend":0}`.
`ModLoadPlan::Apply` fills in the real 26-bit displacement from the patch site to
wherever the guest-code page actually landed once it is known (addresses are only
settled at load time, not at build time) -- this is exactly why the Python builder
never computes that displacement itself; it leaves the relocation for the runtime.

**What gets overwritten and why it must be replayed.** Every BOTW hook site is an
existing `bl <some native routine>` instruction -- the hook patch does not add new
control flow, it *redirects an existing call* through the helper first. The replaced
instruction's effect (the original call) must still happen, in its original program
order, or the surrounding native code breaks. BOTW's hooks replay it as the **first**
thing the hook body does:

```asm
botw_ui_hook:
    stp x29, x30, [sp, #-80]!
    stp x19, x20, [sp, #16]
    ...
    bl   native_ui_update      ; the SAME call the hook site used to make directly
    stp  x0, x1, [sp, #64]     ; preserve the call's return value for the resumed code
    ...                        ; mailbox/request handling, which may run `refused`/`commit` paths
    ...
    ldp  x0, x1, [sp, #64]     ; restore it right before returning
    ...
    ret                        ; falls through to the instruction after the patch site
```

This is *not* a verbatim byte copy of the original instruction at a new address --
that would corrupt a PC-relative branch's encoding at the new location. Instead the
builder (a) decodes the original 4 bytes to confirm they really are a `bl` and to
read its **target address**, (b) cross-checks that target against an operator-
supplied expected value, and (c) the hook source re-emits its *own* `bl` to that same
target, which the runtime then relocates correctly for the new PC via a
`RelocationTarget::Main` entry (the same `aarch64_branch26` kind, just targeting
`main+<addend>` instead of `code`). See `botw_consume_hook` in
`tools/build-allocated-ui-hook.py` (`--consume` branch) for BOTW's version of this
exact check:

```python
original = struct.unpack_from('<I', text, site2)[0]
delta = original & 0x3ffffff
if delta & (1 << 25): delta -= 1 << 26
assert original & 0xfc000000 == 0x94000000 and site2 + delta * 4 == 0xF81F70
```

`build_echoes_helper.py` in this directory does the same thing generically via
`--hook-address`, `--original-bytes`, `--original-call-target`, `--hook-kind`
(`decode_branch_target()`), because at the time this toolchain was first built the
real EoW hook site was not known yet. Section 2a below covers the hook that was
subsequently found and verified by disassembly, which turned out to need a SECOND
`--hook-kind` value, not just a new address -- see that section for why.

After the replay (`--hook-kind bl`), the hook falls through to its own logic
(mailbox read/ack) and then `ret`s back to **the instruction after the patch
site** -- the `ret` works because LR (`x30`) was never touched by anything except
the nested `bl`, which restores it on return, so the hook's own `ret` returns to
whatever originally called into the patched instruction's surrounding function.
This mirrors every BOTW hook and is why the prologue only needs to save/restore
`x29/x30` (frame) plus whatever registers the hook body itself borrows (`x19/x20`
here) and `x0/x1` (the replayed call's return value) -- every other register is
caller-saved under AAPCS64 and already assumed clobberable by any `bl`, including
the one being replaced.

---

## 2a. The real EoW hook: an unconditional tail branch, not a `bl`

**Hook site:** the DoAction HUD per-frame `Update` at `main+0x24ff250` ends every
path at `main+0x24ff4b8..0x24ff4d0` with an epilogue, then an **unconditional tail
branch** at `main+0x24ff4d0`. Verified by disassembly (task-supplied, decoded and
cross-checked by this toolchain, not independently re-derived from the game image
by this builder/test work): bytes `50 39 07 14` little-endian decode as word
`0x14073950`, opcode bits `[31:26] = 0b000101` (`0x14000000`, plain `b` -- **not**
`0x94000000`/`bl`), imm26 `0x073950` -> signed `+473424` words -> target
`main+0x24ff4d0 + 473424*4 = main+0x26cda10`. At this instruction, `x0 = this+0x148`
(the tail-call's own argument) and **x30 is already the caller's return address**
(restored by the `Update` function's own epilogue before this instruction runs,
exactly as a tail call requires).

**Why this needs a second `--hook-kind`, not just a new address.** Every BOTW hook
site, and this toolchain's own `echoes_dsmod_ping_hook` skeleton, replace an
existing `bl <callee>` -- a *call* whose effect must still happen, so the hook
replays it itself (as the first thing it does) and then `ret`s, which only works
because the replaced `bl` sets `x30` to "the instruction after the patch site" as
a side effect of being a call. A plain `b` has no such side effect: it is a TAIL
CALL that hands control (and whatever `x30` already holds) straight to its target,
never returning to anything "after" it. Patching this site with a `bl`-shaped
replacement (even one with the right target) would make the hardware write `x30`
to the address right after the patch -- a value nothing is meant to return to --
before `echoes_helper` even starts running, corrupting the real caller's return
address. So:

- the **patch instruction itself** must stay a plain `b` (opcode `0x14000000`,
  not `0x94000000`) -- `PATCH_OPCODE["tail-b"]` in `build_echoes_helper.py`;
- `echoes_helper` must **not** `ret`; it preserves `x0`/`x30` (and every
  callee-saved register it borrows: `x19`-`x24`) across its own internal calls,
  does its mailbox work, restores them, and finishes with the SAME unconditional
  tail branch the original instruction made (`b original_call_target`, relocated
  exactly like `echoes_dsmod_ping_hook`'s replayed `bl`, just with opcode
  `R_AARCH64_JUMP26` instead of `R_AARCH64_CALL26` -- both already mapped to the
  plan's `aarch64_branch26` kind in `RELOCATION_KINDS`, so no new plan schema was
  needed, only a new guest routine and a new patch opcode).

When nothing is pending this degenerates to exactly `b 0x26cda10` with every
register byte-identical to entry -- this is what `tests/equip_y_hook_test.py`'s
"idle passthrough" scenario asserts (and the one thing every future hook-kind
added here must preserve).

**`hook.S` always assembles every routine together** (one file, one `.text`
section); `--hook-kind` only selects which symbol's offset (found via `llvm-nm`,
`ENTRY_SYMBOL` in `build_echoes_helper.py`) becomes the patch's branch target and
which opcode the patch writes. `echoes_dsmod_ping_hook` is always first (offset
0); `echoes_helper` lands at offset `0x9c` (156), `echoes_helper_entry` (section
2b) at `0x24c` (588) in the combined 1056-byte blob -- none of these offsets are
hardcoded anywhere, the builder always re-derives them via `llvm-nm`. One
consequence worth knowing about if you add a fourth hook kind: multiple routines
share external symbols across builds, and each symbol gets exactly ONE numeric
value per build -- so in any single build, whichever routine is *not* the active
`--hook-kind`'s entry point still gets syntactically valid but semantically
meaningless relocations (dead code, never reached from the patch in that build).
Concretely: `original_call_target` is shared by `echoes_dsmod_ping_hook`'s `bl`
and `echoes_helper`'s tail `b` (same build-time value, since both read
`args.original_call_target`); `equip_y_native_call` is shared by `echoes_helper`
and `echoes_helper_entry` (same FIXED value, `EQUIP_Y_NATIVE_CALL`, regardless of
build); `entry_resume_target` is used only by `echoes_helper_entry` (one
reference, build-specific value `hook_address + 4`). Every test file accounts
for this when picking "the" relevant `main`-target branch26 relocation out of
`guest_code.relocations` -- see their `Counter`-based filtering comments.

---

## 2b. The real EoW entry hook: hooking a function's first instruction

**Why a second real hook was needed.** `echoes_helper` (section 2a) applied
cleanly -- the patch word really did become a branch to the helper, and the
module's mailbox was wired up -- but it never actually RAN: the DoAction HUD
`Update` it hooks is the interact-prompt handler, which is not called during
ordinary Field play unless a prompt is actually showing. `magic`/`heartbeat`
stayed `0` the whole time. The follow-up hook targets the HeartGauge HUD
`Update` at `main+0x2595d60` instead, which (per the owner's live-result report)
runs every frame regardless of what's on screen.

**Hook site:** the HeartGauge HUD Update's own first instruction,
`main+0x2595d60`: `sub sp, sp, #0x40` (bytes `ff 03 01 d1`, confirmed against
clang's own disassembly of that exact mnemonic -- see the task history). This is
neither a call nor a branch: it is the first half of the function's own
prologue, carving out its stack frame before anything else runs. At this point
`x30` is THIS function's own (not yet saved anywhere) return address, `x29` is
the CALLER's frame pointer (also not yet touched), and `x0`-`x7` may ALL be live
incoming arguments -- unlike `echoes_helper`'s hook site, there is no "the
replaced instruction was itself a call/branch" structure to lean on at all.

**Why "verbatim replay," not "replay via hand-written assembly."** Every other
hook in this file re-emits the *semantics* of the replaced instruction as new
assembly source (`bl original_call_target`, `b original_call_target`) and lets
the relocation machinery fix up the PC-relative encoding for the new address.
That approach does not generalize to an ARBITRARY first instruction -- `hook.S`
would need a hand-written equivalent for every possible instruction some future
hook site might replace. Instead, `echoes_helper_entry` copies the EXACT 4 bytes
of `--original-bytes` into a reserved placeholder slot (`entry_replay_slot`,
initially a `nop`) and executes them as-is. This is only safe when those bytes
are **non-PC-relative** -- `is_pc_relative()` in `build_echoes_helper.py` is the
gate that enforces this, covering every PC-relative-addressing class in the base
AArch64 ISA (`B`/`BL`, `B.cond`, `CBZ`/`CBNZ`, `TBZ`/`TBNZ`, `ADR`/`ADRP`, and the
literal-addressing loads `LDR`/`LDRSW`/`PRFM` (literal)); verified by assembling
one instruction from each class plus `sub sp, sp, #0x40` with clang and checking
every verdict. `sub sp, sp, #0x40` only touches the `SP` register's VALUE,
independent of where the instruction itself lives in memory, so it passes.
**This check is not exhaustive for every instruction whose behaviour depends on
context** -- `blr`/`br`/`ret` are classified non-PC-relative (their bytes really
are position-independent) but are still control-flow instructions a human should
look twice at before trusting this generic strategy for; if a future hook site's
first instruction is one of those, treat `is_pc_relative() == False` as
necessary, not sufficient.

**Replay strategy.** `echoes_helper_entry` builds its OWN 128-byte stack frame
(`sub sp, sp, #128`, completely separate from anything the real function's
prologue does), preserves `x0`-`x7`/`x30` and every callee-saved register it
borrows (`x19`-`x24`, same convention as `echoes_helper`) into it, does the
mailbox work, restores everything, then **undoes its own frame**
(`add sp, sp, #128` -- SP is now back to EXACTLY the value it had at entry,
i.e. the caller's SP, completely untouched by anything above), and ONLY THEN
executes the verbatim-spliced instruction. Because the splice runs after this
routine's own frame is fully unwound, it reproduces the real effect
(`entry_sp - 0x40`) exactly, without this routine ever needing to compute or
offset-adjust for its own allocation. Finally, `b entry_resume_target`
(relocated to `main + hook_address + 4`, i.e. `main+0x2595d64`, architecturally
fixed -- the instruction right after the one replaced) hands off with SP
already exactly where that second instruction expects it. `x29` is never
touched at all throughout this routine (no frame-pointer chain is needed; all
addressing is SP-relative with a compile-time-constant frame size), so it is
preserved by construction rather than by explicit save/restore.

**Duplication, not a shared subroutine.** `echoes_helper_entry`'s ping/equip_y
dispatch body (`en_*` labels) is a deliberate, additive COPY of
`echoes_helper`'s (`eh_*` labels), not a call into a shared subroutine. This was
a conscious choice: factoring the dispatch logic into one `echoes_helper_core`
that both outer routines `bl`/`ret` into would have been less code, but it would
have required reworking `echoes_helper` (already built and passing
`tests/equip_y_hook_test.py`) to call through it, adding regression risk to
something that was explicitly meant to keep working untouched. If a FOURTH hook
kind is ever added, revisit this tradeoff -- three independent ~150-line copies
of the same guard chain is a real maintenance cost, not a free lunch.

---

## 3. Placement: helper code, data, and the mailbox

From `AppLoader_NSO::LoadModule` (`src/core/loader/nso.cpp`), in virtual-address
order once a format-2 plan is selected for `module == "main"`:

```
[ main module image: .text | .rodata | .data | .bss ]   (page-aligned end: image_size)
[ mailbox ]                                              (mailbox_size bytes, RW, page-aligned)
[ guest code ]                                           (GuestCodeSize() bytes, RX, page-aligned; CompanionCodeSegment)
```

- **Two passes.** The loader computes addresses in a "layout pass" (`load_into_process == false`, used to size the whole address space) and only *writes* bytes in the "load pass" (`load_into_process == true`). Both passes must agree on layout, which is exactly what `layout.segments`/`layout.bss_size` validate before any `Apply()` call runs.
- **Mailbox address.** `mailbox_address = load_base + image_size` (right after the module's own data+bss, before the mailbox is itself added to `image_size`), *unless* a `DualScreenMailboxSlot` has already reserved space because `module != "main"` is the load-plan target and `main` is not the last-loaded module (`LoadPlanTargetModule`'s doc comment: "the loader reserves the mailbox after the last module when this is not the last one, so mods that branch between modules at fixed offsets keep their layout"). For EoW, until proven otherwise, assume the target module stays `"main"` and this reservation path does not apply.
- **Guest-code address.** Reserved *before* NCE's SVC/TLS patcher ever sees the image (`"Reserve the helper's RX pages before NCE sees the image in either pass"`), directly after the mailbox, also page-aligned. `codeset.CompanionCodeSegment()` records `{offset, offset, size}`.
- **Two `Apply()` calls.** `Apply(module_image, load_base+module_start, mailbox_address)` runs once with `code_address=0` for a format-1 (or a format-2 plan with zero guest-code, which cannot happen since `GuestCodeSize()==0` makes format-2 invalid) -- this path is for the mailbox-only writes before the guest code's own address is known -- and a **second** call with the real `code_address` once the RX pages are placed, which actually resolves every `RelocationTarget::Code`/`RelocationTarget::Main` entry. `Apply()` is deliberately idempotent-safe to call this way: everything it writes is re-derived from the same JSON each time.
- **Both passes must agree.** If the layout pass's address differs from the load pass's (`mailbox_slot->address != load_base + image_size`), the loader logs `"Dual-screen mailbox moved"` and refuses to load -- this should never happen for a well-formed plan; it is a loader-internal consistency check, not something a plan author tunes.

The plan itself never specifies absolute addresses for the mailbox or the guest code
-- only `mailbox_size` (how big) and the relocations' `target` (which base to add the
addend to). This is why `build_echoes_helper.py` can be run and produce a complete,
schema-valid `plan.json` with zero knowledge of where the real mailbox/guest-code
pages will land at runtime.

---

## 4. Build commands

Toolchain actually available and used on this host (verified 2026-10-23; no
`aarch64-linux-gnu-as` installed, not needed -- `clang`'s integrated assembler with
`--target=aarch64-none-elf` covers it, exactly like BOTW's own builders use):

```
clang          23.1.1           /usr/bin/clang
ld.lld                          /usr/bin/ld.lld     (not used by the format-2 path; format-1 only)
llvm-objcopy                    /usr/bin/llvm-objcopy
llvm-nm                         /usr/bin/llvm-nm
llvm-readelf                    /usr/bin/llvm-readelf
```

The Android NDK at `$NDK` also has
`aarch64-linux-android<API>-clang`, which would work identically for this purely
freestanding `.S` file (no libc, no linking to an executable) -- not needed here
since the system `clang --target=aarch64-none-elf` already assembles AArch64 object
code directly, same as BOTW's builders use.

```sh
# 1. Assemble (object file only -- never linked into an executable; the format-2
#    pipeline reads relocations straight out of the unlinked .o).
clang --target=aarch64-none-elf -c hook.S -o runs/ping-hook/hook.o

# 2. Extract the raw instruction bytes.
llvm-objcopy -O binary --only-section=.text runs/ping-hook/hook.o runs/ping-hook/hook.bin

# 3. Read every relocation against an undefined symbol (bridge_mailbox,
#    original_call_target, and -- for a real feature route -- every game-structure
#    symbol the hook reads).
llvm-readelf -r runs/ping-hook/hook.o

# 4. (debugging) list defined symbols, used by the offline test to find
#    `pending_request`'s exact byte offset for a mid-execution breakpoint hook.
llvm-nm --defined-only --format=posix runs/ping-hook/hook.o
```

`build_echoes_helper.py` runs steps 1-3 itself and assembles the result into
`plan.json`:

```sh
# Synthetic toolchain proof (--hook-kind defaults to "bl") -- NOT a real hook site.
python3 build_echoes_helper.py \
    --hook-address 0x1000000 \
    --original-bytes 00004094 \
    --hook-kind bl \
    --original-call-target 0x2000000 \
    --output runs/ping-hook

# The real DoAction-HUD hook (section 2a) -- main+0x24ff4d0, tail branch.
# (Applied cleanly but never actually fires in ordinary Field play -- see 2b.)
python3 build_echoes_helper.py \
    --hook-address 0x24ff4d0 \
    --original-bytes 50390714 \
    --hook-kind tail-b \
    --original-call-target 0x26cda10 \
    --output runs/equip-y-hook

# The real HeartGauge-HUD hook (section 2b) -- main+0x2595d60, a function's
# first instruction. --original-call-target is NOT required for "entry" (it is
# architecturally hook_address + 4, computed automatically).
python3 build_echoes_helper.py \
    --hook-address 0x2595d60 \
    --original-bytes ff0301d1 \
    --hook-kind entry \
    --output runs/equip-y-entry
```

The first command is the exact synthetic example this task originally validated
(section 6) -- not a real hook site, kept only as a regression fixture for
`tests/ping_hook_test.py`. The second and third are the two real hooks found so
far: title_id, build_id, and the `.text`/`.rodata`/`.data`/`.bss` layout all
still come from this tree's own verified fixture in every case (no change needed
unless the fixture build itself changes); only
`--hook-address`/`--original-bytes`/`--hook-kind`/`--original-call-target` differ
between a toolchain proof and a real hook.

The builder enforces, in order: for `bl`/`tail-b`, the original instruction
decodes as the branch kind `--hook-kind` says it should (`bl` for `bl`, a plain
unconditional `b` for `tail-b`; otherwise refuses outright -- section 2a/8) and
its decoded target matches the required `--original-call-target` exactly
(otherwise refuses -- never builds a helper that calls/branches to the wrong
function); for `entry`, the original instruction decodes as NON-PC-relative
(`is_pc_relative()`, section 2b; otherwise refuses) and, if `--original-call-target`
is given at all, it must equal `--hook-address + 4` (otherwise refuses -- this
flag is optional for `entry`, since that resume address is architecturally
fixed, not decoded from the bytes). Exactly 61 relocations come out of
`hook.S`, verified directly against `llvm-readelf -r`'s real output grouped
by routine offset range via `llvm-nm` (not hand-derived -- incremental
arithmetic for this count has repeatedly proved error-prone across several
additions, so it is always re-checked against the tool, never trusted from
a running total): 3 from `echoes_dsmod_ping_hook`, 14 from `echoes_helper`,
and 44 from `echoes_helper_entry` (the same 14 ping/equip_y/HUD-refresh set,
plus everything kinds 3-6 add, ENTRY HOOK ONLY). `hook.S` always assembles
every routine, so this count does not depend on `--hook-kind`. A real
feature route's symbol
table grows exactly the way BOTW's does per added route
(`build-allocated-ui-hook.py`'s `--inventory`/`--armor`/`--holding`/... flags
each add their own `symbols.update(...)` block and their own expected
relocation count at line 71 of that script) --
`ACTOR_SELECT_ROOT_PTR`/`EQUIP_Y_NATIVE_CALL`/`entry_resume_target`/the five
`HUD_*` constants in
`build_echoes_helper.py` are this toolchain's version of that per-route growth.

---

## 5. The BOTW mailbox protocol, field by field

Read directly out of `prototype/native-ui-hook.S` (guest side) and
`packaging/duo-110-docs-r18/native/modules/botw_bridge.h` (module side, class
`SelectionBridge`). This is the pattern every BOTW route (quest select, inventory
equip, consume, map edit, world-rune change, Travel, hold/put-away) implements
identically; only the request-specific argument fields and scope checks differ.

| Offset | Field | Who writes | Notes |
|---|---|---|---|
| `+0` | magic/build signature | guest, every tick | `"DUO3".."DUOG"` (`0x334F5544`..`0x474F5544`) identify which helper revision is installed; `SelectionBridge::helper_level()` maps magic -> feature level, so a module can gate each route on "this helper build added it" |
| `+4` | heartbeat | guest, every tick | free-running tick counter; the module's own liveness window (`health_window()`) is derived from how often this changes |
| `+8` | request epoch | **module** (atomic store), guest (atomic acquire load) | the "go" signal: guest treats "epoch == last acked epoch" as "nothing to do" |
| `+12` | ack epoch | **guest** (atomic release store), module (atomic/poll load) | guest writes this (with `stlr`) only after fully committing or refusing |
| `+16` | result | guest | `1` = ok, `2` = refused; written *before* the release-store of `+12`, so a module that observes the new ack also observes this |
| `+64` | request's own tick stamp | module | the guest checks `heartbeat - this <= 3` (`"Acquire one immutable request, with a separate cancelable scope permit"`) -- a request older than 3 ticks is refused even if everything else matches, catching a request the guest never got around to in time |
| `+72` | **scope permit** | module, written *right before* `+8` | the "separate cancelable scope permit" from the same comment: the guest's acquired epoch at `+8` must also match this value, loaded independently. `SelectionBridge::cancel()` is `store_u32(72, 0)` and nothing else -- it kills a request by invalidating the permit without ever touching the epoch at `+8`, so a half-built or superseded request is refused at the guest's permit check rather than silently acted on later |
| `+88` | request's session-epoch copy | module | compared against the host-owned epoch at `mailbox_size - 4` (next row) |
| `mailbox_size - 4` (BOTW: `4092`) | **session epoch** | **host only** (`core.cpp`, `InvalidateDualScreenGuestMailbox`) | bumped on every `System::Run()`/`Pause()` and every debugger `ResumeEmulation`/`ResumeThreads` call. Neither guest code nor the module may ever write this word |
| request-specific (BOTW: `20`, `24`, `28`, `32`, `40`, `48`, `56`, `60`, `76`, `80`, `96`, `104`, `112`, `120`, and route-specific blocks starting at `416`, `752`, `1024`, ...) | kind/args | module | the request `kind` code at `+20` dispatches which guarded checks the guest hook runs before `commit:` |

**Write order on submit** (`botw_bridge.h`, every `request_*` method, e.g.
`request_warp`): cancel (zero the permit) -> zero the result field -> write every
argument field -> **re-check the session epoch is still the one this request was
built against** -> write the permit (`+72`) -> **re-check the session epoch again**
-> write the request epoch (`+8`) last. Any failed check anywhere in this sequence
calls `cancel()` and aborts, so a request that fails halfway through construction
can never leave a live epoch with a mismatched or missing permit.

**Single-writer rule.** Only the UI-thread guest hook ever writes `+0`, `+4`, `+12`,
`+16`, and every result field. Only the module (through `store_u32`/`store_u64`)
ever writes `+8`, `+72`, and every request-argument field. Only the host C++ runtime
ever writes the session-epoch word. No field has two writers; a reader (either side)
only ever polls, never mutates, a field it does not own. This is what makes the
protocol lock-free: each side's "is this ready" check is a single aligned atomic
load of a word the other side writes with a single aligned atomic store, ordered by
acquire/release (`load_u32`/`store_u32` document "Reads acquire; stores release" in
`dsmod_module_extensions.h`; the ASM side uses `ldar`/`stlr` explicitly).

**Stale-request handling.** Three independent layers, from cheapest to most
expensive: (1) the tick-age check at `+64` (a request older than 3 guest ticks is
refused even though nothing else is wrong with it -- catches "the guest just never
ran again before the module gave up and moved on"); (2) the scope/permit check at
`+72` (catches "the module itself cancelled or superseded this exact request,
possibly microseconds after submitting it"); (3) the session-epoch check against
`mailbox_size - 4` (catches "the emulator paused/resumed, or a save state loaded,
between submit and this guest tick" -- a different kind of staleness than (1)/(2),
because the request's *content* could still be perfectly well-formed, it is just
built against a world state that no longer exists).

**Save/state-reload handling.** There is no explicit "on save loaded" hook in this
ABI. What actually protects a request across a save/state reload is the *same*
session-epoch mechanism as pause/resume: loading a save state goes through
`System::Pause()`/`Run()` (or the debugger path) either way, which is exactly where
`InvalidateDualScreenGuestMailbox()` is called (`core.cpp:224,235`; also
`debugger.cpp:285,302` for a GDB-driven pause/resume). A module does not need to
special-case "a save just loaded" at all -- it only needs the ordinary
`session_epoch() != epoch` check every BOTW route already runs in `poll()`, which
`cancel()`s and treats as "selection cancelled" on any session change, pending or not.

**Module-side health check** (`SelectionBridge::poll`): a request kind is
"supported" only if the live magic decodes to a helper level that is new enough
(`helper_level(magic) >= N`); the helper is "healthy" only if the session epoch is
non-zero *and* matches the guest's own stamped copy at `mailbox_size-4` *and* the
heartbeat has moved within the health window. All three must hold before the module
will even attempt `request_*()`, and `poll()` re-derives all three from scratch every
single call -- there is no cached "is healthy" flag that outlives one `poll()`.

---

## 5a. EoW's own mailbox layout and the equip_y request

**Authoritative source:** `echoes_helper_mailbox.h`'s `offset`/`Status`/`Kind`/
`EquipYRefuseCode` definitions. This table is a convenience summary, not a second
source of truth -- if the two ever disagree, the header wins.

| Offset | Field | Who writes | Notes |
|---|---|---|---|
| `0x000` | magic | guest, every tick | `0x31574F45` ("EOW1"); same convention as BOTW's magic, one fixed value for both request kinds (no per-level helper versioning yet, unlike BOTW's `"DUO3".."DUOG"`) |
| `0x004` | heartbeat | guest, every tick | free-running tick counter |
| `0x008` | request_epoch | module (store), guest (`ldar`) | "go" signal, shared by every kind |
| `0x00C` | ack_epoch | guest (`stlr`), module (load) | "done" signal; **this is the "echo the request epoch" the module matches against** -- there is no separate echo field, `ack_epoch` literally equals `request_epoch` once handled |
| `0x010` | status | guest | `0` idle, `1` ok, `2` refused, `3` already (equip_y only; ping only ever produces `0`/`1`/`2`) |
| `0x014` | ping_counter | guest | incremented on a *successful ping* only (kind 1) |
| `0x018` | session_epoch | module | the host session epoch this request was built against |
| `0x01C` | scope_permit | module, written LAST before the epoch | cancelable scope permit, BOTW's pattern unchanged |
| `0x020` | request_kind | module, written BEFORE the permit | `1` = ping, `2` = equip_y -- dispatches which guarded checks `echoes_helper`/`echoes_helper_entry` run (whichever real hook is actually installed) |
| `0x024` | equip_y_target_id | module (equip_y only) | target native id, zero-extended `u16`. `0xFFFF` is reserved ("never clear Y") and always refused (code 14) |
| `0x028` | result_code | guest (equip_y only) | the specific refuse reason `10..15` on `status == refused`; `0` otherwise; always `0` for ping |
| `mailbox_size - 4` (4092) | session epoch | **host only** | never written by guest or module |

### The equip_y guard chain

`eh_handle_equip_y` (inside `echoes_helper`, section 2a) and `en_handle_equip_y`
(inside `echoes_helper_entry`, section 2b) are each a byte-for-byte replica of
the native ActorSelect release guard chain the task's disassembly identified at
`main+0x24d9150` -> the `bl` at `main+0x24d924c` -- deliberately duplicated, not
shared, per section 2b's tradeoff note:

```
P = *(u64*)(main + 0x34bc840)                   ; if P == 0        -> refuse(10)
r = call *(*(u64*)P + 0x40)(P)                  ; if (r & 1) == 0  -> refuse(11)
Q = *(u64*)(P + 8)                              ; if Q == 0        -> refuse(12)
if (*(u8*)(Q + 0x17) & 0x40)                                       -> refuse(13)
if id == 0xFFFF                        (never clear Y)             -> refuse(14)
if *(u16*)(Q + 0x1970) == id           -> complete(already, status 3)
call main+0x2228a60(x0 = Q + 0x1940, w1 = id)
verify *(u16*)(Q + 0x1970) == id -> complete(status 1) else        -> refuse(15)
```

`main+0x34bc840` (`ACTOR_SELECT_ROOT_PTR`) and `main+0x2228a60`
(`EQUIP_Y_NATIVE_CALL`) are hardcoded constants in `build_echoes_helper.py`
(`symbols` dict, always target `"main"`, the same fallback every non-`bridge_mailbox`
symbol already used) -- **not** CLI flags, because they are fixed to this one
request kind's own structure, not properties of the hook site. **This toolchain
took both addresses, and the pseudocode above, as given by the owner-approved task
spec; it did not independently perform or re-verify the underlying RE.** The one
thing it DID independently verify is that `--original-bytes 50390714` really does
decode to `b main+0x26cda10` from `main+0x24ff4d0` (section 2a), and that
`--original-bytes ff0301d1` really does decode as non-PC-relative
(`is_pc_relative()`, section 2b) -- those are general-purpose checks this
builder always runs, not specific to equip_y.

### The lock-release sequence (eh_equip_release / en_equip_release)

**This was a real, live bug, not a theoretical gap.** The FIRST live install of
`echoes_helper_entry` actually changed the Y-button Echo natively (no picker UI
needed) -- but then hung the game thread on the next frame. Byte-verified cause:
`P->vtable[0x40](P)` (the "guard" call at the top of the chain above) does not
just check a flag, it **ACQUIRES A SHARED LOCK** on the 64-bit word at `P+0x10`,
and the native code releases it on EVERY path that successfully acquired it
(refuse 12-15, already-equipped, success) -- the original guard chain above
replicated the ACQUIRE half byte-for-byte but never replicated the RELEASE
half, so every successful equip_y request left that lock held one count too
high forever, until something else blocked on it.

`eh_equip_release` (and `en_equip_release`, its duplicate for
`echoes_helper_entry`) is the fix, run on every path PAST a successful acquire
(i.e. everything except refuse 10 "P == 0" and refuse 11 "acquire failed" --
those two hold nothing, so release nothing) and BEFORE `eh_finish_ack`'s
ack-epoch write:

```
x8 = P + 0x10
retry:
  old = ldaxr [x8]                  ; one plain ldaxr/stlxr CAS loop (native's
  new = old - 0x1000000             ; own version may peek with a plain ldar
  if ((old & 0xff000000) == 0x1000000):   ; first as an optimisation; one loop
      new &= ~(1 << 56)                   ; is equivalent and is what this uses)
  stlxr new -> [x8]; retry on failure

if (new & 0x00000FFFFF00FF00) == 0: call P->vtable[0x68](P)   // "notify"
if (new & 0x00FFFFFFFF00FF00) == 0: call P->vtable[0x8](P)    // "finalize"
```

Status/result_code are written to the mailbox BEFORE this sequence runs (at
each refuse_12..15/already/success site), not after -- only the ack-epoch
release-store at the very end of `eh_finish_ack` needs to come after status is
set, and this sequence finishes strictly before that regardless. This ordering
choice is also what let the fix avoid introducing a new callee-saved register
to carry "what status to eventually write" across the two extra `blr`s below;
`P` (`x21`) is the only register this sequence depends on staying live, and it
already does (see the register-discipline paragraph below).

The two conditional calls are through `P`'s OWN vtable (`*P`, the SAME vtable
the acquire call used) at offsets `0x68` ("notify" -- fires when the
POST-release value has no bits set in either of its low two bytes or its
middle count field, i.e. "this specific sub-count just hit zero") and `0x8`
("finalize" -- a wider version of the same check, i.e. "the WHOLE refcount
just hit zero, free/finalize the object"). `tests/equip_y_hook_test.py` and
`tests/equip_y_entry_hook_test.py` each added a stub lock word at `P+0x10`,
a stub acquire that increments it (mirroring the real guard call's side
effect this toolchain had been missing), and stub `0x68`/`0x8` targets, then
assert: the lock word returns to its EXACT pre-request value after every
scenario (no leak -- the original bug -- and no double-release), that
`0x68`/`0x8` fire only when the bitmask conditions say they should (a
dedicated "we are the last reader" scenario, lock starting at `0`, makes both
fire), and that idle passthrough never touches the lock word at all.**

**Register discipline for the guard chain (and its release):** `P` (`x21`),
`Q` (`x22`), and `id` (`x23`) are callee-saved under AAPCS64, so they survive
every `blr`/`bl` inside the handler untouched -- the acquire call, the native
assign call, AND the two release-sequence calls above -- *as long as the
called native code itself honours AAPCS64*. This is the one load-bearing
assumption this route adds beyond what ping needed (ping never calls back into
native code at all): if any of these four native calls turns out to clobber
`x19`-`x28` against the AAPCS64 contract (hand-written assembly sometimes
does), `P`/`Q` would need to be reloaded from the mailbox/re-derived after each
call instead of trusted across it. Neither `tests/equip_y_hook_test.py` NOR
`tests/equip_y_entry_hook_test.py`'s stub calls can exercise this risk -- they
are checked stubs, not the real native functions (see section 7's "native calls
are checked stubs, not game proof" caveat, which applies to both in full, and
which is exactly the category of risk the live hang bug fell into on the
guard-acquire side before this fix -- the "stub proves the control flow, not
the game" caveat is not hypothetical here, it is this toolchain's own history).

**Register discipline for the guard chain:** `P` (`x21`), `Q` (`x22`), and `id`
(`x23`) are callee-saved under AAPCS64, so they survive the `blr` (vtable call)
and `bl equip_y_native_call` inside the handler untouched -- *as long as the
called native code itself honours AAPCS64*. This is the one load-bearing
assumption this route adds beyond what ping needed (ping never calls back into
native code at all): if either native call turns out to clobber `x19`-`x28`
against the AAPCS64 contract (hand-written assembly sometimes does), `P`/`Q`
would need to be reloaded from the mailbox/re-derived after each call instead of
trusted across it. Neither `tests/equip_y_hook_test.py` NOR
`tests/equip_y_entry_hook_test.py`'s stub calls can exercise this risk -- they
are checked stubs, not the real native functions (see section 7's "native calls
are checked stubs, not game proof" caveat, which applies to both in full).

### Request layout, by analogy to BOTW's write-order rule

`HelperMailbox::SubmitRequest` (the shared template both `SubmitPing()` and
`SubmitEquipY()` go through in `echoes_helper_mailbox.h`) keeps BOTW's exact
write order (section 5's "Write order on submit"): cancel -> zero status/result ->
re-check session epoch -> stamp session epoch -> write `request_kind` -> write any
kind-specific argument fields (`equip_y_target_id` for equip_y; none for ping) ->
re-check session epoch -> write the permit -> re-check session epoch -> write the
epoch last. Both kinds share ONE epoch/permit pair and one `pending_kind_`
discriminant rather than getting independent slots -- the comment in the original
ping-only header predicted exactly this ("start with one struct per kind and only
share state once a second route actually needs it"); equip_y is that second route.

---

## 5b. The HUD Y-icon refresh (eh_refresh_hud_y_icon / en_refresh_hud_y_icon)

Runs ONLY on the equip_y success path, strictly after `bl equip_y_native_call`
and strictly before `eh_equip_release`/`en_equip_release` -- i.e. while P's
lock is still held, exactly where native does it. Byte-verified against
native `main+0x24d9250..0x24d931c` (identical copy at `main+0x24d57a0`),
INCLUDING the far-jump blocks at `main+0x24d9a98` and `main+0x24d9aa4` (the
owner traced these after an earlier pass of this toolchain mis-nested the
control flow without them -- see below). Five fixed main-relative constants
in `build_echoes_helper.py`: `HUD_REFRESH_FLAG_PTR` (`0x34c8f60`, a byte flag
-- bit0 == 0 skips the entire block), `HUD_MESSAGE_BUILDER` (`0x1591110`,
`bl(x0 = &msg)`, msg is a 0x30-byte LOCAL stack carve-out scoped to this block
only -- `sub`/`add sp, #0x30`, 16-aligned throughout, separate from the
routine's own fixed frame), `HUD_HANDLER_UPDATE` (`0x25a5ff4`), the shared
`HUD_RELEASE_FINALIZE` (`0x2833e50`, called from all three release sites
below), and `HUD_MSG_EXTRA_CALL` (`0x2834260`).

**The reconciled control-flow shape** (handler/listener both read once from
msg, `id` already held in `x23` from the outer guard chain):

```
if handler != 0:
    if listener != 0: AddRef(listener)        // temp pin #1
    saved = ldrh [handler + 0x800]
    if listener != 0: release_once(listener)   // pairs with pin #1, right after
    if saved != id:
        if listener != 0: AddRef(listener)        // temp pin #2
        bl main+0x25a5ff4(x0 = handler, w1 = id)
        if listener != 0: release_once(listener)   // pairs with pin #2, right after
// ALWAYS runs, OUTSIDE the "if handler != 0" gate (reached whether handler
// was 0, the field matched, or it mismatched):
if listener != 0: release_once(listener)       // msg's OWN reference
if msg.byte[0x10] != 0: bl main+0x2834260(x0 = msg.u64[0x8])
```

Net effect on listener's refcount, whenever `listener != 0`, is **always
exactly -1**: every `AddRef` pairs 1:1 with its own immediately-following
`release_once` (net 0 for that pair, regardless of which branch runs), and
only the final, ALWAYS-reached, never-paired release actually changes the
count. `AddRef` is plain `ldxr`/`stxr` +1 at `listener+8` (NO acquire/release
barrier, native's exact form). `release_once(l)`: `ldaxr`/`stlxr` decrement
`[l+8]` (WITH acquire/release barriers); the free path triggers when the
PRE-decrement (OLD) value read by `ldaxr` is **exactly zero** -- re-verified
byte-for-byte against `main+0x24d9294..0x24d92a4` (`cbnz x_old, skip`, tested
on the ldaxr result before the sub) -- neither "old == 1" nor "new == 0".
When taken, the vtable[0x10] call is followed UNCONDITIONALLY by
`bl main+0x2833e50` (no check in between).

**History -- a divergence that was resolved, not just discovered.** An
earlier pass of this toolchain nested BOTH releases inside `if handler != 0`
(following this task's own English pseudocode and test list literally),
which meant `handler == 0` released nothing at all. The owner independently
traced the far-jump targets (`main+0x24d9a98`: listener==0 path, ldrh/cmp
only, `b.eq` back to the merge point; `main+0x24d9aa4`: the mismatch path's
own temp AddRef/call/release, also rejoining the merge point) and confirmed
the merge-point release is reached unconditionally whenever `listener != 0`,
regardless of `handler`. The shape above is the reconciled, authoritative
one; `eh_refresh_hud_y_icon`/`en_refresh_hud_y_icon` now implement it exactly.

**Duplication, not shared:** `en_refresh_hud_y_icon` is an additive copy of
`eh_refresh_hud_y_icon`, same rationale as every other duplicated block in
`echoes_helper_entry` (section 2b).

**Tests:** both `tests/equip_y_hook_test.py` and `tests/equip_y_entry_hook_test.py`
stub `HUD_MESSAGE_BUILDER` to write `msg.u64[0x18]`/`msg.u64[0x20]`/
`msg.byte[0x10]`/`msg.u64[0x8]` directly into the caller's `&msg` at call time
(the address is only known once the guest code computes its own local stack
carve-out), and a listener object (vtable pointer at +0, refcount word at +8).
Covered: handler-field mismatch/match (`hud_handler_update` called/not
called, net refcount -1 either way), `handler == 0` with `listener != 0` (no
compare/call, but still exactly one release, net -1), `handler != 0` with
`listener == 0` (update still called on mismatch, no refcount ever touched),
listener refcount starting at 1 (nets to 0, no free) and at 0 (nets to -1,
free fires exactly once), `msg.byte[0x10]` set/clear (`hud_msg_extra_call`
called/not called), and the flag bit0 == 0 case (whole refresh skipped,
setter still ran).

---

## 5c. Kinds 3-5 (accessory_set, outfit_set, consume) -- ENTRY HOOK ONLY

Unlike every route documented above, these three were **ground-truthed live
on the desktop by diffing memory around native pause-menu actions**, not
derived from a pre-made disassembly dump. They are also **entry-hook only**:
`echoes_helper` (tail-b) never gained them -- only `echoes_helper_entry` did,
per the owner's explicit scope for this follow-up.

**Shared P/Q acquire-guard-chain and lock-release, factored into two new local
subroutines.** All three kinds need the exact same sequence equip_y already
has inline in `en_handle_equip_y`: acquire `P = *(main+0x34bc840)`, call
`P->vtable[0x40](P)` and require bit0 set, load `Q = *(P+8)`, refuse if `Q ==
0` or `Q+0x17` bit 6 is set (refusing STILL releases, exactly like equip_y's
refuse 12/13) -- then, at the end, the same CAS-decrement-and-gated-notify/
finalize release on `P+0x10`. Rather than copying this ~15-instruction block a
third and fourth time (`en_handle_equip_y`'s own copy already exists once),
`en_pq_acquire`/`en_pq_release` in `hook.S` factor it into two `bl`/`ret`
subroutines that kinds 3-5 call into. **`en_handle_equip_y` and
`en_equip_release` were left completely untouched by this refactor** -- these
are NEW subroutines for the NEW kinds only, so nothing already-tested changes
behaviour. `en_pq_acquire` returns `w0 = 0` (success, `x21`=P/`x22`=Q valid)
or a refuse code (10/11: nothing acquired, no release; 12/13: acquired,
caller must release). Four shared exit tails (`en_pqk_refuse_no_release`,
`en_pqk_refuse_with_release`, `en_pqk_already`, `en_pqk_ok`) write
status/result_code and call `en_pq_release` where needed, keyed off `w24`
(reused as a generic "value to act on" register once each kind's own
argument fields no longer need it).

**Mailbox additions:** `target` (offset `0x024`, already existed as
`equip_y_target_id` -- just renamed generically, same value, same field) now
also carries accessory_set's single bit mask, outfit_set's single bit
(zero-extended u16), and consume's quantity index (0-15). A NEW field, `arg`
(offset `0x02C`, u32), carries accessory_set's equip(1)/unequip(0) flag or
consume's heal amount (read by the guest as a SIGNED int32). Refuse codes
16-22 are new (10-15 are the common/equip_y codes already documented in
section 5a) -- see `echoes_helper_mailbox.h`'s `RefuseCode` enum for the
authoritative list; `EquipYRefuseCode` is now an alias for it, since the
common codes (10-13) were never actually equip_y-specific.

### KIND 3 -- ACCESSORY_SET (`en_handle_accessory`)

`target` = a single accessory bit; `arg` = 1 (equip) / 0 (unequip).

```
refuse(16) if target == 0 or more than one bit set
[P/Q acquire]
refuse(17) if !(Q+0x187c & target)                         -- not acquired
capacity = 1 + popcount(((Q+0x1874) >> 20) & 0xF), clamped to 5
slots = u32[5] at Q+0x1880
EQUIP:   slot == target already -> status 3 (already)
         else first slot == 0 -> write target there
         no empty slot -> refuse(18, "full")
UNEQUIP: no slot == target -> status 3 (already, "nothing to remove")
         else remove it, shift every later slot left by one
On any ACTUAL change (never on refuse/already): bl main+0x2229900(x0 = Q+0x1850)
[release]
```

The capacity formula was verified against the task's own example before
writing a single instruction of guest code: `((0x01ffffff >> 20) & 0xF) ==
0xF`, `popcount(0xF) == 4`, `1 + 4 == 5` -- matching "flags 0x01ffffff must
give capacity 5" exactly (matches native's own SWAR popcount check at
`main+0x2507900`, which this toolchain did not re-derive, only cross-checked
this one input/output pair against). Popcount of the already-masked 4-bit
nibble is computed with four plain bit extracts and adds (no NEON, no data
table embedded in the guest-code stream -- the latter would risk tripping
`GuestCodeSize()`'s SVC/HVC byte scan if a table byte ever happened to match).
The unequip-compaction shift was verified against the task's own exact
example (`[2,0x40,0x1000,0x4000,0x8000000]` -> unequip `0x2` ->
`[0x40,0x1000,0x4000,0x8000000,0]`) and the re-equip-after-compaction
follow-up (`-> [...,0x2]`), both reproduced exactly by
`tests/kind345_entry_hook_test.py`.

### KIND 4 -- OUTFIT_SET (`en_handle_outfit`)

`target` = a single outfit bit (u16, zero-extended). Native sequence
`main+0x2502f18..0x2502f58`:

```
[P/Q acquire]
Q+0x187a (u16) == target -> status 3 (already)
!(Q+0x1878 (u16) & target) -> refuse(19, "not owned")
strh target -> Q+0x187a
h = *(Q+0x1850); w = *(u32*)(h+0x14); w |= 0x100; store back  -- "model dirty"
[release]
```

No separate zero/multi-bit validation exists for outfit's own target (unlike
accessory's explicit code 16) -- `target == 0` is never "owned" by
`Q+0x1878`'s own bits, so it is already refused via `RefuseCode::OutfitNotOwned`
without a dedicated check; the task's own spec did not ask for one either.

### KIND 5 -- CONSUME (`en_handle_consume`)

`target` = quantity index 0-15; `arg` = heal amount in quarter-hearts, a
SIGNED int32 the MODULE has already computed as `(int)item_amount` (the same
truncation as native's own `fcvtzs`) -- the guest takes it as given. **Live-
verified on the desktop: a native pause-menu "Eat" of Bubble Kelp changed
exactly current HP 40 -> 41 and count 15 -> 14, nothing else** -- this is the
one request kind in the whole toolchain whose correctness was checked against
a live memory diff rather than (or in addition to) a disassembly reading.

```
refuse(20) if target > 15
refuse(21) if arg < 0 or arg > 0x1000                      -- sanity
[P/Q acquire]
c = byte at Q+0x185e+target; c == 0 -> refuse(22, "none left"); else store c-1
hp = s32 Q+0x17cc, max = s32 Q+0x17c8; hp += arg; floor at 0; clamp to max;
    store back to Q+0x17cc
[release]  -- the decrement and the heal happen under the SAME acquire/release
status 1 on success -- there is NO "already" state for this kind
```

No extra native call is needed afterward (unlike accessory's effect rebuild or
outfit's dirty-bit store) -- the HeartGauge HUD already polls `Q+0x17cc`
every frame by itself, which is also *why* the entry hook exists on the
HeartGauge Update in the first place (section 2b).

**What this toolchain did NOT independently verify for kinds 3-5:** unlike
equip_y and the HUD refresh, there is no pre-made disassembly dump for any of
these three -- every address, bitfield, and formula came directly from the
task's own live-memory-diff-verified spec, taken as given. The one piece of
arithmetic this toolchain DID check before writing guest code was the
accessory capacity formula's worked example above.

### Tests

`tests/kind345_entry_hook_test.py` (new, 22 scenarios): accessory equip into
the first empty slot, already-equipped, full, not-acquired, multi-bit/zero
target, `P == 0` (a dispatch-wiring sanity check for the new kind), locked
flag, the exact unequip-compaction example, unequip-not-equipped, and
re-equip-after-compaction landing in the freed slot -- with
`accessory_effect_rebuild` asserted called exactly once with `x0 = Q+0x1850`
on every real change and never on any refuse/already path; outfit write
(dirty bit set), same-outfit-already, not-owned, locked flag; consume
decrement+heal, clamp-to-max, count-already-0 (with NO HP change asserted),
index-out-of-range, heal-amount-out-of-range, locked flag. Every scenario
additionally asserts the P+0x10 lock round-trips to its exact pre-request
value. `tests/ping_hook_test.py`, `tests/equip_y_hook_test.py`, and
`tests/equip_y_entry_hook_test.py` were all re-run and still pass unchanged --
none of them reach kind 3/4/5 code, since none of their scenarios ever write
request_kind 3, 4, or 5.

---

## 5d. Kind 6 (warp) -- ENTRY HOOK ONLY, NO save-data P lock

Starts the game's MapWarp event flow directly, exactly as the Map menu's
confirm code does (`main+0x25b8050..main+0x25b83c8`). **There is NO save-data
P lock here at all** -- `en_handle_warp` never calls
`en_pq_acquire`/`en_pq_release`; it has its own `en_warp_refuse`/`en_warp_ok`
tails instead.

**This section's sequence was corrected once already.** An earlier pass
modeled `main+0x15c3a90`'s second argument as plain zeroed scratch; the owner
read the menu's FULL function (`text.bin` + rodata, not just the narrower
window this toolchain was first handed) and found it is actually a pointer
to a 16-byte `{obj, ctrl}` "holder" built from a loaded MapWarp flow
resource -- the all-zero-scratch version would have silently produced an
EMPTY name every time.

**This section's sequence was corrected a SECOND time**, after that first
correction's implementation hung the game live on the desktop (gdb: the
stuck thread sat at `PC=0x4` with `LR=blob+0xd1c`, inside the
`result.ctrl` release right after `main+0x163f560` returns). Root cause,
verified against the native code at `main+0x25b8380..main+0x25b8390`: the
helper passed `x3 = container.obj` (the loaded object pointer, held in
`x21`) into `main+0x163f560`, but the native passes `x3 = ADDRESS of the
16-byte container PAIR` (`add x3, sp, #0xa0` in the menu's own frame --
`add x3, sp, #0x10` in `en_handle_warp`'s frame). `x0 = container.obj`
(`x21`) stays correct for the two `main+0x2802f50` `Set()` calls -- native
`main+0x25b8338` does `ldr x21,[sp,#0xa0]; mov x0,x21` right before those,
so only the 4th `main+0x163f560` argument was wrong. Every OTHER call's
arguments were re-checked, argument by argument (address-of-pair vs
loaded value), against this same native range and found already correct:
`main+0x2736940` (`x0=&res`, `x2=&pathview`), `main+0x38a680`
(`x0=res.obj` value), `main+0x65ee00` (`x0=&holder`, `x1=&res`),
`main+0x15c3a90` (`x0=&name`, `x1=&holder`, `x2=&entryview`),
`main+0x28025f0` (`x0=&container`, `w1=2`). The sequence below is the
now-twice-corrected, current one; `tests/warp_entry_hook_test.py` is
written against it, including a regression assertion (added for this
second correction) that `main+0x163f560`'s `x3` equals the container
pair's live stack address, not its `.obj` value.

### Sequence (object-for-object, matching the menu's own staging)

```
A. pathview = {ptr = rodata "event/MapWarp.bfevfl" (main+0x288c304), len = 0x14}
   -- points straight at rodata, not copied
B. resourceMgr = *(main+0x34d23b0); == 0 -> refuse(27); else
   bl main+0x2736940(&res, resourceMgr, &pathview)        -- res: 16B {obj,ctrl}, zeroed first
C. if res.obj != 0: bl main+0x38a680(res.obj)             -- no return value checked
D. bl main+0x65ee00(&holder, &res)                        -- holder: 16B {obj,ctrl}, zeroed first
E. bl main+0x15c3a90(&name, &holder, &entryview)           -- name: 16B {obj,ctrl}, zeroed first
   entryview = {ptr = mailbox entry bytes, len = entry_len}
   name.obj == 0 -> refuse(28)
F. bl main+0x28025f0(&container, capacityHint=2)           -- container: 16B {obj,ctrl}, zeroed first
G. IF level_len > 0: bl main+0x2802f50(container.obj, &"level", 5, &{ptr=mailbox level bytes, len=level_len})
H. ALWAYS (locator_len never 0, refused otherwise):
   bl main+0x2802f50(container.obj, &"locator", 7, &{ptr=mailbox locator bytes, len=locator_len})
   mgr = *(main+0x34bb8d8); == 0 -> refuse(23); else
   bl main+0x163f560(&result, mgr, &name, &container)    -- ADDRESS of the pair, NOT container.obj's value (see correction note above)
I. release result.ctrl right after main+0x163f560 (only reached if it ran),
   THEN -- on EVERY exit path from A onward, including every refusal --
   release, in this order: container.ctrl, name.ctrl, holder.ctrl, res.ctrl
```

Each `release_once(ctrl)` is the SAME ldaxr/stlxr-decrement-then-maybe-free
shape used throughout this file: `cbz ctrl -> skip` (a null ctrl is always a
safe no-op); decrement `[ctrl+8]`; free (via `(*ctrl)->vtable[0x10]` then
`bl main+0x2833e50`) triggers when the PRE-decrement value is exactly zero
(neither "old == 1" nor "new == 0" -- the same rule found for every other
release site in this file). Running the full container/name/holder/res
cleanup UNCONDITIONALLY on every exit path (including refusals as early as
step B) is safe specifically because every object not yet reached on a given
path still has `ctrl == 0` from `en_warp_begin`'s initial zeroing -- there is
no need to track "how far did we get" separately.

Ten fixed main-relative constants in `build_echoes_helper.py`: the five from
before this correction -- `WARP_MGR_PTR` (`0x34bb8d8`), `WARP_NAME_CTOR`
(`0x15c3a90`), `WARP_CONTAINER_CTOR` (`0x28025f0`), `WARP_CONTAINER_SET`
(`0x2802f50`, called twice), `WARP_FLOW_START` (`0x163f560`) -- plus two
rodata string addresses (`WARP_LEVEL_STR` `0x2878c34`, `WARP_LOCATOR_STR`
`0x2895817`) and the five this correction added: `WARP_PATH_STR`
(`0x288c304`, "event/MapWarp.bfevfl", len `0x14`), `WARP_RESOURCE_MGR_PTR`
(`0x34d23b0` -- a DIFFERENT manager pointer than `WARP_MGR_PTR`),
`WARP_RESOURCE_LOAD` (`0x2736940`), `WARP_RESOURCE_PREPARE` (`0x38a680`),
`WARP_HOLDER_CTOR` (`0x65ee00`). Every release in step I reuses the
ALREADY-known `hud_release_finalize` (`0x2833e50`) symbol.

### Stack layout

All eight stack objects are 16 bytes, 16-aligned, zero-initialised at
`en_warp_begin` (the owner's own correction note, confirmed consistent with
every object's `{obj,ctrl}` or `{ptr,len}` shape): `result` (`+0x00`),
`container` (`+0x10`), `name` (`+0x20`), `entryview` (`+0x30`), a
value-pair scratch reused for the level/locator `Set()` calls (`+0x40`),
`pathview` (`+0x50`), `res` (`+0x60`), `holder` (`+0x70`) -- total frame
`0x80` (128 bytes), DOWN from the pre-correction frame's `0x90` (the old
64-byte "unverified work scratch" is gone entirely, replaced by a real,
now-correctly-sized 16-byte `holder` object). `en_handle_warp` lays out this
frame entirely on its own (the menu's literal `sp`-relative offsets have no
meaning once embedded in a different function) -- see the comment block
directly above `en_handle_warp` in `hook.S` for the exact layout.

### result_code semantics (unchanged by this correction)

On success, `result_code` is `main+0x163f560`'s own `x0` return value,
captured immediately after the `bl` returns and before the release's own
`blr`/`bl` calls could clobber it. Per the original trace's reading of
`0x163f560`'s tail, `resultOut`'s first qword is unconditionally zeroed by
`0x163f560` itself before it returns in the common path, so it carries no
diagnostic value; what IS left in `x0` at that point is, per that same
reading, simply `main+0x2834260`'s own passthrough return value -- not
independently re-confirmed by this toolchain beyond reading that analysis.

### Mailbox additions (unchanged by this correction)

`target`/`arg` are NOT reused for warp: `entry_len`/`level_len`/
`locator_len` (u32 each, offsets `0x030`/`0x034`/`0x038`) and three
fixed-size byte areas the guest reads POINTERS into directly -- entry name
(`0x100`, max 32), level (`0x120`, max 64), locator (`0x160`, max 64),
ending at `0x1A0`, well clear of the host-owned epoch word at
`mailbox_size - 4` (4092). `SubmitWarp()` writes the string bytes via a
private `StoreBytes()` helper (the base host extensions offer no generic
byte-array write, only u32/u64), then the three length words, all inside
`SubmitRequest`'s `write_args` callback, which by that template's own
existing structure always runs strictly before the permit/epoch writes.
Verified end-to-end with a standalone mock-ABI C++ program (not just a
syntax check): `SubmitWarp("MapWarpDirectEntry", "Level1", "MyLocator")`
produces exactly `kind=6, entry_len=18, level_len=6, locator_len=9` and the
correct bytes at `0x100`/`0x120`/`0x160`, matching what
`tests/warp_entry_hook_test.py` independently verifies from the guest side.

### Tests

`tests/warp_entry_hook_test.py` (rewritten for the correction, 13 scenarios):
full success asserting the exact call order A-I, the pathview bytes read
back through the guest's own pointer, and all five `ctrl` objects
decrementing by exactly 1 with zero frees; `res.obj == 0` (step C's
`main+0x38a680` call skipped, everything else still succeeds); resource
manager null (refuse 27, NOTHING called, cleanup a provable no-op);
`name.obj == 0` (refuse 28, confirming res/holder DID release while
container/name -- never reached -- stayed untouched); the original `mgr ==
0` check (refuse 23, confirming res/holder/name/container ALL released while
only `result.ctrl`, never reached, stayed untouched); level skipped when
`level_len == 0`; a null-ctrl-is-a-safe-no-op scenario; and -- specifically
because this correction added four MORE release sites whose free path had
never been exercised before -- a dedicated scenario starting all five
`ctrl`s at a pre-decrement value of exactly zero, confirming each
independently frees exactly once with no missed or double frees when five
frees happen back-to-back in the same cleanup block. Plus the four
length-bound refusals, unchanged from before this correction.

For the SECOND correction (the `x3` argument fix above), the existing
`flow_start` stub assertion was strengthened rather than replaced: it now
reads the live `sp` at the call site (`bl` never alters `sp`, so this is
exactly `en_handle_warp`'s own frame -- no hardcoded absolute offset
needed), asserts `x3 == sp + 0x10`, and separately dereferences that
address to confirm the 16 bytes found there really are the container
pair (`.obj == CONTAINER_OBJ_SENTINEL`). This assertion is what would
have caught the live hang: the old assertion checked `x3 ==
CONTAINER_OBJ_SENTINEL` directly, which is exactly what the BUGGY code
produced, so it could not have failed.
`tests/ping_hook_test.py`, `tests/equip_y_hook_test.py`,
`tests/equip_y_entry_hook_test.py`, and `tests/kind345_entry_hook_test.py`
were all re-run and still pass unchanged.

---
## 6. Package declaration

`load_plan`/`load_plan_module`/`min_runtime` all live in `manifest.json` (`load_plan`
and `load_plan_module` are **ignored in `package.json`**; only `min_runtime` is read
from both, and the larger of the two wins -- `PackageMinRuntime`, `mod_manifest.cpp`).
From BOTW's shipped `prototype/package.json` and `prototype/dualscreen/manifest.json`
(field names only; values are BOTW's, shown as a structural reference):

```jsonc
// package.json
{
  "format": 1, "type": "dual-screen-mod",
  "title_id": "01007EF00011E000", "name": "...", "version": "0.1.0",
  "min_runtime": 22,                          // >= 19 required for ANY format-2 plan
  "requires_module": true,
  "module": { "abi": 1, "build_ids": ["..."], "libraries": { ... } }
}
```

```jsonc
// dualscreen/manifest.json (relevant keys only)
{
  "load_plan": "load-plans/<build>.json",     // "<build>" is substituted with the
                                               // running build id, upper-case hex
  "load_plan_module": "main",                 // default; omit unless targeting a
                                               // different module
  "min_runtime": 22
}
```

The plan file itself is named exactly `<build_id>.json` (the full 64-hex-char build
id, upper case) and lives wherever `load_plan`'s path (after `<build>` substitution)
points, relative to `dualscreen/` -- BOTW's is
`dualscreen/load-plans/<BUILD64>.json`.

**The runtime-19 floor is enforced independently of what the package declares.**
`IsLoadPlanRuntimeCompatible(format, declared_runtime, host_runtime)` requires, for
`format == 2`: `declared_runtime >= 19` **and** `host_runtime >= 19`. If the package
under-declares (says `min_runtime: 15` but ships a format-2 plan), `ReadModLoadPlan`
refuses with `"format-2 guest helpers require package min_runtime 19 or newer"` --
this is a hard `Invalid` result, not a soft downgrade; the game starts without any
helper. There is no way to ship a format-2 plan under a lower declared minimum.

**Discovery order.** `DiscoverModLoadPlan`/`SelectLoadPlanPackage` pick the exact
same first-enabled-lexically-sorted package `ModRuntime::Discover` would pick for the
declarative manifest. A package without `load_plan` deliberately opts out (no plan is
taken from a *later* package in that case); a package whose manifest fails to parse,
or that needs a newer runtime than the host provides, contributes nothing and does
not mask a later package's plan.

---

## 7. BOTW's offline test approach (and what this skeleton reuses)

BOTW never requires a live emulator to validate a format-2 helper's *bytes*. Two
independent, composable techniques:

1. **Pure relocation re-derivation** (`tools/_archived-20261019-unused/live_load_plan.py`,
   function `relocated_bytes`): a from-scratch Python re-implementation of
   `ModLoadPlan::Apply`'s four relocation encodings (`aarch64_adrp`,
   `aarch64_add_lo12`, `aarch64_branch26`, and the two `_ldr*_lo12` kinds this
   skeleton does not use), given only `base`/`main`/`mailbox`/`code_base` addresses.
   `live_load_plan.verify()` additionally reads the *live* emulator process's memory
   (through a debugger connection, `c.read`) and asserts the bytes actually installed
   there equal what this function predicts -- this is the "desktop verification of
   plan bytes" the task names, and it is the strongest possible check because it
   compares against the real running process, not a model of it. This skeleton's
   `tests/ping_hook_test.py` reimplements the same four-kind relocation math
   standalone (so this tree has no read dependency on BOTW's archived tools at test
   time) and uses it to re-derive the exact bytes `build_echoes_helper.py` emitted,
   then additionally executes them (step 2) rather than only comparing bytes.

2. **Unicorn-engine execution** (`prototype/tests/holding-hook-test.py` and every
   other `*-hook-test.py` in that directory): map a small flat address space, write
   the *exact relocated bytes* (via technique 1) at a `CODE` address, write `ret`
   (`c0035fd6` little-endian) at every address a native function this hook calls is
   supposed to live at, pre-populate a synthetic mailbox and a synthetic image of
   whatever game structures the hook reads, run with `UC_HOOK_CODE` intercepting
   calls to the native-function stub addresses (asserting their *arguments* are what
   the real game would have received, then injecting a plausible return value/side
   effect), and assert the mailbox's `ack`/`result` fields (and any structure
   mutations) end up correct for each hand-picked scenario (refused-for-every-reason,
   committed, cancelled mid-flight, stale epoch, etc.). BOTW's own caveat applies
   here too: **"native calls are checked stubs, not game proof"** -- this proves the
   guest assembly's control flow and memory protocol are correct, never that the
   real game routine does what the module assumes it does. That second kind of proof
   only comes from RE plus live verification against the actual game.

`tests/ping_hook_test.py` in this directory is technique 2, scoped to the ping
skeleton: it runs the real relocated bytes from `runs/ping-hook/plan.json` under
Unicorn for four scenarios (idle / committed / cancelled-by-permit /
stale-by-session-epoch) and asserts the magic, heartbeat, ack epoch, result, and
ping counter end up exactly right in each, plus that the replayed call's `x0`/`x1`
return value survives untouched and that borrowed callee-saved registers are restored.

a Python venv with `unicorn>=2.1` installed (`$UNICORN_VENV`)
was used, **read-only, as an interpreter only**, to run this test (the system Python
on this host has no `unicorn` package). No file in that venv or anywhere else under
the BOTW research tree was modified.

---

## 8. Pitfalls, carried over from the BOTW handoff and from building this skeleton

- **The `mailbox_epoch_offset` word is sacred.** Neither guest code nor module code
  may ever write it; it is populated and incremented exclusively by `core.cpp`. This
  skeleton's `hook.S` only ever *reads* `mailbox_size - 4`.
- **Session-epoch 0 means "no session yet / not paused-and-resumed even once"** --
  `System::SetDualScreenGuestMailbox` resets the epoch to 0 whenever the mailbox is
  (re)established, and `0` is treated as "unhealthy" by `SelectionBridge::healthy_`
  (`epoch && ...`). Do not special-case epoch 0 as "fine, nothing has changed yet" in
  a module -- treat it exactly like every other session mismatch, as BOTW does.
- **Never roll the request epoch backward, and never reuse epoch 0 for a real
  request.** The guest's only signal that a request is new is "epoch changed since
  last ack" -- reusing or decrementing an epoch can make an old, already-handled
  request look new again. This skeleton's `SubmitPing()` always increments and skips
  0.
- **Write order is not an optimization, it is the correctness property.** BOTW's
  universal "permit before epoch, re-check epoch before each of those two writes"
  sequence exists specifically so that a module that aborts mid-construction leaves
  no observable live epoch with a stale/zero permit *and* so a late write never races
  a session change that happened between validation and commit. Collapsing this into
  fewer, differently-ordered stores is exactly the kind of change that looks correct
  under a quick manual test and then fails intermittently under real timing. Keep the
  order even when a request kind feels "too simple to need it" (ping has essentially
  no content and still follows it, deliberately, as the worked example).
- **`cancel()` only ever touches the permit, never the epoch.** Do not add a second
  "cancel" path that resets `request_epoch` itself; see the epoch-backward pitfall
  above -- it is the same mistake from the other direction.
- **Format-2's guest-code allocation rejects SVC/HVC/SMC bytes wholesale**
  (`GuestCodeSize()`), not just at assembly time but as a hard runtime check on the
  exact bytes in the plan. A helper can never issue a syscall; any native
  functionality it needs must come through a `bl` to existing game code (as this
  skeleton and every BOTW route do) or through the module/host ABI, never through the
  helper trying to do its own system call.
- **The replayed-`bl` assumption was BOTW-wide but did NOT hold for EoW's real
  hook.** Every BOTW hook site happens to be an existing `bl`; EoW's
  (`main+0x24ff4d0`, section 2a) is an unconditional TAIL `b`, which needed a
  second `--hook-kind` ("tail-b") and a whole different replay discipline
  (preserve-then-restore-then-tail-branch, never `ret`) rather than just a new
  address. This is the concrete case the previous version of this bullet warned
  about, and it did happen on the very first real hook site anyone looked for. If
  a FUTURE EoW hook site is a conditional branch, a `cbz`/`tbz`-style test, an
  inlined short sequence with no call/branch at all, or an indirect `blr`/`br`
  through a register, `decode_branch_target()` in `build_echoes_helper.py` will
  refuse to build for either existing `--hook-kind` (loudly, by design) rather
  than silently emitting something wrong -- that site needs a THIRD `--hook-kind`
  value, its own guest routine, and its own replay discipline worked out from
  scratch, the same way tail-b was. Do not assume every future site is one of
  these two known shapes just because the first two turned out to be.
- **BOTW's own hard-won lesson (SESSION-HANDOFF.md, 2026-10-21): "ownership/status
  alone is the original false positive."** Validating that a pointer/handle/flag
  looks right is not the same as validating that the *native game logic* actually
  did the thing a module assumes it did. BOTW's holding-fix investigation spent real
  effort discovering that `hold.active=1` plus a valid generation plus a matching
  owner pointer were *insufficient* evidence that the carried object was actually
  attached and following the player -- the real AI-state transition had to be
  checked too. A future EoW feature route should expect the analogous trap: whatever
  "did it work" check a module runs after a guest ack needs to re-read the *specific*
  game state the feature claims to have changed, not just a generic
  flag/pointer/generation match. (`tests/ping_hook_test.py` has no equivalent of this
  risk yet, because ping changes no game state at all -- this note is for the first
  real feature route, not for the ping skeleton itself.)
- **Do not assume `module == "main"` stays the target forever.** If a real EoW
  feature route ever needs to hook a different NSO (a subsdk), re-read
  `LoadPlanTargetModule`'s doc comment about mailbox placement relative to the
  *last*-loaded module before assuming the simple "right after this module's image"
  placement in section 3 still applies unchanged.

---

## Open questions for whoever picks this up next

1. **The real hook site -- `main+0x24ff4d0` (DoAction HUD) applies but does
   not run; `main+0x2595d60` (HeartGauge HUD) is the one actually in use.**
   `echoes_helper`/tail-b (section 2a) was found and verified by disassembly and
   its patch installs cleanly, but the owner's live-result report found it never
   actually fires in ordinary Field play -- the DoAction HUD Update it hooks is
   the interact-prompt handler, only called while a prompt is on screen.
   `echoes_helper_entry`/entry (section 2b), hooking the HeartGauge HUD Update's
   own first instruction instead, is the follow-up that (per the owner) runs
   every frame regardless. Both hook addresses and their replaced-instruction
   bytes were independently decoded/classified by this toolchain
   (`decode_branch_target()` for 2a, `is_pc_relative()` for 2b) and confirmed to
   match what the task claimed; neither hook site's EXISTENCE or rightness for a
   given feature was independently re-derived from scratch by this work. A third
   hook site, if one is ever needed, is not guaranteed to be either a tail branch
   or a safely-verbatim-replayable first instruction -- see the pitfall in
   section 8 about not assuming future sites match either known shape.
2. **What the first real request kind should be -- RESOLVED: equip_y.** "Set the
   Y-button Echo/Automaton directly" (kind 2), implementing the native
   ActorSelect release guard chain byte-for-byte (section 5a). This toolchain took
   the guard-chain pseudocode and both of its structure addresses
   (`main+0x34bc840`, `main+0x2228a60`) as given by the task spec; it did not
   perform the underlying RE itself, and has no way to confirm those offsets
   survive a future EoW update (BOTW's own history shows structure offsets move
   between builds -- the build_id check in the load plan at least guarantees a
   stale plan refuses to load rather than silently corrupting the wrong update's
   memory, but it does not catch "same build_id, offsets were still wrong").
3. **Whether the replaced instruction is actually a `bl` -- RESOLVED: no, it is a
   plain unconditional `b` (a tail call).** This was the concrete case the old
   version of this question worried about, and it is exactly what happened --
   see section 2a for the full writeup and the new `--hook-kind tail-b` this
   forced into the builder and `hook.S`.
4. **`load_plan_module`.** Still assumed `"main"` throughout (the default, and
   matching every BOTW route, and consistent with every address this task named
   being main-relative). Revisit if a future hook site lives in a different NSO.
5. **Guest code size budget -- effectively a non-issue so far.** The combined
   blob (all three routines, every request kind implemented so far) is 1056
   bytes against the 64 KiB cap. BOTW's richest helper (`--warp`, DUOI level 15)
   is 13 008 bytes, so there is still ample room for several more request kinds
   (or hook kinds) before this becomes a real constraint -- even with the
   duplication section 2b's tradeoff note accepted (three ~150-line copies of
   the same equip_y guard chain) rather than factored into a shared subroutine.
6. **Native-call register-preservation assumption, unverified against the real
   game.** `eh_handle_equip_y`'s `P`/`Q`/`id` (`x21`/`x22`/`x23`) are trusted to
   survive the `blr` (ActorSelect vtable call) and `bl equip_y_native_call`
   untouched, which is only true if those native functions honour AAPCS64's
   callee-saved register contract. `tests/equip_y_hook_test.py`'s stubs cannot
   exercise this -- they are checked stubs, not the real functions (section 7's
   caveat, section 5a). If a future live-verification pass (BOTW's
   `live_load_plan.verify()` technique, section 7) shows this assumption breaks,
   the fix is to reload `P`/`Q` from stack/mailbox-derived state after each call
   instead of trusting the register.
7. **"Status ok" is not the same as "the HUD actually shows the new Echo."**
   HELPER-TOOLCHAIN.md's own section 8 already carries BOTW's hard-won lesson
   that ownership/status-flag matches are not proof the native game logic did
   the thing a module assumes -- this is the first REAL EoW route, so it is the
   first place that lesson actually applies here rather than being a cautionary
   note for later. A module built on `SubmitEquipY()`/`LastEquipYStatus()` should
   still re-read whatever game-visible state means "the Y button icon changed"
   before trusting `Status::Ok` alone, once that state is known.
8. **`is_pc_relative()`'s coverage, and the `blr`/`br`/`ret` caveat.** The
   classifier (section 2b) was checked against one assembled example of every
   PC-relative-addressing class in the base AArch64 ISA plus the one instruction
   this task needed accepted, using clang as the reference disassembler/encoder
   -- it was not checked against the FULL AArch64 instruction set exhaustively,
   nor against any SIMD&FP/SVE encodings. It also only answers "is this
   instruction's result independent of its own address" -- it does NOT answer
   "is this instruction otherwise safe to defer and re-execute after an
   arbitrary amount of this routine's own code has run." `blr`/`br`/`ret` are
   correctly classified non-PC-relative (their bytes really don't encode an
   address) but are still control-flow instructions; if a future `--hook-kind
   entry` site's first instruction turns out to be one of those, read section
   2b's caveat before assuming this gate alone makes it safe.
9. **Whether `entry`'s "preserve all of x0-x7" is complete enough.** The task
   specified x0-x7 (every AAPCS64 integer/pointer argument register) plus x30;
   `echoes_helper_entry` preserves exactly that set, plus x29 is never touched at
   all (so no explicit save/restore was needed for it). It does NOT preserve
   `v0`-`v7` (the SIMD&FP/vector argument registers) -- if HeartGauge::Update
   (or some future entry-hooked function) turns out to take a floating-point or
   vector argument in one of those, this hook would silently corrupt it. Nothing
   in this task's spec mentioned FP/vector arguments, so this was not
   implemented, but it is worth checking before trusting this hook kind for a
   function whose signature is not fully known.
10. **The lock-leak bug -- RESOLVED, and it is exactly the proof that "checked
    stubs, not game proof" is not a hypothetical caveat.** The first live
    install of `echoes_helper_entry` changed the Y-button Echo natively but
    hung the game thread on the next frame: the guard-chain replica acquired
    the shared lock at `P+0x10` (`P->vtable[0x40](P)`) byte-for-byte but never
    released it, because the original pseudocode this toolchain was handed
    described only the acquire+guard half of the native sequence, not the
    release half immediately following it. `eh_equip_release`/
    `en_equip_release` (section 5a) is the fix. The fact that every Unicorn
    test passed before this fix existed -- because the stubs never modeled
    the lock at all -- is the concrete, lived version of section 7's "native
    calls are checked stubs, not game proof" warning: control-flow
    correctness and game-state correctness are genuinely independent claims,
    and this toolchain had the first without the second for one full
    round-trip to a live device.
11. **The release sequence's own untested edges.** The fix is verified for
    control flow and the no-leak/no-double-release/notify-finalize-gating
    invariants under Unicorn with stubbed `0x68`/`0x8` targets -- it has NOT
    been re-verified against a live device the way the original leak was
    discovered (that discovery came from the owner's live-result report, not
    from this toolchain's own testing). Specifically unverified: whether a
    single `ldaxr`/`stlxr` loop (no preliminary plain `ldar` peek) behaves
    identically to whatever the native code actually does under real
    contention from another game thread (the task explicitly said one loop is
    fine, but "fine" was not independently re-derived from the native
    disassembly by this toolchain); and what the `0x68`/`0x8` vtable calls
    actually DO beyond "get called with x0=P" -- this toolchain only checked
    that they are invoked when the bitmask conditions say they should be,
    never what their own correct behaviour looks like.
12. **Kinds 3-5 (section 5c) have no pre-made disassembly to cross-check
    against.** Every other route in this toolchain either started from a
    disassembly dump this toolchain could independently re-verify (equip_y,
    the HUD refresh) or had its byte-level details checked against one. The
    accessory/outfit/consume routes came directly from the owner's own
    live-memory-diff verification with no disassembly handed over at all --
    this toolchain implemented the given formulas/offsets as specified and
    checked them against the given worked examples (the capacity formula,
    the exact unequip-compaction sequence, the Bubble Kelp HP/count shape),
    but has no independent way to confirm e.g. that `main+0x2229900`'s own
    side effects beyond "gets called with the right x0" are correct, or that
    Q's field offsets stay stable across a future game update.
13. **Kind 6/warp (section 5d): entryNameRef/container cleanup -- RESOLVED by
    the owner's own correction.** The earlier gap (this toolchain had no
    trace window showing destructor/cleanup for these two objects) is closed:
    the owner read the menu's full function and found the REAL object chain
    -- `res`/`holder`/`name`/`container` are each a 16-byte `{obj,ctrl}` pair,
    and the menu releases all four `ctrl` fields (plus `result.ctrl`) via the
    standard release pattern at its own scope exit. `en_handle_warp` now
    replicates this unconditionally on every exit path (section 5d, step I).
    The previously-unverified 64-byte WORK scratch guess is also gone
    entirely -- it was never scratch, it is the real, correctly-sized
    `holder` object. Remaining, smaller uncertainties: the exact internal
    behaviour of `main+0x2736940`/`main+0x38a680`/`main+0x65ee00` beyond
    their observable `{obj,ctrl}`-in/out shape (checked stubs, not game
    proof, same caveat as everywhere else in this file), and whether
    `main+0x288c304`'s rodata string or any of these ten addresses shift in a
    future game update.

    **Update: this correction itself shipped a bug that hung the game live**
    (section 5d has the full writeup) -- `x3` into `main+0x163f560` was
    `container.obj`'s value instead of `&container`. Fixed; the `flow_start`
    Unicorn stub now asserts the address relationship directly rather than
    the object-identity check that could not have caught it. This is a
    reminder that "the owner traced it" and "the Unicorn test passed" are
    both necessary but NEITHER is sufficient on its own for a call whose
    argument is itself a struct address -- only a live run (or an assertion
    that specifically checks the address, not just the pointee's identity)
    catches an address-vs-value mixup like this one.
