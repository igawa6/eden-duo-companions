// Generated from the 3.0.3 main (build 6A85262F21B90364,
// AArch32 ARM mode, 4-byte pointers). Each pin: main offset + the exact instruction words the
// reader requires before it uses the offsets those instructions encode. Disassembly quoted for
// review. Globals are PC-relative literal-pool loads: cell = (ldr/add insn address + 8) + literal;
// the pins that resolve a global include the literal word.

// EngineHolder: root cell GOT (ldr lit; ldr [pc,r0]) -> [[cell]] = root; E = [root+0x14] (lit word included)
//   0x87a110: 0c009fe5  ldr r0, [pc, #0xc]   ; lit[0x87a124]=0x698b4c
//   0x87a114: 00009fe7  ldr r0, [pc, r0]   ; -> 0xf12c68 (got->0xf50b1c)
//   0x87a118: 000090e5  ldr r0, [r0]
//   0x87a11c: 140090e5  ldr r0, [r0, #0x14]
//   0x87a120: 1eff2fe1  bx lr
//   0x87a124: 4c8b6900  rsbeq r8, sb, ip, asr #22
inline constexpr u32 PinEngineHolderWords[] = {0xe59f000c, 0xe79f0000, 0xe5900000, 0xe5900014, 0xe12fff1e, 0x00698b4c};
inline constexpr Pin PinEngineHolder{0x87a110, PinEngineHolderWords, "EngineHolder"};

// RaceInfoHolder: root cell GOT -> root; [[root+0x14]+0x140] (lit word included)
//   0x87a144: 10009fe5  ldr r0, [pc, #0x10]   ; lit[0x87a15c]=0x698b18
//   0x87a148: 00009fe7  ldr r0, [pc, r0]   ; -> 0xf12c68 (got->0xf50b1c)
//   0x87a14c: 000090e5  ldr r0, [r0]
//   0x87a150: 140090e5  ldr r0, [r0, #0x14]
//   0x87a154: 400190e5  ldr r0, [r0, #0x140]
//   0x87a158: 1eff2fe1  bx lr
//   0x87a15c: 188b6900  rsbeq r8, sb, r8, lsl fp
inline constexpr u32 PinRaceInfoHolderWords[] = {0xe59f0010, 0xe79f0000, 0xe5900000, 0xe5900014, 0xe5900140, 0xe12fff1e, 0x00698b18};
inline constexpr Pin PinRaceInfoHolder{0x87a144, PinRaceInfoHolderWords, "RaceInfoHolder"};

// RaceInfo: RI = [holder+0x08]
//   0x906fb4: 00482de9  push {fp, lr}
//   0x906fb8: 61ccfdeb  bl #0x87a144
//   0x906fbc: 080090e5  ldr r0, [r0, #8]
//   0x906fc0: 0088bde8  pop {fp, pc}
inline constexpr u32 PinRaceInfoWords[] = {0xe92d4800, 0xebfdcc61, 0xe5900008, 0xe8bd8800};
inline constexpr Pin PinRaceInfo{0x906fb4, PinRaceInfoWords, "RaceInfo"};

// ActiveScene: scene = [[root+0x10]+4], active if u8 +0xd4 bit 2 (lit word included)
//   0x87a17c: 1c009fe5  ldr r0, [pc, #0x1c]   ; lit[0x87a1a0]=0x698ae0
//   0x87a180: 00009fe7  ldr r0, [pc, r0]   ; -> 0xf12c68 (got->0xf50b1c)
//   0x87a184: 000090e5  ldr r0, [r0]
//   0x87a188: 100090e5  ldr r0, [r0, #0x10]
//   0x87a18c: 040090e5  ldr r0, [r0, #4]
//   0x87a190: d410d0e5  ldrb r1, [r0, #0xd4]
//   0x87a194: 811ea0e1  lsl r1, r1, #0x1d
//   0x87a198: c10f00e0  and r0, r0, r1, asr #31
//   0x87a19c: 1eff2fe1  bx lr
//   0x87a1a0: e08a6900  rsbeq r8, sb, r0, ror #21
inline constexpr u32 PinActiveSceneWords[] = {0xe59f001c, 0xe79f0000, 0xe5900000, 0xe5900010, 0xe5900004, 0xe5d010d4, 0xe1a01e81, 0xe0000fc1, 0xe12fff1e, 0x00698ae0};
inline constexpr Pin PinActiveScene{0x87a17c, PinActiveSceneWords, "ActiveScene"};

// SceneKind: scene kind u32 +0xd8 when u32 +0xd4 & 4 (lit word included)
//   0x87a1a4: 2c109fe5  ldr r1, [pc, #0x2c]   ; lit[0x87a1d8]=0x698ab8
//   0x87a1a8: 01109fe7  ldr r1, [pc, r1]   ; -> 0xf12c68 (got->0xf50b1c)
//   0x87a1ac: 001091e5  ldr r1, [r1]
//   0x87a1b0: 101091e5  ldr r1, [r1, #0x10]
//   0x87a1b4: 042091e5  ldr r2, [r1, #4]
//   0x87a1b8: 0010a0e3  mov r1, #0
//   0x87a1bc: 000052e3  cmp r2, #0
//   0x87a1c0: 0200000a  beq #0x87a1d0
//   0x87a1c4: d43092e5  ldr r3, [r2, #0xd4]
//   0x87a1c8: 043013e2  ands r3, r3, #4
//   0x87a1cc: d8109215  ldrne r1, [r2, #0xd8]
//   0x87a1d0: 001080e5  str r1, [r0]
//   0x87a1d4: 1eff2fe1  bx lr
//   0x87a1d8: b88a6900  strhteq r8, [sb], #-0xa8
inline constexpr u32 PinSceneKindWords[] = {0xe59f102c, 0xe79f1001, 0xe5911000, 0xe5911010, 0xe5912004, 0xe3a01000, 0xe3520000, 0x0a000002, 0xe59230d4, 0xe2133004, 0x159210d8, 0xe5801000, 0xe12fff1e, 0x00698ab8};
inline constexpr Pin PinSceneKind{0x87a1a4, PinSceneKindWords, "SceneKind"};

// Phase: OE = [scene+0xe0+4*4], RD = [OE+0x11c], SEQ = [RD+0x2c], phase u32 SEQ+0x1c (==5 predicate)
//   0x905ea0: 10402de9  push {r4, lr}
//   0x905ea4: 08d04de2  sub sp, sp, #8
//   0x905ea8: b3d0fdeb  bl #0x87a17c
//   0x905eac: 000050e3  cmp r0, #0
//   0x905eb0: 1500000a  beq #0x905f0c
//   0x905eb4: 0440a0e3  mov r4, #4
//   0x905eb8: 00408de5  str r4, [sp]
//   0x905ebc: 00009de5  ldr r0, [sp]
//   0x905ec0: add0fdeb  bl #0x87a17c
//   0x905ec4: 0010a0e1  mov r1, r0
//   0x905ec8: 0000a0e3  mov r0, #0
//   0x905ecc: 0020a0e3  mov r2, #0
//   0x905ed0: 000051e3  cmp r1, #0
//   0x905ed4: 0500000a  beq #0x905ef0
//   0x905ed8: 04408de5  str r4, [sp, #4]
//   0x905edc: 04209de5  ldr r2, [sp, #4]
//   0x905ee0: 0a0052e3  cmp r2, #0xa
//   0x905ee4: 0020a023  movhs r2, #0
//   0x905ee8: 021181e0  add r1, r1, r2, lsl #2
//   0x905eec: e02091e5  ldr r2, [r1, #0xe0]
//   0x905ef0: 1c1192e5  ldr r1, [r2, #0x11c]
//   0x905ef4: 2c1091e5  ldr r1, [r1, #0x2c]
//   0x905ef8: 1c1091e5  ldr r1, [r1, #0x1c]
//   0x905efc: 050051e3  cmp r1, #5
//   0x905f00: 01000003  movweq r0, #1
//   0x905f04: 08d08de2  add sp, sp, #8
//   0x905f08: 1080bde8  pop {r4, pc}
inline constexpr u32 PinPhaseWords[] = {0xe92d4010, 0xe24dd008, 0xebfdd0b3, 0xe3500000, 0x0a000015, 0xe3a04004, 0xe58d4000, 0xe59d0000, 0xebfdd0ad, 0xe1a01000, 0xe3a00000, 0xe3a02000, 0xe3510000, 0x0a000005, 0xe58d4004, 0xe59d2004, 0xe352000a, 0x23a02000, 0xe0811102, 0xe59120e0, 0xe592111c, 0xe591102c, 0xe591101c, 0xe3510005, 0x03000001, 0xe28dd008, 0xe8bd8010};
inline constexpr Pin PinPhase{0x905ea0, PinPhaseWords, "Phase"};

// EngineClass: cc = 50*u32 RI+0x10 (+50)
//   0x556ab8: 3dc10eeb  bl #0x906fb4
//   0x556abc: 084097e5  ldr r4, [r7, #8]
//   0x556ac0: 100090e5  ldr r0, [r0, #0x10]
//   0x556ac4: 3210a0e3  mov r1, #0x32
//   0x556ac8: 900100e0  mul r0, r0, r1
inline constexpr u32 PinEngineClassWords[] = {0xeb0ec13d, 0xe5974008, 0xe5900010, 0xe3a01032, 0xe0000190};
inline constexpr Pin PinEngineClass{0x556ab8, PinEngineClassWords, "EngineClass"};

// Mirror: mirror u8 RI+0x26
//   0x551644: 5ad60eeb  bl #0x906fb4
//   0x551648: 2600d0e5  ldrb r0, [r0, #0x26]
//   0x55164c: 000050e3  cmp r0, #0
inline constexpr u32 PinMirrorWords[] = {0xeb0ed65a, 0xe5d00026, 0xe3500000};
inline constexpr Pin PinMirror{0x551644, PinMirrorWords, "Mirror"};

// MapYScale: course u32 RI+0x1a0 == 0x44 -> Y scale
//   0x5537a0: 03ce0eeb  bl #0x906fb4
//   0x5537a4: a00190e5  ldr r0, [r0, #0x1a0]
//   0x5537a8: 440050e3  cmp r0, #0x44
inline constexpr u32 PinMapYScaleWords[] = {0xeb0ece03, 0xe59001a0, 0xe3500044};
inline constexpr Pin PinMapYScale{0x5537a0, PinMapYScaleWords, "MapYScale"};

// MapYScaleMul: Y' = (Y + lit[0x553984]) * lit[0x553988] (vldr pc-rel, vadd, vmul)
//   0x5537b8: 710a9fed  vldr s0, [pc, #0x1c4]
//   0x5537bc: 712a9fed  vldr s4, [pc, #0x1c4]
//   0x5537c0: 070083e8  stm r3, {r0, r1, r2}
//   0x5537c4: 670f84e2  add r0, r4, #0x19c
//   0x5537c8: 863f84e2  add r3, r4, #0x218
//   0x5537cc: 18108de2  add r1, sp, #0x18
//   0x5537d0: 08208de2  add r2, sp, #8
//   0x5537d4: 031a9ded  vldr s2, [sp, #0xc]
//   0x5537d8: 00008de5  str r0, [sp]
//   0x5537dc: 070d84e2  add r0, r4, #0x1c0
//   0x5537e0: 000a31ee  vadd.f32 s0, s2, s0
//   0x5537e4: 020a20ee  vmul.f32 s0, s0, s4
//   0x5537e8: 030a8ded  vstr s0, [sp, #0xc]
inline constexpr u32 PinMapYScaleMulWords[] = {0xed9f0a71, 0xed9f2a71, 0xe8830007, 0xe2840f67, 0xe2843f86, 0xe28d1018, 0xe28d2008, 0xed9d1a03, 0xe58d0000, 0xe2840d07, 0xee310a00, 0xee200a02, 0xed8d0a03};
inline constexpr Pin PinMapYScaleMul{0x5537b8, PinMapYScaleMulWords, "MapYScaleMul"};

// MapYScaleConst: literal pool: 0.0 (Y add), 6.2 (Y mul), 186.0 (non-TV pane x offset)
//   0x553984: 00000000  andeq r0, r0, r0
//   0x553988: 6666c640  sbcmi r6, r6, r6, ror #12
//   0x55398c: 00003a43  teqmi sl, #0
inline constexpr u32 PinMapYScaleConstWords[] = {0x00000000, 0x40c66666, 0x433a0000};
inline constexpr Pin PinMapYScaleConst{0x553984, PinMapYScaleConstWords, "MapYScaleConst"};

// MapProjection: viewport 186,0,666,480 at map+0x1a0..0x1ac; ortho w/h = f[9]/f[10]; mirror swaps left/right (doc pin)
//   0x551748: 000000e3  movw r0, #0
//   0x55174c: 0010a0e3  mov r1, #0
//   0x551750: 00aabeee  vmov.f32 s20, #-5.000000e-01
//   0x551754: 10aa00ee  vmov s0, sl
//   0x551758: 00bab6ee  vmov.f32 s22, #5.000000e-01
//   0x55175c: 109a0cee  vmov s24, sb
//   0x551760: 865f84e2  add r5, r4, #0x218
//   0x551764: 3a0344e3  movt r0, #0x433a
//   0x551768: a00184e5  str r0, [r4, #0x1a0]
//   0x55176c: a41184e5  str r1, [r4, #0x1a4]
//   0x551770: 001008e3  movw r1, #0x8000
//   0x551774: b60880e2  add r0, r0, #0xb60000
//   0x551778: 0a8a20ee  vmul.f32 s16, s0, s20
//   0x55177c: 0b9a20ee  vmul.f32 s18, s0, s22
//   0x551780: 261444e3  movt r1, #0x4426
//   0x551784: a81184e5  str r1, [r4, #0x1a8]
//   0x551788: 001004e3  movw r1, #0x4000
//   0x55178c: ac0184e5  str r0, [r4, #0x1ac]
//   0x551790: fe05a0e3  mov r0, #0x3f800000
//   0x551794: 1c1644e3  movt r1, #0x461c
//   0x551798: ac0284e5  str r0, [r4, #0x2ac]
//   0x55179c: 0100a0e3  mov r0, #1
//   0x5517a0: b01284e5  str r1, [r4, #0x2b0]
//   0x5517a4: 1c02c4e5  strb r0, [r4, #0x21c]
//   0x5517a8: 01d60eeb  bl #0x906fb4
//   0x5517ac: 2600d0e5  ldrb r0, [r0, #0x26]
//   0x5517b0: 000050e3  cmp r0, #0
//   0x5517b4: 0200000a  beq #0x5517c4
//   0x5517b8: 0b1a2cee  vmul.f32 s2, s24, s22
//   0x5517bc: 0a1a6cee  vmul.f32 s3, s24, s20
//   0x5517c0: 010000ea  b #0x5517cc
//   0x5517c4: 0a1a2cee  vmul.f32 s2, s24, s20
//   0x5517c8: 0b1a6cee  vmul.f32 s3, s24, s22
//   0x5517cc: 490ab0ee  vmov.f32 s0, s18
//   0x5517d0: 480af0ee  vmov.f32 s1, s16
//   0x5517d4: 0500a0e1  mov r0, r5
//   0x5517d8: 66f104eb  bl #0x68dd78
inline constexpr u32 PinMapProjectionWords[] = {0xe3000000, 0xe3a01000, 0xeebeaa00, 0xee00aa10, 0xeeb6ba00, 0xee0c9a10, 0xe2845f86, 0xe344033a, 0xe58401a0, 0xe58411a4, 0xe3081000, 0xe28008b6, 0xee208a0a, 0xee209a0b, 0xe3441426, 0xe58411a8, 0xe3041000, 0xe58401ac, 0xe3a005fe, 0xe344161c, 0xe58402ac, 0xe3a00001, 0xe58412b0, 0xe5c4021c, 0xeb0ed601, 0xe5d00026, 0xe3500000, 0x0a000002, 0xee2c1a0b, 0xee6c1a0a, 0xea000001, 0xee2c1a0a, 0xee6c1a0b, 0xeeb00a49, 0xeef00a48, 0xe1a00005, 0xeb04f166};
inline constexpr Pin PinMapProjection{0x551748, PinMapProjectionWords, "MapProjection"};

// RaceCount: racer count s32 RI+0x180
//   0x57ff3c: 80a190e5  ldr sl, [r0, #0x180]
inline constexpr u32 PinRaceCountWords[] = {0xe590a180};
inline constexpr Pin PinRaceCount{0x57ff3c, PinRaceCountWords, "RaceCount"};

// RaceRule: rule u32 RI+0x08
//   0x55d08c: c8a70eeb  bl #0x906fb4
//   0x55d090: 080090e5  ldr r0, [r0, #8]
inline constexpr u32 PinRaceRuleWords[] = {0xeb0ea7c8, 0xe5900008};
inline constexpr Pin PinRaceRule{0x55d08c, PinRaceRuleWords, "RaceRule"};

// LapsTotal: LM = [RD+0x30], laps u8 LM+0x34 (3 when LM null)
//   0x55d064: 1c0190e5  ldr r0, [r0, #0x11c]
//   0x55d068: 300090e5  ldr r0, [r0, #0x30]
//   0x55d06c: 000050e3  cmp r0, #0
//   0x55d070: 0100000a  beq #0x55d07c
//   0x55d074: 3400d0e5  ldrb r0, [r0, #0x34]
inline constexpr u32 PinLapsTotalWords[] = {0xe590011c, 0xe5900030, 0xe3500000, 0x0a000001, 0xe5d00034};
inline constexpr Pin PinLapsTotal{0x55d064, PinLapsTotalWords, "LapsTotal"};

// Timer: RD = hud+0x64, TM = [RD+0xac], time struct TM+0x20
//   0x5602ac: 640094e5  ldr r0, [r4, #0x64]
//   0x5602b0: 704094e5  ldr r4, [r4, #0x70]
//   0x5602b4: 04508de2  add r5, sp, #4
//   0x5602b8: 0020a0e3  mov r2, #0
//   0x5602bc: ac0090e5  ldr r0, [r0, #0xac]
//   0x5602c0: 201080e2  add r1, r0, #0x20
inline constexpr u32 PinTimerWords[] = {0xe5940064, 0xe5944070, 0xe28d5004, 0xe3a02000, 0xe59000ac, 0xe2801020};
inline constexpr Pin PinTimer{0x5602ac, PinTimerWords, "Timer"};

// TimerMin: u8 T+4 minutes
//   0x5603c4: 0410d9e5  ldrb r1, [sb, #4]
inline constexpr u32 PinTimerMinWords[] = {0xe5d91004};
inline constexpr Pin PinTimerMin{0x5603c4, PinTimerMinWords, "TimerMin"};

// TimerSec: u8 T+5 seconds
//   0x5603cc: 0510d9e5  ldrb r1, [sb, #5]
inline constexpr u32 PinTimerSecWords[] = {0xe5d91005};
inline constexpr Pin PinTimerSec{0x5603cc, PinTimerSecWords, "TimerSec"};

// TimerMs: u16 T+6 milliseconds
//   0x560584: b610d9e1  ldrh r1, [sb, #6]
inline constexpr u32 PinTimerMsWords[] = {0xe1d910b6};
inline constexpr Pin PinTimerMs{0x560584, PinTimerMsWords, "TimerMs"};

// Checkers: count u32 RD+0x34, CK[i] = [[RD+0x3c]+4i]; rank u16 CK+0x24
//   0x55df98: 1c1190e5  ldr r1, [r0, #0x11c]
//   0x55df9c: f80094e5  ldr r0, [r4, #0xf8]
//   0x55dfa0: 342091e5  ldr r2, [r1, #0x34]
//   0x55dfa4: 0030a0e3  mov r3, #0
//   0x55dfa8: 0050a0e3  mov r5, #0
//   0x55dfac: 000052e1  cmp r2, r0
//   0x55dfb0: 0020a0e3  mov r2, #0
//   0x55dfb4: 01200083  movwhi r2, #1
//   0x55dfb8: 013000c3  movwgt r3, #1
//   0x55dfbc: 020013e1  tst r3, r2
//   0x55dfc0: 0020a0e3  mov r2, #0
//   0x55dfc4: 0100000a  beq #0x55dfd0
//   0x55dfc8: 3c1091e5  ldr r1, [r1, #0x3c]
//   0x55dfcc: 002191e7  ldr r2, [r1, r0, lsl #2]
//   0x55dfd0: dc2084e5  str r2, [r4, #0xdc]
//   0x55dfd4: b402d2e1  ldrh r0, [r2, #0x24]
inline constexpr u32 PinCheckersWords[] = {0xe590111c, 0xe59400f8, 0xe5912034, 0xe3a03000, 0xe3a05000, 0xe1520000, 0xe3a02000, 0x83002001, 0xc3003001, 0xe1130002, 0xe3a02000, 0x0a000001, 0xe591103c, 0xe7912100, 0xe58420dc, 0xe1d202b4};
inline constexpr Pin PinCheckers{0x55df98, PinCheckersWords, "Checkers"};

// CheckerRank: rank u16 CK+0x24
//   0x55dfd4: b402d2e1  ldrh r0, [r2, #0x24]
inline constexpr u32 PinCheckerRankWords[] = {0xe1d202b4};
inline constexpr Pin PinCheckerRank{0x55dfd4, PinCheckerRankWords, "CheckerRank"};

// CheckerCoins: coins u8 CK+0x34 (hud+0x64 = CK)
//   0x55d330: 641094e5  ldr r1, [r4, #0x64]
//   0x55d334: 7000d4e5  ldrb r0, [r4, #0x70]
//   0x55d338: 3450d1e5  ldrb r5, [r1, #0x34]
inline constexpr u32 PinCheckerCoinsWords[] = {0xe5941064, 0xe5d40070, 0xe5d15034};
inline constexpr Pin PinCheckerCoins{0x55d330, PinCheckerCoinsWords, "CheckerCoins"};

// CheckerLap: lap u32 CK+0x28, shown min(v+1, total)
//   0x55d384: 640094e5  ldr r0, [r4, #0x64]
//   0x55d388: 7210d4e5  ldrb r1, [r4, #0x72]
//   0x55d38c: 280090e5  ldr r0, [r0, #0x28]
//   0x55d390: 010080e2  add r0, r0, #1
//   0x55d394: 7020efe6  uxtb r2, r0
//   0x55d398: 010052e1  cmp r2, r1
//   0x55d39c: 7120d4e5  ldrb r2, [r4, #0x71]
//   0x55d3a0: 0100a081  movhi r0, r1
inline constexpr u32 PinCheckerLapWords[] = {0xe5940064, 0xe5d41072, 0xe5900028, 0xe2800001, 0xe6ef2070, 0xe1520001, 0xe5d42071, 0x81a00001};
inline constexpr Pin PinCheckerLap{0x55d384, PinCheckerLapWords, "CheckerLap"};

// CheckerGoalCheck: checker update: u32 CK+0x28 lap >= u8 LM+0x34 -> goal path
//   0x90863c: 283094e5  ldr r3, [r4, #0x28]
//   0x908640: 30a092e5  ldr sl, [r2, #0x30]
//   0x908644: 20209ae5  ldr r2, [sl, #0x20]
//   0x908648: 24709ae5  ldr r7, [sl, #0x24]
//   0x90864c: 000052e1  cmp r2, r0
//   0x908650: 90782780  mlahi r7, r0, r8, r7
//   0x908654: d002d7e1  ldrsb r0, [r7, #0x20]
//   0x908658: 000050e3  cmp r0, #0
//   0x90865c: 0010a0c1  movgt r1, r0
//   0x908660: 3400dae5  ldrb r0, [sl, #0x34]
//   0x908664: 000053e1  cmp r3, r0
//   0x908668: 6a0000aa  bge #0x908818
inline constexpr u32 PinCheckerGoalCheckWords[] = {0xe5943028, 0xe592a030, 0xe59a2020, 0xe59a7024, 0xe1520000, 0x80277890, 0xe1d702d0, 0xe3500000, 0xc1a01000, 0xe5da0034, 0xe1530000, 0xaa00006a};
inline constexpr Pin PinCheckerGoalCheck{0x90863c, PinCheckerGoalCheckWords, "CheckerGoalCheck"};

// CheckerGoalSet: goal (main+0x908038): u32 CK+0x20 |= 1
//   0x90810c: 200094e5  ldr r0, [r4, #0x20]
//   0x908110: 010080e3  orr r0, r0, #1
//   0x908114: 200084e5  str r0, [r4, #0x20]
inline constexpr u32 PinCheckerGoalSetWords[] = {0xe5940020, 0xe3800001, 0xe5840020};
inline constexpr Pin PinCheckerGoalSet{0x90810c, PinCheckerGoalSetWords, "CheckerGoalSet"};

// CheckerGoalDone: after goal (main+0x908128): u32 CK+0x20 |= 4
//   0x908168: 201094e5  ldr r1, [r4, #0x20]
//   0x90816c: 041081e3  orr r1, r1, #4
//   0x908170: 201084e5  str r1, [r4, #0x20]
inline constexpr u32 PinCheckerGoalDoneWords[] = {0xe5941020, 0xe3811004, 0xe5841020};
inline constexpr Pin PinCheckerGoalDone{0x908168, PinCheckerGoalDoneWords, "CheckerGoalDone"};

// DriverEntryBase: entry cursor = RI+0x3c (driver field of entry 0)
//   0x58004c: 3c4080e2  add r4, r0, #0x3c
inline constexpr u32 PinDriverEntryBaseWords[] = {0xe280403c};
inline constexpr Pin PinDriverEntryBase{0x58004c, PinDriverEntryBaseWords, "DriverEntryBase"};

// DriverEntry: driver u32 [cursor] (RI+0x3c+0x1c*i)
//   0x580168: 000094e5  ldr r0, [r4]
inline constexpr u32 PinDriverEntryWords[] = {0xe5940000};
inline constexpr Pin PinDriverEntry{0x580168, PinDriverEntryWords, "DriverEntry"};

// DriverVariant: variant u8 [cursor+4] (RI+0x40+0x1c*i)
//   0x580178: 0430d4e5  ldrb r3, [r4, #4]
inline constexpr u32 PinDriverVariantWords[] = {0xe5d43004};
inline constexpr Pin PinDriverVariant{0x580178, PinDriverVariantWords, "DriverVariant"};

// DriverEntryStride: loop i+1, cursor += 0x1c, i < 12
//   0x5802cc: 015085e2  add r5, r5, #1
//   0x5802d0: 1c4084e2  add r4, r4, #0x1c
//   0x5802d4: 0c0055e3  cmp r5, #0xc
inline constexpr u32 PinDriverEntryStrideWords[] = {0xe2855001, 0xe284401c, 0xe355000c};
inline constexpr Pin PinDriverEntryStride{0x5802cc, PinDriverEntryStrideWords, "DriverEntryStride"};

// KartDirector: KD = [OE+0x12c], kart count s32 KD+0x64
//   0x551c6c: 2c0190e5  ldr r0, [r0, #0x12c]
//   0x551c70: 648090e5  ldr r8, [r0, #0x64]
//   0x551c74: 010058e3  cmp r8, #1
inline constexpr u32 PinKartDirectorWords[] = {0xe590012c, 0xe5908064, 0xe3580001};
inline constexpr Pin PinKartDirector{0x551c6c, PinKartDirectorWords, "KartDirector"};

// KartUnit: KU[i] = [KD+0x2c+4i], i < 12
//   0x551cd4: 2c1191e5  ldr r1, [r1, #0x12c]
//   0x551cd8: 0b0190e7  ldr r0, [r0, fp, lsl #2]
//   0x551cdc: 640090e5  ldr r0, [r0, #0x64]
//   0x551ce0: 0c0050e3  cmp r0, #0xc
//   0x551ce4: 0400a021  movhs r0, r4
//   0x551ce8: 000181e0  add r0, r1, r0, lsl #2
//   0x551cec: 2c6090e5  ldr r6, [r0, #0x2c]
inline constexpr u32 PinKartUnitWords[] = {0xe591112c, 0xe790010b, 0xe5900064, 0xe350000c, 0x21a00004, 0xe0810100, 0xe590602c};
inline constexpr Pin PinKartUnit{0x551cd4, PinKartUnitWords, "KartUnit"};

// KartUnitPlayer: KU vtable slot 0: V = [KU+0x24], player id u32 V+0x54
//   0x158838: 240090e5  ldr r0, [r0, #0x24]
//   0x15883c: 540090e5  ldr r0, [r0, #0x54]
//   0x158840: 1eff2fe1  bx lr
inline constexpr u32 PinKartUnitPlayerWords[] = {0xe5900024, 0xe5900054, 0xe12fff1e};
inline constexpr Pin PinKartUnitPlayer{0x158838, PinKartUnitPlayerWords, "KartUnitPlayer"};

// KartUnitPos: KU vtable slot 1: slot3() + 0x24
//   0x158844: 00482de9  push {fp, lr}
//   0x158848: 001090e5  ldr r1, [r0]
//   0x15884c: 0c1091e5  ldr r1, [r1, #0xc]
//   0x158850: 31ff2fe1  blx r1
//   0x158854: 240080e2  add r0, r0, #0x24
//   0x158858: 0088bde8  pop {fp, pc}
inline constexpr u32 PinKartUnitPosWords[] = {0xe92d4800, 0xe5901000, 0xe591100c, 0xe12fff31, 0xe2800024, 0xe8bd8800};
inline constexpr Pin PinKartUnitPos{0x158844, PinKartUnitPosWords, "KartUnitPos"};

// KartUnitMove: KU vtable slot 3: [KU+0x28] + 4
//   0x158870: 280090e5  ldr r0, [r0, #0x28]
//   0x158874: 040080e2  add r0, r0, #4
//   0x158878: 1eff2fe1  bx lr
inline constexpr u32 PinKartUnitMoveWords[] = {0xe5900028, 0xe2800004, 0xe12fff1e};
inline constexpr Pin PinKartUnitMove{0x158870, PinKartUnitMoveWords, "KartUnitMove"};

// ItemDirector: ID = [OE+0x130], owners [[ID+0x7c]+4i], i < u32 ID+0x74
//   0x55b474: 301190e5  ldr r1, [r0, #0x130]
//   0x55b478: 680094e5  ldr r0, [r4, #0x68]
//   0x55b47c: 743091e5  ldr r3, [r1, #0x74]
//   0x55b480: 0020a0e3  mov r2, #0
//   0x55b484: 000053e1  cmp r3, r0
//   0x55b488: 0100009a  bls #0x55b494
//   0x55b48c: 7c1091e5  ldr r1, [r1, #0x7c]
//   0x55b490: 002191e7  ldr r2, [r1, r0, lsl #2]
//   0x55b494: 642084e5  str r2, [r4, #0x64]
inline constexpr u32 PinItemDirectorWords[] = {0xe5901130, 0xe5940068, 0xe5913074, 0xe3a02000, 0xe1530000, 0x9a000001, 0xe591107c, 0xe7912100, 0xe5842064};
inline constexpr Pin PinItemDirector{0x55b474, PinItemDirectorWords, "ItemDirector"};

// ItemOwnerCreate: owners: for i < RI+0x180: owner = new(0x100) Owner(i); append to [ID+0x7c] (count ID+0x74, cap +0x78)
//   0x002ea4: 421024eb  bl #0x906fb4
//   0x002ea8: 805190e5  ldr r5, [r0, #0x180]
//   0x002eac: 740084e2  add r0, r4, #0x74
//   0x002eb0: 0020a0e3  mov r2, #0
//   0x002eb4: 0430a0e3  mov r3, #4
//   0x002eb8: 0510a0e1  mov r1, r5
//   0x002ebc: 03a219eb  bl #0x66b6d0
//   0x002ec0: 010055e3  cmp r5, #1
//   0x002ec4: 130000ba  blt #0x2f18
//   0x002ec8: 010ca0e3  mov r0, #0x100
//   0x002ecc: d09f19eb  bl #0x66ae14
//   0x002ed0: 0710a0e1  mov r1, r7
//   0x002ed4: 4dfb00eb  bl #0x41c10
//   0x002ed8: 741094e5  ldr r1, [r4, #0x74]
//   0x002edc: 782094e5  ldr r2, [r4, #0x78]
//   0x002ee0: 020051e1  cmp r1, r2
//   0x002ee4: 040000aa  bge #0x2efc
//   0x002ee8: 7c2094e5  ldr r2, [r4, #0x7c]
//   0x002eec: 010182e7  str r0, [r2, r1, lsl #2]
//   0x002ef0: 741094e5  ldr r1, [r4, #0x74]
//   0x002ef4: 011081e2  add r1, r1, #1
//   0x002ef8: 741084e5  str r1, [r4, #0x74]
inline constexpr u32 PinItemOwnerCreateWords[] = {0xeb241042, 0xe5905180, 0xe2840074, 0xe3a02000, 0xe3a03004, 0xe1a01005, 0xeb19a203, 0xe3550001, 0xba000013, 0xe3a00c01, 0xeb199fd0, 0xe1a01007, 0xeb00fb4d, 0xe5941074, 0xe5942078, 0xe1510002, 0xaa000004, 0xe594207c, 0xe7820101, 0xe5941074, 0xe2811001, 0xe5841074};
inline constexpr Pin PinItemOwnerCreate{0x2ea4, PinItemOwnerCreateWords, "ItemOwnerCreate"};

// ItemOwnerCtor: Owner ctor stores player index r1 at +0x24
//   0x041c10: 70402de9  push {r4, r5, r6, lr}
//   0x041c14: 0150a0e1  mov r5, r1
//   0x041c18: 0040a0e1  mov r4, r0
//   0x041c1c: d6dd1feb  bl #0x83937c
//   0x041c20: 1c019fe5  ldr r0, [pc, #0x11c]   ; lit[0x41d44]=0xed0bec
//   0x041c24: 0060a0e3  mov r6, #0
//   0x041c28: 00009fe7  ldr r0, [pc, r0]   ; -> 0xf1281c (got->0xe56e6c)
//   0x041c2c: 080080e2  add r0, r0, #8
//   0x041c30: 000084e5  str r0, [r4]
//   0x041c34: 0000e0e3  mvn r0, #0
//   0x041c38: 1c60c4e5  strb r6, [r4, #0x1c]
//   0x041c3c: 206084e5  str r6, [r4, #0x20]
//   0x041c40: 245084e5  str r5, [r4, #0x24]
inline constexpr u32 PinItemOwnerCtorWords[] = {0xe92d4070, 0xe1a05001, 0xe1a04000, 0xeb1fddd6, 0xe59f011c, 0xe3a06000, 0xe79f0000, 0xe2800008, 0xe5840000, 0xe3e00000, 0xe5c4601c, 0xe5846020, 0xe5845024};
inline constexpr Pin PinItemOwnerCtor{0x41c10, PinItemOwnerCtorWords, "ItemOwnerCtor"};

// ItemSlotItem: slot k<2 = [IO+0x34+4k]; displayed item u32 S+0x7c; decided u32 S+0x78
//   0x0435e8: 020052e3  cmp r2, #2
//   0x0435ec: 0020a023  movhs r2, #0
//   0x0435f0: 021181e0  add r1, r1, r2, lsl #2
//   0x0435f4: 341091e5  ldr r1, [r1, #0x34]
//   0x0435f8: 7c1091e5  ldr r1, [r1, #0x7c]
//   0x0435fc: 001080e5  str r1, [r0]
//   0x043600: 1eff2fe1  bx lr
//   0x043604: 020052e3  cmp r2, #2
//   0x043608: 0020a023  movhs r2, #0
//   0x04360c: 021181e0  add r1, r1, r2, lsl #2
//   0x043610: 341091e5  ldr r1, [r1, #0x34]
//   0x043614: 781091e5  ldr r1, [r1, #0x78]
//   0x043618: 001080e5  str r1, [r0]
//   0x04361c: 1eff2fe1  bx lr
inline constexpr u32 PinItemSlotItemWords[] = {0xe3520002, 0x23a02000, 0xe0811102, 0xe5911034, 0xe591107c, 0xe5801000, 0xe12fff1e, 0xe3520002, 0x23a02000, 0xe0811102, 0xe5911034, 0xe5911078, 0xe5801000, 0xe12fff1e};
inline constexpr Pin PinItemSlotItem{0x435e8, PinItemSlotItemWords, "ItemSlotItem"};

// ItemSlotState: state u8 S+0x21: 3 held, 1..2 roulette, 2 stopping
//   0x04864c: 2110d0e5  ldrb r1, [r0, #0x21]
//   0x048650: 0000a0e3  mov r0, #0
//   0x048654: 030051e3  cmp r1, #3
//   0x048658: 01000003  movweq r0, #1
//   0x04865c: 1eff2fe1  bx lr
//   0x048660: 2100d0e5  ldrb r0, [r0, #0x21]
//   0x048664: 010040e2  sub r0, r0, #1
//   0x048668: 7010efe6  uxtb r1, r0
//   0x04866c: 0000a0e3  mov r0, #0
//   0x048670: 020051e3  cmp r1, #2
//   0x048674: 01000033  movwlo r0, #1
//   0x048678: 1eff2fe1  bx lr
//   0x04867c: 2110d0e5  ldrb r1, [r0, #0x21]
//   0x048680: 0000a0e3  mov r0, #0
//   0x048684: 020051e3  cmp r1, #2
//   0x048688: 01000003  movweq r0, #1
inline constexpr u32 PinItemSlotStateWords[] = {0xe5d01021, 0xe3a00000, 0xe3510003, 0x03000001, 0xe12fff1e, 0xe5d00021, 0xe2400001, 0xe6ef1070, 0xe3a00000, 0xe3510002, 0x33000001, 0xe12fff1e, 0xe5d01021, 0xe3a00000, 0xe3510002, 0x03000001};
inline constexpr Pin PinItemSlotState{0x4864c, PinItemSlotStateWords, "ItemSlotState"};

// ItemSlotEmpty: state 0 = empty
//   0x0486e0: 2110d0e5  ldrb r1, [r0, #0x21]
//   0x0486e4: 0000a0e3  mov r0, #0
//   0x0486e8: 000051e3  cmp r1, #0
//   0x0486ec: 01000003  movweq r0, #1
inline constexpr u32 PinItemSlotEmptyWords[] = {0xe5d01021, 0xe3a00000, 0xe3510000, 0x03000001};
inline constexpr Pin PinItemSlotEmpty{0x486e0, PinItemSlotEmptyWords, "ItemSlotEmpty"};

// ItemSlotCount: uses left u32 S+0x5c set from S+0x58 (3 for 7/17/18/19, 8 for 20, else 1); S+0x7c = S+0x78
//   0x047814: 580094e5  ldr r0, [r4, #0x58]
//   0x047818: 14008de5  str r0, [sp, #0x14]
//   0x04781c: 14009de5  ldr r0, [sp, #0x14]
//   0x047820: 011080e2  add r1, r0, #1
//   0x047824: 150051e3  cmp r1, #0x15
//   0x047828: 7d00008a  bhi #0x47a24
//   0x04782c: 0111a0e1  lsl r1, r1, #2
//   0x047830: 08208fe2  add r2, pc, #8
//   0x047834: 0000a0e3  mov r0, #0
//   0x047838: 021091e7  ldr r1, [r1, r2]
//   0x04783c: 02f081e0  add pc, r1, r2
//   0x047840: 5c000000  andeq r0, r0, ip, asr r0
//   0x047844: e4010000  andeq r0, r0, r4, ror #3
//   0x047848: e4010000  andeq r0, r0, r4, ror #3
//   0x04784c: e4010000  andeq r0, r0, r4, ror #3
//   0x047850: e4010000  andeq r0, r0, r4, ror #3
//   0x047854: e4010000  andeq r0, r0, r4, ror #3
//   0x047858: e4010000  andeq r0, r0, r4, ror #3
//   0x04785c: e4010000  andeq r0, r0, r4, ror #3
//   0x047860: 58000000  andeq r0, r0, r8, asr r0
//   0x047864: e4010000  andeq r0, r0, r4, ror #3
//   0x047868: e4010000  andeq r0, r0, r4, ror #3
//   0x04786c: e4010000  andeq r0, r0, r4, ror #3
//   0x047870: e4010000  andeq r0, r0, r4, ror #3
//   0x047874: e4010000  andeq r0, r0, r4, ror #3
//   0x047878: e4010000  andeq r0, r0, r4, ror #3
//   0x04787c: e4010000  andeq r0, r0, r4, ror #3
//   0x047880: e4010000  andeq r0, r0, r4, ror #3
//   0x047884: e4010000  andeq r0, r0, r4, ror #3
//   0x047888: 58000000  andeq r0, r0, r8, asr r0
//   0x04788c: 58000000  andeq r0, r0, r8, asr r0
//   0x047890: 58000000  andeq r0, r0, r8, asr r0
//   0x047894: ec010000  andeq r0, r0, ip, ror #3
//   0x047898: 0300a0e3  mov r0, #3
//   0x04789c: 5c0084e5  str r0, [r4, #0x5c]
//   0x0478a0: 780094e5  ldr r0, [r4, #0x78]
//   0x0478a4: 7c0084e5  str r0, [r4, #0x7c]
inline constexpr u32 PinItemSlotCountWords[] = {0xe5940058, 0xe58d0014, 0xe59d0014, 0xe2801001, 0xe3510015, 0x8a00007d, 0xe1a01101, 0xe28f2008, 0xe3a00000, 0xe7911002, 0xe081f002, 0x0000005c, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x00000058, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x000001e4, 0x00000058, 0x00000058, 0x00000058, 0x000001ec, 0xe3a00003, 0xe584005c, 0xe5940078, 0xe584007c};
inline constexpr Pin PinItemSlotCount{0x47814, PinItemSlotCountWords, "ItemSlotCount"};

// ItemSlotLayout: slot ctor: bytes +0x20..+0x23, index +0x38, owner +0x3c, source -1 at +0x58, count +0x5c (doc pin)
//   0x047230: 2070c4e5  strb r7, [r4, #0x20]
//   0x047234: 2170c4e5  strb r7, [r4, #0x21]
//   0x047238: 2270c4e5  strb r7, [r4, #0x22]
//   0x04723c: 2300c4e5  strb r0, [r4, #0x23]
//   0x047240: 88029fe5  ldr r0, [pc, #0x288]   ; lit[0x474d0]=0xecb5f8
//   0x047244: 247084e5  str r7, [r4, #0x24]
//   0x047248: 00009fe7  ldr r0, [pc, r0]   ; -> 0xf12848 (got->0xe56f38)
//   0x04724c: 080080e2  add r0, r0, #8
//   0x047250: 1c00a8e5  str r0, [r8, #0x1c]!
//   0x047254: 380084e2  add r0, r4, #0x38
//   0x047258: 347084e5  str r7, [r4, #0x34]
//   0x04725c: 307084e5  str r7, [r4, #0x30]
//   0x047260: 2c7084e5  str r7, [r4, #0x2c]
//   0x047264: 287084e5  str r7, [r4, #0x28]
//   0x047268: 600280e8  stm r0, {r5, r6, sb}
//   0x04726c: 0000e0e3  mvn r0, #0
//   0x047270: 0460a0e1  mov r6, r4
//   0x047274: 447084e5  str r7, [r4, #0x44]
//   0x047278: 487084e5  str r7, [r4, #0x48]
//   0x04727c: 4c7084e5  str r7, [r4, #0x4c]
//   0x047280: 507084e5  str r7, [r4, #0x50]
//   0x047284: 547084e5  str r7, [r4, #0x54]
//   0x047288: 580084e5  str r0, [r4, #0x58]
//   0x04728c: 5c7084e5  str r7, [r4, #0x5c]
inline constexpr u32 PinItemSlotLayoutWords[] = {0xe5c47020, 0xe5c47021, 0xe5c47022, 0xe5c40023, 0xe59f0288, 0xe5847024, 0xe79f0000, 0xe2800008, 0xe5a8001c, 0xe2840038, 0xe5847034, 0xe5847030, 0xe584702c, 0xe5847028, 0xe8800260, 0xe3e00000, 0xe1a06004, 0xe5847044, 0xe5847048, 0xe584704c, 0xe5847050, 0xe5847054, 0xe5840058, 0xe584705c};
inline constexpr Pin PinItemSlotLayout{0x47230, PinItemSlotLayoutWords, "ItemSlotLayout"};

// ItemFrame: item -> pattern frame byte table (ldr lit; add pc), coin override (lit word included)
//   0x45610c: 0010a0e1  mov r1, r0
//   0x456110: 0000a0e3  mov r0, #0
//   0x456114: 160051e3  cmp r1, #0x16
//   0x456118: 1eff2f81  bxhi lr
//   0x45611c: 10402de9  push {r4, lr}
//   0x456120: 10d04de2  sub sp, sp, #0x10
//   0x456124: 98009fe5  ldr r0, [pc, #0x98]   ; lit[0x4561c4]=0x94a080
//   0x456128: 0f0051e3  cmp r1, #0xf
//   0x45612c: 00008fe0  add r0, pc, r0   ; -> 0xda01b4 (add)
//   0x456130: 0140d0e7  ldrb r4, [r0, r1]
//   0x456134: 1e00001a  bne #0x4561b4
//   0x456138: 9dc312eb  bl #0x906fb4
//   0x45613c: 0010a0e1  mov r1, r0
//   0x456140: 0c008de2  add r0, sp, #0xc
//   0x456144: 17c312eb  bl #0x906da8
//   0x456148: 0c109de5  ldr r1, [sp, #0xc]
//   0x45614c: 1900a0e3  mov r0, #0x19
//   0x456150: 010051e3  cmp r1, #1
//   0x456154: 1700000a  beq #0x4561b8
//   0x456158: 95c312eb  bl #0x906fb4
//   0x45615c: 0010a0e1  mov r1, r0
//   0x456160: 08008de2  add r0, sp, #8
//   0x456164: 0fc312eb  bl #0x906da8
//   0x456168: 08109de5  ldr r1, [sp, #8]
//   0x45616c: 1a00a0e3  mov r0, #0x1a
//   0x456170: 020051e3  cmp r1, #2
//   0x456174: 0f00000a  beq #0x4561b8
//   0x456178: 8dc312eb  bl #0x906fb4
//   0x45617c: 0010a0e1  mov r1, r0
//   0x456180: 04008de2  add r0, sp, #4
//   0x456184: 07c312eb  bl #0x906da8
//   0x456188: 04009de5  ldr r0, [sp, #4]
//   0x45618c: 030050e3  cmp r0, #3
//   0x456190: 0400a0e1  mov r0, r4
//   0x456194: 0700000a  beq #0x4561b8
//   0x456198: 85c312eb  bl #0x906fb4
//   0x45619c: 0010a0e1  mov r1, r0
//   0x4561a0: 0d00a0e1  mov r0, sp
//   0x4561a4: ffc212eb  bl #0x906da8
//   0x4561a8: 00009de5  ldr r0, [sp]
//   0x4561ac: 040050e3  cmp r0, #4
//   0x4561b0: 1d400003  movweq r4, #0x1d
//   0x4561b4: 0400a0e1  mov r0, r4
//   0x4561b8: 10d08de2  add sp, sp, #0x10
//   0x4561bc: 1040bde8  pop {r4, lr}
//   0x4561c0: 1eff2fe1  bx lr
//   0x4561c4: 80a09400  addseq sl, r4, r0, lsl #1
inline constexpr u32 PinItemFrameWords[] = {0xe1a01000, 0xe3a00000, 0xe3510016, 0x812fff1e, 0xe92d4010, 0xe24dd010, 0xe59f0098, 0xe351000f, 0xe08f0000, 0xe7d04001, 0x1a00001e, 0xeb12c39d, 0xe1a01000, 0xe28d000c, 0xeb12c317, 0xe59d100c, 0xe3a00019, 0xe3510001, 0x0a000017, 0xeb12c395, 0xe1a01000, 0xe28d0008, 0xeb12c30f, 0xe59d1008, 0xe3a0001a, 0xe3510002, 0x0a00000f, 0xeb12c38d, 0xe1a01000, 0xe28d0004, 0xeb12c307, 0xe59d0004, 0xe3500003, 0xe1a00004, 0x0a000007, 0xeb12c385, 0xe1a01000, 0xe1a0000d, 0xeb12c2ff, 0xe59d0000, 0xe3500004, 0x0300401d, 0xe1a00004, 0xe28dd010, 0xe8bd4010, 0xe12fff1e, 0x0094a080};
inline constexpr Pin PinItemFrame{0x45610c, PinItemFrameWords, "ItemFrame"};

// ItemTripleFrame: HUD count <= 2 of a triple item (7/17/18/19) -> frame (count==2 ? 1/17/18/19 : 0/3/5/7)
//   0x55c7fc: 020051e3  cmp r1, #2
//   0x55c800: 0120a0e1  mov r2, r1
//   0x55c804: 0020a023  movhs r2, #0
//   0x55c808: 023180e0  add r3, r0, r2, lsl #2
//   0x55c80c: 30c193e5  ldr ip, [r3, #0x130]
//   0x55c810: 02005ce3  cmp ip, #2
//   0x55c814: 000000da  ble #0x55c81c
//   0x55c818: 1eff2fe1  bx lr
//   0x55c81c: 383193e5  ldr r3, [r3, #0x138]
//   0x55c820: 073043e2  sub r3, r3, #7
//   0x55c824: 0c0053e3  cmp r3, #0xc
//   0x55c828: 1eff2f81  bxhi lr
//   0x55c82c: 0331a0e1  lsl r3, r3, #2
//   0x55c830: 04208fe2  add r2, pc, #4
//   0x55c834: 023093e7  ldr r3, [r3, r2]
//   0x55c838: 02f083e0  add pc, r3, r2
//   0x55c83c: 34000000  andeq r0, r0, r4, lsr r0
//   0x55c840: dcffffff  .byte 0xdc, 0xff, 0xff, 0xff
//   0x55c844: dcffffff  .byte 0xdc, 0xff, 0xff, 0xff
//   0x55c848: dcffffff  .byte 0xdc, 0xff, 0xff, 0xff
//   0x55c84c: dcffffff  .byte 0xdc, 0xff, 0xff, 0xff
//   0x55c850: dcffffff  .byte 0xdc, 0xff, 0xff, 0xff
//   0x55c854: dcffffff  .byte 0xdc, 0xff, 0xff, 0xff
//   0x55c858: dcffffff  .byte 0xdc, 0xff, 0xff, 0xff
//   0x55c85c: dcffffff  .byte 0xdc, 0xff, 0xff, 0xff
//   0x55c860: dcffffff  .byte 0xdc, 0xff, 0xff, 0xff
//   0x55c864: 44000000  andeq r0, r0, r4, asr #32
//   0x55c868: 54000000  andeq r0, r0, r4, asr r0
//   0x55c86c: 64000000  andeq r0, r0, r4, rrx
//   0x55c870: 0720a0e3  mov r2, #7
//   0x55c874: 160a9fed  vldr s0, [pc, #0x58]
//   0x55c878: 001ab7ee  vmov.f32 s2, #1.000000e+00
//   0x55c87c: 0a0000ea  b #0x55c8ac
//   0x55c880: 0720a0e3  mov r2, #7
//   0x55c884: 080ab0ee  vmov.f32 s0, #3.000000e+00
//   0x55c888: 011ab3ee  vmov.f32 s2, #1.700000e+01
//   0x55c88c: 060000ea  b #0x55c8ac
//   0x55c890: 0720a0e3  mov r2, #7
//   0x55c894: 040ab1ee  vmov.f32 s0, #5.000000e+00
//   0x55c898: 021ab3ee  vmov.f32 s2, #1.800000e+01
//   0x55c89c: 020000ea  b #0x55c8ac
//   0x55c8a0: 0720a0e3  mov r2, #7
//   0x55c8a4: 0c0ab1ee  vmov.f32 s0, #7.000000e+00
//   0x55c8a8: 031ab3ee  vmov.f32 s2, #1.900000e+01
//   0x55c8ac: 811082e0  add r1, r2, r1, lsl #1
//   0x55c8b0: 342090e5  ldr r2, [r0, #0x34]
//   0x55c8b4: 380090e5  ldr r0, [r0, #0x38]
//   0x55c8b8: 010052e1  cmp r2, r1
//   0x55c8bc: 01018080  addhi r0, r0, r1, lsl #2
//   0x55c8c0: 02005ce3  cmp ip, #2
//   0x55c8c4: 000090e5  ldr r0, [r0]
//   0x55c8c8: 0010a0e3  mov r1, #0
//   0x55c8cc: 000a01fe  vseleq.f32 s0, s2, s0
//   0x55c8d0: 98ac0fea  b #0x947b38
//   0x55c8d4: 00000000  andeq r0, r0, r0
inline constexpr u32 PinItemTripleFrameWords[] = {0xe3510002, 0xe1a02001, 0x23a02000, 0xe0803102, 0xe593c130, 0xe35c0002, 0xda000000, 0xe12fff1e, 0xe5933138, 0xe2433007, 0xe353000c, 0x812fff1e, 0xe1a03103, 0xe28f2004, 0xe7933002, 0xe083f002, 0x00000034, 0xffffffdc, 0xffffffdc, 0xffffffdc, 0xffffffdc, 0xffffffdc, 0xffffffdc, 0xffffffdc, 0xffffffdc, 0xffffffdc, 0x00000044, 0x00000054, 0x00000064, 0xe3a02007, 0xed9f0a16, 0xeeb71a00, 0xea00000a, 0xe3a02007, 0xeeb00a08, 0xeeb31a01, 0xea000006, 0xe3a02007, 0xeeb10a04, 0xeeb31a02, 0xea000002, 0xe3a02007, 0xeeb10a0c, 0xeeb31a03, 0xe0821081, 0xe5902034, 0xe5900038, 0xe1520001, 0x80800101, 0xe35c0002, 0xe5900000, 0xe3a01000, 0xfe010a00, 0xea0fac98, 0x00000000};
inline constexpr Pin PinItemTripleFrame{0x55c7fc, PinItemTripleFrameWords, "ItemTripleFrame"};

// CoinKind: course coin kind from RI+0x1a0
//   0x906da8: 30482de9  push {r4, r5, fp, lr}
//   0x906dac: 08d04de2  sub sp, sp, #8
//   0x906db0: 0150a0e1  mov r5, r1
//   0x906db4: 0040a0e1  mov r4, r0
//   0x906db8: 0110a0e3  mov r1, #1
//   0x906dbc: a00195e5  ldr r0, [r5, #0x1a0]
//   0x906dc0: 330050e3  cmp r0, #0x33
//   0x906dc4: 0b00000a  beq #0x906df8
//   0x906dc8: 04008de5  str r0, [sp, #4]
//   0x906dcc: 04008de2  add r0, sp, #4
//   0x906dd0: 83fcffeb  bl #0x905fe4
//   0x906dd4: 0210a0e3  mov r1, #2
//   0x906dd8: 000050e3  cmp r0, #0
//   0x906ddc: 0500001a  bne #0x906df8
//   0x906de0: a00195e5  ldr r0, [r5, #0x1a0]
//   0x906de4: 0010a0e3  mov r1, #0
//   0x906de8: 660050e3  cmp r0, #0x66
//   0x906dec: 04100003  movweq r1, #4
//   0x906df0: 460050e3  cmp r0, #0x46
//   0x906df4: 03100003  movweq r1, #3
//   0x906df8: 001084e5  str r1, [r4]
//   0x906dfc: 08d08de2  add sp, sp, #8
//   0x906e00: 3088bde8  pop {r4, r5, fp, pc}
inline constexpr u32 PinCoinKindWords[] = {0xe92d4830, 0xe24dd008, 0xe1a05001, 0xe1a04000, 0xe3a01001, 0xe59501a0, 0xe3500033, 0x0a00000b, 0xe58d0004, 0xe28d0004, 0xebfffc83, 0xe3a01002, 0xe3500000, 0x1a000005, 0xe59501a0, 0xe3a01000, 0xe3500066, 0x03001004, 0xe3500046, 0x03001003, 0xe5841000, 0xe28dd008, 0xe8bd8830};
inline constexpr Pin PinCoinKind{0x906da8, PinCoinKindWords, "CoinKind"};

// AnimalCourse: raw 0x34 + bits of 0x7001
//   0x905fe4: 000090e5  ldr r0, [r0]
//   0x905fe8: 340040e2  sub r0, r0, #0x34
//   0x905fec: 0e0050e3  cmp r0, #0xe
//   0x905ff0: 0000a083  movhi r0, #0
//   0x905ff4: 1eff2f81  bxhi lr
//   0x905ff8: 9f07dfe7  bfc r0, #0xf, #0x11
//   0x905ffc: 011007e3  movw r1, #0x7001
//   0x906000: 0120a0e3  mov r2, #1
//   0x906004: 310002e0  and r0, r2, r1, lsr r0
//   0x906008: 1eff2fe1  bx lr
inline constexpr u32 PinAnimalCourseWords[] = {0xe5900000, 0xe2400034, 0xe350000e, 0x83a00000, 0x812fff1e, 0xe7df079f, 0xe3071001, 0xe3a02001, 0xe0020031, 0xe12fff1e};
inline constexpr Pin PinAnimalCourse{0x905fe4, PinAnimalCourseWords, "AnimalCourse"};

// SystemEngine: SE = [E+0xe0+4*0]
//   0x9461fc: 00482de9  push {fp, lr}
//   0x946200: 08d04de2  sub sp, sp, #8
//   0x946204: c1cffceb  bl #0x87a110
//   0x946208: 0010a0e3  mov r1, #0
//   0x94620c: 04108de5  str r1, [sp, #4]
//   0x946210: 04209de5  ldr r2, [sp, #4]
//   0x946214: 0a0052e3  cmp r2, #0xa
//   0x946218: 0210a031  movlo r1, r2
//   0x94621c: 010180e0  add r0, r0, r1, lsl #2
//   0x946220: e00090e5  ldr r0, [r0, #0xe0]
//   0x946224: 08d08de2  add sp, sp, #8
//   0x946228: 0088bde8  pop {fp, pc}
inline constexpr u32 PinSystemEngineWords[] = {0xe92d4800, 0xe24dd008, 0xebfccfc1, 0xe3a01000, 0xe58d1004, 0xe59d2004, 0xe352000a, 0x31a01002, 0xe0800101, 0xe59000e0, 0xe28dd008, 0xe8bd8800};
inline constexpr Pin PinSystemEngine{0x9461fc, PinSystemEngineWords, "SystemEngine"};

// PlayerManager: PM = [SE+0x528]
//   0x94769c: 00482de9  push {fp, lr}
//   0x9476a0: d5faffeb  bl #0x9461fc
//   0x9476a4: 000050e3  cmp r0, #0
//   0x9476a8: 28059015  ldrne r0, [r0, #0x528]
//   0x9476ac: 0000a003  moveq r0, #0
//   0x9476b0: 0088bde8  pop {fp, pc}
inline constexpr u32 PinPlayerManagerWords[] = {0xe92d4800, 0xebfffad5, 0xe3500000, 0x15900528, 0x03a00000, 0xe8bd8800};
inline constexpr Pin PinPlayerManager{0x94769c, PinPlayerManagerWords, "PlayerManager"};

// LocalPlayerObj: PM+0xd0+4k, k<=3
//   0x906280: 10402de9  push {r4, lr}
//   0x906284: 0040a0e1  mov r4, r0
//   0x906288: 0000e0e3  mvn r0, #0
//   0x90628c: 030054e3  cmp r4, #3
//   0x906290: 1080bd88  pophi {r4, pc}
//   0x906294: 000501eb  bl #0x94769c
//   0x906298: 040180e0  add r0, r0, r4, lsl #2
//   0x90629c: d00090e5  ldr r0, [r0, #0xd0]
//   0x9062a0: 1080bde8  pop {r4, pc}
inline constexpr u32 PinLocalPlayerObjWords[] = {0xe92d4010, 0xe1a04000, 0xe3e00000, 0xe3540003, 0x88bd8010, 0xeb010500, 0xe0800104, 0xe59000d0, 0xe8bd8010};
inline constexpr Pin PinLocalPlayerObj{0x906280, PinLocalPlayerObjWords, "LocalPlayerObj"};

// RacerPlayerObj: PM+0x120+4i, i<=11
//   0x906054: 10402de9  push {r4, lr}
//   0x906058: 0040a0e1  mov r4, r0
//   0x90605c: 0000e0e3  mvn r0, #0
//   0x906060: 0b0054e3  cmp r4, #0xb
//   0x906064: 1080bd88  pophi {r4, pc}
//   0x906068: 8b0501eb  bl #0x94769c
//   0x90606c: 040180e0  add r0, r0, r4, lsl #2
//   0x906070: 200190e5  ldr r0, [r0, #0x120]
//   0x906074: 1080bde8  pop {r4, pc}
inline constexpr u32 PinRacerPlayerObjWords[] = {0xe92d4010, 0xe1a04000, 0xe3e00000, 0xe354000b, 0x88bd8010, 0xeb01058b, 0xe0800104, 0xe5900120, 0xe8bd8010};
inline constexpr Pin PinRacerPlayerObj{0x906054, PinRacerPlayerObjWords, "RacerPlayerObj"};

// UiRoot: D = [[[cell]]+4] (GOT lit word included)
//   0x94e8e8: 0c009fe5  ldr r0, [pc, #0xc]   ; lit[0x94e8fc]=0x5ca114
//   0x94e8ec: 00009fe7  ldr r0, [pc, r0]   ; -> 0xf18a08 (got->0xf53290)
//   0x94e8f0: 000090e5  ldr r0, [r0]
//   0x94e8f4: 040090e5  ldr r0, [r0, #4]
//   0x94e8f8: 1eff2fe1  bx lr
//   0x94e8fc: 14a15c00  subseq sl, ip, r4, lsl r1
inline constexpr u32 PinUiRootWords[] = {0xe59f000c, 0xe79f0000, 0xe5900000, 0xe5900004, 0xe12fff1e, 0x005ca114};
inline constexpr Pin PinUiRoot{0x94e8e8, PinUiRootWords, "UiRoot"};

// UiResource: R = [D+0xc+4k], k<13
//   0x959e8c: 10402de9  push {r4, lr}
//   0x959e90: 08d04de2  sub sp, sp, #8
//   0x959e94: 0040a0e1  mov r4, r0
//   0x959e98: 92d2ffeb  bl #0x94e8e8
//   0x959e9c: 001094e5  ldr r1, [r4]
//   0x959ea0: 04108de5  str r1, [sp, #4]
//   0x959ea4: 04109de5  ldr r1, [sp, #4]
//   0x959ea8: 0d0051e3  cmp r1, #0xd
//   0x959eac: 0010a023  movhs r1, #0
//   0x959eb0: 010180e0  add r0, r0, r1, lsl #2
//   0x959eb4: 0c0090e5  ldr r0, [r0, #0xc]
//   0x959eb8: 08d08de2  add sp, sp, #8
//   0x959ebc: 1080bde8  pop {r4, pc}
inline constexpr u32 PinUiResourceWords[] = {0xe92d4010, 0xe24dd008, 0xe1a04000, 0xebffd292, 0xe5941000, 0xe58d1004, 0xe59d1004, 0xe351000d, 0x23a01000, 0xe0800101, 0xe590000c, 0xe28dd008, 0xe8bd8010};
inline constexpr Pin PinUiResource{0x959e8c, PinUiResourceWords, "UiResource"};

// UiTypeCheck: R3 vtable slot 0 type check, then T = [R3+0x1c]
//   0x557124: 0570a0e1  mov r7, r5
//   0x557128: 1c0097e4  ldr r0, [r7], #0x1c
//   0x55712c: 002090e5  ldr r2, [r0]
//   0x557130: 380a9fe5  ldr r0, [pc, #0xa38]   ; lit[0x557b70]=0x9bb8b4
//   0x557134: 00109fe7  ldr r1, [pc, r0]   ; -> 0xf129f0 (got->0xf2328c)
//   0x557138: 0500a0e1  mov r0, r5
//   0x55713c: 32ff2fe1  blx r2
//   0x557140: 000050e3  cmp r0, #0
//   0x557144: 1c70a003  moveq r7, #0x1c
//   0x557148: 000097e5  ldr r0, [r7]
inline constexpr u32 PinUiTypeCheckWords[] = {0xe1a07005, 0xe497001c, 0xe5902000, 0xe59f0a38, 0xe79f1000, 0xe1a00005, 0xe12fff32, 0xe3500000, 0x03a0701c, 0xe5970000};
inline constexpr Pin PinUiTypeCheck{0x557124, PinUiTypeCheckWords, "UiTypeCheck"};

// DriverTableInit: DT = R3+0x170, R3+0x1c0, pict table R3+0x1d0
//   0x4335a4: 170e84e2  add r0, r4, #0x170
//   0x4335a8: 574500eb  bl #0x444b0c
//   0x4335ac: 070d84e2  add r0, r4, #0x1c0
//   0x4335b0: 485600eb  bl #0x448ed8
//   0x4335b4: 1d0e84e2  add r0, r4, #0x1d0
//   0x4335b8: cf5800eb  bl #0x4498fc
inline constexpr u32 PinDriverTableInitWords[] = {0xe2840e17, 0xeb004557, 0xe2840d07, 0xeb005648, 0xe2840e1d, 0xeb0058cf};
inline constexpr Pin PinDriverTableInit{0x4335a4, PinDriverTableInitWords, "DriverTableInit"};

// DriverTableAlloc: DT names char*[0x36] at +0x44 (count +0x40), msgs u32[0x57] at +0x4c (count +0x48)
//   0x444b18: 5c009fe5  ldr r0, [pc, #0x5c]   ; lit[0x444b7c]=0xacd7a0
//   0x444b1c: 0010a0e3  mov r1, #0
//   0x444b20: 0420a0e3  mov r2, #4
//   0x444b24: 00309fe7  ldr r3, [pc, r0]   ; -> 0xf122cc (ldrpc)
//   0x444b28: d800a0e3  mov r0, #0xd8
//   0x444b2c: a89908eb  bl #0x66b1d4
//   0x444b30: 000050e3  cmp r0, #0
//   0x444b34: 0420a0e3  mov r2, #4
//   0x444b38: 0050a0e3  mov r5, #0
//   0x444b3c: 0460a0e3  mov r6, #4
//   0x444b40: 3610a013  movne r1, #0x36
//   0x444b44: 40108415  strne r1, [r4, #0x40]
//   0x444b48: 44008415  strne r0, [r4, #0x44]
//   0x444b4c: 2c009fe5  ldr r0, [pc, #0x2c]   ; lit[0x444b80]=0xacd770
//   0x444b50: 0010a0e3  mov r1, #0
//   0x444b54: 00309fe7  ldr r3, [pc, r0]   ; -> 0xf122cc (ldrpc)
//   0x444b58: 570fa0e3  mov r0, #0x15c
//   0x444b5c: 9c9908eb  bl #0x66b1d4
//   0x444b60: 000050e3  cmp r0, #0
//   0x444b64: 5710a013  movne r1, #0x57
//   0x444b68: 48108415  strne r1, [r4, #0x48]
//   0x444b6c: 4c008415  strne r0, [r4, #0x4c]
inline constexpr u32 PinDriverTableAllocWords[] = {0xe59f005c, 0xe3a01000, 0xe3a02004, 0xe79f3000, 0xe3a000d8, 0xeb0899a8, 0xe3500000, 0xe3a02004, 0xe3a05000, 0xe3a06004, 0x13a01036, 0x15841040, 0x15840044, 0xe59f002c, 0xe3a01000, 0xe79f3000, 0xe3a00f57, 0xeb08999c, 0xe3500000, 0x13a01057, 0x15841048, 0x1584004c};
inline constexpr Pin PinDriverTableAlloc{0x444b18, PinDriverTableAllocWords, "DriverTableAlloc"};

// DriverName: name = names[id+1] (+0x44 char*[], count +0x40)
//   0x446ecc: 08009de5  ldr r0, [sp, #8]
//   0x446ed0: 000090e5  ldr r0, [r0]
//   0x446ed4: 18008de5  str r0, [sp, #0x18]
//   0x446ed8: 18009de5  ldr r0, [sp, #0x18]
//   0x446edc: 40109be5  ldr r1, [fp, #0x40]
//   0x446ee0: 44209be5  ldr r2, [fp, #0x44]
//   0x446ee4: 010080e2  add r0, r0, #1
//   0x446ee8: 000051e1  cmp r1, r0
//   0x446eec: 00218280  addhi r2, r2, r0, lsl #2
//   0x446ef0: 007092e5  ldr r7, [r2]
inline constexpr u32 PinDriverNameWords[] = {0xe59d0008, 0xe5900000, 0xe58d0018, 0xe59d0018, 0xe59b1040, 0xe59b2044, 0xe2800001, 0xe1510000, 0x80822100, 0xe5927000};
inline constexpr Pin PinDriverName{0x446ecc, PinDriverNameWords, "DriverName"};

// DriverMsg: msg = u32 [+0x4c][id+1], count +0x48
//   0x444c74: d4109de5  ldr r1, [sp, #0xd4]
//   0x444c78: 482094e5  ldr r2, [r4, #0x48]
//   0x444c7c: 4c3094e5  ldr r3, [r4, #0x4c]
//   0x444c80: 011081e2  add r1, r1, #1
//   0x444c84: 010052e1  cmp r2, r1
//   0x444c88: 01318380  addhi r3, r3, r1, lsl #2
//   0x444c8c: 000083e5  str r0, [r3]
inline constexpr u32 PinDriverMsgWords[] = {0xe59d10d4, 0xe5942048, 0xe594304c, 0xe2811001, 0xe1520001, 0x80833101, 0xe5830000};
inline constexpr Pin PinDriverMsg{0x444c74, PinDriverMsgWords, "DriverMsg"};

// DriverSuffix: switch(id-4) jump table: ids 4,16,41,42,44 get '0'+variant
//   0x447014: 05b0a0e1  mov fp, r5
//   0x447018: 000090e5  ldr r0, [r0]
//   0x44701c: 040040e2  sub r0, r0, #4
//   0x447020: 280050e3  cmp r0, #0x28
//   0x447024: 9a00008a  bhi #0x447294
//   0x447028: 0001a0e1  lsl r0, r0, #2
//   0x44702c: 04108fe2  add r1, pc, #4
//   0x447030: 010090e7  ldr r0, [r0, r1]
//   0x447034: 01f080e0  add pc, r0, r1
//   0x447038: a4000000  andeq r0, r0, r4, lsr #1
//   0x44703c: 5c020000  andeq r0, r0, ip, asr r2
//   0x447040: 5c020000  andeq r0, r0, ip, asr r2
//   0x447044: 5c020000  andeq r0, r0, ip, asr r2
//   0x447048: 5c020000  andeq r0, r0, ip, asr r2
//   0x44704c: 5c020000  andeq r0, r0, ip, asr r2
//   0x447050: 5c020000  andeq r0, r0, ip, asr r2
//   0x447054: 5c020000  andeq r0, r0, ip, asr r2
//   0x447058: 5c020000  andeq r0, r0, ip, asr r2
//   0x44705c: 5c020000  andeq r0, r0, ip, asr r2
//   0x447060: 5c020000  andeq r0, r0, ip, asr r2
//   0x447064: 5c020000  andeq r0, r0, ip, asr r2
//   0x447068: a4000000  andeq r0, r0, r4, lsr #1
//   0x44706c: 5c020000  andeq r0, r0, ip, asr r2
//   0x447070: 5c020000  andeq r0, r0, ip, asr r2
//   0x447074: 5c020000  andeq r0, r0, ip, asr r2
//   0x447078: 5c020000  andeq r0, r0, ip, asr r2
//   0x44707c: 5c020000  andeq r0, r0, ip, asr r2
//   0x447080: 5c020000  andeq r0, r0, ip, asr r2
//   0x447084: 5c020000  andeq r0, r0, ip, asr r2
//   0x447088: 5c020000  andeq r0, r0, ip, asr r2
//   0x44708c: 5c020000  andeq r0, r0, ip, asr r2
//   0x447090: 5c020000  andeq r0, r0, ip, asr r2
//   0x447094: 5c020000  andeq r0, r0, ip, asr r2
//   0x447098: 5c020000  andeq r0, r0, ip, asr r2
//   0x44709c: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470a0: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470a4: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470a8: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470ac: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470b0: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470b4: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470b8: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470bc: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470c0: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470c4: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470c8: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470cc: a4000000  andeq r0, r0, r4, lsr #1
//   0x4470d0: a4000000  andeq r0, r0, r4, lsr #1
//   0x4470d4: 5c020000  andeq r0, r0, ip, asr r2
//   0x4470d8: a4000000  andeq r0, r0, r4, lsr #1
inline constexpr u32 PinDriverSuffixWords[] = {0xe1a0b005, 0xe5900000, 0xe2400004, 0xe3500028, 0x8a00009a, 0xe1a00100, 0xe28f1004, 0xe7900001, 0xe080f001, 0x000000a4, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x000000a4, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x0000025c, 0x000000a4, 0x000000a4, 0x0000025c, 0x000000a4};
inline constexpr Pin PinDriverSuffix{0x447014, PinDriverSuffixWords, "DriverSuffix"};

// CoursePict: pict name = names[raw+1] (+0x44, count +0x40)
//   0x44b018: 000095e5  ldr r0, [r5]
//   0x44b01c: 1c008de5  str r0, [sp, #0x1c]
//   0x44b020: 1c009de5  ldr r0, [sp, #0x1c]
//   0x44b024: 40109ae5  ldr r1, [sl, #0x40]
//   0x44b028: 44209ae5  ldr r2, [sl, #0x44]
//   0x44b02c: 24a09de5  ldr sl, [sp, #0x24]
//   0x44b030: 010080e2  add r0, r0, #1
//   0x44b034: 000051e1  cmp r1, r0
//   0x44b038: 00218280  addhi r2, r2, r0, lsl #2
//   0x44b03c: 20009de5  ldr r0, [sp, #0x20]
//   0x44b040: 005092e5  ldr r5, [r2]
inline constexpr u32 PinCoursePictWords[] = {0xe5950000, 0xe58d001c, 0xe59d001c, 0xe59a1040, 0xe59a2044, 0xe59da024, 0xe2800001, 0xe1510000, 0x80822100, 0xe59d0020, 0xe5925000};
inline constexpr Pin PinCoursePict{0x44b018, PinCoursePictWords, "CoursePict"};

// PictTableAlloc: pict table: 0x7c names at +0x44, count +0x40
//   0x449938: 4b0aa0e3  mov r0, #0x4b000
//   0x44993c: 21008de8  stm sp, {r0, r5}
//   0x449940: 0400a0e1  mov r0, r4
//   0x449944: daccffeb  bl #0x43ccb4
//   0x449948: 34009fe5  ldr r0, [pc, #0x34]   ; lit[0x449984]=0xac896c
//   0x44994c: 0010a0e3  mov r1, #0
//   0x449950: 0420a0e3  mov r2, #4
//   0x449954: 0460a0e3  mov r6, #4
//   0x449958: 00309fe7  ldr r3, [pc, r0]   ; -> 0xf122cc (ldrpc)
//   0x44995c: 1f0ea0e3  mov r0, #0x1f0
//   0x449960: 1b8608eb  bl #0x66b1d4
//   0x449964: 000050e3  cmp r0, #0
//   0x449968: 40708415  strne r7, [r4, #0x40]
//   0x44996c: 44008415  strne r0, [r4, #0x44]
inline constexpr u32 PinPictTableAllocWords[] = {0xe3a00a4b, 0xe88d0021, 0xe1a00004, 0xebffccda, 0xe59f0034, 0xe3a01000, 0xe3a02004, 0xe3a06004, 0xe79f3000, 0xe3a00e1f, 0xeb08861b, 0xe3500000, 0x15847040, 0x15840044};
inline constexpr Pin PinPictTableAlloc{0x449938, PinPictTableAllocWords, "PictTableAlloc"};

// CupOfCourse: T = [R3+0x1c]; {cup,slot} = T+0xc80+8*raw (raw<0x7b); cup name ptr table (ldr lit; add pc)
//   0x55714c: 50408de5  str r4, [sp, #0x50]
//   0x557150: 50109de5  ldr r1, [sp, #0x50]
//   0x557154: 7b0051e3  cmp r1, #0x7b
//   0x557158: 0010a023  movhs r1, #0
//   0x55715c: 817180e0  add r7, r0, r1, lsl #3
//   0x557160: 800cb7e5  ldr r0, [r7, #0xc80]!
//   0x557164: 010070e3  cmn r0, #1
//   0x557168: c801000a  beq #0x557890
//   0x55716c: 20008de5  str r0, [sp, #0x20]
//   0x557170: 24408de2  add r4, sp, #0x24
//   0x557174: 20009de5  ldr r0, [sp, #0x20]
//   0x557178: 170050e3  cmp r0, #0x17
//   0x55717c: 0010a0e1  mov r1, r0
//   0x557180: 1710a0a3  movge r1, #0x17
//   0x557184: 000050e3  cmp r0, #0
//   0x557188: e4099fe5  ldr r0, [pc, #0x9e4]   ; lit[0x557b74]=0x96b490
//   0x55718c: 001000b3  movwlt r1, #0
//   0x557190: 00008fe0  add r0, pc, r0   ; -> 0xec2628 (add)
//   0x557194: 012190e7  ldr r2, [r0, r1, lsl #2]
inline constexpr u32 PinCupOfCourseWords[] = {0xe58d4050, 0xe59d1050, 0xe351007b, 0x23a01000, 0xe0807181, 0xe5b70c80, 0xe3700001, 0x0a0001c8, 0xe58d0020, 0xe28d4024, 0xe59d0020, 0xe3500017, 0xe1a01000, 0xa3a01017, 0xe3500000, 0xe59f09e4, 0xb3001000, 0xe08f0000, 0xe7902101};
inline constexpr Pin PinCupOfCourse{0x55714c, PinCupOfCourseWords, "CupOfCourse"};

// CupNameTableLit: literal for CupOfCourse's add pc at 0x557190 -> .data 0xec2628 (u32 ptr[24])
//   0x557b74: 90b49600  umullseq fp, r6, r0, r4
inline constexpr u32 PinCupNameTableLitWords[] = {0x0096b490};
inline constexpr Pin PinCupNameTableLit{0x557b74, PinCupNameTableLitWords, "CupNameTableLit"};

// CupTableInit: cup table: count +0x1430, course map +0xc80, cup stride 0x80, entry 0x10 {raw,-1,msg,flag}
//   0x4286a0: f04f2de9  push {r4, r5, r6, r7, r8, sb, sl, fp, lr}
//   0x4286a4: 6bdf4de2  sub sp, sp, #0x1ac
//   0x4286a8: 0040a0e1  mov r4, r0
//   0x4286ac: 300401e3  movw r0, #0x1430
//   0x4286b0: 0010a0e3  mov r1, #0
//   0x4286b4: 0060a0e3  mov r6, #0
//   0x4286b8: 0090e0e3  mvn sb, #0
//   0x4286bc: 0130a0e3  mov r3, #1
//   0x4286c0: 01a0a0e3  mov sl, #1
//   0x4286c4: 0420a0e1  mov r2, r4
//   0x4286c8: 0450a0e1  mov r5, r4
//   0x4286cc: 32bd84e2  add fp, r4, #0xc80
//   0x4286d0: 0010a2e7  str r1, [r2, r0]!
//   0x4286d4: 1b00a0e3  mov r0, #0x1b
//   0x4286d8: a8018de5  str r0, [sp, #0x1a8]
//   0x4286dc: 007092e5  ldr r7, [r2]
//   0x4286e0: 190057e3  cmp r7, #0x19
//   0x4286e4: 0760a031  movlo r6, r7
//   0x4286e8: 8603a5e7  str r0, [r5, r6, lsl #7]!
//   0x4286ec: 0000e0e3  mvn r0, #0
//   0x4286f0: 796500e3  movw r6, #0x579
//   0x4286f4: 040085e5  str r0, [r5, #4]
//   0x4286f8: 086085e5  str r6, [r5, #8]
//   0x4286fc: 0c10c5e5  strb r1, [r5, #0xc]
inline constexpr u32 PinCupTableInitWords[] = {0xe92d4ff0, 0xe24ddf6b, 0xe1a04000, 0xe3010430, 0xe3a01000, 0xe3a06000, 0xe3e09000, 0xe3a03001, 0xe3a0a001, 0xe1a02004, 0xe1a05004, 0xe284bd32, 0xe7a21000, 0xe3a0001b, 0xe58d01a8, 0xe5927000, 0xe3570019, 0x31a06007, 0xe7a50386, 0xe3e00000, 0xe3006579, 0xe5850004, 0xe5856008, 0xe5c5100c};
inline constexpr Pin PinCupTableInit{0x4286a0, PinCupTableInitWords, "CupTableInit"};

// CupMsg: cup name msg = u32 rodata table (ldr lit; add pc)[cup], -1 when cup < 0 (lit word included)
//   0x451f3c: 0000e0e3  mvn r0, #0
//   0x451f40: 002091e5  ldr r2, [r1]
//   0x451f44: 010072e3  cmn r2, #1
//   0x451f48: 0600000a  beq #0x451f68
//   0x451f4c: 002091e5  ldr r2, [r1]
//   0x451f50: 000052e3  cmp r2, #0
//   0x451f54: 1eff2fb1  bxlt lr
//   0x451f58: 000091e5  ldr r0, [r1]
//   0x451f5c: 08109fe5  ldr r1, [pc, #8]   ; lit[0x451f6c]=0x94e1c4
//   0x451f60: 01108fe0  add r1, pc, r1   ; -> 0xda012c (add)
//   0x451f64: 000191e7  ldr r0, [r1, r0, lsl #2]
//   0x451f68: 1eff2fe1  bx lr
//   0x451f6c: c4e19400  addseq lr, r4, r4, asr #3
inline constexpr u32 PinCupMsgWords[] = {0xe3e00000, 0xe5912000, 0xe3720001, 0x0a000006, 0xe5912000, 0xe3520000, 0xb12fff1e, 0xe5910000, 0xe59f1008, 0xe08f1001, 0xe7910100, 0xe12fff1e, 0x0094e1c4};
inline constexpr Pin PinCupMsg{0x451f3c, PinCupMsgWords, "CupMsg"};

// DriverParamDb: driver param DB = [[[GOT]]+0x78]
//   0x8f2a0c: 30482de9  push {r4, r5, fp, lr}
//   0x8f2a10: 0040a0e1  mov r4, r0
//   0x8f2a14: 7c009fe5  ldr r0, [pc, #0x7c]   ; lit[0x8f2a98]=0x61f758
//   0x8f2a18: 0120a0e1  mov r2, r1
//   0x8f2a1c: 00509fe7  ldr r5, [pc, r0]   ; -> 0xf1217c (got->0xf50dec)
//   0x8f2a20: 000095e5  ldr r0, [r5]
//   0x8f2a24: 781090e5  ldr r1, [r0, #0x78]
//   0x8f2a28: 040084e2  add r0, r4, #4
//   0x8f2a2c: 6dffffeb  bl #0x8f27e8
//   0x8f2a30: 000095e5  ldr r0, [r5]
//   0x8f2a34: 785090e5  ldr r5, [r0, #0x78]
//   0x8f2a38: 000055e3  cmp r5, #0
//   0x8f2a3c: 3088bd08  popeq {r4, r5, fp, pc}
//   0x8f2a40: 0400a0e1  mov r0, r4
//   0x8f2a44: 0510a0e1  mov r1, r5
//   0x8f2a48: 460200eb  bl #0x8f3368
inline constexpr u32 PinDriverParamDbWords[] = {0xe92d4830, 0xe1a04000, 0xe59f007c, 0xe1a02001, 0xe79f5000, 0xe5950000, 0xe5901078, 0xe2840004, 0xebffff6d, 0xe5950000, 0xe5905078, 0xe3550000, 0x08bd8830, 0xe1a00004, 0xe1a01005, 0xeb000246};
inline constexpr Pin PinDriverParamDb{0x8f2a0c, PinDriverParamDbWords, "DriverParamDb"};

// DriverParamDbLit: literal for DriverParamDb's ldr [pc,r0] at 0x8f2a1c -> GOT 0xf1217c
//   0x8f2a98: 58f76100  rsbeq pc, r1, r8, asr r7
inline constexpr u32 PinDriverParamDbLitWords[] = {0x0061f758};
inline constexpr Pin PinDriverParamDbLit{0x8f2a98, PinDriverParamDbLitWords, "DriverParamDbLit"};

// DriverParamRecord: record = [DB+0x1c] + id*0x244 if [DB+0x14] && [DB+0x1c] && 0 <= id < u16 DB+0x56 (else DB+0x80)
//   0x8f3384: 142091e5  ldr r2, [r1, #0x14]
//   0x8f3388: 000052e3  cmp r2, #0
//   0x8f338c: 8100000a  beq #0x8f3598
//   0x8f3390: 1c2091e5  ldr r2, [r1, #0x1c]
//   0x8f3394: 000052e3  cmp r2, #0
//   0x8f3398: 0a00000a  beq #0x8f33c8
//   0x8f339c: 18319de5  ldr r3, [sp, #0x118]
//   0x8f33a0: 000053e3  cmp r3, #0
//   0x8f33a4: 070000ba  blt #0x8f33c8
//   0x8f33a8: 18319de5  ldr r3, [sp, #0x118]
//   0x8f33ac: b675d1e1  ldrh r7, [r1, #0x56]
//   0x8f33b0: 070053e1  cmp r3, r7
//   0x8f33b4: 030000aa  bge #0x8f33c8
//   0x8f33b8: 18319de5  ldr r3, [sp, #0x118]
//   0x8f33bc: 917fa0e3  mov r7, #0x244
//   0x8f33c0: 932722e0  mla r2, r3, r7, r2
//   0x8f33c4: 000000ea  b #0x8f33cc
//   0x8f33c8: 802081e2  add r2, r1, #0x80
inline constexpr u32 PinDriverParamRecordWords[] = {0xe5912014, 0xe3520000, 0x0a000081, 0xe591201c, 0xe3520000, 0x0a00000a, 0xe59d3118, 0xe3530000, 0xba000007, 0xe59d3118, 0xe1d175b6, 0xe1530007, 0xaa000003, 0xe59d3118, 0xe3a07f91, 0xe0222793, 0xea000000, 0xe2812080};
inline constexpr Pin PinDriverParamRecord{0x8f3384, PinDriverParamRecordWords, "DriverParamRecord"};

// EmblemFlag: suffix flag = bit 1 of u32 record+0x60 (-> +0xa64)
//   0x8f3770: 58009de5  ldr r0, [sp, #0x58]
//   0x8f3774: 600090e5  ldr r0, [r0, #0x60]
//   0x8f3778: d000e0e7  ubfx r0, r0, #1, #1
//   0x8f377c: 640acbe5  strb r0, [fp, #0xa64]
inline constexpr u32 PinEmblemFlagWords[] = {0xe59d0058, 0xe5900060, 0xe7e000d0, 0xe5cb0a64};
inline constexpr Pin PinEmblemFlag{0x8f3770, PinEmblemFlagWords, "EmblemFlag"};

// EmblemCode: code = (char*) u32 record+0x10, copied into this+0xa44 (buffer +0xa48)
//   0x8f3f1c: 58009de5  ldr r0, [sp, #0x58]
//   0x8f3f20: 485a9be5  ldr r5, [fp, #0xa48]
//   0x8f3f24: 101090e5  ldr r1, [r0, #0x10]
//   0x8f3f28: 010055e1  cmp r5, r1
inline constexpr u32 PinEmblemCodeWords[] = {0xe59d0058, 0xe59b5a48, 0xe5901010, 0xe1550001};
inline constexpr Pin PinEmblemCode{0x8f3f1c, PinEmblemCodeWords, "EmblemCode"};

// EmblemName: driver [this+4] 0x34 / 0x1d (Mii) -> Emblem_Mii
//   0x8f3780: 04109be5  ldr r1, [fp, #4]
//   0x8f3784: 38808de5  str r8, [sp, #0x38]
//   0x8f3788: 340051e3  cmp r1, #0x34
//   0x8f378c: 1b00000a  beq #0x8f3800
//   0x8f3790: 1d0051e3  cmp r1, #0x1d
//   0x8f3794: 3300001a  bne #0x8f3868
inline constexpr u32 PinEmblemNameWords[] = {0xe59b1004, 0xe58d8038, 0xe3510034, 0x0a00001b, 0xe351001d, 0x1a000033};
inline constexpr Pin PinEmblemName{0x8f3780, PinEmblemNameWords, "EmblemName"};

// EmblemSuffix: Emblem_%s(this+0xa48); if flag +0xa64 && driver != 0x10: + %02d(variant this+8)
//   0x8f4058: 0b50a0e1  mov r5, fp
//   0x8f405c: 440ab5e5  ldr r0, [r5, #0xa44]!
//   0x8f4060: 0c1090e5  ldr r1, [r0, #0xc]
//   0x8f4064: 0500a0e1  mov r0, r5
//   0x8f4068: 31ff2fe1  blx r1
//   0x8f406c: 8c0f9fe5  ldr r0, [pc, #0xf8c]   ; lit[0x8f5000]=0x48e5c0
//   0x8f4070: 042095e5  ldr r2, [r5, #4]
//   0x8f4074: 00108fe0  add r1, pc, r0   ; -> 0xd8263c (add)
//   0x8f4078: 0a00a0e1  mov r0, sl
//   0x8f407c: 5625f6eb  bl #0x67d5dc
//   0x8f4080: 2000d5e5  ldrb r0, [r5, #0x20]
//   0x8f4084: 000050e3  cmp r0, #0
//   0x8f4088: d0ffff0a  beq #0x8f3fd0
//   0x8f408c: 44009de5  ldr r0, [sp, #0x44]
//   0x8f4090: 000090e5  ldr r0, [r0]
//   0x8f4094: f8008de5  str r0, [sp, #0xf8]
//   0x8f4098: f8009de5  ldr r0, [sp, #0xf8]
//   0x8f409c: 100050e3  cmp r0, #0x10
//   0x8f40a0: caffff0a  beq #0x8f3fd0
//   0x8f40a4: 580f9fe5  ldr r0, [pc, #0xf58]   ; lit[0x8f5004]=0x48e592
//   0x8f40a8: 08209be5  ldr r2, [fp, #8]
//   0x8f40ac: 00108fe0  add r1, pc, r0   ; -> 0xd82646 (add)
inline constexpr u32 PinEmblemSuffixWords[] = {0xe1a0500b, 0xe5b50a44, 0xe590100c, 0xe1a00005, 0xe12fff31, 0xe59f0f8c, 0xe5952004, 0xe08f1000, 0xe1a0000a, 0xebf62556, 0xe5d50020, 0xe3500000, 0x0affffd0, 0xe59d0044, 0xe5900000, 0xe58d00f8, 0xe59d00f8, 0xe3500010, 0x0affffca, 0xe59f0f58, 0xe59b2008, 0xe08f1000};
inline constexpr Pin PinEmblemSuffix{0x8f4058, PinEmblemSuffixWords, "EmblemSuffix"};

// PlayerInput: player input obj = [[SE+0x520]+0x34][p] + 0x1c (p < u32 [SE+0x520]+0x2c)
//   0x947744: 10402de9  push {r4, lr}
//   0x947748: 0040a0e1  mov r4, r0
//   0x94774c: aafaffeb  bl #0x9461fc
//   0x947750: 000050e3  cmp r0, #0
//   0x947754: 0020a0e3  mov r2, #0
//   0x947758: 0010a0e3  mov r1, #0
//   0x94775c: 20259015  ldrne r2, [r0, #0x520]
//   0x947760: 2c0092e5  ldr r0, [r2, #0x2c]
//   0x947764: 040050e1  cmp r0, r4
//   0x947768: 0100009a  bls #0x947774
//   0x94776c: 340092e5  ldr r0, [r2, #0x34]
//   0x947770: 041190e7  ldr r1, [r0, r4, lsl #2]
//   0x947774: 1c0081e2  add r0, r1, #0x1c
//   0x947778: 1080bde8  pop {r4, pc}
inline constexpr u32 PinPlayerInputWords[] = {0xe92d4010, 0xe1a04000, 0xebfffaaa, 0xe3500000, 0xe3a02000, 0xe3a01000, 0x15902520, 0xe592002c, 0xe1500004, 0x9a000001, 0xe5920034, 0xe7901104, 0xe281001c, 0xe8bd8010};
inline constexpr Pin PinPlayerInput{0x947744, PinPlayerInputWords, "PlayerInput"};

// HornEvent: one-shot horn request u8 obj+0x321: word +0x110 |= 0x100, then +0x321 = 0 (main+0x859fd4)
//   0x859f68: 2113d4e5  ldrb r1, [r4, #0x321]
//   0x859f6c: 100194e5  ldr r0, [r4, #0x110]
//   0x859f70: 000051e3  cmp r1, #0
//   0x859f74: 010c8013  orrne r0, r0, #0x100
//   0x859f78: 10018415  strne r0, [r4, #0x110]
//   0x859f7c: 042010e2  ands r2, r0, #4
//   0x859f80: 0030a003  moveq r3, #0
//   0x859f84: 2233c405  strbeq r3, [r4, #0x322]
//   0x859f88: 343394e5  ldr r3, [r4, #0x334]
//   0x859f8c: 000053e3  cmp r3, #0
//   0x859f90: 0600001a  bne #0x859fb0
//   0x859f94: ce3fa0e3  mov r3, #0x338
//   0x859f98: b33094e1  ldrh r3, [r4, r3]
//   0x859f9c: 000053e3  cmp r3, #0
//   0x859fa0: 0200001a  bne #0x859fb0
//   0x859fa4: 2233d4e5  ldrb r3, [r4, #0x322]
//   0x859fa8: 000053e3  cmp r3, #0
//   0x859fac: 2c00000a  beq #0x85a064
//   0x859fb0: 000051e3  cmp r1, #0
//   0x859fb4: 0100001a  bne #0x859fc0
//   0x859fb8: 1f04c8e7  bfc r0, #8, #1
//   0x859fbc: 100184e5  str r0, [r4, #0x110]
//   0x859fc0: 410f00e2  and r0, r0, #0x104
//   0x859fc4: 040050e3  cmp r0, #4
//   0x859fc8: 0100a003  moveq r0, #1
//   0x859fcc: 2203c405  strbeq r0, [r4, #0x322]
//   0x859fd0: 0000a0e3  mov r0, #0
//   0x859fd4: 2103c4e5  strb r0, [r4, #0x321]
inline constexpr u32 PinHornEventWords[] = {0xe5d41321, 0xe5940110, 0xe3510000, 0x13800c01, 0x15840110, 0xe2102004, 0x03a03000, 0x05c43322, 0xe5943334, 0xe3530000, 0x1a000006, 0xe3a03fce, 0xe19430b3, 0xe3530000, 0x1a000002, 0xe5d43322, 0xe3530000, 0x0a00002c, 0xe3510000, 0x1a000001, 0xe7c8041f, 0xe5840110, 0xe2000f41, 0xe3500004, 0x03a00001, 0x05c40322, 0xe3a00000, 0xe5c40321};
inline constexpr Pin PinHornEvent{0x859f68, PinHornEventWords, "HornEvent"};

// HornFill: kart input fill: horn byte (+7) = bit 8 of player action word +0x110
//   0x193238: 040096e5  ldr r0, [r6, #4]
//   0x19323c: 4ed11eeb  bl #0x94777c
//   0x193240: 0050a0e1  mov r5, r0
//   0x193244: 0030a0e3  mov r3, #0
//   0x193248: 100195e5  ldr r0, [r5, #0x110]
//   0x19324c: 5004e0e7  ubfx r0, r0, #8, #1
//   0x193250: 0700c4e5  strb r0, [r4, #7]
inline constexpr u32 PinHornFillWords[] = {0xe5960004, 0xeb1ed14e, 0xe1a05000, 0xe3a03000, 0xe5950110, 0xe7e00450, 0xe5c40007};
inline constexpr Pin PinHornFill{0x193238, PinHornFillWords, "HornFill"};

// HornBit: V+0x178 bit 16 = u8 V+0x123 (horn)
//   0x18cd68: 781194e5  ldr r1, [r4, #0x178]
//   0x18cd6c: 2301d4e5  ldrb r0, [r4, #0x123]
//   0x18cd70: 0120a0e1  mov r2, r1
//   0x18cd74: 000050e3  cmp r0, #0
//   0x18cd78: 0400a0e1  mov r0, r4
//   0x18cd7c: 1f28d0e7  bfc r2, #0x10, #1
//   0x18cd80: 01288113  orrne r2, r1, #0x10000
//   0x18cd84: 0510a0e1  mov r1, r5
//   0x18cd88: 782184e5  str r2, [r4, #0x178]
inline constexpr u32 PinHornBitWords[] = {0xe5941178, 0xe5d40123, 0xe1a02001, 0xe3500000, 0xe1a00004, 0xe7d0281f, 0x13812801, 0xe1a01005, 0xe5842178};
inline constexpr Pin PinHornBit{0x18cd68, PinHornBitWords, "HornBit"};

// HornRequest: sound obj +0x11a7 = bit 0 of u16 V+0x17a
//   0x1459f0: 041094e5  ldr r1, [r4, #4]
//   0x1459f4: 7a2100e3  movw r2, #0x17a
//   0x1459f8: b21091e1  ldrh r1, [r1, r2]
//   0x1459fc: a72101e3  movw r2, #0x11a7
//   0x145a00: 011001e2  and r1, r1, #1
//   0x145a04: 0210c0e7  strb r1, [r0, r2]
inline constexpr u32 PinHornRequestWords[] = {0xe5941004, 0xe300217a, 0xe19110b2, 0xe30121a7, 0xe2011001, 0xe7c01002};
inline constexpr Pin PinHornRequest{0x1459f0, PinHornRequestWords, "HornRequest"};

// HornLatch: +0x11a8 = +0x11a7; +0x11a7 = 0
//   0x0b8674: a70101e3  movw r0, #0x11a7
//   0x0b8678: 0000f4e7  ldrb r0, [r4, r0]!
//   0x0b867c: 0100c4e5  strb r0, [r4, #1]
//   0x0b8680: 0010c4e5  strb r1, [r4]
inline constexpr u32 PinHornLatchWords[] = {0xe30101a7, 0xe7f40000, 0xe5c40001, 0xe5c41000};
inline constexpr Pin PinHornLatch{0xb8674, PinHornLatchWords, "HornLatch"};

// HornPlay: play when +0x11a7 == 0 and +0x11a8 != 0 (release edge)
//   0x0b8eac: a70101e3  movw r0, #0x11a7
//   0x0b8eb0: 0000d4e7  ldrb r0, [r4, r0]
//   0x0b8eb4: 000050e3  cmp r0, #0
//   0x0b8eb8: 0100000a  beq #0xb8ec4
//   0x0b8ebc: b0d08de2  add sp, sp, #0xb0
//   0x0b8ec0: 7080bde8  pop {r4, r5, r6, pc}
//   0x0b8ec4: a80101e3  movw r0, #0x11a8
//   0x0b8ec8: 0000d4e7  ldrb r0, [r4, r0]
//   0x0b8ecc: 000050e3  cmp r0, #0
//   0x0b8ed0: f9ffff0a  beq #0xb8ebc
inline constexpr u32 PinHornPlayWords[] = {0xe30101a7, 0xe7d40000, 0xe3500000, 0x0a000001, 0xe28dd0b0, 0xe8bd8070, 0xe30101a8, 0xe7d40000, 0xe3500000, 0x0afffff9};
inline constexpr Pin PinHornPlay{0xb8eac, PinHornPlayWords, "HornPlay"};

// CourseTokens: course enum text (ldr lit; add pc)
//   0x01c540: 28019fe5  ldr r0, [pc, #0x128]   ; lit[0x1c670]=0xd79c38
//   0x01c544: 048096e5  ldr r8, [r6, #4]
//   0x01c548: 00008fe0  add r0, pc, r0   ; -> 0xd96188 (add)
inline constexpr u32 PinCourseTokensWords[] = {0xe59f0128, 0xe5968004, 0xe08f0000};
inline constexpr Pin PinCourseTokens{0x1c540, PinCourseTokensWords, "CourseTokens"};

// CourseTokensLit: literal for CourseTokens' add pc at 0x1c548 -> rodata 0xd96188
//   0x01c670: 389cd700  sbcseq sb, r7, r8, lsr ip
inline constexpr u32 PinCourseTokensLitWords[] = {0x00d79c38};
inline constexpr Pin PinCourseTokensLit{0x1c670, PinCourseTokensLitWords, "CourseTokensLit"};

// CourseTokenIndex: token index = raw + 1
//   0x939028: 010080e2  add r0, r0, #1
//   0x93902c: 178ddbeb  bl #0x1c490
inline constexpr u32 PinCourseTokenIndexWords[] = {0xe2800001, 0xebdb8d17};
inline constexpr Pin PinCourseTokenIndex{0x939028, PinCourseTokenIndexWords, "CourseTokenIndex"};
