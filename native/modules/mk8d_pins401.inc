// Ported from the 4.0.0 pins to the 4.0.1 main (build 158C0C7399AABD7B): every pin is the
// 4.0.0 instruction sequence found in 4.0.1 with only PC-relative targets (ADRP/ADR, B/BL,
// B.cond/CBZ/TBZ, LDR literal) and ADRP-paired low12 offsets differing. Disassembly quoted for review.

// RaceInfoHolder: root cell (ADRP+LDR) -> [[cell]] = root; [root+0x28]+0x240
//   0x7e7404: a8580090  adrp x8, #0x12fb000
//   0x7e7408: 08c541f9  ldr x8, [x8, #0x388]
//   0x7e740c: 080140f9  ldr x8, [x8]
//   0x7e7410: 081540f9  ldr x8, [x8, #0x28]
//   0x7e7414: 002141f9  ldr x0, [x8, #0x240]
//   0x7e7418: c0035fd6  ret
inline constexpr u32 PinRaceInfoHolderWords[] = {0x900058a8, 0xf941c508, 0xf9400108, 0xf9401508, 0xf9412100, 0xd65f03c0};
inline constexpr Pin PinRaceInfoHolder{0x7e7404, PinRaceInfoHolderWords, "RaceInfoHolder"};

// RaceInfo: RI = [holder+0x10]
//   0x86f7bc: fd7bbfa9  stp x29, x30, [sp, #-0x10]!
//   0x86f7c0: fd030091  mov x29, sp
//   0x86f7c4: 10dffd97  bl #0x7e7404
//   0x86f7c8: 000840f9  ldr x0, [x0, #0x10]
//   0x86f7cc: fd7bc1a8  ldp x29, x30, [sp], #0x10
//   0x86f7d0: c0035fd6  ret
inline constexpr u32 PinRaceInfoWords[] = {0xa9bf7bfd, 0x910003fd, 0x97fddf10, 0xf9400800, 0xa8c17bfd, 0xd65f03c0};
inline constexpr Pin PinRaceInfo{0x86f7bc, PinRaceInfoWords, "RaceInfo"};

// ActiveScene: scene = [[root+0x20]+8], active if u8 +0x180 & 4
//   0x7e7434: a8580090  adrp x8, #0x12fb000
//   0x7e7438: 08c541f9  ldr x8, [x8, #0x388]
//   0x7e743c: 080140f9  ldr x8, [x8]
//   0x7e7440: 081140f9  ldr x8, [x8, #0x20]
//   0x7e7444: 080540f9  ldr x8, [x8, #8]
//   0x7e7448: 09014639  ldrb w9, [x8, #0x180]
//   0x7e744c: 3f011e72  tst w9, #4
//   0x7e7450: e003889a  csel x0, xzr, x8, eq
//   0x7e7454: c0035fd6  ret
inline constexpr u32 PinActiveSceneWords[] = {0x900058a8, 0xf941c508, 0xf9400108, 0xf9401108, 0xf9400508, 0x39460109, 0x721e013f, 0x9a8803e0, 0xd65f03c0};
inline constexpr Pin PinActiveScene{0x7e7434, PinActiveSceneWords, "ActiveScene"};

// SceneKind: scene kind u32 +0x184
//   0x7e7458: a9580090  adrp x9, #0x12fb000
//   0x7e745c: 29c541f9  ldr x9, [x9, #0x388]
//   0x7e7460: 290140f9  ldr x9, [x9]
//   0x7e7464: 291140f9  ldr x9, [x9, #0x20]
//   0x7e7468: 290540f9  ldr x9, [x9, #8]
//   0x7e746c: 2a8141b9  ldr w10, [x9, #0x180]
//   0x7e7470: 4a011e12  and w10, w10, #4
//   0x7e7474: 3f0100f1  cmp x9, #0
//   0x7e7478: 4419407a  ccmp w10, #0, #4, ne
//   0x7e747c: 81000054  b.ne #0x7e748c
//   0x7e7480: e9031f2a  mov w9, wzr
//   0x7e7484: 090100b9  str w9, [x8]
//   0x7e7488: c0035fd6  ret
//   0x7e748c: 298541b9  ldr w9, [x9, #0x184]
//   0x7e7490: 090100b9  str w9, [x8]
//   0x7e7494: c0035fd6  ret
inline constexpr u32 PinSceneKindWords[] = {0x900058a9, 0xf941c529, 0xf9400129, 0xf9401129, 0xf9400529, 0xb941812a, 0x121e014a, 0xf100013f, 0x7a401944, 0x54000081, 0x2a1f03e9, 0xb9000109, 0xd65f03c0, 0xb9418529, 0xb9000109, 0xd65f03c0};
inline constexpr Pin PinSceneKind{0x7e7458, PinSceneKindWords, "SceneKind"};

// Phase: OE = [scene+0x190+8*4], RD = [OE+0x218], SEQ = [RD+0x50], phase u32 SEQ+0x38
//   0x86e71c: f30f1ef8  str x19, [sp, #-0x20]!
//   0x86e720: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x86e724: fd430091  add x29, sp, #0x10
//   0x86e728: 43e3fd97  bl #0x7e7434
//   0x86e72c: c00200b4  cbz x0, #0x86e784
//   0x86e730: f3031e32  mov w19, #4
//   0x86e734: f30b00b9  str w19, [sp, #8]
//   0x86e738: e80b40b9  ldr w8, [sp, #8]
//   0x86e73c: 3ee3fd97  bl #0x7e7434
//   0x86e740: 000100b4  cbz x0, #0x86e760
//   0x86e744: b3c31fb8  stur w19, [x29, #-4]
//   0x86e748: a8c39fb8  ldursw x8, [x29, #-4]
//   0x86e74c: 1f290071  cmp w8, #0xa
//   0x86e750: 08319f9a  csel x8, x8, xzr, lo
//   0x86e754: 080c088b  add x8, x0, x8, lsl #3
//   0x86e758: 08c940f9  ldr x8, [x8, #0x190]
//   0x86e75c: 02000014  b #0x86e764
//   0x86e760: e8031faa  mov x8, xzr
//   0x86e764: 080d41f9  ldr x8, [x8, #0x218]
//   0x86e768: 082940f9  ldr x8, [x8, #0x50]
//   0x86e76c: 083940b9  ldr w8, [x8, #0x38]
//   0x86e770: fd7b41a9  ldp x29, x30, [sp, #0x10]
//   0x86e774: 1f150071  cmp w8, #5
//   0x86e778: e0179f1a  cset w0, eq
//   0x86e77c: f30742f8  ldr x19, [sp], #0x20
//   0x86e780: c0035fd6  ret
inline constexpr u32 PinPhaseWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x97fde343, 0xb40002c0, 0x321e03f3, 0xb9000bf3, 0xb9400be8, 0x97fde33e, 0xb4000100, 0xb81fc3b3, 0xb89fc3a8, 0x7100291f, 0x9a9f3108, 0x8b080c08, 0xf940c908, 0x14000002, 0xaa1f03e8, 0xf9410d08, 0xf9402908, 0xb9403908, 0xa9417bfd, 0x7100151f, 0x1a9f17e0, 0xf84207f3, 0xd65f03c0};
inline constexpr Pin PinPhase{0x86e71c, PinPhaseWords, "Phase"};

// EngineClass: cc = 50*u32 RI+0x10 (+50)
//   0x50cdf8: 718a0d94  bl #0x86f7bc
//   0x50cdfc: 970a40b9  ldr w23, [x20, #8]
//   0x50ce00: 081040b9  ldr w8, [x0, #0x10]
//   0x50ce04: 49068052  mov w9, #0x32
//   0x50ce08: 087d091b  mul w8, w8, w9
inline constexpr u32 PinEngineClassWords[] = {0x940d8a71, 0xb9400a97, 0xb9401008, 0x52800649, 0x1b097d08};
inline constexpr Pin PinEngineClass{0x50cdf8, PinEngineClassWords, "EngineClass"};

// Mirror: mirror u8 RI+0x26
//   0x507e4c: 5c9e0d94  bl #0x86f7bc
//   0x507e50: 08984039  ldrb w8, [x0, #0x26]
//   0x507e54: 28010034  cbz w8, #0x507e78
inline constexpr u32 PinMirrorWords[] = {0x940d9e5c, 0x39409808, 0x34000128};
inline constexpr Pin PinMirror{0x507e4c, PinMirrorWords, "Mirror"};

// MapYScale: course u32 RI+0x1a0 == 0x44 -> Y scale
//   0x508c24: e69a0d94  bl #0x86f7bc
//   0x508c28: 08a041b9  ldr w8, [x0, #0x1a0]
//   0x508c2c: 1f110171  cmp w8, #0x44
inline constexpr u32 PinMapYScaleWords[] = {0x940d9ae6, 0xb941a008, 0x7101111f};
inline constexpr Pin PinMapYScale{0x508c24, PinMapYScaleWords, "MapYScale"};

// MapYScaleMul: Y scale float at rodata (ADRP+LDR s0)
//   0x508c4c: 694e00d0  adrp x9, #0xed6000
//   0x508c50: 201547bd  ldr s0, [x9, #0x714]
inline constexpr u32 PinMapYScaleMulWords[] = {0xd0004e69, 0xbd471520};
inline constexpr Pin PinMapYScaleMul{0x508c4c, PinMapYScaleMulWords, "MapYScaleMul"};

// RaceCount: racer count s32 RI+0x180
//   0x532bc4: 1c8081b9  ldrsw x28, [x0, #0x180]
inline constexpr u32 PinRaceCountWords[] = {0xb981801c};
inline constexpr Pin PinRaceCount{0x532bc4, PinRaceCountWords, "RaceCount"};

// RaceRule: rule u32 RI+0x08
//   0x512b7c: 10730d94  bl #0x86f7bc
//   0x512b80: 080840b9  ldr w8, [x0, #8]
inline constexpr u32 PinRaceRuleWords[] = {0x940d7310, 0xb9400808};
inline constexpr Pin PinRaceRule{0x512b7c, PinRaceRuleWords, "RaceRule"};

// LapsTotal: LM = [RD+0x58], laps u8 LM+0x64
//   0x512b58: 080d41f9  ldr x8, [x8, #0x218]
//   0x512b5c: 082d40f9  ldr x8, [x8, #0x58]
//   0x512b60: 680000b4  cbz x8, #0x512b6c
//   0x512b64: 08914139  ldrb w8, [x8, #0x64]
inline constexpr u32 PinLapsTotalWords[] = {0xf9410d08, 0xf9402d08, 0xb4000068, 0x39419108};
inline constexpr Pin PinLapsTotal{0x512b58, PinLapsTotalWords, "LapsTotal"};

// Timer: TM = [RD+0xe0] (hud+0xb8 = RD), time struct TM+0x40
//   0x515ac0: 685e40f9  ldr x8, [x19, #0xb8]
//   0x515ac4: 736a40f9  ldr x19, [x19, #0xd0]
//   0x515ac8: e1031f2a  mov w1, wzr
//   0x515acc: 087140f9  ldr x8, [x8, #0xe0]
//   0x515ad0: 00010191  add x0, x8, #0x40
inline constexpr u32 PinTimerWords[] = {0xf9405e68, 0xf9406a73, 0x2a1f03e1, 0xf9407108, 0x91010100};
inline constexpr Pin PinTimer{0x515ac0, PinTimerWords, "Timer"};

// TimerMin: u8 T+4 minutes
//   0x515bec: 81124039  ldrb w1, [x20, #4]
inline constexpr u32 PinTimerMinWords[] = {0x39401281};
inline constexpr Pin PinTimerMin{0x515bec, PinTimerMinWords, "TimerMin"};

// TimerSec: u8 T+5 seconds
//   0x515bfc: 81164039  ldrb w1, [x20, #5]
inline constexpr u32 PinTimerSecWords[] = {0x39401681};
inline constexpr Pin PinTimerSec{0x515bfc, PinTimerSecWords, "TimerSec"};

// TimerMs: u16 T+6 milliseconds
//   0x515dcc: 810e4079  ldrh w1, [x20, #6]
inline constexpr u32 PinTimerMsWords[] = {0x79400e81};
inline constexpr Pin PinTimerMs{0x515dcc, PinTimerMsWords, "TimerMs"};

// Checkers: count u32 RD+0x60, CK[i] = [[RD+0x68]+8i]
//   0x512958: 080d41f9  ldr x8, [x8, #0x218]
//   0x51295c: 694e4339  ldrb w9, [x19, #0xd3]
//   0x512960: 0a6140b9  ldr w10, [x8, #0x60]
//   0x512964: 5f01096b  cmp w10, w9
//   0x512968: 4481497a  ccmp w10, w9, #4, hi
//   0x51296c: 8d000054  b.le #0x51297c
//   0x512970: 083540f9  ldr x8, [x8, #0x68]
//   0x512974: 087969f8  ldr x8, [x8, x9, lsl #3]
inline constexpr u32 PinCheckersWords[] = {0xf9410d08, 0x39434e69, 0xb940610a, 0x6b09015f, 0x7a498144, 0x5400008d, 0xf9403508, 0xf8697908};
inline constexpr Pin PinCheckers{0x512958, PinCheckersWords, "Checkers"};

// CheckerRank: rank u16 CK+0x40
//   0x5139b8: 08814079  ldrh w8, [x8, #0x40]
inline constexpr u32 PinCheckerRankWords[] = {0x79408108};
inline constexpr Pin PinCheckerRank{0x5139b8, PinCheckerRankWords, "CheckerRank"};

// CheckerCoins: coins u8 CK+0x50
//   0x512e20: 685e40f9  ldr x8, [x19, #0xb8]
//   0x512e24: 14414139  ldrb w20, [x8, #0x50]
inline constexpr u32 PinCheckerCoinsWords[] = {0xf9405e68, 0x39414114};
inline constexpr Pin PinCheckerCoins{0x512e20, PinCheckerCoinsWords, "CheckerCoins"};

// CheckerLap: lap u32 CK+0x44, shown min(v+1, total)
//   0x512e78: 685e40f9  ldr x8, [x19, #0xb8]
//   0x512e7c: 084540b9  ldr w8, [x8, #0x44]
//   0x512e80: 6a4a4339  ldrb w10, [x19, #0xd2]
//   0x512e84: 09050011  add w9, w8, #1
//   0x512e88: 291d0012  and w9, w9, #0xff
//   0x512e8c: 3f010a6b  cmp w9, w10
//   0x512e90: 69464339  ldrb w9, [x19, #0xd1]
//   0x512e94: 4885881a  csinc w8, w10, w8, hi
inline constexpr u32 PinCheckerLapWords[] = {0xf9405e68, 0xb9404508, 0x39434a6a, 0x11000509, 0x12001d29, 0x6b0a013f, 0x39434669, 0x1a888548};
inline constexpr Pin PinCheckerLap{0x512e78, PinCheckerLapWords, "CheckerLap"};

// CheckerGoalCheck: checker update: u32 CK+0x44 lap >= u8 LM+0x64 -> goal path
//   0x871100: 694640b9  ldr w9, [x19, #0x44]
//   0x871104: 8a924139  ldrb w10, [x20, #0x64]
//   0x871108: 3f010a6b  cmp w9, w10
inline constexpr u32 PinCheckerGoalCheckWords[] = {0xb9404669, 0x3941928a, 0x6b0a013f};
inline constexpr Pin PinCheckerGoalCheck{0x871100, PinCheckerGoalCheckWords, "CheckerGoalCheck"};

// CheckerGoalSet: goal: u32 CK+0x3c |= 1 (main+0x880ff0)
//   0x870ba8: 683e40b9  ldr w8, [x19, #0x3c]
//   0x870bac: 08010032  orr w8, w8, #1
//   0x870bb0: 683e00b9  str w8, [x19, #0x3c]
inline constexpr u32 PinCheckerGoalSetWords[] = {0xb9403e68, 0x32000108, 0xb9003e68};
inline constexpr Pin PinCheckerGoalSet{0x870ba8, PinCheckerGoalSetWords, "CheckerGoalSet"};

// CheckerGoalDone: after goal: u32 CK+0x3c |= 4 (main+0x8810e0)
//   0x870c10: 683e40b9  ldr w8, [x19, #0x3c]
//   0x870c14: 08011e32  orr w8, w8, #4
//   0x870c18: 683e00b9  str w8, [x19, #0x3c]
inline constexpr u32 PinCheckerGoalDoneWords[] = {0xb9403e68, 0x321e0108, 0xb9003e68};
inline constexpr Pin PinCheckerGoalDone{0x870c10, PinCheckerGoalDoneWords, "CheckerGoalDone"};

// DriverEntry: driver u32 [entry+0x40-4] (RI+0x3c+0x1c*i)
//   0x532d48: e8c25fb8  ldur w8, [x23, #-4]
inline constexpr u32 PinDriverEntryWords[] = {0xb85fc2e8};
inline constexpr Pin PinDriverEntry{0x532d48, PinDriverEntryWords, "DriverEntry"};

// DriverVariant: variant u8 RI+0x40+0x1c*i
//   0x532d58: e3024039  ldrb w3, [x23]
inline constexpr u32 PinDriverVariantWords[] = {0x394002e3};
inline constexpr Pin PinDriverVariant{0x532d58, PinDriverVariantWords, "DriverVariant"};

// KartDirector: KD = [OE+0x238], kart count s32 KD+0xc0
//   0x5083dc: 081d41f9  ldr x8, [x8, #0x238]
//   0x5083e0: 18c180b9  ldrsw x24, [x8, #0xc0]
inline constexpr u32 PinKartDirectorWords[] = {0xf9411d08, 0xb980c118};
inline constexpr Pin PinKartDirector{0x5083dc, PinKartDirectorWords, "KartDirector"};

// KartUnit: KU[i] = [KD+0x50+8i], i < 12
//   0x508440: 081d41f9  ldr x8, [x8, #0x238]
//   0x508444: 3f310071  cmp w9, #0xc
//   0x508448: 29319f9a  csel x9, x9, xzr, lo
//   0x50844c: 080d098b  add x8, x8, x9, lsl #3
//   0x508450: 152940f9  ldr x21, [x8, #0x50]
inline constexpr u32 PinKartUnitWords[] = {0xf9411d08, 0x7100313f, 0x9a9f3129, 0x8b090d08, 0xf9402915};
inline constexpr Pin PinKartUnit{0x508440, PinKartUnitWords, "KartUnit"};

// KartUnitPlayer: KU vtable slot 0: V = [KU+0x48], player id u32 V+0xa8
//   0x141e3c: 082440f9  ldr x8, [x0, #0x48]
//   0x141e40: 00a940b9  ldr w0, [x8, #0xa8]
//   0x141e44: c0035fd6  ret
inline constexpr u32 PinKartUnitPlayerWords[] = {0xf9402408, 0xb940a900, 0xd65f03c0};
inline constexpr Pin PinKartUnitPlayer{0x141e3c, PinKartUnitPlayerWords, "KartUnitPlayer"};

// KartUnitPos: KU vtable slot 1: slot3() + 0x24
//   0x141e48: fd7bbfa9  stp x29, x30, [sp, #-0x10]!
//   0x141e4c: fd030091  mov x29, sp
//   0x141e50: 080040f9  ldr x8, [x0]
//   0x141e54: 080d40f9  ldr x8, [x8, #0x18]
//   0x141e58: 00013fd6  blr x8
//   0x141e5c: 00900091  add x0, x0, #0x24
//   0x141e60: fd7bc1a8  ldp x29, x30, [sp], #0x10
inline constexpr u32 PinKartUnitPosWords[] = {0xa9bf7bfd, 0x910003fd, 0xf9400008, 0xf9400d08, 0xd63f0100, 0x91009000, 0xa8c17bfd};
inline constexpr Pin PinKartUnitPos{0x141e48, PinKartUnitPosWords, "KartUnitPos"};

// KartUnitMove: KU vtable slot 3: [KU+0x50] + 8
//   0x141e84: 082840f9  ldr x8, [x0, #0x50]
//   0x141e88: 00210091  add x0, x8, #8
//   0x141e8c: c0035fd6  ret
inline constexpr u32 PinKartUnitMoveWords[] = {0xf9402808, 0x91002100, 0xd65f03c0};
inline constexpr Pin PinKartUnitMove{0x141e84, PinKartUnitMoveWords, "KartUnitMove"};

// ItemDirector: ID = [OE+0x240], owners [[ID+0xe8]+8i], i < u32 ID+0xe0
//   0x510ee8: 082141f9  ldr x8, [x8, #0x240]
//   0x510eec: 69c280b9  ldrsw x9, [x19, #0xc0]
//   0x510ef0: 0ae140b9  ldr w10, [x8, #0xe0]
//   0x510ef4: 5f01096b  cmp w10, w9
//   0x510ef8: 89000054  b.ls #0x510f08
//   0x510efc: 087540f9  ldr x8, [x8, #0xe8]
//   0x510f00: 087969f8  ldr x8, [x8, x9, lsl #3]
inline constexpr u32 PinItemDirectorWords[] = {0xf9412108, 0xb980c269, 0xb940e10a, 0x6b09015f, 0x54000089, 0xf9407508, 0xf8697908};
inline constexpr Pin PinItemDirector{0x510ee8, PinItemDirectorWords, "ItemDirector"};

// ItemOwnerCreate: owners: for i < RI+0x180: owner = new Owner(i); append to [ID+0xe8] (count ID+0xe0)
//   0x2b04: 2eb32194  bl #0x86f7bc
//   0x2b08: 158041b9  ldr w21, [x0, #0x180]
//   0x2b0c: 60820391  add x0, x19, #0xe0
//   0x2b10: e3031d32  mov w3, #8
//   0x2b14: e103152a  mov w1, w21
//   0x2b18: e2031faa  mov x2, xzr
//   0x2b1c: e2e11794  bl #0x5fb2a4
//   0x2b20: bf060071  cmp w21, #1
//   0x2b24: 2b030054  b.lt #0x2b88
//   0x2b28: f6031f2a  mov w22, wzr
//   0x2b2c: 17348052  mov w23, #0x1a0
//   0x2b30: e00317aa  mov x0, x23
//   0x2b34: 8bdf1794  bl #0x5fa960
//   0x2b38: e103162a  mov w1, w22
//   0x2b3c: f80300aa  mov x24, x0
//   0x2b40: e5f20094  bl #0x3f6d4
//   0x2b44: 68265c29  ldp w8, w9, [x19, #0xe0]
//   0x2b48: 087d4093  sxtw x8, w8
//   0x2b4c: 1f01096b  cmp w8, w9
//   0x2b50: ca000054  b.ge #0x2b68
//   0x2b54: 697640f9  ldr x9, [x19, #0xe8]
//   0x2b58: 387928f8  str x24, [x9, x8, lsl #3]
//   0x2b5c: 68e240b9  ldr w8, [x19, #0xe0]
//   0x2b60: 08050011  add w8, w8, #1
//   0x2b64: 68e200b9  str w8, [x19, #0xe0]
inline constexpr u32 PinItemOwnerCreateWords[] = {0x9421b32e, 0xb9418015, 0x91038260, 0x321d03e3, 0x2a1503e1, 0xaa1f03e2, 0x9417e1e2, 0x710006bf, 0x5400032b, 0x2a1f03f6, 0x52803417, 0xaa1703e0, 0x9417df8b, 0x2a1603e1, 0xaa0003f8, 0x9400f2e5, 0x295c2668, 0x93407d08, 0x6b09011f, 0x540000ca, 0xf9407669, 0xf8287938, 0xb940e268, 0x11000508, 0xb900e268};
inline constexpr Pin PinItemOwnerCreate{0x2b04, PinItemOwnerCreateWords, "ItemOwnerCreate"};

// ItemOwnerCtor: Owner ctor stores player index w1 at +0x40 (stp wzr,w20,[x19,#0x3c])
//   0x3f6e0: f403012a  mov w20, w1
//   0x3f6e4: f30300aa  mov x19, x0
//   0x3f6e8: 7ea61d94  bl #0x7a90e0
//   0x3f6ec: 7fe20039  strb wzr, [x19, #0x38]
//   0x3f6f0: c89500f0  adrp x8, #0x12fa000
//   0x3f6f4: 087145f9  ldr x8, [x8, #0xae0]
//   0x3f6f8: 09008012  mov w9, #-1
//   0x3f6fc: 7f7e05a9  stp xzr, xzr, [x19, #0x50]
//   0x3f700: 7f2600f9  str xzr, [x19, #0x48]
//   0x3f704: 08410091  add x8, x8, #0x10
//   0x3f708: 7fd20729  stp wzr, w20, [x19, #0x3c]
inline constexpr u32 PinItemOwnerCtorWords[] = {0x2a0103f4, 0xaa0003f3, 0x941da67e, 0x3900e27f, 0xf00095c8, 0xf9457108, 0x12800009, 0xa9057e7f, 0xf900267f, 0x91004108, 0x2907d27f};
inline constexpr Pin PinItemOwnerCtor{0x3f6e0, PinItemOwnerCtorWords, "ItemOwnerCtor"};

// ItemSlotItem: slot k<2 = [IO+0x60+8k], item u32 S+0xc8
//   0x40fe4: 297c4093  sxtw x9, w1
//   0x40fe8: 3f080071  cmp w1, #2
//   0x40fec: 29319f9a  csel x9, x9, xzr, lo
//   0x40ff0: 090c098b  add x9, x0, x9, lsl #3
//   0x40ff4: 293140f9  ldr x9, [x9, #0x60]
//   0x40ff8: 29c940b9  ldr w9, [x9, #0xc8]
//   0x40ffc: 090100b9  str w9, [x8]
//   0x41000: c0035fd6  ret
inline constexpr u32 PinItemSlotItemWords[] = {0x93407c29, 0x7100083f, 0x9a9f3129, 0x8b090c09, 0xf9403129, 0xb940c929, 0xb9000109, 0xd65f03c0};
inline constexpr Pin PinItemSlotItem{0x40fe4, PinItemSlotItemWords, "ItemSlotItem"};

// ItemSlotState: state u8 S+0x41: 3 held, 1..2 roulette
//   0x45c14: 08044139  ldrb w8, [x0, #0x41]
//   0x45c18: 1f0d0071  cmp w8, #3
//   0x45c1c: e0179f1a  cset w0, eq
//   0x45c20: c0035fd6  ret
//   0x45c24: 08044139  ldrb w8, [x0, #0x41]
//   0x45c28: 08050051  sub w8, w8, #1
//   0x45c2c: 1f090071  cmp w8, #2
//   0x45c30: e0279f1a  cset w0, lo
//   0x45c34: c0035fd6  ret
//   0x45c38: 08044139  ldrb w8, [x0, #0x41]
//   0x45c3c: 1f090071  cmp w8, #2
//   0x45c40: e0179f1a  cset w0, eq
inline constexpr u32 PinItemSlotStateWords[] = {0x39410408, 0x71000d1f, 0x1a9f17e0, 0xd65f03c0, 0x39410408, 0x51000508, 0x7100091f, 0x1a9f27e0, 0xd65f03c0, 0x39410408, 0x7100091f, 0x1a9f17e0};
inline constexpr Pin PinItemSlotState{0x45c14, PinItemSlotStateWords, "ItemSlotState"};

// ItemSlotEmpty: state 0 = empty
//   0x45c6c: 08044139  ldrb w8, [x0, #0x41]
//   0x45c70: 68000034  cbz w8, #0x45c7c
inline constexpr u32 PinItemSlotEmptyWords[] = {0x39410408, 0x34000068};
inline constexpr Pin PinItemSlotEmpty{0x45c6c, PinItemSlotEmptyWords, "ItemSlotEmpty"};

// ItemSlotCount: uses left u32 S+0xa0 set from S+0x9c
//   0x453a8: 689e40b9  ldr w8, [x19, #0x9c]
//   0x453ac: e80f00b9  str w8, [sp, #0xc]
//   0x453b0: e80f40b9  ldr w8, [sp, #0xc]
//   0x453b4: 09050011  add w9, w8, #1
//   0x453b8: 3f550071  cmp w9, #0x15
//   0x453bc: 28010054  b.hi #0x453e0
//   0x453c0: ea760090  adrp x10, #0xf21000
//   0x453c4: 4a610a91  add x10, x10, #0x298
//   0x453c8: e8031f2a  mov w8, wzr
//   0x453cc: 4979a9b8  ldrsw x9, [x10, x9, lsl #2]
//   0x453d0: 29010a8b  add x9, x9, x10
//   0x453d4: 20011fd6  br x9
//   0x453d8: e8070032  mov w8, #3
//   0x453dc: 04000014  b #0x453ec
//   0x453e0: e8030032  mov w8, #1
//   0x453e4: 02000014  b #0x453ec
//   0x453e8: e8031d32  mov w8, #8
//   0x453ec: 68a200b9  str w8, [x19, #0xa0]
inline constexpr u32 PinItemSlotCountWords[] = {0xb9409e68, 0xb9000fe8, 0xb9400fe8, 0x11000509, 0x7100553f, 0x54000128, 0x900076ea, 0x910a614a, 0x2a1f03e8, 0xb8a97949, 0x8b0a0129, 0xd61f0120, 0x320007e8, 0x14000004, 0x320003e8, 0x14000002, 0x321d03e8, 0xb900a268};
inline constexpr Pin PinItemSlotCount{0x453a8, PinItemSlotCountWords, "ItemSlotCount"};

// ItemFrame: item -> pattern frame byte table (ADRP+ADD), coin override
//   0x423614: 085800f0  adrp x8, #0xf26000
//   0x423618: 08313391  add x8, x8, #0xccc
//   0x42361c: 1f3c0071  cmp w0, #0xf
//   0x423620: 13c96038  ldrb w19, [x8, w0, sxtw]
//   0x423624: c1030054  b.ne #0x42369c
//   0x423628: 65301194  bl #0x86f7bc
//   0x42362c: a82300d1  sub x8, x29, #8
//   0x423630: e02f1194  bl #0x86f5b0
//   0x423634: a8835fb8  ldur w8, [x29, #-8]
//   0x423638: 1f050071  cmp w8, #1
//   0x42363c: 61000054  b.ne #0x423648
//   0x423640: 33038052  mov w19, #0x19
//   0x423644: 16000014  b #0x42369c
//   0x423648: 5d301194  bl #0x86f7bc
//   0x42364c: e8630091  add x8, sp, #0x18
//   0x423650: d82f1194  bl #0x86f5b0
//   0x423654: e81b40b9  ldr w8, [sp, #0x18]
//   0x423658: 1f090071  cmp w8, #2
//   0x42365c: 61000054  b.ne #0x423668
//   0x423660: 53038052  mov w19, #0x1a
//   0x423664: 0e000014  b #0x42369c
//   0x423668: 55301194  bl #0x86f7bc
//   0x42366c: e8430091  add x8, sp, #0x10
//   0x423670: d02f1194  bl #0x86f5b0
//   0x423674: e81340b9  ldr w8, [sp, #0x10]
//   0x423678: 1f0d0071  cmp w8, #3
//   0x42367c: 00010054  b.eq #0x42369c
//   0x423680: 4f301194  bl #0x86f7bc
//   0x423684: e8230091  add x8, sp, #8
//   0x423688: ca2f1194  bl #0x86f5b0
//   0x42368c: e80b40b9  ldr w8, [sp, #8]
//   0x423690: 1f110071  cmp w8, #4
//   0x423694: a8038052  mov w8, #0x1d
//   0x423698: 1301931a  csel w19, w8, w19, eq
//   0x42369c: e003132a  mov w0, w19
//   0x4236a0: fd7b43a9  ldp x29, x30, [sp, #0x30]
//   0x4236a4: f31340f9  ldr x19, [sp, #0x20]
//   0x4236a8: ff030191  add sp, sp, #0x40
//   0x4236ac: c0035fd6  ret
inline constexpr u32 PinItemFrameWords[] = {0xf0005808, 0x91333108, 0x71003c1f, 0x3860c913, 0x540003c1, 0x94113065, 0xd10023a8, 0x94112fe0, 0xb85f83a8, 0x7100051f, 0x54000061, 0x52800333, 0x14000016, 0x9411305d, 0x910063e8, 0x94112fd8, 0xb9401be8, 0x7100091f, 0x54000061, 0x52800353, 0x1400000e, 0x94113055, 0x910043e8, 0x94112fd0, 0xb94013e8, 0x71000d1f, 0x54000100, 0x9411304f, 0x910023e8, 0x94112fca, 0xb9400be8, 0x7100111f, 0x528003a8, 0x1a930113, 0x2a1303e0, 0xa9437bfd, 0xf94013f3, 0x910103ff, 0xd65f03c0};
inline constexpr Pin PinItemFrame{0x423614, PinItemFrameWords, "ItemFrame"};

// ItemTripleFrame: HUD count <= 2 of a triple item (7/17/18/19) -> frame (count==2 ? 1/17/18/19 : 0/3/5/7)
//   0x5122d8: 287c4093  sxtw x8, w1
//   0x5122dc: 3f080071  cmp w1, #2
//   0x5122e0: 08319f9a  csel x8, x8, xzr, lo
//   0x5122e4: 0908088b  add x9, x0, x8, lsl #2
//   0x5122e8: 280942b9  ldr w8, [x9, #0x208]
//   0x5122ec: 1f090071  cmp w8, #2
//   0x5122f0: 4d000054  b.le #0x5122f8
//   0x5122f4: c0035fd6  ret
//   0x5122f8: 291142b9  ldr w9, [x9, #0x210]
//   0x5122fc: 291d0051  sub w9, w9, #7
//   0x512300: 3f310071  cmp w9, #0xc
//   0x512304: 88ffff54  b.hi #0x5122f4
//   0x512308: ca500090  adrp x10, #0xf2a000
//   0x51230c: 4a211691  add x10, x10, #0x588
//   0x512310: 4979a9b8  ldrsw x9, [x10, x9, lsl #2]
//   0x512314: 29010a8b  add x9, x9, x10
//   0x512318: 20011fd6  br x9
//   0x51231c: 0b3840f9  ldr x11, [x0, #0x70]
//   0x512320: 0a6840b9  ldr w10, [x0, #0x68]
//   0x512324: 29781f53  lsl w9, w1, #1
//   0x512328: 291d0011  add w9, w9, #7
//   0x51232c: 6ccd298b  add x12, x11, w9, sxtw #3
//   0x512330: 5f01096b  cmp w10, w9
//   0x512334: 89818b9a  csel x9, x12, x11, hi
//   0x512338: 1f090071  cmp w8, #2
//   0x51233c: 200140f9  ldr x0, [x9]
//   0x512340: e003271e  fmov s0, wzr
//   0x512344: 01102e1e  fmov s1, #1.00000000
//   0x512348: 24000014  b #0x5123d8
//   0x51234c: 0b3840f9  ldr x11, [x0, #0x70]
//   0x512350: 0a6840b9  ldr w10, [x0, #0x68]
//   0x512354: 29781f53  lsl w9, w1, #1
//   0x512358: 291d0011  add w9, w9, #7
//   0x51235c: 6ccd298b  add x12, x11, w9, sxtw #3
//   0x512360: 5f01096b  cmp w10, w9
//   0x512364: 89818b9a  csel x9, x12, x11, hi
//   0x512368: 1f090071  cmp w8, #2
//   0x51236c: 200140f9  ldr x0, [x9]
//   0x512370: 0010211e  fmov s0, #3.00000000
//   0x512374: 0130261e  fmov s1, #17.00000000
//   0x512378: 18000014  b #0x5123d8
//   0x51237c: 0b3840f9  ldr x11, [x0, #0x70]
//   0x512380: 0a6840b9  ldr w10, [x0, #0x68]
//   0x512384: 29781f53  lsl w9, w1, #1
//   0x512388: 291d0011  add w9, w9, #7
//   0x51238c: 6ccd298b  add x12, x11, w9, sxtw #3
//   0x512390: 5f01096b  cmp w10, w9
//   0x512394: 89818b9a  csel x9, x12, x11, hi
//   0x512398: 1f090071  cmp w8, #2
//   0x51239c: 200140f9  ldr x0, [x9]
//   0x5123a0: 0090221e  fmov s0, #5.00000000
//   0x5123a4: 0150261e  fmov s1, #18.00000000
//   0x5123a8: 0c000014  b #0x5123d8
//   0x5123ac: 0b3840f9  ldr x11, [x0, #0x70]
//   0x5123b0: 0a6840b9  ldr w10, [x0, #0x68]
//   0x5123b4: 29781f53  lsl w9, w1, #1
//   0x5123b8: 291d0011  add w9, w9, #7
//   0x5123bc: 6ccd298b  add x12, x11, w9, sxtw #3
//   0x5123c0: 5f01096b  cmp w10, w9
//   0x5123c4: 89818b9a  csel x9, x12, x11, hi
//   0x5123c8: 1f090071  cmp w8, #2
//   0x5123cc: 200140f9  ldr x0, [x9]
//   0x5123d0: 0090231e  fmov s0, #7.00000000
//   0x5123d4: 0170261e  fmov s1, #19.00000000
//   0x5123d8: 200c201e  fcsel s0, s1, s0, eq
//   0x5123dc: e1031f2a  mov w1, wzr
inline constexpr u32 PinItemTripleFrameWords[] = {0x93407c28, 0x7100083f, 0x9a9f3108, 0x8b080809, 0xb9420928, 0x7100091f, 0x5400004d, 0xd65f03c0, 0xb9421129, 0x51001d29, 0x7100313f, 0x54ffff88, 0x900050ca, 0x9116214a, 0xb8a97949, 0x8b0a0129, 0xd61f0120, 0xf940380b, 0xb940680a, 0x531f7829, 0x11001d29, 0x8b29cd6c, 0x6b09015f, 0x9a8b8189, 0x7100091f, 0xf9400120, 0x1e2703e0, 0x1e2e1001, 0x14000024, 0xf940380b, 0xb940680a, 0x531f7829, 0x11001d29, 0x8b29cd6c, 0x6b09015f, 0x9a8b8189, 0x7100091f, 0xf9400120, 0x1e211000, 0x1e263001, 0x14000018, 0xf940380b, 0xb940680a, 0x531f7829, 0x11001d29, 0x8b29cd6c, 0x6b09015f, 0x9a8b8189, 0x7100091f, 0xf9400120, 0x1e229000, 0x1e265001, 0x1400000c, 0xf940380b, 0xb940680a, 0x531f7829, 0x11001d29, 0x8b29cd6c, 0x6b09015f, 0x9a8b8189, 0x7100091f, 0xf9400120, 0x1e239000, 0x1e267001, 0x1e200c20, 0x2a1f03e1};
inline constexpr Pin PinItemTripleFrame{0x5122d8, PinItemTripleFrameWords, "ItemTripleFrame"};

// CoinKind: course coin kind from RI+0x1a0
//   0x86f5b0: ffc300d1  sub sp, sp, #0x30
//   0x86f5b4: f44f01a9  stp x20, x19, [sp, #0x10]
//   0x86f5b8: fd7b02a9  stp x29, x30, [sp, #0x20]
//   0x86f5bc: fd830091  add x29, sp, #0x20
//   0x86f5c0: f40300aa  mov x20, x0
//   0x86f5c4: f30308aa  mov x19, x8
//   0x86f5c8: 88a241b9  ldr w8, [x20, #0x1a0]
//   0x86f5cc: 1fcd0071  cmp w8, #0x33
//   0x86f5d0: 61000054  b.ne #0x86f5dc
//   0x86f5d4: e8030032  mov w8, #1
//   0x86f5d8: 0e000014  b #0x86f610
//   0x86f5dc: e0230091  add x0, sp, #8
//   0x86f5e0: e80b00b9  str w8, [sp, #8]
//   0x86f5e4: 9cfcff97  bl #0x86e854
//   0x86f5e8: 60000036  tbz w0, #0, #0x86f5f4
//   0x86f5ec: e8031f32  mov w8, #2
//   0x86f5f0: 08000014  b #0x86f610
//   0x86f5f4: 88a241b9  ldr w8, [x20, #0x1a0]
//   0x86f5f8: 1f990171  cmp w8, #0x66
//   0x86f5fc: e9179f1a  cset w9, eq
//   0x86f600: 1f190171  cmp w8, #0x46
//   0x86f604: e8070032  mov w8, #3
//   0x86f608: 29751e53  lsl w9, w9, #2
//   0x86f60c: 0801891a  csel w8, w8, w9, eq
//   0x86f610: 680200b9  str w8, [x19]
//   0x86f614: fd7b42a9  ldp x29, x30, [sp, #0x20]
//   0x86f618: f44f41a9  ldp x20, x19, [sp, #0x10]
//   0x86f61c: ffc30091  add sp, sp, #0x30
//   0x86f620: c0035fd6  ret
inline constexpr u32 PinCoinKindWords[] = {0xd100c3ff, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xaa0003f4, 0xaa0803f3, 0xb941a288, 0x7100cd1f, 0x54000061, 0x320003e8, 0x1400000e, 0x910023e0, 0xb9000be8, 0x97fffc9c, 0x36000060, 0x321f03e8, 0x14000008, 0xb941a288, 0x7101991f, 0x1a9f17e9, 0x7101191f, 0x320007e8, 0x531e7529, 0x1a890108, 0xb9000268, 0xa9427bfd, 0xa9414ff4, 0x9100c3ff, 0xd65f03c0};
inline constexpr Pin PinCoinKind{0x86f5b0, PinCoinKindWords, "CoinKind"};

// AnimalCourse: raw 0x34 + bits of 0x7001
//   0x86e854: 080040b9  ldr w8, [x0]
//   0x86e858: 08d10051  sub w8, w8, #0x34
//   0x86e85c: 1f390071  cmp w8, #0xe
//   0x86e860: c8000054  b.hi #0x86e878
//   0x86e864: 08390012  and w8, w8, #0x7fff
//   0x86e868: 29008e52  mov w9, #0x7001
//   0x86e86c: 2825c81a  lsr w8, w9, w8
//   0x86e870: 00010012  and w0, w8, #1
//   0x86e874: c0035fd6  ret
inline constexpr u32 PinAnimalCourseWords[] = {0xb9400008, 0x5100d108, 0x7100391f, 0x540000c8, 0x12003908, 0x528e0029, 0x1ac82528, 0x12000100, 0xd65f03c0};
inline constexpr Pin PinAnimalCourse{0x86e854, PinAnimalCourseWords, "AnimalCourse"};

// SystemEngine: SE = [[root+0x28]+0x190]
//   0x8aba80: ff8300d1  sub sp, sp, #0x20
//   0x8aba84: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x8aba88: fd430091  add x29, sp, #0x10
//   0x8aba8c: 53eefc97  bl #0x7e73d8
//   0x8aba90: bfc31fb8  stur wzr, [x29, #-4]
//   0x8aba94: a8c39fb8  ldursw x8, [x29, #-4]
//   0x8aba98: 1f290071  cmp w8, #0xa
//   0x8aba9c: fd7b41a9  ldp x29, x30, [sp, #0x10]
//   0x8abaa0: 08319f9a  csel x8, x8, xzr, lo
//   0x8abaa4: 080c088b  add x8, x0, x8, lsl #3
//   0x8abaa8: 00c940f9  ldr x0, [x8, #0x190]
//   0x8abaac: ff830091  add sp, sp, #0x20
//   0x8abab0: c0035fd6  ret
inline constexpr u32 PinSystemEngineWords[] = {0xd10083ff, 0xa9017bfd, 0x910043fd, 0x97fcee53, 0xb81fc3bf, 0xb89fc3a8, 0x7100291f, 0xa9417bfd, 0x9a9f3108, 0x8b080c08, 0xf940c900, 0x910083ff, 0xd65f03c0};
inline constexpr Pin PinSystemEngine{0x8aba80, PinSystemEngineWords, "SystemEngine"};

// PlayerManager: PM = [SE+0x800]
//   0x8ace28: fd7bbfa9  stp x29, x30, [sp, #-0x10]!
//   0x8ace2c: fd030091  mov x29, sp
//   0x8ace30: 14fbff97  bl #0x8aba80
//   0x8ace34: 400000b4  cbz x0, #0x8ace3c
//   0x8ace38: 000044f9  ldr x0, [x0, #0x800]
//   0x8ace3c: fd7bc1a8  ldp x29, x30, [sp], #0x10
//   0x8ace40: c0035fd6  ret
inline constexpr u32 PinPlayerManagerWords[] = {0xa9bf7bfd, 0x910003fd, 0x97fffb14, 0xb4000040, 0xf9440000, 0xa8c17bfd, 0xd65f03c0};
inline constexpr Pin PinPlayerManager{0x8ace28, PinPlayerManagerWords, "PlayerManager"};

// LocalPlayerObj: PM+0x17c+4k, k<=3
//   0x86ebf8: f30f1ef8  str x19, [sp, #-0x20]!
//   0x86ebfc: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x86ec00: fd430091  add x29, sp, #0x10
//   0x86ec04: f303002a  mov w19, w0
//   0x86ec08: 7f0e0071  cmp w19, #3
//   0x86ec0c: 69000054  b.ls #0x86ec18
//   0x86ec10: 00008012  mov w0, #-1
//   0x86ec14: 04000014  b #0x86ec24
//   0x86ec18: 84f80094  bl #0x8ace28
//   0x86ec1c: 08c8338b  add x8, x0, w19, sxtw #2
//   0x86ec20: 007d41b9  ldr w0, [x8, #0x17c]
inline constexpr u32 PinLocalPlayerObjWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x2a0003f3, 0x71000e7f, 0x54000069, 0x12800000, 0x14000004, 0x9400f884, 0x8b33c808, 0xb9417d00};
inline constexpr Pin PinLocalPlayerObj{0x86ebf8, PinLocalPlayerObjWords, "LocalPlayerObj"};

// RacerPlayerObj: PM+0x1cc+4i, i<=11
//   0x86e8e4: f30f1ef8  str x19, [sp, #-0x20]!
//   0x86e8e8: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x86e8ec: fd430091  add x29, sp, #0x10
//   0x86e8f0: f303002a  mov w19, w0
//   0x86e8f4: 7f2e0071  cmp w19, #0xb
//   0x86e8f8: 69000054  b.ls #0x86e904
//   0x86e8fc: 00008012  mov w0, #-1
//   0x86e900: 04000014  b #0x86e910
//   0x86e904: 49f90094  bl #0x8ace28
//   0x86e908: 08c8338b  add x8, x0, w19, sxtw #2
//   0x86e90c: 00cd41b9  ldr w0, [x8, #0x1cc]
inline constexpr u32 PinRacerPlayerObjWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x2a0003f3, 0x71002e7f, 0x54000069, 0x12800000, 0x14000004, 0x9400f949, 0x8b33c808, 0xb941cd00};
inline constexpr Pin PinRacerPlayerObj{0x86e8e4, PinRacerPlayerObjWords, "RacerPlayerObj"};

// UiRoot: D = [[[cell]]+8] (ADRP+LDR)
//   0x8b3e90: 885200f0  adrp x8, #0x1306000
//   0x8b3e94: 083947f9  ldr x8, [x8, #0xe70]
//   0x8b3e98: 080140f9  ldr x8, [x8]
//   0x8b3e9c: 000540f9  ldr x0, [x8, #8]
//   0x8b3ea0: c0035fd6  ret
inline constexpr u32 PinUiRootWords[] = {0xf0005288, 0xf9473908, 0xf9400108, 0xf9400500, 0xd65f03c0};
inline constexpr Pin PinUiRoot{0x8b3e90, PinUiRootWords, "UiRoot"};

// UiResource: R = [D+0x18+8k], k<13
//   0x8bf0b0: f30f1ef8  str x19, [sp, #-0x20]!
//   0x8bf0b4: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x8bf0b8: fd430091  add x29, sp, #0x10
//   0x8bf0bc: f30300aa  mov x19, x0
//   0x8bf0c0: 74d3ff97  bl #0x8b3e90
//   0x8bf0c4: 680240b9  ldr w8, [x19]
//   0x8bf0c8: a8c31fb8  stur w8, [x29, #-4]
//   0x8bf0cc: a8c39fb8  ldursw x8, [x29, #-4]
//   0x8bf0d0: fd7b41a9  ldp x29, x30, [sp, #0x10]
//   0x8bf0d4: 1f350071  cmp w8, #0xd
//   0x8bf0d8: 08319f9a  csel x8, x8, xzr, lo
//   0x8bf0dc: 080c088b  add x8, x0, x8, lsl #3
//   0x8bf0e0: 000d40f9  ldr x0, [x8, #0x18]
inline constexpr u32 PinUiResourceWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0x97ffd374, 0xb9400268, 0xb81fc3a8, 0xb89fc3a8, 0xa9417bfd, 0x7100351f, 0x9a9f3108, 0x8b080c08, 0xf9400d00};
inline constexpr Pin PinUiResource{0x8bf0b0, PinUiResourceWords, "UiResource"};

// UiTypeCheck: R3 used as [R3+0x30] after its type check
//   0x50cd40: 880240f9  ldr x8, [x20]
//   0x50cd44: 616f00d0  adrp x1, #0x12fa000
//   0x50cd48: 080140f9  ldr x8, [x8]
//   0x50cd4c: 214047f9  ldr x1, [x1, #0xe80]
//   0x50cd50: e00314aa  mov x0, x20
//   0x50cd54: 00013fd6  blr x8
//   0x50cd58: 1f000072  tst w0, #1
//   0x50cd5c: 88129f9a  csel x8, x20, xzr, ne
//   0x50cd60: 151940f9  ldr x21, [x8, #0x30]
inline constexpr u32 PinUiTypeCheckWords[] = {0xf9400288, 0xd0006f61, 0xf9400108, 0xf9474021, 0xaa1403e0, 0xd63f0100, 0x7200001f, 0x9a9f1288, 0xf9401915};
inline constexpr Pin PinUiTypeCheck{0x50cd40, PinUiTypeCheckWords, "UiTypeCheck"};

// DriverTableInit: DT = R3+0x298, pict table R3+0x350
//   0x402f7c: 60620a91  add x0, x19, #0x298
//   0x402f80: f73e0094  bl #0x412b5c
//   0x402f84: 60c20c91  add x0, x19, #0x330
//   0x402f88: 754f0094  bl #0x416d5c
//   0x402f8c: 60420d91  add x0, x19, #0x350
inline constexpr u32 PinDriverTableInitWords[] = {0x910a6260, 0x94003ef7, 0x910cc260, 0x94004f75, 0x910d4260};
inline constexpr Pin PinDriverTableInit{0x402f7c, PinDriverTableInitWords, "DriverTableInit"};

// DriverName: name = names[id+1] (+0x80 char*[], count +0x78)
//   0x414fc0: a80240b9  ldr w8, [x21]
//   0x414fc4: e81f00b9  str w8, [sp, #0x1c]
//   0x414fc8: e81f80b9  ldrsw x8, [sp, #0x1c]
//   0x414fcc: 8a4240f9  ldr x10, [x20, #0x80]
//   0x414fd0: 897a40b9  ldr w9, [x20, #0x78]
//   0x414fd4: 08050091  add x8, x8, #1
//   0x414fd8: 4b0d088b  add x11, x10, x8, lsl #3
//   0x414fdc: 3f01086b  cmp w9, w8
//   0x414fe0: 68818a9a  csel x8, x11, x10, hi
//   0x414fe4: 180140f9  ldr x24, [x8]
inline constexpr u32 PinDriverNameWords[] = {0xb94002a8, 0xb9001fe8, 0xb9801fe8, 0xf940428a, 0xb9407a89, 0x91000508, 0x8b080d4b, 0x6b08013f, 0x9a8a8168, 0xf9400118};
inline constexpr Pin PinDriverName{0x414fc0, PinDriverNameWords, "DriverName"};

// DriverMsg: msg = u32 [+0x90][id+1], count +0x88
//   0x412c98: a9c39fb8  ldursw x9, [x29, #-4]
//   0x412c9c: 6b4a40f9  ldr x11, [x19, #0x90]
//   0x412ca0: 6a8a40b9  ldr w10, [x19, #0x88]
//   0x412ca4: 29050091  add x9, x9, #1
//   0x412ca8: 6c09098b  add x12, x11, x9, lsl #2
//   0x412cac: 5f01096b  cmp w10, w9
//   0x412cb0: 89818b9a  csel x9, x12, x11, hi
//   0x412cb4: 280100b9  str w8, [x9]
inline constexpr u32 PinDriverMsgWords[] = {0xb89fc3a9, 0xf9404a6b, 0xb9408a6a, 0x91000529, 0x8b09096c, 0x6b09015f, 0x9a8b8189, 0xb9000128};
inline constexpr Pin PinDriverMsg{0x412c98, PinDriverMsgWords, "DriverMsg"};

// DriverSuffix: ids in mask 0x160000010010 get '0'+variant
//   0x4150e4: a80240b9  ldr w8, [x21]
//   0x4150e8: 1fb10071  cmp w8, #0x2c
//   0x4150ec: 080f0054  b.hi #0x4152cc
//   0x4150f0: e9030032  mov w9, #1
//   0x4150f4: 2821c89a  lsl x8, x9, x8
//   0x4150f8: 09c0c2d2  mov x9, #0x160000000000
//   0x4150fc: 2900a0f2  movk x9, #1, lsl #16
//   0x415100: 090280f2  movk x9, #0x10
//   0x415104: 0801098a  and x8, x8, x9
//   0x415108: 280e00b4  cbz x8, #0x4152cc
inline constexpr u32 PinDriverSuffixWords[] = {0xb94002a8, 0x7100b11f, 0x54000f08, 0x320003e9, 0x9ac82128, 0xd2c2c009, 0xf2a00029, 0xf2800209, 0x8a090108, 0xb4000e28};
inline constexpr Pin PinDriverSuffix{0x4150e4, PinDriverSuffixWords, "DriverSuffix"};

// CoursePict: pict name = names[raw+1] (+0x80, count +0x78)
//   0x419040: a80240b9  ldr w8, [x21]
//   0x419044: e82700b9  str w8, [sp, #0x24]
//   0x419048: e82780b9  ldrsw x8, [sp, #0x24]
//   0x41904c: 8a4240f9  ldr x10, [x20, #0x80]
//   0x419050: 897a40b9  ldr w9, [x20, #0x78]
//   0x419054: 08050091  add x8, x8, #1
//   0x419058: 4b0d088b  add x11, x10, x8, lsl #3
//   0x41905c: 3f01086b  cmp w9, w8
//   0x419060: 68818a9a  csel x8, x11, x10, hi
inline constexpr u32 PinCoursePictWords[] = {0xb94002a8, 0xb90027e8, 0xb98027e8, 0xf940428a, 0xb9407a89, 0x91000508, 0x8b080d4b, 0x6b08013f, 0x9a8a8168};
inline constexpr Pin PinCoursePict{0x419040, PinCoursePictWords, "CoursePict"};

// CupOfCourse: T = [R3+0x30]; {cup,slot} = T+0xc80+8*raw (raw<0x7b); cup name ptr table (ADRP+ADD)
//   0x50d2e0: 081940f9  ldr x8, [x8, #0x30]
//   0x50d2e4: f76b00b9  str w23, [sp, #0x68]
//   0x50d2e8: e96b80b9  ldrsw x9, [sp, #0x68]
//   0x50d2ec: 3fed0171  cmp w9, #0x7b
//   0x50d2f0: 29319f9a  csel x9, x9, xzr, lo
//   0x50d2f4: 180d098b  add x24, x8, x9, lsl #3
//   0x50d2f8: 08834cb9  ldr w8, [x24, #0xc80]
//   0x50d2fc: 1f050031  cmn w8, #1
//   0x50d300: 40340054  b.eq #0x50d988
//   0x50d304: e82f00b9  str w8, [sp, #0x2c]
//   0x50d308: e82f40b9  ldr w8, [sp, #0x2c]
//   0x50d30c: 1f5d0071  cmp w8, #0x17
//   0x50d310: e9028052  mov w9, #0x17
//   0x50d314: 09b1891a  csel w9, w8, w9, lt
//   0x50d318: 1f010071  cmp w8, #0
//   0x50d31c: a86b00f0  adrp x8, #0x1284000
//   0x50d320: 297d4093  sxtw x9, w9
//   0x50d324: 08e10291  add x8, x8, #0xb8
//   0x50d328: e9b3899a  csel x9, xzr, x9, lt
inline constexpr u32 PinCupOfCourseWords[] = {0xf9401908, 0xb9006bf7, 0xb9806be9, 0x7101ed3f, 0x9a9f3129, 0x8b090d18, 0xb94c8308, 0x3100051f, 0x54003440, 0xb9002fe8, 0xb9402fe8, 0x71005d1f, 0x528002e9, 0x1a89b109, 0x7100011f, 0xf0006ba8, 0x93407d29, 0x9102e108, 0x9a89b3e9};
inline constexpr Pin PinCupOfCourse{0x50d2e0, PinCupOfCourseWords, "CupOfCourse"};

// CupMsg: cup name msg = u32 rodata table (ADRP+ADD)[cup], -1 when cup < 0
//   0x41fd6c: 080040b9  ldr w8, [x0]
//   0x41fd70: 1f050031  cmn w8, #1
//   0x41fd74: 00010054  b.eq #0x41fd94
//   0x41fd78: 080040b9  ldr w8, [x0]
//   0x41fd7c: 0801f837  tbnz w8, #0x1f, #0x41fd9c
//   0x41fd80: 090080b9  ldrsw x9, [x0]
//   0x41fd84: 285800f0  adrp x8, #0xf26000
//   0x41fd88: 08113191  add x8, x8, #0xc44
//   0x41fd8c: 007969b8  ldr w0, [x8, x9, lsl #2]
//   0x41fd90: c0035fd6  ret
inline constexpr u32 PinCupMsgWords[] = {0xb9400008, 0x3100051f, 0x54000100, 0xb9400008, 0x37f80108, 0xb9800009, 0xf0005828, 0x91311108, 0xb8697900, 0xd65f03c0};
inline constexpr Pin PinCupMsg{0x41fd6c, PinCupMsgWords, "CupMsg"};

// DriverParamDb: driver param DB = [[[GOT (ADRP+LDR)]]+0xe8]
//   0x85b490: f45400d0  adrp x20, #0x12f9000
//   0x85b494: 94ce46f9  ldr x20, [x20, #0xd98]
//   0x85b498: 890240f9  ldr x9, [x20]
//   0x85b49c: e80301aa  mov x8, x1
//   0x85b4a0: 217540f9  ldr x1, [x9, #0xe8]
//   0x85b4a4: f30300aa  mov x19, x0
//   0x85b4a8: 60220091  add x0, x19, #8
//   0x85b4ac: e20308aa  mov x2, x8
//   0x85b4b0: 7bffff97  bl #0x85b29c
//   0x85b4b4: 880240f9  ldr x8, [x20]
//   0x85b4b8: 147540f9  ldr x20, [x8, #0xe8]
//   0x85b4bc: 340300b4  cbz x20, #0x85b520
//   0x85b4c0: e00313aa  mov x0, x19
//   0x85b4c4: e10314aa  mov x1, x20
//   0x85b4c8: 04020094  bl #0x85bcd8
inline constexpr u32 PinDriverParamDbWords[] = {0xd00054f4, 0xf946ce94, 0xf9400289, 0xaa0103e8, 0xf9407521, 0xaa0003f3, 0x91002260, 0xaa0803e2, 0x97ffff7b, 0xf9400288, 0xf9407514, 0xb4000334, 0xaa1303e0, 0xaa1403e1, 0x94000204};
inline constexpr Pin PinDriverParamDb{0x85b490, PinDriverParamDbWords, "DriverParamDb"};

// DriverParamRecord: record = [DB+0x30] + id*0x244 if 0 <= id < u16 DB+0xa2 (else DB+0xcc)
//   0x85bd0c: 291040f9  ldr x9, [x1, #0x20]
//   0x85bd10: c90d00b4  cbz x9, #0x85bec8
//   0x85bd14: 291840f9  ldr x9, [x1, #0x30]
//   0x85bd18: 690100b4  cbz x9, #0x85bd44
//   0x85bd1c: aac35ab8  ldur w10, [x29, #-0x54]
//   0x85bd20: 2a01f837  tbnz w10, #0x1f, #0x85bd44
//   0x85bd24: aac35ab8  ldur w10, [x29, #-0x54]
//   0x85bd28: 2b444179  ldrh w11, [x1, #0xa2]
//   0x85bd2c: 5f010b6b  cmp w10, w11
//   0x85bd30: aa000054  b.ge #0x85bd44
//   0x85bd34: aac39ab8  ldursw x10, [x29, #-0x54]
//   0x85bd38: 8b488052  mov w11, #0x244
//   0x85bd3c: 5a250b9b  madd x26, x10, x11, x9
//   0x85bd40: 02000014  b #0x85bd48
//   0x85bd44: 3a300391  add x26, x1, #0xcc
inline constexpr u32 PinDriverParamRecordWords[] = {0xf9401029, 0xb4000dc9, 0xf9401829, 0xb4000169, 0xb85ac3aa, 0x37f8012a, 0xb85ac3aa, 0x7941442b, 0x6b0b015f, 0x540000aa, 0xb89ac3aa, 0x5280488b, 0x9b0b255a, 0x14000002, 0x9103303a};
inline constexpr Pin PinDriverParamRecord{0x85bd0c, PinDriverParamRecordWords, "DriverParamRecord"};

// EmblemFlag: suffix flag = bit 1 of u32 record+0x60 (-> +0xc94)
//   0x85c090: 486340b9  ldr w8, [x26, #0x60]
//   0x85c094: 690a40b9  ldr w9, [x19, #8]
//   0x85c098: 08050153  ubfx w8, w8, #1, #1
//   0x85c09c: 3fd10071  cmp w9, #0x34
//   0x85c0a0: 68523239  strb w8, [x19, #0xc94]
inline constexpr u32 PinEmblemFlagWords[] = {0xb9406348, 0xb9400a69, 0x53010508, 0x7100d13f, 0x39325268};
inline constexpr Pin PinEmblemFlag{0x85c090, PinEmblemFlagWords, "EmblemFlag"};

// EmblemCode: code = [[GOT (ADRP+LDR)]] + u32 record+0x10 (0 -> empty)
//   0x85c850: fa4740f9  ldr x26, [sp, #0x88]
//   0x85c854: 895500f0  adrp x9, #0x130f000
//   0x85c858: 295543f9  ldr x9, [x9, #0x6a8]
//   0x85c85c: 481340b9  ldr w8, [x26, #0x10]
//   0x85c860: 290140f9  ldr x9, [x9]
//   0x85c864: 743a46f9  ldr x20, [x19, #0xc70]
//   0x85c868: ca3300d0  adrp x10, #0xed6000
//   0x85c86c: 4aa12c91  add x10, x10, #0xb28
//   0x85c870: 2901088b  add x9, x9, x8
//   0x85c874: 1f010071  cmp w8, #0
//   0x85c878: 4101899a  csel x1, x10, x9, eq
inline constexpr u32 PinEmblemCodeWords[] = {0xf94047fa, 0xf0005589, 0xf9435529, 0xb9401348, 0xf9400129, 0xf9463a74, 0xd00033ca, 0x912ca14a, 0x8b080129, 0x7100011f, 0x9a890141};
inline constexpr Pin PinEmblemCode{0x85c850, PinEmblemCodeWords, "EmblemCode"};

// EmblemName: driver 0x34 / 0x1d (Mii) -> Emblem_Mii
//   0x85c908: 680a40b9  ldr w8, [x19, #8]
//   0x85c90c: 77c20b91  add x23, x19, #0x2f0
//   0x85c910: 1fd10071  cmp w8, #0x34
//   0x85c914: 60000054  b.eq #0x85c920
//   0x85c918: 1f750071  cmp w8, #0x1d
//   0x85c91c: e1040054  b.ne #0x85c9b8
inline constexpr u32 PinEmblemNameWords[] = {0xb9400a68, 0x910bc277, 0x7100d11f, 0x54000060, 0x7100751f, 0x540004e1};
inline constexpr Pin PinEmblemName{0x85c908, PinEmblemNameWords, "EmblemName"};

// EmblemSuffix: Emblem_%s; if flag && driver != 0x10: + %02d(variant)
//   0x85c9b8: 683646f9  ldr x8, [x19, #0xc68]
//   0x85c9bc: 080d40f9  ldr x8, [x8, #0x18]
//   0x85c9c0: 60a23191  add x0, x19, #0xc68
//   0x85c9c4: 00013fd6  blr x8
//   0x85c9c8: 623a46f9  ldr x2, [x19, #0xc70]
//   0x85c9cc: 81350090  adrp x1, #0xf0c000
//   0x85c9d0: 21801d91  add x1, x1, #0x760
//   0x85c9d4: e00317aa  mov x0, x23
//   0x85c9d8: 3bbff697  bl #0x60c6c4
//   0x85c9dc: 68527239  ldrb w8, [x19, #0xc94]
//   0x85c9e0: 88faff34  cbz w8, #0x85c930
//   0x85c9e4: e83740f9  ldr x8, [sp, #0x68]
//   0x85c9e8: 080140b9  ldr w8, [x8]
//   0x85c9ec: a8c318b8  stur w8, [x29, #-0x74]
//   0x85c9f0: a8c358b8  ldur w8, [x29, #-0x74]
//   0x85c9f4: 1f410071  cmp w8, #0x10
//   0x85c9f8: c0f9ff54  b.eq #0x85c930
//   0x85c9fc: 620e40b9  ldr w2, [x19, #0xc]
//   0x85ca00: 81350090  adrp x1, #0xf0c000
//   0x85ca04: 21a81d91  add x1, x1, #0x76a
//   0x85ca08: e00317aa  mov x0, x23
inline constexpr u32 PinEmblemSuffixWords[] = {0xf9463668, 0xf9400d08, 0x9131a260, 0xd63f0100, 0xf9463a62, 0x90003581, 0x911d8021, 0xaa1703e0, 0x97f6bf3b, 0x39725268, 0x34fffa88, 0xf94037e8, 0xb9400108, 0xb818c3a8, 0xb858c3a8, 0x7100411f, 0x54fff9c0, 0xb9400e62, 0x90003581, 0x911da821, 0xaa1703e0};
inline constexpr Pin PinEmblemSuffix{0x85c9b8, PinEmblemSuffixWords, "EmblemSuffix"};

// PlayerInput: player input obj = [[SE+0x7f0]+0x58][p] + 0x38 (p < u32 [SE+0x7f0]+0x50)
//   0x8acf3c: f30f1ef8  str x19, [sp, #-0x20]!
//   0x8acf40: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x8acf44: fd430091  add x29, sp, #0x10
//   0x8acf48: f303002a  mov w19, w0
//   0x8acf4c: cdfaff97  bl #0x8aba80
//   0x8acf50: 600000b4  cbz x0, #0x8acf5c
//   0x8acf54: 08f843f9  ldr x8, [x0, #0x7f0]
//   0x8acf58: 02000014  b #0x8acf60
//   0x8acf5c: e8031faa  mov x8, xzr
//   0x8acf60: 095140b9  ldr w9, [x8, #0x50]
//   0x8acf64: 3f01136b  cmp w9, w19
//   0x8acf68: 89000054  b.ls #0x8acf78
//   0x8acf6c: 082d40f9  ldr x8, [x8, #0x58]
//   0x8acf70: 08d973f8  ldr x8, [x8, w19, sxtw #3]
//   0x8acf74: 02000014  b #0x8acf7c
//   0x8acf78: e8031faa  mov x8, xzr
//   0x8acf7c: fd7b41a9  ldp x29, x30, [sp, #0x10]
//   0x8acf80: 00e10091  add x0, x8, #0x38
inline constexpr u32 PinPlayerInputWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x2a0003f3, 0x97fffacd, 0xb4000060, 0xf943f808, 0x14000002, 0xaa1f03e8, 0xb9405109, 0x6b13013f, 0x54000089, 0xf9402d08, 0xf873d908, 0x14000002, 0xaa1f03e8, 0xa9417bfd, 0x9100e100};
inline constexpr Pin PinPlayerInput{0x8acf3c, PinPlayerInputWords, "PlayerInput"};

// HornEvent: one-shot horn request u8 obj+0x35d: word +0x114 |= 0x100, then +0x35d = 0 (main+0x7d9260)
//   0x7c8b74: 69764d39  ldrb w9, [x19, #0x35d]
//   0x7c8b78: 681641b9  ldr w8, [x19, #0x114]
//   0x7c8b7c: 69000034  cbz w9, #0x7c8b88
//   0x7c8b80: 08011832  orr w8, w8, #0x100
//   0x7c8b84: 681601b9  str w8, [x19, #0x114]
//   0x7c8b88: 48001037  tbnz w8, #2, #0x7c8b90
//   0x7c8b8c: 7f7a0d39  strb wzr, [x19, #0x35e]
//   0x7c8b90: 6a7243b9  ldr w10, [x19, #0x370]
//   0x7c8b94: aa000035  cbnz w10, #0x7c8ba8
//   0x7c8b98: 6aea4679  ldrh w10, [x19, #0x374]
//   0x7c8b9c: 6a000035  cbnz w10, #0x7c8ba8
//   0x7c8ba0: 6a7a4d39  ldrb w10, [x19, #0x35e]
//   0x7c8ba4: aa050034  cbz w10, #0x7c8c58
//   0x7c8ba8: 69000035  cbnz w9, #0x7c8bb4
//   0x7c8bac: 08791712  and w8, w8, #0xfffffeff
//   0x7c8bb0: 681601b9  str w8, [x19, #0x114]
//   0x7c8bb4: 89208052  mov w9, #0x104
//   0x7c8bb8: 0801090a  and w8, w8, w9
//   0x7c8bbc: 1f110071  cmp w8, #4
//   0x7c8bc0: 61000054  b.ne #0x7c8bcc
//   0x7c8bc4: e8030032  mov w8, #1
//   0x7c8bc8: 687a0d39  strb w8, [x19, #0x35e]
//   0x7c8bcc: 684643b9  ldr w8, [x19, #0x344]
//   0x7c8bd0: 1f090071  cmp w8, #2
//   0x7c8bd4: 7f760d39  strb wzr, [x19, #0x35d]
//   0x7c8bd8: 20030054  b.eq #0x7c8c3c
inline constexpr u32 PinHornEventWords[] = {0x394d7669, 0xb9411668, 0x34000069, 0x32180108, 0xb9011668, 0x37100048, 0x390d7a7f, 0xb943726a, 0x350000aa, 0x7946ea6a, 0x3500006a, 0x394d7a6a, 0x340005aa, 0x35000069, 0x12177908, 0xb9011668, 0x52802089, 0x0a090108, 0x7100111f, 0x54000061, 0x320003e8, 0x390d7a68, 0xb9434668, 0x7100091f, 0x390d767f, 0x54000320};
inline constexpr Pin PinHornEvent{0x7c8b74, PinHornEventWords, "HornEvent"};

// HornFill: kart input fill: horn byte (+7) = bit 8 of player action word +0x114
//   0x1799a8: a00a40b9  ldr w0, [x21, #8]
//   0x1799ac: f30301aa  mov x19, x1
//   0x1799b0: 63cd1c94  bl #0x8acf3c
//   0x1799b4: f40300aa  mov x20, x0
//   0x1799b8: 881641b9  ldr w8, [x20, #0x114]
//   0x1799bc: 08210853  ubfx w8, w8, #8, #1
//   0x1799c0: 681e0039  strb w8, [x19, #7]
inline constexpr u32 PinHornFillWords[] = {0xb9400aa0, 0xaa0103f3, 0x941ccd63, 0xaa0003f4, 0xb9411688, 0x53082108, 0x39001e68};
inline constexpr Pin PinHornFill{0x1799a8, PinHornFillWords, "HornFill"};

// HornBit: V+0x1cc bit 16 = u8 V+0x177 (horn)
//   0x173f34: 68de4539  ldrb w8, [x19, #0x177]
//   0x173f38: 69ce41b9  ldr w9, [x19, #0x1cc]
//   0x173f3c: 2a011032  orr w10, w9, #0x10000
//   0x173f40: 29790f12  and w9, w9, #0xfffeffff
//   0x173f44: 1f010071  cmp w8, #0
//   0x173f48: 4811891a  csel w8, w10, w9, ne
inline constexpr u32 PinHornBitWords[] = {0x3945de68, 0xb941ce69, 0x3210012a, 0x120f7929, 0x7100011f, 0x1a891148};
inline constexpr Pin PinHornBit{0x173f34, PinHornBitWords, "HornBit"};

// HornRequest: sound obj +0x16e7 = bit 0 of u16 V+0x1ce
//   0x1301a8: 680640f9  ldr x8, [x19, #8]
//   0x1301ac: 089d4379  ldrh w8, [x8, #0x1ce]
//   0x1301b0: 08010012  and w8, w8, #1
//   0x1301b4: e9dc8252  mov w9, #0x16e7
//   0x1301b8: 08682938  strb w8, [x0, x9]
inline constexpr u32 PinHornRequestWords[] = {0xf9400668, 0x79439d08, 0x12000108, 0x5282dce9, 0x38296808};
inline constexpr Pin PinHornRequest{0x1301a8, PinHornRequestWords, "HornRequest"};

// HornLatch: +0x16e8 = +0x16e7; +0x16e7 = 0
//   0xaeb74: e8dc8252  mov w8, #0x16e7
//   0xaeb78: 696a6838  ldrb w9, [x19, x8]
//   0xaeb7c: 696a2a38  strb w9, [x19, x10]
//   0xaeb80: 7f6a2838  strb wzr, [x19, x8]
inline constexpr u32 PinHornLatchWords[] = {0x5282dce8, 0x38686a69, 0x382a6a69, 0x38286a7f};
inline constexpr Pin PinHornLatch{0xaeb74, PinHornLatchWords, "HornLatch"};

// HornPlay: play when +0x16e7 == 0 and +0x16e8 != 0 (release edge)
//   0xaf2dc: e8dc8252  mov w8, #0x16e7
//   0xaf2e0: 686a6838  ldrb w8, [x19, x8]
//   0xaf2e4: c8000034  cbz w8, #0xaf2fc
//   0xaf2e8: fd7b50a9  ldp x29, x30, [sp, #0x100]
//   0xaf2ec: f44f4fa9  ldp x20, x19, [sp, #0xf0]
//   0xaf2f0: fc574ea9  ldp x28, x21, [sp, #0xe0]
//   0xaf2f4: ff430491  add sp, sp, #0x110
//   0xaf2f8: c0035fd6  ret
//   0xaf2fc: 08dd8252  mov w8, #0x16e8
//   0xaf300: 686a6838  ldrb w8, [x19, x8]
//   0xaf304: 28ffff34  cbz w8, #0xaf2e8
inline constexpr u32 PinHornPlayWords[] = {0x5282dce8, 0x38686a68, 0x340000c8, 0xa9507bfd, 0xa94f4ff4, 0xa94e57fc, 0x910443ff, 0xd65f03c0, 0x5282dd08, 0x38686a68, 0x34ffff28};
inline constexpr Pin PinHornPlay{0xaf2dc, PinHornPlayWords, "HornPlay"};

// CourseTokens: course enum text (ADRP+ADD)
//   0x1b554: e9750090  adrp x9, #0xed7000
//   0x1b558: 29751991  add x9, x9, #0x65d
inline constexpr u32 PinCourseTokensWords[] = {0x900075e9, 0x91197529};
inline constexpr Pin PinCourseTokens{0x1b554, PinCourseTokensWords, "CourseTokens"};

// CourseTokenIndex: token index = raw + 1
//   0x89ecd4: 00050011  add w0, w8, #1
//   0x89ecd8: f5f1dd97  bl #0x1b4ac
inline constexpr u32 PinCourseTokenIndexWords[] = {0x11000500, 0x97ddf1f5};
inline constexpr Pin PinCourseTokenIndex{0x89ecd4, PinCourseTokenIndexWords, "CourseTokenIndex"};
