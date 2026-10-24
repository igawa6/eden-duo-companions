// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Echoes of Wisdom (01008CF01BAAC000) format-2 guest helper -- PING SKELETON.
//
// This is a toolchain proof, not a feature. It implements exactly one request kind
// ("ping": the module arms a request, the helper acknowledges it with an incrementing
// counter) so the build/relocate/install/poll pipeline can be exercised end to end
// before any real EoW feature route is designed. Modeled directly on the Breath of
// the Wild format-2 helper family (see HELPER-TOOLCHAIN.md for the side-by-side
// mapping); do not copy BOTW's addresses or structure-offset guards -- those are
// BOTW-specific and have no meaning for EoW.
//
// Build: build_echoes_helper.py assembles this with `clang --target=aarch64-none-elf
// -c`, extracts .text with llvm-objcopy, and reads undefined-symbol relocations with
// llvm-readelf -r. Two external symbols are required at build time:
//   bridge_mailbox        always linked at value 0; every reference to it becomes a
//                         RelocationTarget::Mailbox entry in the load plan (the
//                         runtime fills in the real mailbox VA at install time).
//   original_call_target  the absolute `main`-relative address the replaced
//                         instruction used to call (RelocationTarget::Main). Passed
//                         in via --original-call-target; the builder cross-checks it
//                         against --original-bytes before assembling anything.
//
// Register discipline (AAPCS64): x19/x20 are callee-saved scratch this routine
// borrows across its whole body, so they are saved/restored like any ordinary
// subroutine. x0/x1 are saved and restored around the *entire* hook body (not just
// around the bl) because they hold the original call's return value, which the code
// right after the patched site is about to consume -- exactly the discipline BOTW's
// hooks use (see native-ui-hook.S, botw_ui_hook: the stp/ldp x0,x1 pair). Every other
// register (x9-x18, flags) is caller-saved scratch under AAPCS64 and already assumed
// clobberable by any `bl`, including the one this hook replaced, so it is used freely
// below without saving it.
.text
.global echoes_dsmod_ping_hook
echoes_dsmod_ping_hook:
    stp x29, x30, [sp, #-48]!
    stp x19, x20, [sp, #16]

    // Replay the overwritten instruction's effect FIRST, in its original program
    // order, before any helper logic runs. The patch that installed this hook turned
    // one `bl original_call_target` into a `bl` to this routine (format-2 patch,
    // RelocationTarget::Code, see build_echoes_helper.py); replaying it here as the
    // very first thing keeps every caller's view of control flow identical to the
    // unmodified game, whether or not a ping request is pending.
    bl  original_call_target
    stp x0, x1, [sp, #32]

.global mailbox_adrp, mailbox_add
mailbox_adrp:
    adrp x19, bridge_mailbox
mailbox_add:
    add  x19, x19, :lo12:bridge_mailbox

    // Diagnostic heartbeat, written on every invocation regardless of whether a
    // request is pending: magic build signature ("EOW1") so a module can tell this
    // exact helper is installed and alive, and a free-running tick counter a module
    // can use for its own staleness windows (mirrors BOTW's offset 0 / offset 4).
    mov  w9, #0x4f45
    movk w9, #0x3157, lsl #16        // w9 = 0x31574F45 ("EOW1" little-endian)
    str  w9, [x19]
    ldr  w9, [x19, #4]
    add  w9, w9, #1
    str  w9, [x19, #4]

    // Acquire the pending-request epoch. The module bumps mailbox+8 (relaxed CAS
    // store through store_u32, observed here as a plain acquire load) to mean "there
    // is a new ping to handle"; mailbox+12 is the last epoch THIS hook fully handled.
    // Equal: nothing new, skip straight past all the request handling to `done`,
    // exactly like BOTW's "b.eq done" after loading its own offset+8/offset+12 pair.
    add  x9, x19, #8
    ldar w20, [x9]
    ldr  w9, [x19, #12]
    cmp  w20, w9
    b.eq done

.global pending_request
pending_request:
    // Scope permit: the module writes mailbox+0x1C to the SAME value as the epoch it
    // is about to publish at mailbox+8, in that order (permit, then epoch -- see
    // echoes_helper_mailbox.h Submit()). A module that cancels a request it has not
    // finished constructing zeroes the permit (mailbox+0x1C = 0) without touching the
    // epoch at mailbox+8, so a half-built or superseded request is refused here
    // instead of acted on. This is BOTW's "separate cancelable scope permit" pattern
    // (prototype/native-ui-hook.S, mailbox+72), carried over unchanged because it is
    // generic to the mailbox protocol, not to any BOTW structure.
    add  x9, x19, #0x1c
    ldar w9, [x9]
    cmp  w9, w20
    b.ne refused

    // Cross-session guard. The host (core.cpp, InvalidateDualScreenGuestMailbox)
    // bumps the epoch word at mailbox+<mailbox_epoch_offset> -- here, mailbox_size-4
    // -- on every emulation pause/resume and every debugger pause/resume. The module
    // stamps the session epoch it observed into mailbox+0x18 while building the
    // request (echoes_helper_mailbox.h Submit()); if the host's copy has moved on
    // since then, this request was built against a session that no longer exists
    // (the emulator was paused, a save/state was loaded, or a debugger stepped
    // through this exact window) and must be refused rather than honoured.
    ldr  w10, [x19, #0x18]
    add  x9, x19, #4092
    ldar w9, [x9]
    cmp  w9, w10
    b.ne refused

    // Nothing else to validate for a bare ping (a real request kind would check its
    // own scope/UI-context/structure guards here, exactly where BOTW's hooks check
    // page/menu/actor identity before `commit:`). Bump the one-shot counter the
    // module reads back as proof this exact request was handled, then acknowledge.
    ldr  w9, [x19, #0x14]
    add  w9, w9, #1
    str  w9, [x19, #0x14]
    mov  w9, #1
    b    acknowledge
refused:
    mov  w9, #2
acknowledge:
    // Status is written before the epoch's release-store, so a module that observes
    // the new ack epoch (acquire load) is guaranteed to also observe this status
    // (store-release orders everything before it). Same ordering as BOTW's
    // "str w9,[x19,#16]" immediately followed by "stlr w20,[...+12]".
    str  w9, [x19, #16]
    add  x9, x19, #12
    stlr w20, [x9]
done:
    ldp  x0, x1, [sp, #32]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #48
    ret

// ===========================================================================
// echoes_helper -- the REAL EoW hook routine (format-2, --hook-kind tail-b).
//
// Hook site: the DoAction HUD per-frame Update at main+0x24ff250 ends every path
// at main+0x24ff4b8..0x24ff4d0 with an epilogue, then an UNCONDITIONAL TAIL
// BRANCH at main+0x24ff4d0 -- bytes `50 39 07 14` little-endian decode as
// `b 0x26cda10` (opcode 0x14000000, NOT 0x94000000/`bl`), with x0 = this+0x148 as
// the tail-call argument and x30 ALREADY RESTORED to the caller's return address
// by the Update function's own epilogue before this instruction runs. This is
// categorically different from every BOTW hook site and from
// echoes_dsmod_ping_hook above (always an existing `bl <callee>`, replayed by
// calling it ourselves first and then `ret`ing to the instruction after the
// patch site): here the replaced instruction is itself a TAIL CALL that never
// returns to this function's own caller -- it hands control, and the CALLER's
// return address already sitting in x30, straight to main+0x26cda10. So this
// routine must NOT `ret`; it must preserve x0/x30 (and every callee-saved
// register it borrows) UNCHANGED across its own internal calls, do its mailbox
// work, restore them, and finish with the SAME unconditional tail branch the
// original instruction made. When nothing is pending this degenerates to
// exactly `b 0x26cda10` with x0/x30 byte-identical to entry -- see
// HELPER-TOOLCHAIN.md section 2a and the idle-passthrough test in
// tests/equip_y_hook_test.py.
//
// Dispatches on a `kind` field at mailbox+0x20 (module-written, see
// echoes_helper_mailbox.h): kind 1 = ping (same protocol as the bl-hook skeleton
// above, now shared through this routine too), kind 2 = equip_y (sets the
// Y-button Echo/Automaton directly).
//
// Register discipline: x19 = mailbox base, x20 = acquired request epoch (same
// convention as echoes_dsmod_ping_hook); x21 = P, x22 = Q, x23 = target id for
// the equip_y guard chain -- all five are callee-saved under AAPCS64, so they
// survive the `blr`/`bl` native calls in the equip_y handler UNTOUCHED as long
// as the called native code itself honours AAPCS64 (the same assumption every
// other register-preservation comment in this file already relies on; this is
// the one load-bearing assumption this routine adds beyond what the ping
// skeleton needed, because ping never calls back into native code at all). x0
// and x30 are preserved on the stack explicitly because this routine's own
// internal `bl`/`blr`s clobber both.
.text
.global echoes_helper
echoes_helper:
    stp x29, x30, [sp, #-96]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    str x0,  [sp, #64]

    adrp x19, bridge_mailbox
    add  x19, x19, :lo12:bridge_mailbox

    // Diagnostic heartbeat (identical convention to echoes_dsmod_ping_hook).
    mov  w9, #0x4f45
    movk w9, #0x3157, lsl #16        // w9 = 0x31574F45 ("EOW1")
    str  w9, [x19]
    ldr  w9, [x19, #4]
    add  w9, w9, #1
    str  w9, [x19, #4]

    // Acquire the pending-request epoch; equal to the last-acked epoch means
    // nothing new -- skip straight to the epilogue (idle passthrough).
    add  x9, x19, #8
    ldar w20, [x9]
    ldr  w9, [x19, #12]
    cmp  w20, w9
    b.eq eh_done

    // Scope permit (mailbox+0x1C): same cancel-without-touching-epoch pattern as
    // the ping skeleton and BOTW.
    add  x9, x19, #0x1c
    ldar w9, [x9]
    cmp  w9, w20
    b.ne eh_refused_generic

    // Cross-session guard (mailbox+0x18 vs the host-owned word at
    // mailbox_size-4, here 4092).
    ldr  w10, [x19, #0x18]
    add  x9, x19, #4092
    ldar w9, [x9]
    cmp  w9, w10
    b.ne eh_refused_generic

    // Dispatch on request kind (mailbox+0x20).
    ldr  w9, [x19, #0x20]
    cmp  w9, #2
    b.eq eh_handle_equip_y
    cmp  w9, #1
    b.eq eh_handle_ping
    b    eh_refused_generic           // unknown/unsupported kind -- refuse, never act

eh_handle_ping:
    // Identical to echoes_dsmod_ping_hook's pending_request success path.
    ldr  w9, [x19, #0x14]
    add  w9, w9, #1
    str  w9, [x19, #0x14]
    mov  w11, #0
    str  w11, [x19, #0x28]              // result_code: unused by ping, keep clean
    mov  w9, #1
    str  w9, [x19, #0x10]                // status = ok
    b    eh_finish_ack

eh_refused_generic:
    mov  w9, #2
    str  w9, [x19, #0x10]                // status = refused (ping / unknown kind)
    b    eh_finish_ack

// ---------------------------------------------------------------------------
// eh_handle_equip_y -- byte-for-byte replica of the native ActorSelect release
// guard chain at main+0x24d9150 -> the `bl` at main+0x24d924c:
//
//   P = *(u64*)(main+0x34bc840);                 if P == 0        -> refuse(10)
//   r = call *(*(u64*)P + 0x40)(P);               if (r & 1) == 0  -> refuse(11)
//   Q = *(u64*)(P + 8);                           if Q == 0        -> refuse(12)
//   if (*(u8*)(Q + 0x17) & 0x40)                                   -> refuse(13)
//   if id == 0xFFFF (never clear Y)                                -> refuse(14)
//   if *(u16*)(Q + 0x1970) == id       -> complete(already, status 3)
//   call main+0x2228a60(x0 = Q + 0x1940, w1 = id)
//   verify *(u16*)(Q + 0x1970) == id -> complete(status 1) else    -> refuse(15)
eh_handle_equip_y:
    ldr  w23, [x19, #0x24]               // target native id (zero-extended u16)

    adrp x9, actor_select_root
    add  x9, x9, :lo12:actor_select_root
    ldr  x21, [x9]                       // P
    cbz  x21, eh_equip_refuse_10

    ldr  x9, [x21]                       // vtable = *P
    ldr  x9, [x9, #0x40]                 // fn = vtable[0x40]
    mov  x0, x21                         // arg: P
    blr  x9                              // r = fn(P)  (result in x0)
    tbz  w0, #0, eh_equip_refuse_11      // (r & 1) == 0 -> refuse(11)

    ldr  x22, [x21, #8]                  // Q = *(P + 8)
    cbz  x22, eh_equip_refuse_12

    ldrb w9, [x22, #0x17]
    tst  w9, #0x40
    b.ne eh_equip_refuse_13

    mov  w9, #0xFFFF
    cmp  w23, w9
    b.eq eh_equip_refuse_14              // never clear Y

    ldrh w9, [x22, #0x1970]
    cmp  w9, w23
    b.eq eh_equip_already

    mov  w9, #0x1940
    add  x0, x22, x9                     // x0 = Q + 0x1940
    mov  w1, w23                         // w1 = id
    bl   equip_y_native_call

    ldrh w9, [x22, #0x1970]
    cmp  w9, w23
    b.eq eh_equip_success
    b    eh_equip_refuse_15

eh_equip_refuse_10:
    mov  w24, #10
    b    eh_equip_refused_no_release
eh_equip_refuse_11:
    mov  w24, #11
    b    eh_equip_refused_no_release
eh_equip_refuse_12:
    mov  w24, #12
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    eh_equip_release
eh_equip_refuse_13:
    mov  w24, #13
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    eh_equip_release
eh_equip_refuse_14:
    mov  w24, #14
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    eh_equip_release
eh_equip_refuse_15:
    mov  w24, #15
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    eh_equip_release
eh_equip_refused_no_release:
    // Reached only from refuse_10 (P == 0) or refuse_11 (acquire failed): the
    // native release guard at P+0x10 was never successfully acquired, so there
    // is nothing to release -- contrast eh_equip_release below, for every path
    // that DID acquire it.
    str  w24, [x19, #0x28]               // result_code
    mov  w9, #2
    str  w9, [x19, #0x10]                 // status = refused
    b    eh_finish_ack
eh_equip_already:
    mov  w9, #0
    str  w9, [x19, #0x28]
    mov  w9, #3
    str  w9, [x19, #0x10]                 // status = already
    b    eh_equip_release
eh_equip_success:
    mov  w9, #0
    str  w9, [x19, #0x28]
    mov  w9, #1
    str  w9, [x19, #0x10]                 // status = ok
    b    eh_refresh_hud_y_icon

// ---------------------------------------------------------------------------
// eh_refresh_hud_y_icon -- HUD Y-icon refresh, byte-verified against native
// main+0x24d9250..0x24d931c (identical copy at main+0x24d57a0), INCLUDING the
// far-jump blocks at main+0x24d9a98 and main+0x24d9aa4 (the owner traced
// these; this is the reconciled, authoritative shape). Runs ONLY on the
// success path, after the setter (`bl equip_y_native_call`) already landed
// and BEFORE eh_equip_release -- exactly where native runs it, while P's lock
// is still held.
//
//   flag = ldarb [main+0x34c8f60]; if (flag & 1) == 0 -> skip entirely
//   msg = 0x30 zeroed bytes on our OWN local stack carve-out (sub/add sp,
//         #0x30 -- scoped to this block only, 16-aligned throughout);
//         bl main+0x1591110(x0 = &msg)
//   handler = msg.u64[0x18]; listener = msg.u64[0x20]
//   if handler != 0:
//       if listener: AddRef(listener)         -- temp pin #1, pairs with...
//       saved = ldrh [handler + 0x800]
//       if listener: release_once(listener)   -- ...release #1 right after
//       if saved != id:
//           if listener: AddRef(listener)         -- temp pin #2, pairs with...
//           bl main+0x25a5ff4(x0 = handler, w1 = id)
//           if listener: release_once(listener)   -- ...release #2 right after
//   // ALWAYS runs, OUTSIDE the "if handler != 0" gate -- reached whether
//   // handler was 0, matched, or mismatched:
//   if listener: release_once(listener)       -- msg's OWN reference
//   if msg.byte[0x10] != 0: bl main+0x2834260(x0 = msg.u64[0x8])
//
// Net effect on listener's refcount when listener != 0 is ALWAYS exactly -1
// (AddRef(s) always pair 1:1 with their own immediately-following release;
// only the final, ungated release is the one that is not undone). AddRef is
// plain ldxr/stxr +1 at listener+8 (NO acquire/release barrier, native's
// exact form). release_once(l): ldaxr/stlxr decrement [l+8] (WITH
// acquire/release barriers); the free path triggers when the PRE-decrement
// (OLD) value read by ldaxr is EXACTLY ZERO (re-verified byte-for-byte
// against main+0x24d9294..0x24d92a4, `cbnz x_old, skip`, tested on the ldaxr
// result before the sub -- neither "old == 1" nor "new == 0"), and when
// taken, the vtable[0x10] call is followed UNCONDITIONALLY by
// `bl main+0x2833e50` (no check in between).
//
// Register discipline: `id` (x23), `P` (x21) and `Q` (x22, though unused
// below) all survive untouched -- this block only ever uses x9-x18/x0/x1
// (caller-saved scratch) plus the local stack carve-out, never x19-x24/x29/
// x30 directly, so nothing here needs its own save/restore on top of what
// AAPCS64 already guarantees for every nested `bl`/`blr` (the same
// native-code-honours-AAPCS64 assumption every other call in this file
// already relies on -- see HELPER-TOOLCHAIN.md section 5a).
eh_refresh_hud_y_icon:
    adrp x9, hud_refresh_flag
    add  x9, x9, :lo12:hud_refresh_flag
    ldarb w9, [x9]
    tbz  w9, #0, eh_refresh_done

    sub  sp, sp, #0x30
    stp  xzr, xzr, [sp, #0]
    stp  xzr, xzr, [sp, #16]
    stp  xzr, xzr, [sp, #32]
    mov  x0, sp
    bl   hud_message_builder

    ldr  x9, [sp, #0x18]                  // handler = msg.u64[0x18]
    cbz  x9, eh_refresh_final_release      // handler == 0 -> straight to the
                                            // ALWAYS-runs release (outside
                                            // the handler gate)

    ldr  x10, [sp, #0x20]                  // listener = msg.u64[0x20]
    cbz  x10, eh_refresh_read_handler       // listener == 0 -> no pin needed

    // Temp pin #1: AddRef(listener), plain ldxr/stxr +1 at listener+8.
    add  x11, x10, #8
eh_refresh_addref1_retry:
    ldxr x12, [x11]
    add  x12, x12, #1
    stxr w13, x12, [x11]
    cbnz w13, eh_refresh_addref1_retry

eh_refresh_read_handler:
    ldrh w14, [x9, #0x800]                 // saved field, compared below

    ldr  x10, [sp, #0x20]                  // reload listener for release #1
    cbz  x10, eh_refresh_compare
    // Release #1: pairs with temp pin #1 above.
    add  x11, x10, #8
eh_refresh_release1_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w15, x13, [x11]
    cbnz  w15, eh_refresh_release1_retry
    cbnz  x12, eh_refresh_compare
    ldr   x16, [x10]                       // vtable = *listener
    mov   x0, x10
    ldr   x16, [x16, #0x10]
    blr   x16
    mov   x0, x10
    bl    hud_release_finalize             // unconditional after the vtable call

eh_refresh_compare:
    cmp  w14, w23, uxth
    b.eq eh_refresh_final_release
    // Mismatch: temp pin #2 wraps the hud_handler_update call specifically.
    ldr  x10, [sp, #0x20]
    cbz  x10, eh_refresh_call_update
    add  x11, x10, #8
eh_refresh_addref2_retry:
    ldxr x12, [x11]
    add  x12, x12, #1
    stxr w13, x12, [x11]
    cbnz w13, eh_refresh_addref2_retry

eh_refresh_call_update:
    mov  x0, x9                            // handler
    mov  w1, w23                           // id
    bl   hud_handler_update

    ldr  x10, [sp, #0x20]                  // reload listener for release #2
    cbz  x10, eh_refresh_final_release
    // Release #2: pairs with temp pin #2 above.
    add  x11, x10, #8
eh_refresh_release2_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w15, x13, [x11]
    cbnz  w15, eh_refresh_release2_retry
    cbnz  x12, eh_refresh_final_release
    ldr   x16, [x10]
    mov   x0, x10
    ldr   x16, [x16, #0x10]
    blr   x16
    mov   x0, x10
    bl    hud_release_finalize

eh_refresh_final_release:
    // ALWAYS runs when listener != 0 -- OUTSIDE the "if handler != 0" gate;
    // reached whether handler was 0, the field matched, or it mismatched.
    // This is msg's OWN reference, net -1, never paired with its own AddRef.
    ldr  x10, [sp, #0x20]
    cbz  x10, eh_refresh_msg_flag
    add  x11, x10, #8
eh_refresh_final_release_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w15, x13, [x11]
    cbnz  w15, eh_refresh_final_release_retry
    cbnz  x12, eh_refresh_msg_flag
    ldr   x16, [x10]
    mov   x0, x10
    ldr   x16, [x16, #0x10]
    blr   x16
    mov   x0, x10
    bl    hud_release_finalize

eh_refresh_msg_flag:
    ldrb w9, [sp, #0x10]                  // msg.byte[0x10]
    cbz  w9, eh_refresh_pop
    ldr  x0, [sp, #0x8]                   // msg.u64[0x8]
    bl   hud_msg_extra_call
eh_refresh_pop:
    add  sp, sp, #0x30
eh_refresh_done:
    b    eh_equip_release

// ---------------------------------------------------------------------------
// eh_equip_release -- releases the shared lock `P->vtable[0x40](P)` acquired
// at the TOP of eh_handle_equip_y, on every path that reached it (refuse
// 12-15, already-equipped, success -- i.e. every path past the `tbz w0,#0`
// acquire check). Byte-for-byte replica of the native release sequence at
// main+0x24d9150 (immediately following the guard chain): a 64-bit
// ldaxr/stlxr CAS loop decrementing the word at P+0x10 by 0x1000000, clearing
// bit 56 if the top byte (bits[31:24]) was exactly 1 before the decrement
// (the "last secondary reader" case), then two conditional calls through P's
// OWN vtable (*P, the SAME vtable the guard call used) at offsets 0x68
// ("notify") and 0x8 ("finalize"), each gated on a different bitmask of the
// POST-decrement value being exactly zero. Status/result_code are written
// BEFORE this runs (every branch above), not after -- the write-order rule
// (section 5) only requires status before the ack-epoch release-store in
// eh_finish_ack, which still happens strictly after this whole block, so
// writing status earlier here changes nothing observable and avoids needing a
// new callee-saved register to carry it across the calls below. `P` (x21) is
// already callee-saved and untouched since the guard chain set it, so it
// survives these two `blr`s the same way it survives the guard/native-assign
// calls above (see HELPER-TOOLCHAIN.md section 5a's register-discipline note
// -- this is that same assumption, extended to two more native calls).
eh_equip_release:
    add  x8, x21, #0x10
    mov  x16, #0x1000000
eh_release_retry:
    ldaxr x9, [x8]
    sub   x10, x9, x16
    and   x11, x9, #0xFF000000
    cmp   x11, x16
    b.ne  eh_release_store
    mov   x17, #0x100
    lsl   x17, x17, #48
    bic   x10, x10, x17
eh_release_store:
    stlxr w12, x10, [x8]
    cbnz  w12, eh_release_retry

    mov   x13, #0xff00
    movk  x13, #0xff00, lsl #16
    movk  x13, #0xfff, lsl #32
    and   x14, x10, x13
    cbnz  x14, eh_release_skip_notify
    ldr   x9, [x21]
    ldr   x9, [x9, #0x68]
    mov   x0, x21
    blr   x9
eh_release_skip_notify:
    mov   x13, #0xff00
    movk  x13, #0xff00, lsl #16
    movk  x13, #0xffff, lsl #32
    movk  x13, #0xff, lsl #48
    and   x14, x10, x13
    cbnz  x14, eh_release_skip_finalize
    ldr   x9, [x21]
    ldr   x9, [x9, #0x8]
    mov   x0, x21
    blr   x9
eh_release_skip_finalize:
    b     eh_finish_ack

eh_finish_ack:
    // Status before the epoch's release-store -- same ordering as the ping
    // skeleton and BOTW ("a module that observes the new ack also observes the
    // status written just before it").
    add  x9, x19, #12
    stlr w20, [x9]
    b    eh_epilogue
eh_done:
    b    eh_epilogue
eh_epilogue:
    ldr  x0,  [sp, #64]
    ldp  x23, x24, [sp, #48]
    ldp  x21, x22, [sp, #32]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #96
    b    original_call_target

// ===========================================================================
// echoes_helper_entry -- a THIRD hook kind (format-2, --hook-kind entry).
//
// Hook site: the HeartGauge HUD Update's very FIRST instruction, at
// main+0x2595d60: `sub sp, sp, #0x40` (bytes `ff 03 01 d1`). Unlike every other
// hook in this file, the replaced instruction is NEITHER a call (`bl`, replay +
// `ret`) NOR a tail branch (`b`, preserve + resume) -- it is an ordinary
// non-branching, non-PC-relative data-processing instruction that is simply the
// first half of the function's own prologue. At this exact point x30 is THIS
// function's own return address (not yet saved anywhere -- the function hasn't
// run far enough to save it), x29 is the CALLER's frame pointer (also not yet
// touched), and x0-x7 may ALL be live incoming arguments (unknown how many this
// function actually uses, so all eight are preserved defensively, per spec).
//
// Replay strategy: build our OWN 128-byte frame (via `sub sp, sp, #128`, a
// completely separate allocation from anything the original function's prologue
// does), save x0-x7/x30/every callee-saved register this routine borrows into
// it, do the mailbox work, restore everything, UNDO our own frame (`add sp, sp,
// #128` -- SP is now back to exactly the value it had at entry, i.e. the
// caller's SP, untouched), and ONLY THEN execute a VERBATIM COPY of the
// replaced instruction (not re-typed as `sub sp, sp, #0x40` -- the literal
// bytes `build_echoes_helper.py` received via --original-bytes, spliced in
// after assembly at the `entry_replay_slot` placeholder below). Because this
// happens after SP has been restored to its original value, the verbatim
// instruction reproduces EXACTLY the effect the real first instruction would
// have had (original_sp - 0x40), without this routine ever needing to compute
// or adjust for its own frame's offset. Finally, `b entry_resume_target`
// (relocated to main+hook_address+4, i.e. main+0x2595d64, the SECOND
// instruction of the real function's prologue) hands off with SP already
// exactly where that second instruction expects it.
//
// build_echoes_helper.py's `is_pc_relative()` gate is why this strategy is only
// safe for SOME first instructions: a verbatim-copied PC-relative instruction
// (ADR/ADRP, a literal load, any branch) would compute the WRONG address once
// moved to this routine's own location in the guest-code page -- the builder
// refuses to build rather than silently emit a helper that corrupts whatever
// the real instruction was computing. `sub sp, sp, #0x40` touches only the SP
// register's value, independent of where the instruction itself lives in
// memory, so it passes.
//
// Dispatches on the SAME kind/ping/equip_y mailbox protocol as echoes_helper
// above (mailbox+0x20, identical field layout, identical guard chain for
// equip_y) -- this body is a deliberate, additive DUPLICATE of echoes_helper's
// dispatch logic rather than a shared subroutine, specifically so that neither
// echoes_dsmod_ping_hook nor echoes_helper (already built, tested, and in use)
// needed to change AT ALL to add this third hook kind. See HELPER-TOOLCHAIN.md
// section 2b for the tradeoff this makes (duplication vs. a shared
// echoes_helper_core subroutine) and why it was chosen here.
.text
.global echoes_helper_entry
echoes_helper_entry:
    sub  sp, sp, #128
    stp  x0, x1, [sp, #0]
    stp  x2, x3, [sp, #16]
    stp  x4, x5, [sp, #32]
    stp  x6, x7, [sp, #48]
    stp  x19, x20, [sp, #64]
    stp  x21, x22, [sp, #80]
    stp  x23, x24, [sp, #96]
    str  x30, [sp, #112]

    adrp x19, bridge_mailbox
    add  x19, x19, :lo12:bridge_mailbox

    // Diagnostic heartbeat (identical convention to echoes_dsmod_ping_hook /
    // echoes_helper).
    mov  w9, #0x4f45
    movk w9, #0x3157, lsl #16        // w9 = 0x31574F45 ("EOW1")
    str  w9, [x19]
    ldr  w9, [x19, #4]
    add  w9, w9, #1
    str  w9, [x19, #4]

    // Acquire the pending-request epoch; equal to the last-acked epoch means
    // nothing new -- skip straight to the epilogue (entry passthrough).
    add  x9, x19, #8
    ldar w20, [x9]
    ldr  w9, [x19, #12]
    cmp  w20, w9
    b.eq en_done

    // Scope permit (mailbox+0x1C).
    add  x9, x19, #0x1c
    ldar w9, [x9]
    cmp  w9, w20
    b.ne en_refused_generic

    // Cross-session guard (mailbox+0x18 vs the host-owned word at
    // mailbox_size-4, here 4092).
    ldr  w10, [x19, #0x18]
    add  x9, x19, #4092
    ldar w9, [x9]
    cmp  w9, w10
    b.ne en_refused_generic

    // Dispatch on request kind (mailbox+0x20). Kinds 3/4/5 (accessory/outfit/
    // consume) are ENTRY-HOOK ONLY -- echoes_helper (tail-b) stays ping+equip_y
    // only, per the owner's explicit scope for this follow-up.
    ldr  w9, [x19, #0x20]
    cmp  w9, #2
    b.eq en_handle_equip_y
    cmp  w9, #1
    b.eq en_handle_ping
    cmp  w9, #3
    b.eq en_handle_accessory
    cmp  w9, #4
    b.eq en_handle_outfit
    cmp  w9, #5
    b.eq en_handle_consume
    cmp  w9, #6
    b.eq en_handle_warp
    b    en_refused_generic           // unknown/unsupported kind -- refuse, never act

en_handle_ping:
    ldr  w9, [x19, #0x14]
    add  w9, w9, #1
    str  w9, [x19, #0x14]
    mov  w11, #0
    str  w11, [x19, #0x28]              // result_code: unused by ping, keep clean
    mov  w9, #1
    str  w9, [x19, #0x10]                // status = ok
    b    en_finish_ack

en_refused_generic:
    mov  w9, #2
    str  w9, [x19, #0x10]                // status = refused (ping / unknown kind)
    b    en_finish_ack

// eh_handle_equip_y's guard chain, duplicated verbatim for the entry hook (see
// HELPER-TOOLCHAIN.md section 5a for the pseudocode this mirrors).
en_handle_equip_y:
    ldr  w23, [x19, #0x24]               // target native id (zero-extended u16)

    adrp x9, actor_select_root
    add  x9, x9, :lo12:actor_select_root
    ldr  x21, [x9]                       // P
    cbz  x21, en_equip_refuse_10

    ldr  x9, [x21]                       // vtable = *P
    ldr  x9, [x9, #0x40]                 // fn = vtable[0x40]
    mov  x0, x21                         // arg: P
    blr  x9                              // r = fn(P)  (result in x0)
    tbz  w0, #0, en_equip_refuse_11      // (r & 1) == 0 -> refuse(11)

    ldr  x22, [x21, #8]                  // Q = *(P + 8)
    cbz  x22, en_equip_refuse_12

    ldrb w9, [x22, #0x17]
    tst  w9, #0x40
    b.ne en_equip_refuse_13

    mov  w9, #0xFFFF
    cmp  w23, w9
    b.eq en_equip_refuse_14              // never clear Y

    ldrh w9, [x22, #0x1970]
    cmp  w9, w23
    b.eq en_equip_already

    mov  w9, #0x1940
    add  x0, x22, x9                     // x0 = Q + 0x1940
    mov  w1, w23                         // w1 = id
    bl   equip_y_native_call

    ldrh w9, [x22, #0x1970]
    cmp  w9, w23
    b.eq en_equip_success
    b    en_equip_refuse_15

en_equip_refuse_10:
    mov  w24, #10
    b    en_equip_refused_no_release
en_equip_refuse_11:
    mov  w24, #11
    b    en_equip_refused_no_release
en_equip_refuse_12:
    mov  w24, #12
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    en_equip_release
en_equip_refuse_13:
    mov  w24, #13
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    en_equip_release
en_equip_refuse_14:
    mov  w24, #14
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    en_equip_release
en_equip_refuse_15:
    mov  w24, #15
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    en_equip_release
en_equip_refused_no_release:
    // Reached only from refuse_10 (P == 0) or refuse_11 (acquire failed) -- see
    // eh_equip_release's comment (echoes_helper, above) for why these two paths
    // never acquired anything and so release nothing.
    str  w24, [x19, #0x28]               // result_code
    mov  w9, #2
    str  w9, [x19, #0x10]                 // status = refused
    b    en_finish_ack
en_equip_already:
    mov  w9, #0
    str  w9, [x19, #0x28]
    mov  w9, #3
    str  w9, [x19, #0x10]                 // status = already
    b    en_equip_release
en_equip_success:
    mov  w9, #0
    str  w9, [x19, #0x28]
    mov  w9, #1
    str  w9, [x19, #0x10]                 // status = ok
    b    en_refresh_hud_y_icon

// en_refresh_hud_y_icon -- duplicate of eh_refresh_hud_y_icon (echoes_helper,
// above) for echoes_helper_entry's own success path. See that routine's
// comment for the full writeup; duplicated rather than shared for the same
// reason section 2b gives for the rest of this routine's body.
en_refresh_hud_y_icon:
    adrp x9, hud_refresh_flag
    add  x9, x9, :lo12:hud_refresh_flag
    ldarb w9, [x9]
    tbz  w9, #0, en_refresh_done

    sub  sp, sp, #0x30
    stp  xzr, xzr, [sp, #0]
    stp  xzr, xzr, [sp, #16]
    stp  xzr, xzr, [sp, #32]
    mov  x0, sp
    bl   hud_message_builder

    ldr  x9, [sp, #0x18]                  // handler = msg.u64[0x18]
    cbz  x9, en_refresh_final_release      // handler == 0 -> straight to the
                                            // ALWAYS-runs release

    ldr  x10, [sp, #0x20]                  // listener = msg.u64[0x20]
    cbz  x10, en_refresh_read_handler

    add  x11, x10, #8
en_refresh_addref1_retry:
    ldxr x12, [x11]
    add  x12, x12, #1
    stxr w13, x12, [x11]
    cbnz w13, en_refresh_addref1_retry

en_refresh_read_handler:
    ldrh w14, [x9, #0x800]

    ldr  x10, [sp, #0x20]
    cbz  x10, en_refresh_compare
    add  x11, x10, #8
en_refresh_release1_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w15, x13, [x11]
    cbnz  w15, en_refresh_release1_retry
    cbnz  x12, en_refresh_compare
    ldr   x16, [x10]
    mov   x0, x10
    ldr   x16, [x16, #0x10]
    blr   x16
    mov   x0, x10
    bl    hud_release_finalize

en_refresh_compare:
    cmp  w14, w23, uxth
    b.eq en_refresh_final_release
    ldr  x10, [sp, #0x20]
    cbz  x10, en_refresh_call_update
    add  x11, x10, #8
en_refresh_addref2_retry:
    ldxr x12, [x11]
    add  x12, x12, #1
    stxr w13, x12, [x11]
    cbnz w13, en_refresh_addref2_retry

en_refresh_call_update:
    mov  x0, x9
    mov  w1, w23
    bl   hud_handler_update

    ldr  x10, [sp, #0x20]
    cbz  x10, en_refresh_final_release
    add  x11, x10, #8
en_refresh_release2_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w15, x13, [x11]
    cbnz  w15, en_refresh_release2_retry
    cbnz  x12, en_refresh_final_release
    ldr   x16, [x10]
    mov   x0, x10
    ldr   x16, [x16, #0x10]
    blr   x16
    mov   x0, x10
    bl    hud_release_finalize

en_refresh_final_release:
    // ALWAYS runs when listener != 0 -- OUTSIDE the "if handler != 0" gate.
    ldr  x10, [sp, #0x20]
    cbz  x10, en_refresh_msg_flag
    add  x11, x10, #8
en_refresh_final_release_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w15, x13, [x11]
    cbnz  w15, en_refresh_final_release_retry
    cbnz  x12, en_refresh_msg_flag
    ldr   x16, [x10]
    mov   x0, x10
    ldr   x16, [x16, #0x10]
    blr   x16
    mov   x0, x10
    bl    hud_release_finalize

en_refresh_msg_flag:
    ldrb w9, [sp, #0x10]
    cbz  w9, en_refresh_pop
    ldr  x0, [sp, #0x8]
    bl   hud_msg_extra_call
en_refresh_pop:
    add  sp, sp, #0x30
en_refresh_done:
    b    en_equip_release

// en_equip_release -- duplicate of eh_equip_release (echoes_helper, above) for
// echoes_helper_entry's own equip_y path. See that routine's comment for the
// full writeup; duplicated rather than shared for the same reason section 2b
// gives for the rest of this routine's body.
en_equip_release:
    add  x8, x21, #0x10
    mov  x16, #0x1000000
en_release_retry:
    ldaxr x9, [x8]
    sub   x10, x9, x16
    and   x11, x9, #0xFF000000
    cmp   x11, x16
    b.ne  en_release_store
    mov   x17, #0x100
    lsl   x17, x17, #48
    bic   x10, x10, x17
en_release_store:
    stlxr w12, x10, [x8]
    cbnz  w12, en_release_retry

    mov   x13, #0xff00
    movk  x13, #0xff00, lsl #16
    movk  x13, #0xfff, lsl #32
    and   x14, x10, x13
    cbnz  x14, en_release_skip_notify
    ldr   x9, [x21]
    ldr   x9, [x9, #0x68]
    mov   x0, x21
    blr   x9
en_release_skip_notify:
    mov   x13, #0xff00
    movk  x13, #0xff00, lsl #16
    movk  x13, #0xffff, lsl #32
    movk  x13, #0xff, lsl #48
    and   x14, x10, x13
    cbnz  x14, en_release_skip_finalize
    ldr   x9, [x21]
    ldr   x9, [x9, #0x8]
    mov   x0, x21
    blr   x9
en_release_skip_finalize:
    b     en_finish_ack

// ===========================================================================
// Kinds 3 (ACCESSORY_SET), 4 (OUTFIT_SET), 5 (CONSUME) -- ENTRY HOOK ONLY
// (echoes_helper/tail-b stays ping+equip_y only, per explicit scope). All
// three share equip_y's exact P/Q acquire-guard-chain and lock-release (CAS on
// P+0x10, then the vtable[0x68]/[0x8] gates) -- factored here into
// en_pq_acquire/en_pq_release (local subroutines, bl/ret) rather than
// quadruplicating that ~15-instruction sequence a third and fourth time.
// en_handle_equip_y/en_equip_release (above) are DELIBERATELY left untouched
// by this refactor -- these are NEW subroutines for the NEW kinds only, so
// nothing already-tested changes behaviour.
//
// en_pq_acquire(): no inputs. Clobbers x9-x18/x0 (scratch) and x30 (saved/
// restored internally via a plain str/ldr pair -- never touches x29, same
// discipline as the rest of echoes_helper_entry). On return: w0 = 0 on
// success (x21 = P, x22 = Q valid), or one of refuse codes 10/11/12/13 on
// failure (x21/x22 undefined in that case). Codes 10 (P==0) and 11 (acquire
// bit0==0) mean NOTHING was acquired -- callers must NOT release. Codes 12
// (Q==0) and 13 (Q+0x17 bit6 set) mean the acquire itself succeeded -- callers
// MUST still release, exactly like equip_y's refuse 12/13.
en_pq_acquire:
    str  x30, [sp, #-16]!
    adrp x9, actor_select_root
    add  x9, x9, :lo12:actor_select_root
    ldr  x21, [x9]                       // P
    cbz  x21, en_pq_acquire_refuse_10

    ldr  x9, [x21]                       // vtable = *P
    ldr  x9, [x9, #0x40]                 // fn = vtable[0x40]
    mov  x0, x21
    blr  x9                              // r = fn(P)
    tbz  w0, #0, en_pq_acquire_refuse_11

    ldr  x22, [x21, #8]                  // Q = *(P + 8)
    cbz  x22, en_pq_acquire_refuse_12

    ldrb w9, [x22, #0x17]
    tst  w9, #0x40
    b.ne en_pq_acquire_refuse_13

    mov  w0, #0
    b    en_pq_acquire_ret
en_pq_acquire_refuse_10:
    mov  w0, #10
    b    en_pq_acquire_ret
en_pq_acquire_refuse_11:
    mov  w0, #11
    b    en_pq_acquire_ret
en_pq_acquire_refuse_12:
    mov  w0, #12
    b    en_pq_acquire_ret
en_pq_acquire_refuse_13:
    mov  w0, #13
en_pq_acquire_ret:
    ldr  x30, [sp], #16
    ret

// en_pq_release(): input x21 = P (set by en_pq_acquire). Byte-for-byte
// identical to en_equip_release's own CAS-decrement-and-gated-notify/finalize
// sequence (HELPER-TOOLCHAIN.md section 5a) -- duplicated as a SEPARATE
// subroutine rather than calling en_equip_release itself, so this refactor
// cannot affect equip_y's already-tested code path at all.
en_pq_release:
    str  x30, [sp, #-16]!
    add  x8, x21, #0x10
    mov  x16, #0x1000000
en_pq_release_retry:
    ldaxr x9, [x8]
    sub   x10, x9, x16
    and   x11, x9, #0xFF000000
    cmp   x11, x16
    b.ne  en_pq_release_store
    mov   x17, #0x100
    lsl   x17, x17, #48
    bic   x10, x10, x17
en_pq_release_store:
    stlxr w12, x10, [x8]
    cbnz  w12, en_pq_release_retry

    mov   x13, #0xff00
    movk  x13, #0xff00, lsl #16
    movk  x13, #0xfff, lsl #32
    and   x14, x10, x13
    cbnz  x14, en_pq_release_skip_notify
    ldr   x9, [x21]
    ldr   x9, [x9, #0x68]
    mov   x0, x21
    blr   x9
en_pq_release_skip_notify:
    mov   x13, #0xff00
    movk  x13, #0xff00, lsl #16
    movk  x13, #0xffff, lsl #32
    movk  x13, #0xff, lsl #48
    and   x14, x10, x13
    cbnz  x14, en_pq_release_skip_finalize
    ldr   x9, [x21]
    ldr   x9, [x9, #0x8]
    mov   x0, x21
    blr   x9
en_pq_release_skip_finalize:
    ldr  x30, [sp], #16
    ret

// Shared exit tails for kinds 3/4/5, all expecting w24 = the value to act on
// (a refuse code for the two refuse tails; otherwise unused). x21 = P must
// still be valid wherever the _with_release/_already/_ok tails are reached.
en_pqk_refuse_no_release:
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    en_finish_ack
en_pqk_refuse_with_release:
    bl   en_pq_release
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    en_finish_ack
en_pqk_already:
    bl   en_pq_release
    mov  w9, #0
    str  w9, [x19, #0x28]
    mov  w9, #3
    str  w9, [x19, #0x10]
    b    en_finish_ack
en_pqk_ok:
    bl   en_pq_release
    mov  w9, #0
    str  w9, [x19, #0x28]
    mov  w9, #1
    str  w9, [x19, #0x10]
    b    en_finish_ack

// ---------------------------------------------------------------------------
// en_handle_accessory -- KIND 3, ACCESSORY_SET. target (mailbox+0x24) = a
// single accessory bit; arg (mailbox+0x2C) = 1 (equip) / 0 (unequip).
//
//   refuse(16) if target == 0 or more than one bit set
//   [P/Q acquire, as above]
//   refuse(17) if !(Q+0x187c & target)                        -- not acquired
//   capacity = 1 + popcount(((Q+0x1874) >> 20) & 0xF), clamped to 5
//       (verified: flags 0x01ffffff -> nibble 0xF -> popcount 4 -> capacity 5,
//       matching native's own SWAR check at main+0x2507900)
//   slots = u32[5] at Q+0x1880
//   EQUIP:   slot == target already -> status 3 (already); else first slot
//            == 0 -> write target there; no empty slot -> refuse(18, "full")
//   UNEQUIP: no slot == target -> status 3 (already, "nothing to remove");
//            else remove it and shift every later slot left by one (verified
//            against the exact native example: [2,0x40,0x1000,0x4000,
//            0x8000000], unequip 0x2 -> [0x40,0x1000,0x4000,0x8000000,0])
//   On any actual change (and ONLY then): bl main+0x2229900(x0 = Q+0x1850)
//   -- the effect rebuild, done while STILL holding the lock, matching
//   native's revoke path at main+0x21bf9a0..0x21bf9dc -- then release.
en_handle_accessory:
    ldr  w23, [x19, #0x24]        // target (accessory bit mask)
    ldr  w24, [x19, #0x2c]        // arg (1 = equip, 0 = unequip) -- reused
                                   // below as a refuse-code holder, but only
                                   // AFTER the equip/unequip branch has
                                   // consumed it, or on a refuse path where
                                   // it is no longer needed

    cbz  w23, en_accessory_bad_target
    sub  w9, w23, #1
    and  w9, w23, w9               // target & (target - 1): nonzero iff >1 bit
    cbz  w9, en_accessory_acquire
en_accessory_bad_target:
    mov  w24, #16
    b    en_pqk_refuse_no_release

en_accessory_acquire:
    bl   en_pq_acquire
    cmp  w0, #0
    b.eq en_accessory_acquired
    cmp  w0, #12
    b.ge en_accessory_acquire_refuse_release
    mov  w24, w0
    b    en_pqk_refuse_no_release
en_accessory_acquire_refuse_release:
    mov  w24, w0
    b    en_pqk_refuse_with_release

en_accessory_acquired:
    // x21 = P, x22 = Q (from en_pq_acquire).
    ldr  w9, [x22, #0x187c]
    and  w9, w9, w23
    cbnz w9, en_accessory_compute_capacity
    mov  w24, #17
    b    en_pqk_refuse_with_release

en_accessory_compute_capacity:
    ldr  w9, [x22, #0x1874]
    lsr  w9, w9, #20
    and  w9, w9, #0xF              // nibble
    and  w10, w9, #1
    lsr  w11, w9, #1
    and  w11, w11, #1
    add  w10, w10, w11
    lsr  w11, w9, #2
    and  w11, w11, #1
    add  w10, w10, w11
    lsr  w11, w9, #3
    and  w11, w11, #1
    add  w10, w10, w11              // w10 = popcount(nibble), 0..4
    add  w10, w10, #1                // w10 = 1 + popcount
    mov  w11, #5
    cmp  w10, w11
    csel w10, w11, w10, hi           // clamp to 5 -- w10 = capacity

    mov  w9, #0x1880
    add  x9, x22, x9               // x9 = &slots[0]
    mov  w12, #0                      // i
    mov  w13, #-1                     // found index (-1 = not found)
en_accessory_scan:
    cmp  w12, w10
    b.ge en_accessory_scan_done
    ldr  w14, [x9, w12, uxtw #2]
    cmp  w14, w23
    b.ne en_accessory_scan_next
    mov  w13, w12
    b    en_accessory_scan_done
en_accessory_scan_next:
    add  w12, w12, #1
    b    en_accessory_scan
en_accessory_scan_done:

    cbz  w24, en_accessory_unequip
    // EQUIP.
    cmn  w13, #1
    b.ne en_pqk_already               // found -> already equipped
    mov  w12, #0
en_accessory_find_empty:
    cmp  w12, w10
    b.ge en_accessory_full
    ldr  w14, [x9, w12, uxtw #2]
    cbz  w14, en_accessory_place
    add  w12, w12, #1
    b    en_accessory_find_empty
en_accessory_place:
    str  w23, [x9, w12, uxtw #2]
    b    en_accessory_changed
en_accessory_full:
    mov  w24, #18
    b    en_pqk_refuse_with_release

en_accessory_unequip:
    cmn  w13, #1
    b.eq en_pqk_already               // not found -> already (nothing to remove)
    mov  w12, w13
en_accessory_shift:
    add  w14, w12, #1
    cmp  w14, w10
    b.ge en_accessory_shift_done
    ldr  w15, [x9, w14, uxtw #2]
    str  w15, [x9, w12, uxtw #2]
    mov  w12, w14
    b    en_accessory_shift
en_accessory_shift_done:
    mov  w15, #0
    str  w15, [x9, w12, uxtw #2]
    b    en_accessory_changed

en_accessory_changed:
    mov  w9, #0x1850
    add  x0, x22, x9
    bl   accessory_effect_rebuild
    b    en_pqk_ok

// ---------------------------------------------------------------------------
// en_handle_outfit -- KIND 4, OUTFIT_SET. target (mailbox+0x24) = a single
// outfit bit (u16, zero-extended). Native sequence main+0x2502f18..0x2502f58:
//   Q+0x187a (u16) == target -> status 3 (already)
//   !(Q+0x1878 (u16) & target) -> refuse(19, "not owned")
//   strh target -> Q+0x187a; h = *(Q+0x1850); w = *(u32*)(h+0x14); w |= 0x100;
//   store back ("model dirty")
//   release
en_handle_outfit:
    ldr  w23, [x19, #0x24]        // target (outfit mask, zero-extended u16)

    bl   en_pq_acquire
    cmp  w0, #0
    b.eq en_outfit_acquired
    cmp  w0, #12
    b.ge en_outfit_acquire_refuse_release
    mov  w24, w0
    b    en_pqk_refuse_no_release
en_outfit_acquire_refuse_release:
    mov  w24, w0
    b    en_pqk_refuse_with_release

en_outfit_acquired:
    ldrh w9, [x22, #0x187a]
    cmp  w9, w23
    b.eq en_pqk_already

    ldrh w9, [x22, #0x1878]
    and  w9, w9, w23
    cbnz w9, en_outfit_owned
    mov  w24, #19
    b    en_pqk_refuse_with_release

en_outfit_owned:
    strh w23, [x22, #0x187a]
    ldr  x9, [x22, #0x1850]       // h
    ldr  w10, [x9, #0x14]          // w = *(h + 0x14)
    orr  w10, w10, #0x100           // "model dirty"
    str  w10, [x9, #0x14]
    b    en_pqk_ok

// ---------------------------------------------------------------------------
// en_handle_consume -- KIND 5, CONSUME. target (mailbox+0x24) = quantity
// index 0..15; arg (mailbox+0x2C, read as SIGNED int32) = heal amount in
// quarter-hearts, already truncated by the module (the same truncation as
// native's fcvtzs; the guest takes the int32 value as given). Live-verified
// on the desktop: a native pause-menu "Eat" of Bubble Kelp changed exactly
// current HP 40 -> 41 and count 15 -> 14, nothing else.
//   refuse(20) if target > 15
//   refuse(21) if arg < 0 or arg > 0x1000 (sanity)
//   [P/Q acquire, as above]
//   c = byte at Q+0x185e+target; c == 0 -> refuse(22, "none left"); else
//       store c-1 (native main+0x21bfa20 additionally clamps the SETTER's
//       own range to 0..99 -- not relevant here since we only ever decrement)
//   hp = s32 Q+0x17cc, max = s32 Q+0x17c8; hp += arg; floor at 0; clamp to
//       max; store back to Q+0x17cc (native ItemMenu eat confirm
//       main+0x2520d70, kind==1 branch: inline add, floor 0, clamp to max)
//   BOTH the decrement and the heal happen under the SAME acquire/release.
//   status 1 on success -- there is no "already" state for this kind.
//   No other native calls: the HeartGauge HUD polls Q+0x17cc every frame by
//   itself, so no explicit "dirty"/rebuild call is needed here (unlike
//   accessory's effect rebuild or outfit's dirty-bit store).
en_handle_consume:
    ldr  w23, [x19, #0x24]        // target = quantity index
    ldr  w24, [x19, #0x2c]        // arg = heal amount (signed quarter-hearts)

    cmp  w23, #15
    b.hi en_consume_bad_index
    cmp  w24, #0
    b.lt en_consume_bad_heal
    mov  w9, #0x1000
    cmp  w24, w9
    b.gt en_consume_bad_heal
    b    en_consume_acquire

en_consume_bad_index:
    mov  w24, #20
    b    en_pqk_refuse_no_release
en_consume_bad_heal:
    mov  w24, #21
    b    en_pqk_refuse_no_release

en_consume_acquire:
    bl   en_pq_acquire
    cmp  w0, #0
    b.eq en_consume_acquired
    cmp  w0, #12
    b.ge en_consume_acquire_refuse_release
    mov  w24, w0
    b    en_pqk_refuse_no_release
en_consume_acquire_refuse_release:
    mov  w24, w0
    b    en_pqk_refuse_with_release

en_consume_acquired:
    // x21 = P, x22 = Q.
    mov  w9, #0x185e
    add  x9, x22, x9
    ldrb w10, [x9, w23, uxtw]
    cbnz w10, en_consume_has_stock
    mov  w24, #22
    b    en_pqk_refuse_with_release

en_consume_has_stock:
    sub  w10, w10, #1
    strb w10, [x9, w23, uxtw]

    ldr  w10, [x22, #0x17cc]       // hp
    ldr  w11, [x22, #0x17c8]       // max
    add  w10, w10, w24               // hp += heal (w24 still holds arg here --
                                       // never clobbered on this success path)
    cmp  w10, #0
    csel w10, wzr, w10, lt            // floor at 0
    cmp  w10, w11
    csel w10, w11, w10, gt            // clamp to max
    str  w10, [x22, #0x17cc]
    b    en_pqk_ok

// ===========================================================================
// Kind 6 (WARP) -- ENTRY HOOK ONLY. Starts the game's MapWarp event flow
// directly, exactly as the Map menu's confirm code does (main+0x25b827c..
// main+0x25b83c8), ground-truthed byte-exact against
// disasm-flowstart-0x163f560.txt / disasm-menu-postcall-cleanup-0x25b8394.txt
// / the annotated trace the owner supplied (NOT independently re-verified by
// this script beyond reading those bytes). UNLIKE kinds 2-5, there is NO
// save-data P lock here at all -- this mechanism does not use it, so none of
// en_pq_acquire/en_pq_release/en_pqk_* apply; this kind has its own simple
// refuse/ok tails (en_warp_refuse/en_warp_ok) below.
//
// Sequence (mirrors the menu's own staging, object-for-object):
//   1. construct entryNameRef: bl main+0x15c3a90(out=&entryNameRef,
//      work=&scratch, &{ptr=mailbox entry bytes, len=entry_len})
//   2. construct container:    bl main+0x28025f0(out=&containerHandle, 2)
//   3. IF level_len > 0:  bl main+0x2802f50(container, &"level", 5,
//      &{ptr=mailbox level bytes, len=level_len})
//   4. ALWAYS (locator_len is never 0, refused otherwise):
//      bl main+0x2802f50(container, &"locator", 7,
//      &{ptr=mailbox locator bytes, len=locator_len})
//   5. mgr = *(main+0x34bb8d8); mgr == 0 -> refuse(23); else
//      bl main+0x163f560(&result, mgr, &entryNameRef, &container) -- the 4th
//      argument is the ADDRESS of the 16-byte container pair (native:
//      main+0x25b8380..0x25b8390 does `add x3, sp, #0xa0`), NOT the
//      container.obj value that x21 holds for the Set() calls -- corrected
//      after this bug hung the game live (gdb: stuck at PC=0x4, LR=blob+0xd1c,
//      inside the result.ctrl free right after this call, because the
//      original code passed x21 here instead of &container).
//   6. release result.second (offset +8 of the 16-byte result pair) if
//      non-null: the SAME CAS-decrement-then-maybe-free pattern used
//      throughout this file, but the SIMPLE +1/-1 refcount (no scaled/
//      multi-field P-lock encoding) -- byte-for-byte from
//      main+0x25b839c..0x25b83c8.
//   result_code on success = the x0 main+0x163f560 itself returns (captured
//   immediately after the call, before the release's own blr/bl clobber it).
//   Per the trace, resultOut's FIRST qword is unconditionally zeroed by
//   0x163f560 before it returns in the common path, so it carries no
//   diagnostic information; what IS left in x0 at that point is, per the
//   trace's own reading of 0x163f560's tail, simply main+0x2834260's own
//   passthrough return value (the last call 0x163f560 makes before its
//   epilogue, with nothing re-assigning x0 afterward) -- not independently
//   confirmed beyond that reading.
//
// STACK OBJECT SIZES -- UNCERTAIN, see HELPER-TOOLCHAIN.md section 5d. The
// menu's own frame shows resultOut/containerHandle/entryNameRef/the
// name-ctor's {ptr,len} input all spaced exactly 0x10 (16 bytes) apart
// (sp+0x90,0xa0,0xb0,0xc0,0xd0) -- strong, but not certain, evidence each is
// exactly 16 bytes. The WORK scratch for main+0x15c3a90 (menu: sp+0xd0
// onward) has NO visible upper bound in the trace we were given (the called
// function is undecompiled, "budget"); this helper allocates 0x40 (64)
// bytes for it, a generous guess, not a verified size.
//
// Register discipline: x19 (mailbox) and x21 (container pointer, reloaded
// once after step 2, used by steps 3/4) are the only registers that must
// survive native calls here, both already callee-saved under the same
// AAPCS64 assumption this whole file relies on. w23/w24 are plain scratch
// (w24 doubles as the diagnostic result_code holder across the release call).
en_handle_warp:
    ldr  w9, [x19, #0x30]          // entry_len
    cbz  w9, en_warp_bad_entry
    cmp  w9, #32
    b.hi en_warp_bad_entry
    b    en_warp_check_level
en_warp_bad_entry:
    mov  w24, #24
    b    en_warp_refuse

en_warp_check_level:
    ldr  w10, [x19, #0x34]         // level_len
    cmp  w10, #64
    b.hi en_warp_bad_level
    b    en_warp_check_locator
en_warp_bad_level:
    mov  w24, #25
    b    en_warp_refuse

en_warp_check_locator:
    ldr  w11, [x19, #0x38]         // locator_len
    cbz  w11, en_warp_bad_locator
    cmp  w11, #64
    b.hi en_warp_bad_locator
    b    en_warp_begin
en_warp_bad_locator:
    mov  w24, #26
    b    en_warp_refuse

en_warp_begin:
    // CORRECTED staging, per the owner's own reading of the menu's full
    // function (text.bin + rodata) -- main+0x15c3a90's second argument is
    // NOT scratch, it is a pointer to a 16-byte {obj,ctrl} "holder" built
    // from a loaded MapWarp flow resource (steps A-D below); the original
    // all-zero-scratch version silently produced an empty name.
    //   +0x00  result   (16B: [0]=scratch word, [8]=result.ctrl)
    //   +0x10  container (16B: [0]=obj used for Set() calls, [8]=ctrl)
    //   +0x20  name      (16B: [0]=obj, [8]=ctrl; built by warp_name_ctor)
    //   +0x30  entryview (16B: {ptr=mailbox entry bytes, len=entry_len})
    //   +0x40  value-pair scratch (16B), reused for level AND locator Set()
    //   +0x50  pathview  (16B: {ptr=rodata "event/MapWarp.bfevfl", len=0x14})
    //   +0x60  res       (16B: {obj,ctrl}; built by warp_resource_load)
    //   +0x70  holder    (16B: {obj,ctrl}; built by warp_holder_ctor)
    sub  sp, sp, #0x80
    stp  xzr, xzr, [sp, #0x00]
    stp  xzr, xzr, [sp, #0x10]
    stp  xzr, xzr, [sp, #0x20]
    stp  xzr, xzr, [sp, #0x30]
    stp  xzr, xzr, [sp, #0x40]
    stp  xzr, xzr, [sp, #0x50]
    stp  xzr, xzr, [sp, #0x60]
    stp  xzr, xzr, [sp, #0x70]

    // Step A: pathview = {ptr = rodata "event/MapWarp.bfevfl", len = 0x14}.
    // Pointed straight at rodata (not copied) -- matches the menu's own x2.
    adrp x9, warp_path_str
    add  x9, x9, :lo12:warp_path_str
    mov  w10, #0x14
    stp  x9, x10, [sp, #0x50]

    // Step B: resource-manager check, then load res from the pathview.
    adrp x9, warp_resource_mgr_ptr
    add  x9, x9, :lo12:warp_resource_mgr_ptr
    ldr  x9, [x9]
    cbz  x9, en_warp_no_resource_mgr

    add  x0, sp, #0x60              // &res (already zeroed)
    mov  x1, x9                      // resource manager
    add  x2, sp, #0x50                // &pathview
    bl   warp_resource_load

    // Step C: if res.obj != 0, bl main+0x38a680(x0 = res.obj) -- replicated
    // as-is (menu: main+0x25b8108..0x25b8110), no return value checked.
    ldr  x9, [sp, #0x60]
    cbz  x9, en_warp_after_prepare
    mov  x0, x9
    bl   warp_resource_prepare
en_warp_after_prepare:

    // Step D: holder = ctor(&res).
    add  x0, sp, #0x70               // &holder (already zeroed)
    add  x1, sp, #0x60                // &res
    bl   warp_holder_ctor

    // entryview = {ptr = &mailbox entry bytes, len = entry_len}.
    add  x9, x19, #0x100
    ldr  w10, [x19, #0x30]
    stp  x9, x10, [sp, #0x30]

    // Step E: name = ctor(&holder, &entryview). name.obj == 0 -> refuse(28),
    // matching the menu's own `ldr x8,[sp,#0xb0]; cbz` right after this call.
    add  x0, sp, #0x20                // &name (already zeroed)
    add  x1, sp, #0x70                 // &holder
    add  x2, sp, #0x30                 // &entryview
    bl   warp_name_ctor

    ldr  x9, [sp, #0x20]
    cbz  x9, en_warp_empty_name

    // Step F: construct the container (capacity hint 2).
    add  x0, sp, #0x10
    mov  w1, #2
    bl   warp_container_ctor
    ldr  x21, [sp, #0x10]            // container.obj (reload, persistent)

    // Step G: Set("level", 5, {ptr, len}) -- ONLY if level_len > 0.
    ldr  w9, [x19, #0x34]
    cbz  w9, en_warp_locator
    add  x8, x19, #0x120
    stp  x8, x9, [sp, #0x40]
    mov  x0, x21
    adrp x1, warp_level_str
    add  x1, x1, :lo12:warp_level_str
    mov  w2, #5
    add  x3, sp, #0x40
    bl   warp_container_set

en_warp_locator:
    // Set("locator", 7, {ptr, len}) -- always (locator_len is never 0 here).
    ldr  w9, [x19, #0x38]
    add  x8, x19, #0x160
    stp  x8, x9, [sp, #0x40]
    mov  x0, x21
    adrp x1, warp_locator_str
    add  x1, x1, :lo12:warp_locator_str
    mov  w2, #7
    add  x3, sp, #0x40
    bl   warp_container_set

    // Step H: mgr check, then the flow-start call.
    adrp x9, warp_mgr_ptr
    add  x9, x9, :lo12:warp_mgr_ptr
    ldr  x9, [x9]
    cbz  x9, en_warp_no_mgr

    add  x0, sp, #0x00               // &result
    mov  x1, x9                       // mgr
    add  x2, sp, #0x20                // &name (the entryNameRef argument)
    add  x3, sp, #0x10                // &container (ADDRESS of the 16-byte pair,
                                        // NOT container.obj value -- native
                                        // main+0x25b8380..0x25b8390 does
                                        // `add x3, sp, #0xa0`, confirmed live via
                                        // gdb after kind 6 hung with PC=0x4,
                                        // LR=blob+0xd1c; x21 (container.obj VALUE)
                                        // stays correct for the two Set() calls
                                        // above (x0 = x21), only this x3 was wrong)
    bl   warp_flow_start
    mov  w24, w0                      // diagnostic result_code -- see comment above

    // "Plus result.ctrl right after 163f560 (already done)" -- released
    // here, BEFORE the unified container/name/holder/res cleanup below.
    ldr  x10, [sp, #0x8]
    cbz  x10, en_warp_result_skip
    add  x11, x10, #8
en_warp_result_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w14, x13, [x11]
    cbnz  w14, en_warp_result_retry
    cbnz  x12, en_warp_result_skip
    ldr   x15, [x10]
    mov   x0, x10
    ldr   x15, [x15, #0x10]
    blr   x15
    mov   x0, x10
    bl    hud_release_finalize
en_warp_result_skip:

    b    en_warp_cleanup_for_ok

en_warp_no_mgr:
    mov  w24, #23
    b    en_warp_cleanup_for_refuse
en_warp_empty_name:
    mov  w24, #28
    b    en_warp_cleanup_for_refuse
en_warp_no_resource_mgr:
    mov  w24, #27
    b    en_warp_cleanup_for_refuse

// Unified cleanup -- run on EVERY exit path from en_warp_begin onward
// (including every refusal): releases container.ctrl, name.ctrl,
// holder.ctrl, res.ctrl, in that order. Any object not yet constructed at
// the point of exit still has ctrl == 0 (zeroed at en_warp_begin), and
// release_once(0) is a no-op, so this is safe to run unconditionally rather
// than needing to track "how far did we get" separately. Duplicated once
// per exit shape (refuse vs ok) rather than shared via a callable
// subroutine, to avoid that subroutine needing to reference this routine's
// own sp-relative frame across its own nested bl/ret saves/restores.
en_warp_cleanup_for_refuse:
    ldr  x10, [sp, #0x18]
    cbz  x10, en_warp_rc_container_skip
    add  x11, x10, #8
en_warp_rc_container_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w14, x13, [x11]
    cbnz  w14, en_warp_rc_container_retry
    cbnz  x12, en_warp_rc_container_skip
    ldr   x15, [x10]
    mov   x0, x10
    ldr   x15, [x15, #0x10]
    blr   x15
    mov   x0, x10
    bl    hud_release_finalize
en_warp_rc_container_skip:
    ldr  x10, [sp, #0x28]
    cbz  x10, en_warp_rc_name_skip
    add  x11, x10, #8
en_warp_rc_name_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w14, x13, [x11]
    cbnz  w14, en_warp_rc_name_retry
    cbnz  x12, en_warp_rc_name_skip
    ldr   x15, [x10]
    mov   x0, x10
    ldr   x15, [x15, #0x10]
    blr   x15
    mov   x0, x10
    bl    hud_release_finalize
en_warp_rc_name_skip:
    ldr  x10, [sp, #0x78]
    cbz  x10, en_warp_rc_holder_skip
    add  x11, x10, #8
en_warp_rc_holder_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w14, x13, [x11]
    cbnz  w14, en_warp_rc_holder_retry
    cbnz  x12, en_warp_rc_holder_skip
    ldr   x15, [x10]
    mov   x0, x10
    ldr   x15, [x15, #0x10]
    blr   x15
    mov   x0, x10
    bl    hud_release_finalize
en_warp_rc_holder_skip:
    ldr  x10, [sp, #0x68]
    cbz  x10, en_warp_rc_res_skip
    add  x11, x10, #8
en_warp_rc_res_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w14, x13, [x11]
    cbnz  w14, en_warp_rc_res_retry
    cbnz  x12, en_warp_rc_res_skip
    ldr   x15, [x10]
    mov   x0, x10
    ldr   x15, [x15, #0x10]
    blr   x15
    mov   x0, x10
    bl    hud_release_finalize
en_warp_rc_res_skip:
    add  sp, sp, #0x80
    b    en_warp_refuse

en_warp_cleanup_for_ok:
    ldr  x10, [sp, #0x18]
    cbz  x10, en_warp_ro_container_skip
    add  x11, x10, #8
en_warp_ro_container_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w14, x13, [x11]
    cbnz  w14, en_warp_ro_container_retry
    cbnz  x12, en_warp_ro_container_skip
    ldr   x15, [x10]
    mov   x0, x10
    ldr   x15, [x15, #0x10]
    blr   x15
    mov   x0, x10
    bl    hud_release_finalize
en_warp_ro_container_skip:
    ldr  x10, [sp, #0x28]
    cbz  x10, en_warp_ro_name_skip
    add  x11, x10, #8
en_warp_ro_name_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w14, x13, [x11]
    cbnz  w14, en_warp_ro_name_retry
    cbnz  x12, en_warp_ro_name_skip
    ldr   x15, [x10]
    mov   x0, x10
    ldr   x15, [x15, #0x10]
    blr   x15
    mov   x0, x10
    bl    hud_release_finalize
en_warp_ro_name_skip:
    ldr  x10, [sp, #0x78]
    cbz  x10, en_warp_ro_holder_skip
    add  x11, x10, #8
en_warp_ro_holder_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w14, x13, [x11]
    cbnz  w14, en_warp_ro_holder_retry
    cbnz  x12, en_warp_ro_holder_skip
    ldr   x15, [x10]
    mov   x0, x10
    ldr   x15, [x15, #0x10]
    blr   x15
    mov   x0, x10
    bl    hud_release_finalize
en_warp_ro_holder_skip:
    ldr  x10, [sp, #0x68]
    cbz  x10, en_warp_ro_res_skip
    add  x11, x10, #8
en_warp_ro_res_retry:
    ldaxr x12, [x11]
    sub   x13, x12, #1
    stlxr w14, x13, [x11]
    cbnz  w14, en_warp_ro_res_retry
    cbnz  x12, en_warp_ro_res_skip
    ldr   x15, [x10]
    mov   x0, x10
    ldr   x15, [x15, #0x10]
    blr   x15
    mov   x0, x10
    bl    hud_release_finalize
en_warp_ro_res_skip:
    add  sp, sp, #0x80
    b    en_warp_ok

en_warp_refuse:
    str  w24, [x19, #0x28]
    mov  w9, #2
    str  w9, [x19, #0x10]
    b    en_finish_ack
en_warp_ok:
    str  w24, [x19, #0x28]
    mov  w9, #1
    str  w9, [x19, #0x10]
    b    en_finish_ack

en_finish_ack:
    add  x9, x19, #12
    stlr w20, [x9]
    b    en_epilogue
en_done:
    b    en_epilogue
en_epilogue:
    ldr  x30, [sp, #112]
    ldp  x23, x24, [sp, #96]
    ldp  x21, x22, [sp, #80]
    ldp  x19, x20, [sp, #64]
    ldp  x6, x7, [sp, #48]
    ldp  x4, x5, [sp, #32]
    ldp  x2, x3, [sp, #16]
    ldp  x0, x1, [sp, #0]
    add  sp, sp, #128
    // SP is now exactly the value it had at entry (the caller's SP). Only now
    // is it safe to apply the replaced instruction's real effect.
.global entry_replay_slot
entry_replay_slot:
    nop                        // placeholder word; build_echoes_helper.py
                               // overwrites these exact 4 bytes with
                               // --original-bytes verbatim (a direct byte
                               // splice, not a relocation -- is_pc_relative()
                               // is the gate that makes this safe)
.global entry_resume
entry_resume:
    b    entry_resume_target  // relocation target "main", addend = hook_address + 4
