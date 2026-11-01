// SPDX-License-Identifier: GPL-3.0-or-later
// Sword1.3.2 native background League Card renderer; first-load/cancel verified.
// F1CD30 is the AppManager's single lifecycle-dispatch BL674110. The background
// owner ADF8B0/ADFE00/ADFF50 is shipped code; no application/menu is opened.
// Serialization is ours: do not advance a queued native app/process or finalize
// the original field heap while this singleton renderer is cancelling/shutting down.
// 700..767 protocol is independent of the item/battle request channels.
.global sw_portrait_entry
sw_portrait_entry:
    sub sp, sp, #0x80
    stp x0, x1, [sp, #0x00]
    stp x19, x20, [sp, #0x10]
    stp x21, x22, [sp, #0x20]
    stp x23, x24, [sp, #0x30]
    stp x25, x26, [sp, #0x40]
    stp x27, x28, [sp, #0x50]
    stp x29, x30, [sp, #0x60]
    mov x19, x0
    adrp x20, sw_mailbox_base
    add x20, x20, :lo12:sw_mailbox_base
    bl portrait_poll
    cbnz w0, 1f
    mov x0, x19
    bl sw_app_lifecycle
1:  ldr x1, [sp, #0x08]
    ldp x19, x20, [sp, #0x10]
    ldp x21, x22, [sp, #0x20]
    ldp x23, x24, [sp, #0x30]
    ldp x25, x26, [sp, #0x40]
    ldp x27, x28, [sp, #0x50]
    ldp x29, x30, [sp, #0x60]
    add sp, sp, #0x80
    b sw_portrait_resume

// x19 AppManager,x20 mailbox; return1 defers ONLY original lifecycle-dispatch.
portrait_poll:
    stp x29, x30, [sp, #-0x30]!
    stp x21, x22, [sp, #0x10]
    stp x23, x24, [sp, #0x20]
    ldr w9, [x20, #0x71c]
    add w9, w9, #1
    str w9, [x20, #0x71c]
    ldr w21, [x20, #0x714]
    cbnz w21, portrait_active
    ldr w22, [x20, #0x700]
    cbz w22, portrait_allow
    ldr w9, [x20, #0x710]
    cmp w9, w22
    b.eq portrait_allow
    str w22, [x20, #0x710]
    str wzr, [x20, #0x718]
    ldr w9, [x20]
    movz w10, #0x5753
    movk w10, #0x314d, lsl #16
    cmp w9, w10
    b.ne portrait_refuse
    ldr w9, [x20, #0x704]
    add x10, x20, #0xffc
    ldar w10, [x10]
    cbz w10, portrait_refuse
    cmp w9, w10
    b.ne portrait_refuse
    ldr w9, [x20, #0x708]
    cmp w9, w22
    b.eq portrait_refuse
    // Atomically exclude either native worker before observing singleton0.
    add x12, x20, #0x764
1:  ldaxr w9, [x12]
    cbnz w9, portrait_refuse
    mov w10, #1
    stxr w11, w10, [x12]
    cbnz w11, 1b
    bl portrait_field
    cbz x0, portrait_refuse_lease
    mov x23, x0                       // original field app
    bl field_free
    cbz w0, portrait_refuse_lease
    ldr w9, [x20, #0x170]
    cbnz w9, portrait_refuse_lease
    ldr w9, [x20, #0x110]
    ldr w10, [x20, #0x140]
    cmp w9, w10
    b.ne portrait_refuse_lease
    adrp x9, sw_portrait_singleton
    add x9, x9, :lo12:sw_portrait_singleton
    ldr x9, [x9]
    cbnz x9, portrait_refuse_lease
    adrp x9, sw_fieldmgr_slot
    add x9, x9, :lo12:sw_fieldmgr_slot
    ldr x9, [x9]
    ldr x24, [x9]
    cbz x24, portrait_refuse_lease
    ldr x9, [x24, #0x60]
    cmp x9, x19
    b.ne portrait_refuse_lease
    ldr x21, [x24, #8]                 // persistent Global heap, same as FieldApp heap
    cbz x21, portrait_refuse_lease
    bl gamedata
    cbz x0, portrait_refuse_lease
    ldr x22, [x0, #0x288]              // authoritative own printed-card object
    cbz x22, portrait_refuse_lease
    str x23, [x20, #0x738]
    str x24, [x20, #0x730]
    str x19, [x20, #0x758]
    str x21, [x20, #0x748]
    mov x0, x22
    bl obj_lock
    str x22, [x20, #0x728]
    bl portrait_tls_begin
    cbz x0, portrait_start_failed
    // Allocate native ref-counted owner0x98. Native allocation metadata must be
    // initialized exactly like ADCD50 so final ref_release frees the right block.
    mov x0, x21
    mov w1, #0x98
    bl portrait_alloc
    cbz x0, portrait_start_failed
    mov x23, x0
    mov x1, x21
    bl sw_portrait_ctor
    mov x0, x23
    mov x1, x21
    mov w2, #0x98
    bl portrait_object
    str x23, [x20, #0x740]
    // Exact native900-square resource descriptor from1532790; one target only.
    mov x0, x21
    mov w1, #0x4a8
    bl portrait_alloc
    cbz x0, portrait_start_failed
    mov x23, x0
    adr x1, portrait_target_desc
    bl sw_portrait_target_ctor
    mov x0, x23
    mov x1, x21
    mov w2, #0x4a8
    bl portrait_object
    str x23, [x20, #0x720]
    // Native clear color alpha1 (15328AC).
    mov x9, #0x3f80000000000000
    stp xzr, x9, [x23, #0x98]
    ldr x0, [x20, #0x740]
    add x1, x20, #0x728
    add x2, x20, #0x720
    mov w3, #1                        // worker performs asynchronous backend shutdown
    bl sw_portrait_prepare
    cbz w0, portrait_start_failed
    bl portrait_tls_end
    mov w9, #1
    str w9, [x20, #0x714]
    str wzr, [x20, #0x750]
    str wzr, [x20, #0x754]
    b portrait_tick
portrait_start_failed:
    bl portrait_tls_end
    mov w9, #2
    str w9, [x20, #0x718]
    bl portrait_release
    b portrait_allow
portrait_refuse_lease:
    add x9, x20, #0x764
    stlr wzr, [x9]
portrait_refuse:
    mov w9, #1
    str w9, [x20, #0x718]
    bl portrait_complete
    b portrait_allow
portrait_active:
    ldr w9, [x20, #0x760]             // hard lifetime fault is never retried
    cbnz w9, portrait_defer
    adrp x9, sw_fieldmgr_slot
    add x9, x9, :lo12:sw_fieldmgr_slot
    ldr x9, [x9]
    cbz x9, portrait_lifetime_fault
    ldr x9, [x9]
    ldr x10, [x20, #0x730]
    cmp x9, x10
    b.ne portrait_lifetime_fault
    ldr x9, [x20, #0x758]
    cmp x9, x19
    b.ne portrait_lifetime_fault
    ldr x9, [x10, #0x60]
    cmp x9, x19
    b.ne portrait_lifetime_fault
    ldr x9, [x10, #8]
    ldr x10, [x20, #0x748]
    cmp x9, x10
    b.ne portrait_lifetime_fault
    cmp w21, #2
    b.eq portrait_ready
    // Keep global/app/heap and queued requests alive. Even epoch cancellation
    // drives native cleanup to idle; never free an active renderer on timeout.
    ldr w9, [x20, #0x750]
    add w9, w9, #1
    str w9, [x20, #0x750]
    cmp w21, #1
    b.ne portrait_cancel_tick
    cmp w9, #900
    b.hs portrait_timeout
    ldr w9, [x20, #0x700]
    ldr w10, [x20, #0x710]
    cmp w9, w10
    b.ne portrait_cancel
    ldr w9, [x20, #0x708]
    cmp w9, w10
    b.eq portrait_cancel
    ldr w9, [x20, #0x704]
    add x10, x20, #0xffc
    ldar w10, [x10]
    cmp w9, w10
    b.ne portrait_cancel
    cmp x19, xzr
    b.eq portrait_cancel
    ldr x9, [x20, #0x758]
    cmp x9, x19
    b.ne portrait_cancel
    bl portrait_field
    ldr x9, [x20, #0x738]
    cmp x0, x9
    b.ne portrait_cancel
    bl field_free
    cbz w0, portrait_cancel
    ldr w9, [x20, #0x170]
    cbnz w9, portrait_cancel
    ldr w9, [x20, #0x110]
    ldr w10, [x20, #0x140]
    cmp w9, w10
    b.ne portrait_cancel
    b portrait_tick
portrait_lifetime_fault:
    // A replaced Global/heap is not an ordinary UI interruption. Never invoke
    // old GPU objects or free them through a new singleton. Guest reset is the
    // only safe recovery from this impossible-within-serialized-world boundary.
    mov w9, #1
    str w9, [x20, #0x760]
    mov w9, #4
    str w9, [x20, #0x714]
    mov w9, #5
    str w9, [x20, #0x718]
    b portrait_defer
portrait_timeout:
    mov w9, #3
    str w9, [x20, #0x718]
portrait_cancel:
    mov w9, #3
    str w9, [x20, #0x714]
    ldr w9, [x20, #0x718]
    cbnz w9, portrait_cancel_tick
    mov w9, #4
    str w9, [x20, #0x718]
portrait_cancel_tick:
    ldr x0, [x20, #0x740]
    bl sw_portrait_cancel
    ldr w9, [x20, #0x754]
    add w9, w9, #1
    str w9, [x20, #0x754]
    cmp w9, #1800
    b.lo portrait_tick
    mov w9, #4                        // fault-retained: continuecleanup, neverfree live GPU state
    str w9, [x20, #0x714]
portrait_tick:
    ldr x0, [x20, #0x740]
    bl sw_portrait_tick
    ldr x9, [x20, #0x740]
    ldr w10, [x9, #0x68]
    cbnz w10, portrait_running
    ldr w10, [x20, #0x714]
    cmp w10, #1
    b.ne portrait_finish_cancel
    ldrb w10, [x9, #0x92]
    cbz w10, portrait_finish_cancel
    adrp x10, sw_portrait_singleton
    add x10, x10, :lo12:sw_portrait_singleton
    ldr x10, [x10]
    cbnz x10, portrait_finish_cancel
    mov x0, x9
    bl ref_release
    str xzr, [x20, #0x740]
    mov w9, #2                        // target remainsstrong while hostcopies GPU-synced bytes
    str w9, [x20, #0x714]
    add x9, x20, #0x764
    stlr wzr, [x9]                    // backendfullyshut; retainedoutputdoesnotownsingleton
    b portrait_allow
portrait_running:
    ldr w9, [x20, #0x714]
    cmp w9, #1
    b.eq portrait_allow
    b portrait_defer
portrait_finish_cancel:
    bl portrait_release
    b portrait_allow
portrait_ready:
    ldr w9, [x20, #0x710]
    ldr w10, [x20, #0x70c]
    cmp w9, w10
    b.eq portrait_finish_cancel
    ldr w10, [x20, #0x708]
    cmp w9, w10
    b.eq portrait_finish_cancel
    ldr w9, [x20, #0x704]
    add x10, x20, #0xffc
    ldar w10, [x10]
    cmp w9, w10
    b.ne portrait_finish_cancel
    b portrait_allow
portrait_allow: mov w0, wzr; b portrait_return
portrait_defer: mov w0, #1
portrait_return:
    ldp x23, x24, [sp, #0x20]
    ldp x21, x22, [sp, #0x10]
    ldp x29, x30, [sp], #0x30
    ret

// Return currentFieldApp onlywhen exactown field VT and lifecycle phase2.
portrait_field:
    ldr x9, [x19, #0x108]
    cbz x9, 1f
    ldr x10, [x19, #0x100]
    sub x9, x9, #1
    ldr x0, [x10, x9, lsl #3]
    cbz x0, 1f
    ldr w9, [x0, #0x68]
    cmp w9, #2
    b.ne 1f
    ldr x9, [x0]
    ldr x9, [x9]
    adrp x10, sw_portrait_field_dtor
    add x10, x10, :lo12:sw_portrait_field_dtor
    cmp x9, x10
    b.ne 1f
    ret
1:  mov x0, xzr
    ret
// Exact native scopedheap idiom: constructors5E1890/5F1A40 capture the
// current TLSheap and RenderResource allocates its internal arrays through it.
portrait_tls_begin:
    stp x29, x30, [sp, #-0x10]!
    adrp x9, sw_portrait_tls_index
    add x9, x9, :lo12:sw_portrait_tls_index
    ldr x9, [x9]
    ldr w0, [x9]
    bl sw_portrait_tls
    ldr x9, [x0, #0x80]
    cmp x9, #0x10
    b.hs 1f
    str x21, [x0, x9, lsl #3]
    add x9, x9, #1
    str x9, [x0, #0x80]
    str x0, [x20, #0x778]
    ldp x29, x30, [sp], #0x10
    ret
1:  mov x0, xzr
    ldp x29, x30, [sp], #0x10
    ret
portrait_tls_end:
    ldr x9, [x20, #0x778]
    cbz x9, 1f
    ldr x10, [x9, #0x80]
    sub x10, x10, #1
    str x10, [x9, #0x80]
    str xzr, [x20, #0x778]
1:  ret
// Allocate from persistentGlobalheap; no calls when returnednull.
portrait_alloc:
    mov w2, #8
    ldr x8, [x0]
    ldr x8, [x8, #0x10]
    br x8
// x0object,x1parentHeap,w2size. Base ctoralreadyinitialized refcount.
portrait_object:
    stp x1, x0, [x0, #8]
    mov w9, #8
    stp x2, x9, [x0, #0x18]
    b obj_lock
portrait_release:
    stp x29, x30, [sp, #-0x10]!
    ldr x0, [x20, #0x740]
    cbz x0, 1f
    bl ref_release
    str xzr, [x20, #0x740]
1:  ldr x0, [x20, #0x720]
    cbz x0, 2f
    bl ref_release
    str xzr, [x20, #0x720]
2:  ldr x0, [x20, #0x728]
    cbz x0, 3f
    bl ref_release
    str xzr, [x20, #0x728]
3:  str wzr, [x20, #0x714]
    add x9, x20, #0x764
    stlr wzr, [x9]
    bl portrait_complete
    ldp x29, x30, [sp], #0x10
    ret
portrait_complete:
    ldr w9, [x20, #0x710]
    add x10, x20, #0x780
    stlr w9, [x10]                    // terminal marker LAST, afterallrefs/lease released
    ret
.balign 8
portrait_target_desc:
    .quad 0, 0x0000038400000384
    .word 0xf05
    .quad 0x0000038400000384
    .word 0xf05
    .word 8, 8, 0xb01, 1, 1
    .word 0, 0, 0
.balign 4

// Precise nativephotoentry guards close the independent field/network callback
// race. Native requests remain untouched. Only a caller already owning the same
// native worker may recurse under lease2. Our exactworker bypasses under lease1.
.global sw_portrait_small_entry
sw_portrait_small_entry:
    stp x19, x20, [sp, #-0x30]!
    stp x21, x22, [sp, #0x10]
    stp x29, x30, [sp, #0x20]
    mov x19, x0
    adrp x20, sw_mailbox_base
    add x20, x20, :lo12:sw_mailbox_base
    mov w22, wzr
    bl portrait_native_lease
    mov w21, w0
    cbz w0, portrait_native_small_defer
    mov x0, x19
    bl portrait_small_original
    b portrait_native_return
portrait_native_small_defer:
    ldr w8, [x19, #0x68]
    cmp w8, #0
    cset w0, eq                        // same pending/doneABI, no guestobjectmutation
    b portrait_native_return
portrait_small_original:
    sub sp, sp, #0x40                  // reproducedADFF50entry
    b sw_portrait_small_resume

.global sw_portrait_queue_entry
sw_portrait_queue_entry:
    stp x19, x20, [sp, #-0x30]!
    stp x21, x22, [sp, #0x10]
    stp x29, x30, [sp, #0x20]
    mov x19, x0
    adrp x20, sw_mailbox_base
    add x20, x20, :lo12:sw_mailbox_base
    mov w22, #1
    bl portrait_native_lease
    mov w21, w0
    cbz w0, portrait_native_queue_defer
    mov x0, x19
    bl portrait_queue_original
    b portrait_native_return
portrait_native_queue_defer:
    ldr w8, [x19, #0x68]
    mov w9, #13
    lsrv w9, w9, w8
    and w9, w9, #1
    cmp w8, #4
    csel w9, w9, wzr, lo
    ldr w8, [x19, #0x6c]
    cmp w8, #0
    cset w0, ne
    orr w0, w0, w9
    b portrait_native_return
portrait_queue_original:
    stp x28, x27, [sp, #-0x60]!        // reproduced10F28A0entry
    b sw_portrait_queue_resume

// Return1 outer nativelease,2 sameowner/nativeAuto bypass,0 defer. x1..x7
// are untouched; usesonlycaller-saved temporaries. No spinwaitingonanotherjob.
portrait_native_lease:
    add x12, x20, #0x764
1:  ldaxr w9, [x12]
    cmp w9, #1
    b.eq 3f
    cmp w9, #2
    b.eq 4f
    cbnz w9, 5f
    mov w10, #2
    stxr w11, w10, [x12]
    cbnz w11, 1b
    str x19, [x20, #0x768]
    mov w0, #1
    ret
3:  cbnz w22, 5f
    ldr w9, [x20, #0x760]
    cbnz w9, 5f
    ldr w9, [x20, #0x714]
    cbz w9, 5f
    cmp w9, #4
    b.hi 5f
    cmp w9, #2
    b.eq 5f
    ldr x9, [x20, #0x740]
    cmp x9, x19
    b.eq 6f
    b 5f
4:  ldr x9, [x20, #0x768]
    cmp x9, x19
    b.ne 5f
6:  mov w0, #2
    ret
5:  mov w0, wzr
    ret
portrait_native_return:
    cmp w21, #1
    b.ne 1f
    str xzr, [x20, #0x768]
    add x9, x20, #0x764
    stlr wzr, [x9]
1:  ldp x29, x30, [sp, #0x20]
    ldp x21, x22, [sp, #0x10]
    ldp x19, x20, [sp], #0x30
    ret
