// Generated from CTGPDX.nro v1.1.1 (build id
// 9ea21ff4b8b8648b8a7459e72eacdf5b67abf614, ARM, loaded by Skyline next to 3.0.3 main).
// Offsets are PLUGIN-relative; the plugin base comes from the hook the plugin itself
// writes at main+0x451f38 (ldr pc,[pc,#-4] + target = plugin+0x28cb0).

// CtThunkCupMsg: main+0x451f38 function-hook target: push; bl 0x66ec; pop (anchor: plugin base = target - 0x28cb0)
//   0x28cb0: 00502de9  push {ip, lr}
//   0x28cb4: 8c76ffeb  bl #0x66ec
//   0x28cb8: 0090bde8  pop {ip, pc}
inline constexpr u32 PinCtThunkCupMsgWords[] = {0xe92d5000, 0xebff768c, 0xe8bd9000};
inline constexpr Pin PinCtThunkCupMsg{0x28cb0, PinCtThunkCupMsgWords, "CtThunkCupMsg"};

// CtCupMsg: cup name label: ctx = 0x1be0(); cups = [ctx+8]; cup = s8 [r4]; b 0x35e0
//   0x066ec: 10402de9  push {r4, lr}
//   0x066f0: 0040a0e1  mov r4, r0
//   0x066f4: 39edffeb  bl #0x1be0
//   0x066f8: 080090e5  ldr r0, [r0, #8]
//   0x066fc: d010d4e1  ldrsb r1, [r4]
//   0x06700: 1040bde8  pop {r4, lr}
//   0x06704: b5f3ffea  b #0x35e0
inline constexpr u32 PinCtCupMsgWords[] = {0xe92d4010, 0xe1a04000, 0xebffed39, 0xe5900008, 0xe1d410d0, 0xe8bd4010, 0xeafff3b5};
inline constexpr Pin PinCtCupMsg{0x66ec, PinCtCupMsgWords, "CtCupMsg"};

// CtCtx: ctx accessor: ldr lit 0x1c04; ldr lit 0x1c08; add pc; ldr [r3,r2] (GOT slot -> ctx object); literals included
//   0x01be0: 1c309fe5  ldr r3, [pc, #0x1c]
//   0x01be4: 08d04de2  sub sp, sp, #8
//   0x01be8: 18209fe5  ldr r2, [pc, #0x18]
//   0x01bec: 03308fe0  add r3, pc, r3
//   0x01bf0: 023093e7  ldr r3, [r3, r2]
//   0x01bf4: 0300a0e1  mov r0, r3
//   0x01bf8: 04308de5  str r3, [sp, #4]
//   0x01bfc: 08d08de2  add sp, sp, #8
//   0x01c00: 1eff2fe1  bx lr
//   0x01c04: 70350800  andeq r3, r8, r0, ror r5
//   0x01c08: b8050000  strheq r0, [r0], -r8
inline constexpr u32 PinCtCtxWords[] = {0xe59f301c, 0xe24dd008, 0xe59f2018, 0xe08f3003, 0xe7933002, 0xe1a00003, 0xe58d3004, 0xe28dd008, 0xe12fff1e, 0x00083570, 0x000005b8};
inline constexpr Pin PinCtCtx{0x1be0, PinCtCtxWords, "CtCtx"};

// CtCourseLookup: course entry of the race: [[ctx+0xc]] list, index = u32 RI+0x1a0 + 1, bl 0x9ab4
//   0x06724: f3412de9  push {r0, r1, r4, r5, r6, r7, r8, lr}
//   0x06728: 0040a0e1  mov r4, r0
//   0x0672c: ebefffeb  bl #0x26e0
//   0x06730: 0050a0e1  mov r5, r0
//   0x06734: a0609fe5  ldr r6, [pc, #0xa0]
//   0x06738: 28edffeb  bl #0x1be0
//   0x0673c: 0c3090e5  ldr r3, [r0, #0xc]
//   0x06740: a01195e5  ldr r1, [r5, #0x1a0]
//   0x06744: 06608fe0  add r6, pc, r6
//   0x06748: 000093e5  ldr r0, [r3]
//   0x0674c: 011081e2  add r1, r1, #1
//   0x06750: d70c00eb  bl #0x9ab4
inline constexpr u32 PinCtCourseLookupWords[] = {0xe92d41f3, 0xe1a04000, 0xebffefeb, 0xe1a05000, 0xe59f60a0, 0xebffed28, 0xe590300c, 0xe59511a0, 0xe08f6006, 0xe5930000, 0xe2811001, 0xeb000cd7};
inline constexpr Pin PinCtCourseLookup{0x6724, PinCtCourseLookupWords, "CtCourseLookup"};

// CtListCount: list {u32 count, vec*}: index < count, 0x9770(vec, i), entry = [slot]; +0x24 author
//   0x09ab4: 003090e5  ldr r3, [r0]
//   0x09ab8: 010053e1  cmp r3, r1
//   0x09abc: 0500009a  bls #0x9ad8
//   0x09ac0: 040090e5  ldr r0, [r0, #4]
//   0x09ac4: 10402de9  push {r4, lr}
//   0x09ac8: 28ffffeb  bl #0x9770
//   0x09acc: 003090e5  ldr r3, [r0]
//   0x09ad0: 240093e5  ldr r0, [r3, #0x24]
//   0x09ad4: 1080bde8  pop {r4, pc}
//   0x09ad8: 0000e0e3  mvn r0, #0
//   0x09adc: 1eff2fe1  bx lr
inline constexpr u32 PinCtListCountWords[] = {0xe5903000, 0xe1530001, 0x9a000005, 0xe5900004, 0xe92d4010, 0xebffff28, 0xe5903000, 0xe5930024, 0xe8bd8010, 0xe3e00000, 0xe12fff1e};
inline constexpr Pin PinCtListCount{0x9ab4, PinCtListCountWords, "CtListCount"};

// CtListIndex: vec {begin, end}: i < (end-begin)/4 -> begin + 4*i
//   0x09770: 04c090e5  ldr ip, [r0, #4]
//   0x09774: 10402de9  push {r4, lr}
//   0x09778: 00e090e5  ldr lr, [r0]
//   0x0977c: 0ec04ce0  sub ip, ip, lr
//   0x09780: 4c0151e1  cmp r1, ip, asr #2
//   0x09784: 0300003a  blo #0x9798
//   0x09788: 10009fe5  ldr r0, [pc, #0x10]
//   0x0978c: 4c21a0e1  asr r2, ip, #2
//   0x09790: 00008fe0  add r0, pc, r0
//   0x09794: dadb00eb  bl #0x40704
//   0x09798: 01018ee0  add r0, lr, r1, lsl #2
//   0x0979c: 1080bde8  pop {r4, pc}
inline constexpr u32 PinCtListIndexWords[] = {0xe590c004, 0xe92d4010, 0xe590e000, 0xe04cc00e, 0xe151014c, 0x3a000003, 0xe59f0010, 0xe1a0214c, 0xe08f0000, 0xeb00dbda, 0xe08e0101, 0xe8bd8010};
inline constexpr Pin PinCtListIndex{0x9770, PinCtListIndexWords, "CtListIndex"};

// CtEntryName: entry +0x18 = course name message label
//   0x09ae0: 003090e5  ldr r3, [r0]
//   0x09ae4: 010053e1  cmp r3, r1
//   0x09ae8: 0500009a  bls #0x9b04
//   0x09aec: 040090e5  ldr r0, [r0, #4]
//   0x09af0: 10402de9  push {r4, lr}
//   0x09af4: 1dffffeb  bl #0x9770
//   0x09af8: 003090e5  ldr r3, [r0]
//   0x09afc: 180093e5  ldr r0, [r3, #0x18]
//   0x09b00: 1080bde8  pop {r4, pc}
//   0x09b04: 0000e0e3  mvn r0, #0
//   0x09b08: 1eff2fe1  bx lr
inline constexpr u32 PinCtEntryNameWords[] = {0xe5903000, 0xe1530001, 0x9a000005, 0xe5900004, 0xe92d4010, 0xebffff1d, 0xe5903000, 0xe5930018, 0xe8bd8010, 0xe3e00000, 0xe12fff1e};
inline constexpr Pin PinCtEntryName{0x9ae0, PinCtEntryNameWords, "CtEntryName"};

// CtEntryCup: entry +0x33 = cup index (s8), 0 when out of range
//   0x09b80: 003090e5  ldr r3, [r0]
//   0x09b84: 010053e1  cmp r3, r1
//   0x09b88: 0500009a  bls #0x9ba4
//   0x09b8c: 040090e5  ldr r0, [r0, #4]
//   0x09b90: 10402de9  push {r4, lr}
//   0x09b94: f5feffeb  bl #0x9770
//   0x09b98: 003090e5  ldr r3, [r0]
//   0x09b9c: d303d3e1  ldrsb r0, [r3, #0x33]
//   0x09ba0: 1080bde8  pop {r4, pc}
//   0x09ba4: 0000a0e3  mov r0, #0
//   0x09ba8: 1eff2fe1  bx lr
inline constexpr u32 PinCtEntryCupWords[] = {0xe5903000, 0xe1530001, 0x9a000005, 0xe5900004, 0xe92d4010, 0xebfffef5, 0xe5903000, 0xe1d303d3, 0xe8bd8010, 0xe3a00000, 0xe12fff1e};
inline constexpr Pin PinCtEntryCup{0x9b80, PinCtEntryCupWords, "CtEntryCup"};

// CtEntryFolder: entry +0x00 = course folder char*
//   0x09bac: 003090e5  ldr r3, [r0]
//   0x09bb0: 10402de9  push {r4, lr}
//   0x09bb4: 040090e5  ldr r0, [r0, #4]
//   0x09bb8: 010053e1  cmp r3, r1
//   0x09bbc: 0010a093  movls r1, #0
//   0x09bc0: eafeffeb  bl #0x9770
//   0x09bc4: 003090e5  ldr r3, [r0]
//   0x09bc8: 000093e5  ldr r0, [r3]
//   0x09bcc: 1080bde8  pop {r4, pc}
inline constexpr u32 PinCtEntryFolderWords[] = {0xe5903000, 0xe92d4010, 0xe5900004, 0xe1530001, 0x93a01000, 0xebfffeea, 0xe5903000, 0xe5930000, 0xe8bd8010};
inline constexpr Pin PinCtEntryFolder{0x9bac, PinCtEntryFolderWords, "CtEntryFolder"};

// CtEntryPict: entry +0x04 = course picture base char*
//   0x09bd0: 003090e5  ldr r3, [r0]
//   0x09bd4: 10402de9  push {r4, lr}
//   0x09bd8: 040090e5  ldr r0, [r0, #4]
//   0x09bdc: 010053e1  cmp r3, r1
//   0x09be0: 0010a093  movls r1, #0
//   0x09be4: e1feffeb  bl #0x9770
//   0x09be8: 003090e5  ldr r3, [r0]
//   0x09bec: 040093e5  ldr r0, [r3, #4]
//   0x09bf0: 1080bde8  pop {r4, pc}
inline constexpr u32 PinCtEntryPictWords[] = {0xe5903000, 0xe92d4010, 0xe5900004, 0xe1530001, 0x93a01000, 0xebfffee1, 0xe5903000, 0xe5930004, 0xe8bd8010};
inline constexpr Pin PinCtEntryPict{0x9bd0, PinCtEntryPictWords, "CtEntryPict"};

// CtCupFind: cup lookup: map = [cups+8]; find(cup) -> node; value = [node+8]
//   0x0356c: f0412de9  push {r4, r5, r6, r7, r8, lr}
//   0x03570: 0050a0e1  mov r5, r0
//   0x03574: 0140a0e1  mov r4, r1
//   0x03578: 0260a0e1  mov r6, r2
//   0x0357c: 087090e5  ldr r7, [r0, #8]
//   0x03580: 0700a0e1  mov r0, r7
//   0x03584: e3ffffeb  bl #0x3518
//   0x03588: 000050e3  cmp r0, #0
//   0x0358c: 0800000a  beq #0x35b4
//   0x03590: 0410a0e1  mov r1, r4
//   0x03594: 0700a0e1  mov r0, r7
//   0x03598: deffffeb  bl #0x3518
//   0x0359c: 000050e3  cmp r0, #0
//   0x035a0: 08009015  ldrne r0, [r0, #8]
//   0x035a4: 0a00001a  bne #0x35d4
//   0x035a8: 2c009fe5  ldr r0, [pc, #0x2c]
//   0x035ac: 00008fe0  add r0, pc, r0
//   0x035b0: 3af400eb  bl #0x406a0
//   0x035b4: 145095e5  ldr r5, [r5, #0x14]
//   0x035b8: 0410a0e1  mov r1, r4
//   0x035bc: 0500a0e1  mov r0, r5
//   0x035c0: d4ffffeb  bl #0x3518
//   0x035c4: 000050e3  cmp r0, #0
//   0x035c8: 0410a011  movne r1, r4
//   0x035cc: 0500a011  movne r0, r5
//   0x035d0: f0ffff1a  bne #0x3598
//   0x035d4: 000086e5  str r0, [r6]
//   0x035d8: f081bde8  pop {r4, r5, r6, r7, r8, pc}
inline constexpr u32 PinCtCupFindWords[] = {0xe92d41f0, 0xe1a05000, 0xe1a04001, 0xe1a06002, 0xe5907008, 0xe1a00007, 0xebffffe3, 0xe3500000, 0x0a000008, 0xe1a01004, 0xe1a00007, 0xebffffde, 0xe3500000, 0x15900008, 0x1a00000a, 0xe59f002c, 0xe08f0000, 0xeb00f43a, 0xe5955014, 0xe1a01004, 0xe1a00005, 0xebffffd4, 0xe3500000, 0x11a01004, 0x11a00005, 0x1afffff0, 0xe5860000, 0xe8bd81f0};
inline constexpr Pin PinCtCupFind{0x356c, PinCtCupFindWords, "CtCupFind"};

// CtCupMapFind: map {buckets, bucket_count, first, size}: list walk from +8, node {next, s8 key +4, value +8}
//   0x03518: 0120a0e1  mov r2, r1
//   0x0351c: 0c1090e5  ldr r1, [r0, #0xc]
//   0x03520: 0030a0e1  mov r3, r0
//   0x03524: 000051e3  cmp r1, #0
//   0x03528: 08009005  ldreq r0, [r0, #8]
//   0x0352c: 0b00000a  beq #0x3560
//   0x03530: 10402de9  push {r4, lr}
//   0x03534: 043093e5  ldr r3, [r3, #4]
//   0x03538: 12f331e7  udiv r1, r2, r3
//   0x0353c: 932161e0  mls r1, r3, r1, r2
//   0x03540: 4bffffeb  bl #0x3274
//   0x03544: 000050e3  cmp r0, #0
//   0x03548: 00009015  ldrne r0, [r0]
//   0x0354c: 1080bde8  pop {r4, pc}
//   0x03550: d430d0e1  ldrsb r3, [r0, #4]
//   0x03554: 020053e1  cmp r3, r2
//   0x03558: 1eff2f01  bxeq lr
//   0x0355c: 000090e5  ldr r0, [r0]
//   0x03560: 000050e3  cmp r0, #0
//   0x03564: f9ffff1a  bne #0x3550
//   0x03568: 1eff2fe1  bx lr
inline constexpr u32 PinCtCupMapFindWords[] = {0xe1a02001, 0xe590100c, 0xe1a03000, 0xe3510000, 0x05900008, 0x0a00000b, 0xe92d4010, 0xe5933004, 0xe731f312, 0xe0612193, 0xebffff4b, 0xe3500000, 0x15900000, 0xe8bd8010, 0xe1d030d4, 0xe1530002, 0x012fff1e, 0xe5900000, 0xe3500000, 0x1afffff9, 0xe12fff1e};
inline constexpr Pin PinCtCupMapFind{0x3518, PinCtCupMapFindWords, "CtCupMapFind"};

// CtCupLabel: cup object +4 = cup name label; +8 = cup icon name char*
//   0x031f0: 040090e5  ldr r0, [r0, #4]
//   0x031f4: 1eff2fe1  bx lr
//   0x031f8: 080090e5  ldr r0, [r0, #8]
//   0x031fc: 1eff2fe1  bx lr
inline constexpr u32 PinCtCupLabelWords[] = {0xe5900004, 0xe12fff1e, 0xe5900008, 0xe12fff1e};
inline constexpr Pin PinCtCupLabel{0x31f0, PinCtCupLabelWords, "CtCupLabel"};
