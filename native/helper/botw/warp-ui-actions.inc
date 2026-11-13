// Exact 1.6.0 Travel (fast travel) to a native MapData shrine/tower record. World only.
// Native call: main+123C990(map, pos, 0) -- the routine the native menu-close path
// (1203730 -> 1203C74) runs with the Travel destination; strings are the record's own
// LocationMarker WarpDestMapName/WarpDestPosName (BYML data owned by the UI manager's
// marker list, lifetime of the session), never companion memory.
// Mailbox (op 13): 1024 MapData, 1032 record, 1040 index, 1044 kind (2 shrine / 3 tower /
// 42 Travel Medallion), 1048 marker entry (record+58; 0 for the medallion), 1056 map char*, 1064 pos char*, 1072 fnv(map),
// 1076 fnv(pos), 1080 event manager. Guest-owned: 1088 last committed beat,
// 1092 commit count. Result: 1 native latch observed, 2 refused (no native call),
// 3 native call made but the latch was not observed (uncertain: module stays blocked).
warp_request:
    ldr w9, [x19, #160]
    cbnz w9, refused
    sub x11, x11, #4
    cmp x10, x11
    b.ne refused
    // At most one Travel per 300 helper beats (frames), whatever the companion asks.
    ldr w9, [x19, #1092]
    cbz w9, warp_cooldown_ok
    ldr w9, [x19, #1088]
    sub w9, w17, w9
    cmp w9, #300
    b.lo refused
warp_cooldown_ok:
    // No active event (evt::Manager +1D2B8 = current event, the inlined hasActiveEvent).
    adrp x9, event_root
    ldr x9, [x9, :lo12:event_root]
    ldr x10, [x19, #1080]
    cmp x9, x10
    b.ne refused
    cbz x9, refused
    ldr x10, [x9]
    adrp x11, event_vtable
    add x11, x11, :lo12:event_vtable
    cmp x10, x11
    b.ne refused
    mov w10, #0xd2b8
    movk w10, #1, lsl #16
    ldr x10, [x9, x10]
    cbnz x10, refused
    // No Travel already latched by 123C990 ([[[2CD1590]+28]+98]+28).
    adrp x9, warp_latch_root
    ldr x9, [x9, :lo12:warp_latch_root]
    cbz x9, warp_latch_ok
    ldr x9, [x9, #0x28]
    cbz x9, warp_latch_ok
    ldr x9, [x9, #0x98]
    cbz x9, warp_latch_ok
    ldrb w10, [x9, #0x28]
    cbnz w10, refused
warp_latch_ok:
    // Neither native loading screen (ScreenMgr slots 69/78) open or requested
    // (the 1A43F20 predicate: state +FE != 0 or signed request +FD > 0).
    adrp x9, screen_root
    ldr x9, [x9, :lo12:screen_root]
    cbz x9, refused
    ldr w10, [x9, #0x28]
    cmp w10, #99
    b.ne refused
    ldr x9, [x9, #0x30]
    cbz x9, refused
    ldr x10, [x9, #552]
    cbz x10, warp_loading_b
    ldrb w11, [x10, #0xfe]
    cbnz w11, refused
    ldrsb w11, [x10, #0xfd]
    cmp w11, #0
    b.gt refused
warp_loading_b:
    ldr x10, [x9, #624]
    cbz x10, warp_loading_ok
    ldrb w11, [x10, #0xfe]
    cbnz w11, refused
    ldrsb w11, [x10, #0xfd]
    cmp w11, #0
    b.gt refused
warp_loading_ok:
    // Player present (123C990 latches without warping for a missing player).
    adrp x9, player_info_root
    ldr x9, [x9, :lo12:player_info_root]
    cbz x9, refused
    ldr x10, [x9]
    adrp x11, player_info_vtable
    add x11, x11, :lo12:player_info_vtable
    cmp x10, x11
    b.ne refused
    ldr x9, [x9, #0x60]
    cbz x9, refused
    ldr x10, [x9]
    adrp x11, player_vtable
    add x11, x11, :lo12:player_vtable
    cmp x10, x11
    b.ne refused
    // MapData record: exact singleton, array slot, class, kind, travel point enabled.
    adrp x9, map_root
    ldr x21, [x9, :lo12:map_root]
    ldr x9, [x19, #1024]
    cmp x21, x9
    b.ne refused
    cbz x21, refused
    ldr x9, [x21]
    adrp x10, map_vtable
    add x10, x10, :lo12:map_vtable
    cmp x9, x10
    b.ne refused
    ldr w12, [x19, #1044]
    cmp w12, #42
    b.eq warp_medallion
    cmp w12, #2
    b.eq warp_shrine_array
    cmp w12, #3
    b.ne refused
    add x9, x21, #0x310
    adrp x13, tower_vtable
    add x13, x13, :lo12:tower_vtable
    mov w14, #15
    b warp_array_ready
warp_shrine_array:
    add x9, x21, #0x2f0
    adrp x13, shrine_vtable
    add x13, x13, :lo12:shrine_vtable
    mov w14, #136
warp_array_ready:
    ldr w10, [x9]
    ldr w11, [x9, #4]
    cmp w10, w11
    b.gt refused
    cmp w11, w14
    b.hi refused
    ldr w24, [x19, #1040]
    cmp w24, w10
    b.hs refused
    ldr x9, [x9, #8]
    cbz x9, refused
    tst x9, #7
    b.ne refused
    ldr x21, [x9, x24, lsl #3]
    ldr x9, [x19, #1032]
    cmp x21, x9
    b.ne refused
    cbz x21, refused
    tst x21, #7
    b.ne refused
    ldr x9, [x21]
    cmp x9, x13
    b.ne refused
    ldr w9, [x21, #0x1c]
    cmp w9, w12
    b.ne refused
    // The value the native slot feeds to 109CC10 ("travel point enabled"): shrine +3C bit17
    // (Enter_, slot 109D960), tower +64 open byte (slot 109FAF0). The +3C bit9 result itself is
    // refreshed lazily (live save A: 49/135 set while 121 points are enabled), so it is not used.
    cmp w12, #3
    b.eq warp_tower_open
    ldr w9, [x21, #0x3c]
    tbz w9, #17, refused
    b warp_point_enabled
warp_tower_open:
    ldrb w9, [x21, #0x64]
    cmp w9, #1
    b.ne refused
warp_point_enabled:
    // Marker entry and its destination strings (exact pointers + bounded FNV names).
    ldr x23, [x21, #0x58]
    ldr x9, [x19, #1048]
    cmp x23, x9
    b.ne refused
    cbz x23, refused
    // LocationMarker pool entries are 4-byte aligned (live: ...c6c, ...cd4, ...d3c).
    tst x23, #3
    b.ne refused
    ldr x0, [x23, #0x28]
    ldr x9, [x19, #1056]
    cmp x0, x9
    b.ne refused
    cbz x0, refused
    ldrb w9, [x0]
    cbz w9, refused
    bl inventory_hash_name
    ldr w9, [x19, #1072]
    cmp w0, w9
    b.ne refused
    cbz w0, refused
    ldr x0, [x23, #0x30]
    ldr x9, [x19, #1064]
    cmp x0, x9
    b.ne refused
    cbz x0, refused
    ldrb w9, [x0]
    cbz w9, refused
    bl inventory_hash_name
    ldr w9, [x19, #1076]
    cmp w0, w9
    b.ne refused
    cbz w0, refused
warp_gates:
    // The game's own Travel prohibitions (GameData bools registered at 1049280:
    // WarpProhibition 2B7E9A8, WarpProhibitionArea 2B7E9AC,
    // WarpProhibitionArea_Fire_Relic 2B7E9B0), read through the native getter.
    adrp x9, warp_prohibition_handles
    add x24, x9, :lo12:warp_prohibition_handles
    ldr w0, [x24]
    .ifdef BOTW_190
    mov w1, wzr
    .endif
    bl native_flag_bool
    tbnz w0, #0, refused
    ldr w0, [x24, #4]
    .ifdef BOTW_190
    mov w1, wzr
    .endif
    bl native_flag_bool
    tbnz w0, #0, refused
    ldr w0, [x24, #8]
    .ifdef BOTW_190
    mov w1, wzr
    .endif
    bl native_flag_bool
    tbnz w0, #0, refused
    // Permit/sequence/epoch exactly as every other route, immediately before the call.
    add x9, x19, #8
    ldar w9, [x9]
    cmp w9, w20
    b.ne refused
    add x9, x19, #72
    ldar w9, [x9]
    cmp w9, w20
    b.ne refused
    add x9, x19, #4092
    ldar w9, [x9]
    ldr w10, [x19, #88]
    cmp w9, w10
    b.ne refused
    // Pointers are re-read after the native getters (UI thread; nothing frees them in
    // between, but keep the call bound to what was validated). The medallion path is chosen
    // by the validated record's own class, not by the mailbox kind again.
    ldr x9, [x21]
    adrp x10, medallion_vtable
    add x10, x10, :lo12:medallion_vtable
    cmp x9, x10
    b.eq warp_medallion_final
    ldr x9, [x21, #0x58]
    cmp x9, x23
    b.ne refused
    ldr x0, [x23, #0x28]
    ldr x9, [x19, #1056]
    cmp x0, x9
    b.ne refused
    ldr x1, [x23, #0x30]
    ldr x9, [x19, #1064]
    cmp x1, x9
    b.ne refused
warp_commit:
    ldr w17, [x19, #4]
    str w17, [x19, #1088]
    ldr w9, [x19, #1092]
    add w9, w9, #1
    str w9, [x19, #1092]
    mov x2, xzr
    bl native_warp
    // Acknowledge 1 only when the native one-shot latch is now set.
    adrp x9, warp_latch_root
    ldr x9, [x9, :lo12:warp_latch_root]
    cbz x9, warp_uncertain
    ldr x9, [x9, #0x28]
    cbz x9, warp_uncertain
    ldr x9, [x9, #0x98]
    cbz x9, warp_uncertain
    ldrb w10, [x9, #0x28]
    cbz w10, warp_uncertain
    b successful
warp_uncertain:
    mov w9, #3
    b acknowledge

// Travel Medallion (DLC "WarpDLC", MapData+480 class, native DLC Travel strings):
// the single MapData+480 record (class vtable 2B875E8, kind 42), placed = +3C bit4 (the bit
// the native map draws it by). The native map Travel producer's DLC branch (0x10F0074) hands
// 123C990 its own rodata destination, "MainField/WarpIcon" / "WarpPoint01", through the same
// ui-manager mailbox the shrine branch uses; we pass those same two pointers. x21 = MapData.
warp_medallion:
    ldr w9, [x19, #1040]
    cbnz w9, refused
    ldr x9, [x19, #1048]
    cbnz x9, refused
    mov x23, x21
    ldr x21, [x21, #0x480]
    ldr x9, [x19, #1032]
    cmp x21, x9
    b.ne refused
    cbz x21, refused
    tst x21, #7
    b.ne refused
    ldr x9, [x21]
    adrp x10, medallion_vtable
    add x10, x10, :lo12:medallion_vtable
    cmp x9, x10
    b.ne refused
    ldr w9, [x21, #0x1c]
    cmp w9, #42
    b.ne refused
    ldr w9, [x21, #0x3c]
    tbz w9, #4, refused
    adrp x0, medallion_map_name
    add x0, x0, :lo12:medallion_map_name
    ldr x9, [x19, #1056]
    cmp x0, x9
    b.ne refused
    bl inventory_hash_name
    ldr w9, [x19, #1072]
    cmp w0, w9
    b.ne refused
    cbz w0, refused
    adrp x0, medallion_pos_name
    add x0, x0, :lo12:medallion_pos_name
    ldr x9, [x19, #1064]
    cmp x0, x9
    b.ne refused
    bl inventory_hash_name
    ldr w9, [x19, #1076]
    cmp w0, w9
    b.ne refused
    cbz w0, refused
    b warp_gates
// After the gates: x23 = MapData, x21 = the medallion record. Still the singleton's record,
// still placed, still the native strings the mailbox named.
warp_medallion_final:
    adrp x9, map_root
    ldr x9, [x9, :lo12:map_root]
    cmp x9, x23
    b.ne refused
    ldr x9, [x23, #0x480]
    cmp x9, x21
    b.ne refused
    ldr w9, [x21, #0x1c]
    cmp w9, #42
    b.ne refused
    ldr w9, [x21, #0x3c]
    tbz w9, #4, refused
    adrp x0, medallion_map_name
    add x0, x0, :lo12:medallion_map_name
    ldr x9, [x19, #1056]
    cmp x0, x9
    b.ne refused
    adrp x1, medallion_pos_name
    add x1, x1, :lo12:medallion_pos_name
    ldr x9, [x19, #1064]
    cmp x1, x9
    b.ne refused
    b warp_commit
