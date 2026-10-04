// Generated from the 4.0.0 main (build 2C336A9BCF79C304).
// Each pin: main offset + the exact instruction words the reader requires before it uses
// the offsets those instructions encode. Disassembly quoted for review.

// RaceInfoHolder: root cell (ADRP+LDR) -> [[cell]] = root; [root+0x28]+0x240
//   0x7f7a90: 285800b0  adrp x8, #0x12fc000
//   0x7f7a94: 08c541f9  ldr x8, [x8, #0x388]
//   0x7f7a98: 080140f9  ldr x8, [x8]
//   0x7f7a9c: 081540f9  ldr x8, [x8, #0x28]
//   0x7f7aa0: 002141f9  ldr x0, [x8, #0x240]
//   0x7f7aa4: c0035fd6  ret
inline constexpr u32 PinRaceInfoHolderWords[] = {0xb0005828, 0xf941c508, 0xf9400108, 0xf9401508, 0xf9412100, 0xd65f03c0};
inline constexpr Pin PinRaceInfoHolder{0x7f7a90, PinRaceInfoHolderWords, "RaceInfoHolder"};

// RaceInfo: RI = [holder+0x10]
//   0x87fcd0: fd7bbfa9  stp x29, x30, [sp, #-0x10]!
//   0x87fcd4: fd030091  mov x29, sp
//   0x87fcd8: 6edffd97  bl #0x7f7a90
//   0x87fcdc: 000840f9  ldr x0, [x0, #0x10]
//   0x87fce0: fd7bc1a8  ldp x29, x30, [sp], #0x10
//   0x87fce4: c0035fd6  ret
inline constexpr u32 PinRaceInfoWords[] = {0xa9bf7bfd, 0x910003fd, 0x97fddf6e, 0xf9400800, 0xa8c17bfd, 0xd65f03c0};
inline constexpr Pin PinRaceInfo{0x87fcd0, PinRaceInfoWords, "RaceInfo"};

// ActiveScene: scene = [[root+0x20]+8], active if u8 +0x180 & 4
//   0x7f7ac0: 285800b0  adrp x8, #0x12fc000
//   0x7f7ac4: 08c541f9  ldr x8, [x8, #0x388]
//   0x7f7ac8: 080140f9  ldr x8, [x8]
//   0x7f7acc: 081140f9  ldr x8, [x8, #0x20]
//   0x7f7ad0: 080540f9  ldr x8, [x8, #8]
//   0x7f7ad4: 09014639  ldrb w9, [x8, #0x180]
//   0x7f7ad8: 3f011e72  tst w9, #4
//   0x7f7adc: e003889a  csel x0, xzr, x8, eq
//   0x7f7ae0: c0035fd6  ret
inline constexpr u32 PinActiveSceneWords[] = {0xb0005828, 0xf941c508, 0xf9400108, 0xf9401108, 0xf9400508, 0x39460109, 0x721e013f, 0x9a8803e0, 0xd65f03c0};
inline constexpr Pin PinActiveScene{0x7f7ac0, PinActiveSceneWords, "ActiveScene"};

// SceneKind: scene kind u32 +0x184
//   0x7f7ae4: 295800b0  adrp x9, #0x12fc000
//   0x7f7ae8: 29c541f9  ldr x9, [x9, #0x388]
//   0x7f7aec: 290140f9  ldr x9, [x9]
//   0x7f7af0: 291140f9  ldr x9, [x9, #0x20]
//   0x7f7af4: 290540f9  ldr x9, [x9, #8]
//   0x7f7af8: 2a8141b9  ldr w10, [x9, #0x180]
//   0x7f7afc: 4a011e12  and w10, w10, #4
//   0x7f7b00: 3f0100f1  cmp x9, #0
//   0x7f7b04: 4419407a  ccmp w10, #0, #4, ne
//   0x7f7b08: 81000054  b.ne #0x7f7b18
//   0x7f7b0c: e9031f2a  mov w9, wzr
//   0x7f7b10: 090100b9  str w9, [x8]
//   0x7f7b14: c0035fd6  ret
//   0x7f7b18: 298541b9  ldr w9, [x9, #0x184]
//   0x7f7b1c: 090100b9  str w9, [x8]
//   0x7f7b20: c0035fd6  ret
inline constexpr u32 PinSceneKindWords[] = {0xb0005829, 0xf941c529, 0xf9400129, 0xf9401129, 0xf9400529, 0xb941812a, 0x121e014a, 0xf100013f, 0x7a401944, 0x54000081, 0x2a1f03e9, 0xb9000109, 0xd65f03c0, 0xb9418529, 0xb9000109, 0xd65f03c0};
inline constexpr Pin PinSceneKind{0x7f7ae4, PinSceneKindWords, "SceneKind"};

// Phase: OE = [scene+0x190+8*4], RD = [OE+0x218], SEQ = [RD+0x50], phase u32 SEQ+0x38
//   0x87ec30: f30f1ef8  str x19, [sp, #-0x20]!
//   0x87ec34: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x87ec38: fd430091  add x29, sp, #0x10
//   0x87ec3c: a1e3fd97  bl #0x7f7ac0
//   0x87ec40: c00200b4  cbz x0, #0x87ec98
//   0x87ec44: f3031e32  mov w19, #4
//   0x87ec48: f30b00b9  str w19, [sp, #8]
//   0x87ec4c: e80b40b9  ldr w8, [sp, #8]
//   0x87ec50: 9ce3fd97  bl #0x7f7ac0
//   0x87ec54: 000100b4  cbz x0, #0x87ec74
//   0x87ec58: b3c31fb8  stur w19, [x29, #-4]
//   0x87ec5c: a8c39fb8  ldursw x8, [x29, #-4]
//   0x87ec60: 1f290071  cmp w8, #0xa
//   0x87ec64: 08319f9a  csel x8, x8, xzr, lo
//   0x87ec68: 080c088b  add x8, x0, x8, lsl #3
//   0x87ec6c: 08c940f9  ldr x8, [x8, #0x190]
//   0x87ec70: 02000014  b #0x87ec78
//   0x87ec74: e8031faa  mov x8, xzr
//   0x87ec78: 080d41f9  ldr x8, [x8, #0x218]
//   0x87ec7c: 082940f9  ldr x8, [x8, #0x50]
//   0x87ec80: 083940b9  ldr w8, [x8, #0x38]
//   0x87ec84: fd7b41a9  ldp x29, x30, [sp, #0x10]
//   0x87ec88: 1f150071  cmp w8, #5
//   0x87ec8c: e0179f1a  cset w0, eq
//   0x87ec90: f30742f8  ldr x19, [sp], #0x20
//   0x87ec94: c0035fd6  ret
inline constexpr u32 PinPhaseWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x97fde3a1, 0xb40002c0, 0x321e03f3, 0xb9000bf3, 0xb9400be8, 0x97fde39c, 0xb4000100, 0xb81fc3b3, 0xb89fc3a8, 0x7100291f, 0x9a9f3108, 0x8b080c08, 0xf940c908, 0x14000002, 0xaa1f03e8, 0xf9410d08, 0xf9402908, 0xb9403908, 0xa9417bfd, 0x7100151f, 0x1a9f17e0, 0xf84207f3, 0xd65f03c0};
inline constexpr Pin PinPhase{0x87ec30, PinPhaseWords, "Phase"};

// EngineClass: cc = 50*u32 RI+0x10 (+50)
//   0x50cc70: 18cc0d94  bl #0x87fcd0
//   0x50cc74: 970a40b9  ldr w23, [x20, #8]
//   0x50cc78: 081040b9  ldr w8, [x0, #0x10]
//   0x50cc7c: 49068052  mov w9, #0x32
//   0x50cc80: 087d091b  mul w8, w8, w9
inline constexpr u32 PinEngineClassWords[] = {0x940dcc18, 0xb9400a97, 0xb9401008, 0x52800649, 0x1b097d08};
inline constexpr Pin PinEngineClass{0x50cc70, PinEngineClassWords, "EngineClass"};

// Mirror: mirror u8 RI+0x26
//   0x507cc4: 03e00d94  bl #0x87fcd0
//   0x507cc8: 08984039  ldrb w8, [x0, #0x26]
//   0x507ccc: 28010034  cbz w8, #0x507cf0
inline constexpr u32 PinMirrorWords[] = {0x940de003, 0x39409808, 0x34000128};
inline constexpr Pin PinMirror{0x507cc4, PinMirrorWords, "Mirror"};

// MapYScale: course u32 RI+0x1a0 == 0x44 -> Y scale
//   0x508a9c: 8ddc0d94  bl #0x87fcd0
//   0x508aa0: 08a041b9  ldr w8, [x0, #0x1a0]
//   0x508aa4: 1f110171  cmp w8, #0x44
inline constexpr u32 PinMapYScaleWords[] = {0x940ddc8d, 0xb941a008, 0x7101111f};
inline constexpr Pin PinMapYScale{0x508a9c, PinMapYScaleWords, "MapYScale"};

// MapYScaleMul: Y scale float at rodata (ADRP+LDR s0)
//   0x508ac4: 694e00d0  adrp x9, #0xed6000
//   0x508ac8: 20e546bd  ldr s0, [x9, #0x6e4]
inline constexpr u32 PinMapYScaleMulWords[] = {0xd0004e69, 0xbd46e520};
inline constexpr Pin PinMapYScaleMul{0x508ac4, PinMapYScaleMulWords, "MapYScaleMul"};

// RaceCount: racer count s32 RI+0x180
//   0x532a3c: 1c8081b9  ldrsw x28, [x0, #0x180]
inline constexpr u32 PinRaceCountWords[] = {0xb981801c};
inline constexpr Pin PinRaceCount{0x532a3c, PinRaceCountWords, "RaceCount"};

// RaceRule: rule u32 RI+0x08
//   0x5129f4: b7b40d94  bl #0x87fcd0
//   0x5129f8: 080840b9  ldr w8, [x0, #8]
inline constexpr u32 PinRaceRuleWords[] = {0x940db4b7, 0xb9400808};
inline constexpr Pin PinRaceRule{0x5129f4, PinRaceRuleWords, "RaceRule"};

// LapsTotal: LM = [RD+0x58], laps u8 LM+0x64
//   0x5129d0: 080d41f9  ldr x8, [x8, #0x218]
//   0x5129d4: 082d40f9  ldr x8, [x8, #0x58]
//   0x5129d8: 680000b4  cbz x8, #0x5129e4
//   0x5129dc: 08914139  ldrb w8, [x8, #0x64]
inline constexpr u32 PinLapsTotalWords[] = {0xf9410d08, 0xf9402d08, 0xb4000068, 0x39419108};
inline constexpr Pin PinLapsTotal{0x5129d0, PinLapsTotalWords, "LapsTotal"};

// Timer: TM = [RD+0xe0] (hud+0xb8 = RD), time struct TM+0x40
//   0x515938: 685e40f9  ldr x8, [x19, #0xb8]
//   0x51593c: 736a40f9  ldr x19, [x19, #0xd0]
//   0x515940: e1031f2a  mov w1, wzr
//   0x515944: 087140f9  ldr x8, [x8, #0xe0]
//   0x515948: 00010191  add x0, x8, #0x40
inline constexpr u32 PinTimerWords[] = {0xf9405e68, 0xf9406a73, 0x2a1f03e1, 0xf9407108, 0x91010100};
inline constexpr Pin PinTimer{0x515938, PinTimerWords, "Timer"};

// TimerMin: u8 T+4 minutes
//   0x515a64: 81124039  ldrb w1, [x20, #4]
inline constexpr u32 PinTimerMinWords[] = {0x39401281};
inline constexpr Pin PinTimerMin{0x515a64, PinTimerMinWords, "TimerMin"};

// TimerSec: u8 T+5 seconds
//   0x515a74: 81164039  ldrb w1, [x20, #5]
inline constexpr u32 PinTimerSecWords[] = {0x39401681};
inline constexpr Pin PinTimerSec{0x515a74, PinTimerSecWords, "TimerSec"};

// TimerMs: u16 T+6 milliseconds
//   0x515c44: 810e4079  ldrh w1, [x20, #6]
inline constexpr u32 PinTimerMsWords[] = {0x79400e81};
inline constexpr Pin PinTimerMs{0x515c44, PinTimerMsWords, "TimerMs"};

// Checkers: count u32 RD+0x60, CK[i] = [[RD+0x68]+8i]
//   0x5127d0: 080d41f9  ldr x8, [x8, #0x218]
//   0x5127d4: 694e4339  ldrb w9, [x19, #0xd3]
//   0x5127d8: 0a6140b9  ldr w10, [x8, #0x60]
//   0x5127dc: 5f01096b  cmp w10, w9
//   0x5127e0: 4481497a  ccmp w10, w9, #4, hi
//   0x5127e4: 8d000054  b.le #0x5127f4
//   0x5127e8: 083540f9  ldr x8, [x8, #0x68]
//   0x5127ec: 087969f8  ldr x8, [x8, x9, lsl #3]
inline constexpr u32 PinCheckersWords[] = {0xf9410d08, 0x39434e69, 0xb940610a, 0x6b09015f, 0x7a498144, 0x5400008d, 0xf9403508, 0xf8697908};
inline constexpr Pin PinCheckers{0x5127d0, PinCheckersWords, "Checkers"};

// CheckerRank: rank u16 CK+0x40
//   0x513830: 08814079  ldrh w8, [x8, #0x40]
inline constexpr u32 PinCheckerRankWords[] = {0x79408108};
inline constexpr Pin PinCheckerRank{0x513830, PinCheckerRankWords, "CheckerRank"};

// CheckerCoins: coins u8 CK+0x50
//   0x512c98: 685e40f9  ldr x8, [x19, #0xb8]
//   0x512c9c: 14414139  ldrb w20, [x8, #0x50]
inline constexpr u32 PinCheckerCoinsWords[] = {0xf9405e68, 0x39414114};
inline constexpr Pin PinCheckerCoins{0x512c98, PinCheckerCoinsWords, "CheckerCoins"};

// CheckerLap: lap u32 CK+0x44, shown min(v+1, total)
//   0x512cf0: 685e40f9  ldr x8, [x19, #0xb8]
//   0x512cf4: 084540b9  ldr w8, [x8, #0x44]
//   0x512cf8: 6a4a4339  ldrb w10, [x19, #0xd2]
//   0x512cfc: 09050011  add w9, w8, #1
//   0x512d00: 291d0012  and w9, w9, #0xff
//   0x512d04: 3f010a6b  cmp w9, w10
//   0x512d08: 69464339  ldrb w9, [x19, #0xd1]
//   0x512d0c: 4885881a  csinc w8, w10, w8, hi
inline constexpr u32 PinCheckerLapWords[] = {0xf9405e68, 0xb9404508, 0x39434a6a, 0x11000509, 0x12001d29, 0x6b0a013f, 0x39434669, 0x1a888548};
inline constexpr Pin PinCheckerLap{0x512cf0, PinCheckerLapWords, "CheckerLap"};

// CheckerGoalCheck: checker update: u32 CK+0x44 lap >= u8 LM+0x64 -> goal path
//   0x881614: 694640b9  ldr w9, [x19, #0x44]
//   0x881618: 8a924139  ldrb w10, [x20, #0x64]
//   0x88161c: 3f010a6b  cmp w9, w10
inline constexpr u32 PinCheckerGoalCheckWords[] = {0xb9404669, 0x3941928a, 0x6b0a013f};
inline constexpr Pin PinCheckerGoalCheck{0x881614, PinCheckerGoalCheckWords, "CheckerGoalCheck"};

// CheckerGoalSet: goal: u32 CK+0x3c |= 1 (main+0x880ff0)
//   0x8810bc: 683e40b9  ldr w8, [x19, #0x3c]
//   0x8810c0: 08010032  orr w8, w8, #1
//   0x8810c4: 683e00b9  str w8, [x19, #0x3c]
inline constexpr u32 PinCheckerGoalSetWords[] = {0xb9403e68, 0x32000108, 0xb9003e68};
inline constexpr Pin PinCheckerGoalSet{0x8810bc, PinCheckerGoalSetWords, "CheckerGoalSet"};

// CheckerGoalDone: after goal: u32 CK+0x3c |= 4 (main+0x8810e0)
//   0x881124: 683e40b9  ldr w8, [x19, #0x3c]
//   0x881128: 08011e32  orr w8, w8, #4
//   0x88112c: 683e00b9  str w8, [x19, #0x3c]
inline constexpr u32 PinCheckerGoalDoneWords[] = {0xb9403e68, 0x321e0108, 0xb9003e68};
inline constexpr Pin PinCheckerGoalDone{0x881124, PinCheckerGoalDoneWords, "CheckerGoalDone"};

// DriverEntry: driver u32 [entry+0x40-4] (RI+0x3c+0x1c*i)
//   0x532bc0: e8c25fb8  ldur w8, [x23, #-4]
inline constexpr u32 PinDriverEntryWords[] = {0xb85fc2e8};
inline constexpr Pin PinDriverEntry{0x532bc0, PinDriverEntryWords, "DriverEntry"};

// DriverVariant: variant u8 RI+0x40+0x1c*i
//   0x532bd0: e3024039  ldrb w3, [x23]
inline constexpr u32 PinDriverVariantWords[] = {0x394002e3};
inline constexpr Pin PinDriverVariant{0x532bd0, PinDriverVariantWords, "DriverVariant"};

// KartDirector: KD = [OE+0x238], kart count s32 KD+0xc0
//   0x508254: 081d41f9  ldr x8, [x8, #0x238]
//   0x508258: 18c180b9  ldrsw x24, [x8, #0xc0]
inline constexpr u32 PinKartDirectorWords[] = {0xf9411d08, 0xb980c118};
inline constexpr Pin PinKartDirector{0x508254, PinKartDirectorWords, "KartDirector"};

// KartUnit: KU[i] = [KD+0x50+8i], i < 12
//   0x5082b8: 081d41f9  ldr x8, [x8, #0x238]
//   0x5082bc: 3f310071  cmp w9, #0xc
//   0x5082c0: 29319f9a  csel x9, x9, xzr, lo
//   0x5082c4: 080d098b  add x8, x8, x9, lsl #3
//   0x5082c8: 152940f9  ldr x21, [x8, #0x50]
inline constexpr u32 PinKartUnitWords[] = {0xf9411d08, 0x7100313f, 0x9a9f3129, 0x8b090d08, 0xf9402915};
inline constexpr Pin PinKartUnit{0x5082b8, PinKartUnitWords, "KartUnit"};

// KartUnitPlayer: KU vtable slot 0: V = [KU+0x48], player id u32 V+0xa8
//   0x141c4c: 082440f9  ldr x8, [x0, #0x48]
//   0x141c50: 00a940b9  ldr w0, [x8, #0xa8]
//   0x141c54: c0035fd6  ret
inline constexpr u32 PinKartUnitPlayerWords[] = {0xf9402408, 0xb940a900, 0xd65f03c0};
inline constexpr Pin PinKartUnitPlayer{0x141c4c, PinKartUnitPlayerWords, "KartUnitPlayer"};

// KartUnitPos: KU vtable slot 1: slot3() + 0x24
//   0x141c58: fd7bbfa9  stp x29, x30, [sp, #-0x10]!
//   0x141c5c: fd030091  mov x29, sp
//   0x141c60: 080040f9  ldr x8, [x0]
//   0x141c64: 080d40f9  ldr x8, [x8, #0x18]
//   0x141c68: 00013fd6  blr x8
//   0x141c6c: 00900091  add x0, x0, #0x24
//   0x141c70: fd7bc1a8  ldp x29, x30, [sp], #0x10
inline constexpr u32 PinKartUnitPosWords[] = {0xa9bf7bfd, 0x910003fd, 0xf9400008, 0xf9400d08, 0xd63f0100, 0x91009000, 0xa8c17bfd};
inline constexpr Pin PinKartUnitPos{0x141c58, PinKartUnitPosWords, "KartUnitPos"};

// KartUnitMove: KU vtable slot 3: [KU+0x50] + 8
//   0x141c94: 082840f9  ldr x8, [x0, #0x50]
//   0x141c98: 00210091  add x0, x8, #8
//   0x141c9c: c0035fd6  ret
inline constexpr u32 PinKartUnitMoveWords[] = {0xf9402808, 0x91002100, 0xd65f03c0};
inline constexpr Pin PinKartUnitMove{0x141c94, PinKartUnitMoveWords, "KartUnitMove"};

// ItemDirector: ID = [OE+0x240], owners [[ID+0xe8]+8i], i < u32 ID+0xe0
//   0x510d60: 082141f9  ldr x8, [x8, #0x240]
//   0x510d64: 69c280b9  ldrsw x9, [x19, #0xc0]
//   0x510d68: 0ae140b9  ldr w10, [x8, #0xe0]
//   0x510d6c: 5f01096b  cmp w10, w9
//   0x510d70: 89000054  b.ls #0x510d80
//   0x510d74: 087540f9  ldr x8, [x8, #0xe8]
//   0x510d78: 087969f8  ldr x8, [x8, x9, lsl #3]
inline constexpr u32 PinItemDirectorWords[] = {0xf9412108, 0xb980c269, 0xb940e10a, 0x6b09015f, 0x54000089, 0xf9407508, 0xf8697908};
inline constexpr Pin PinItemDirector{0x510d60, PinItemDirectorWords, "ItemDirector"};

// ItemOwnerCreate: owners: for i < RI+0x180: owner = new Owner(i); append to [ID+0xe8] (count ID+0xe0)
//   0x002b04: 73f42194  bl #0x87fcd0
//   0x002b08: 158041b9  ldr w21, [x0, #0x180]
//   0x002b0c: 60820391  add x0, x19, #0xe0
//   0x002b10: e3031d32  mov w3, #8
//   0x002b14: e103152a  mov w1, w21
//   0x002b18: e2031faa  mov x2, xzr
//   0x002b1c: 85231894  bl #0x60b930
//   0x002b20: bf060071  cmp w21, #1
//   0x002b24: 2b030054  b.lt #0x2b88
//   0x002b28: f6031f2a  mov w22, wzr
//   0x002b2c: 17348052  mov w23, #0x1a0
//   0x002b30: e00317aa  mov x0, x23
//   0x002b34: 2e211894  bl #0x60afec
//   0x002b38: e103162a  mov w1, w22
//   0x002b3c: f80300aa  mov x24, x0
//   0x002b40: e3f20094  bl #0x3f6cc
//   0x002b44: 68265c29  ldp w8, w9, [x19, #0xe0]
//   0x002b48: 087d4093  sxtw x8, w8
//   0x002b4c: 1f01096b  cmp w8, w9
//   0x002b50: ca000054  b.ge #0x2b68
//   0x002b54: 697640f9  ldr x9, [x19, #0xe8]
//   0x002b58: 387928f8  str x24, [x9, x8, lsl #3]
//   0x002b5c: 68e240b9  ldr w8, [x19, #0xe0]
//   0x002b60: 08050011  add w8, w8, #1
//   0x002b64: 68e200b9  str w8, [x19, #0xe0]
inline constexpr u32 PinItemOwnerCreateWords[] = {0x9421f473, 0xb9418015, 0x91038260, 0x321d03e3, 0x2a1503e1, 0xaa1f03e2, 0x94182385, 0x710006bf, 0x5400032b, 0x2a1f03f6, 0x52803417, 0xaa1703e0, 0x9418212e, 0x2a1603e1, 0xaa0003f8, 0x9400f2e3, 0x295c2668, 0x93407d08, 0x6b09011f, 0x540000ca, 0xf9407669, 0xf8287938, 0xb940e268, 0x11000508, 0xb900e268};
inline constexpr Pin PinItemOwnerCreate{0x2b04, PinItemOwnerCreateWords, "ItemOwnerCreate"};

// ItemOwnerCtor: Owner ctor stores player index w1 at +0x40 (stp wzr,w20,[x19,#0x3c])
//   0x03f6d8: f403012a  mov w20, w1
//   0x03f6dc: f30300aa  mov x19, x0
//   0x03f6e0: 23e81d94  bl #0x7b976c
//   0x03f6e4: 7fe20039  strb wzr, [x19, #0x38]
//   0x03f6e8: e8950090  adrp x8, #0x12fb000
//   0x03f6ec: 087145f9  ldr x8, [x8, #0xae0]
//   0x03f6f0: 09008012  mov w9, #-1
//   0x03f6f4: 7f7e05a9  stp xzr, xzr, [x19, #0x50]
//   0x03f6f8: 7f2600f9  str xzr, [x19, #0x48]
//   0x03f6fc: 08410091  add x8, x8, #0x10
//   0x03f700: 7fd20729  stp wzr, w20, [x19, #0x3c]
inline constexpr u32 PinItemOwnerCtorWords[] = {0x2a0103f4, 0xaa0003f3, 0x941de823, 0x3900e27f, 0x900095e8, 0xf9457108, 0x12800009, 0xa9057e7f, 0xf900267f, 0x91004108, 0x2907d27f};
inline constexpr Pin PinItemOwnerCtor{0x3f6d8, PinItemOwnerCtorWords, "ItemOwnerCtor"};

// ItemSlotItem: slot k<2 = [IO+0x60+8k], item u32 S+0xc8
//   0x040fdc: 297c4093  sxtw x9, w1
//   0x040fe0: 3f080071  cmp w1, #2
//   0x040fe4: 29319f9a  csel x9, x9, xzr, lo
//   0x040fe8: 090c098b  add x9, x0, x9, lsl #3
//   0x040fec: 293140f9  ldr x9, [x9, #0x60]
//   0x040ff0: 29c940b9  ldr w9, [x9, #0xc8]
//   0x040ff4: 090100b9  str w9, [x8]
//   0x040ff8: c0035fd6  ret
inline constexpr u32 PinItemSlotItemWords[] = {0x93407c29, 0x7100083f, 0x9a9f3129, 0x8b090c09, 0xf9403129, 0xb940c929, 0xb9000109, 0xd65f03c0};
inline constexpr Pin PinItemSlotItem{0x40fdc, PinItemSlotItemWords, "ItemSlotItem"};

// ItemSlotState: state u8 S+0x41: 3 held, 1..2 roulette
//   0x045c0c: 08044139  ldrb w8, [x0, #0x41]
//   0x045c10: 1f0d0071  cmp w8, #3
//   0x045c14: e0179f1a  cset w0, eq
//   0x045c18: c0035fd6  ret
//   0x045c1c: 08044139  ldrb w8, [x0, #0x41]
//   0x045c20: 08050051  sub w8, w8, #1
//   0x045c24: 1f090071  cmp w8, #2
//   0x045c28: e0279f1a  cset w0, lo
//   0x045c2c: c0035fd6  ret
//   0x045c30: 08044139  ldrb w8, [x0, #0x41]
//   0x045c34: 1f090071  cmp w8, #2
//   0x045c38: e0179f1a  cset w0, eq
inline constexpr u32 PinItemSlotStateWords[] = {0x39410408, 0x71000d1f, 0x1a9f17e0, 0xd65f03c0, 0x39410408, 0x51000508, 0x7100091f, 0x1a9f27e0, 0xd65f03c0, 0x39410408, 0x7100091f, 0x1a9f17e0};
inline constexpr Pin PinItemSlotState{0x45c0c, PinItemSlotStateWords, "ItemSlotState"};

// ItemSlotEmpty: state 0 = empty
//   0x045c64: 08044139  ldrb w8, [x0, #0x41]
//   0x045c68: 68000034  cbz w8, #0x45c74
inline constexpr u32 PinItemSlotEmptyWords[] = {0x39410408, 0x34000068};
inline constexpr Pin PinItemSlotEmpty{0x45c64, PinItemSlotEmptyWords, "ItemSlotEmpty"};

// ItemSlotCount: uses left u32 S+0xa0 set from S+0x9c
//   0x0453a0: 689e40b9  ldr w8, [x19, #0x9c]
//   0x0453a4: e80f00b9  str w8, [sp, #0xc]
//   0x0453a8: e80f40b9  ldr w8, [sp, #0xc]
//   0x0453ac: 09050011  add w9, w8, #1
//   0x0453b0: 3f550071  cmp w9, #0x15
//   0x0453b4: 28010054  b.hi #0x453d8
//   0x0453b8: ea760090  adrp x10, #0xf21000
//   0x0453bc: 4aa10991  add x10, x10, #0x268
//   0x0453c0: e8031f2a  mov w8, wzr
//   0x0453c4: 4979a9b8  ldrsw x9, [x10, x9, lsl #2]
//   0x0453c8: 29010a8b  add x9, x9, x10
//   0x0453cc: 20011fd6  br x9
//   0x0453d0: e8070032  mov w8, #3
//   0x0453d4: 04000014  b #0x453e4
//   0x0453d8: e8030032  mov w8, #1
//   0x0453dc: 02000014  b #0x453e4
//   0x0453e0: e8031d32  mov w8, #8
//   0x0453e4: 68a200b9  str w8, [x19, #0xa0]
inline constexpr u32 PinItemSlotCountWords[] = {0xb9409e68, 0xb9000fe8, 0xb9400fe8, 0x11000509, 0x7100553f, 0x54000128, 0x900076ea, 0x9109a14a, 0x2a1f03e8, 0xb8a97949, 0x8b0a0129, 0xd61f0120, 0x320007e8, 0x14000004, 0x320003e8, 0x14000002, 0x321d03e8, 0xb900a268};
inline constexpr Pin PinItemSlotCount{0x453a0, PinItemSlotCountWords, "ItemSlotCount"};

// ItemFrame: item -> pattern frame byte table (ADRP+ADD), coin override
//   0x4234a8: 085800f0  adrp x8, #0xf26000
//   0x4234ac: 08713291  add x8, x8, #0xc9c
//   0x4234b0: 1f3c0071  cmp w0, #0xf
//   0x4234b4: 13c96038  ldrb w19, [x8, w0, sxtw]
//   0x4234b8: c1030054  b.ne #0x423530
//   0x4234bc: 05721194  bl #0x87fcd0
//   0x4234c0: a82300d1  sub x8, x29, #8
//   0x4234c4: 80711194  bl #0x87fac4
//   0x4234c8: a8835fb8  ldur w8, [x29, #-8]
//   0x4234cc: 1f050071  cmp w8, #1
//   0x4234d0: 61000054  b.ne #0x4234dc
//   0x4234d4: 33038052  mov w19, #0x19
//   0x4234d8: 16000014  b #0x423530
//   0x4234dc: fd711194  bl #0x87fcd0
//   0x4234e0: e8630091  add x8, sp, #0x18
//   0x4234e4: 78711194  bl #0x87fac4
//   0x4234e8: e81b40b9  ldr w8, [sp, #0x18]
//   0x4234ec: 1f090071  cmp w8, #2
//   0x4234f0: 61000054  b.ne #0x4234fc
//   0x4234f4: 53038052  mov w19, #0x1a
//   0x4234f8: 0e000014  b #0x423530
//   0x4234fc: f5711194  bl #0x87fcd0
//   0x423500: e8430091  add x8, sp, #0x10
//   0x423504: 70711194  bl #0x87fac4
//   0x423508: e81340b9  ldr w8, [sp, #0x10]
//   0x42350c: 1f0d0071  cmp w8, #3
//   0x423510: 00010054  b.eq #0x423530
//   0x423514: ef711194  bl #0x87fcd0
//   0x423518: e8230091  add x8, sp, #8
//   0x42351c: 6a711194  bl #0x87fac4
//   0x423520: e80b40b9  ldr w8, [sp, #8]
//   0x423524: 1f110071  cmp w8, #4
//   0x423528: a8038052  mov w8, #0x1d
//   0x42352c: 1301931a  csel w19, w8, w19, eq
//   0x423530: e003132a  mov w0, w19
//   0x423534: fd7b43a9  ldp x29, x30, [sp, #0x30]
//   0x423538: f31340f9  ldr x19, [sp, #0x20]
//   0x42353c: ff030191  add sp, sp, #0x40
//   0x423540: c0035fd6  ret
inline constexpr u32 PinItemFrameWords[] = {0xf0005808, 0x91327108, 0x71003c1f, 0x3860c913, 0x540003c1, 0x94117205, 0xd10023a8, 0x94117180, 0xb85f83a8, 0x7100051f, 0x54000061, 0x52800333, 0x14000016, 0x941171fd, 0x910063e8, 0x94117178, 0xb9401be8, 0x7100091f, 0x54000061, 0x52800353, 0x1400000e, 0x941171f5, 0x910043e8, 0x94117170, 0xb94013e8, 0x71000d1f, 0x54000100, 0x941171ef, 0x910023e8, 0x9411716a, 0xb9400be8, 0x7100111f, 0x528003a8, 0x1a930113, 0x2a1303e0, 0xa9437bfd, 0xf94013f3, 0x910103ff, 0xd65f03c0};
inline constexpr Pin PinItemFrame{0x4234a8, PinItemFrameWords, "ItemFrame"};

// ItemTripleFrame: HUD count <= 2 of a triple item (7/17/18/19) -> frame (count==2 ? 1/17/18/19 : 0/3/5/7)
//   0x512150: 287c4093  sxtw x8, w1
//   0x512154: 3f080071  cmp w1, #2
//   0x512158: 08319f9a  csel x8, x8, xzr, lo
//   0x51215c: 0908088b  add x9, x0, x8, lsl #2
//   0x512160: 280942b9  ldr w8, [x9, #0x208]
//   0x512164: 1f090071  cmp w8, #2
//   0x512168: 4d000054  b.le #0x512170
//   0x51216c: c0035fd6  ret
//   0x512170: 291142b9  ldr w9, [x9, #0x210]
//   0x512174: 291d0051  sub w9, w9, #7
//   0x512178: 3f310071  cmp w9, #0xc
//   0x51217c: 88ffff54  b.hi #0x51216c
//   0x512180: ca500090  adrp x10, #0xf2a000
//   0x512184: 4a611591  add x10, x10, #0x558
//   0x512188: 4979a9b8  ldrsw x9, [x10, x9, lsl #2]
//   0x51218c: 29010a8b  add x9, x9, x10
//   0x512190: 20011fd6  br x9
//   0x512194: 0b3840f9  ldr x11, [x0, #0x70]
//   0x512198: 0a6840b9  ldr w10, [x0, #0x68]
//   0x51219c: 29781f53  lsl w9, w1, #1
//   0x5121a0: 291d0011  add w9, w9, #7
//   0x5121a4: 6ccd298b  add x12, x11, w9, sxtw #3
//   0x5121a8: 5f01096b  cmp w10, w9
//   0x5121ac: 89818b9a  csel x9, x12, x11, hi
//   0x5121b0: 1f090071  cmp w8, #2
//   0x5121b4: 200140f9  ldr x0, [x9]
//   0x5121b8: e003271e  fmov s0, wzr
//   0x5121bc: 01102e1e  fmov s1, #1.00000000
//   0x5121c0: 24000014  b #0x512250
//   0x5121c4: 0b3840f9  ldr x11, [x0, #0x70]
//   0x5121c8: 0a6840b9  ldr w10, [x0, #0x68]
//   0x5121cc: 29781f53  lsl w9, w1, #1
//   0x5121d0: 291d0011  add w9, w9, #7
//   0x5121d4: 6ccd298b  add x12, x11, w9, sxtw #3
//   0x5121d8: 5f01096b  cmp w10, w9
//   0x5121dc: 89818b9a  csel x9, x12, x11, hi
//   0x5121e0: 1f090071  cmp w8, #2
//   0x5121e4: 200140f9  ldr x0, [x9]
//   0x5121e8: 0010211e  fmov s0, #3.00000000
//   0x5121ec: 0130261e  fmov s1, #17.00000000
//   0x5121f0: 18000014  b #0x512250
//   0x5121f4: 0b3840f9  ldr x11, [x0, #0x70]
//   0x5121f8: 0a6840b9  ldr w10, [x0, #0x68]
//   0x5121fc: 29781f53  lsl w9, w1, #1
//   0x512200: 291d0011  add w9, w9, #7
//   0x512204: 6ccd298b  add x12, x11, w9, sxtw #3
//   0x512208: 5f01096b  cmp w10, w9
//   0x51220c: 89818b9a  csel x9, x12, x11, hi
//   0x512210: 1f090071  cmp w8, #2
//   0x512214: 200140f9  ldr x0, [x9]
//   0x512218: 0090221e  fmov s0, #5.00000000
//   0x51221c: 0150261e  fmov s1, #18.00000000
//   0x512220: 0c000014  b #0x512250
//   0x512224: 0b3840f9  ldr x11, [x0, #0x70]
//   0x512228: 0a6840b9  ldr w10, [x0, #0x68]
//   0x51222c: 29781f53  lsl w9, w1, #1
//   0x512230: 291d0011  add w9, w9, #7
//   0x512234: 6ccd298b  add x12, x11, w9, sxtw #3
//   0x512238: 5f01096b  cmp w10, w9
//   0x51223c: 89818b9a  csel x9, x12, x11, hi
//   0x512240: 1f090071  cmp w8, #2
//   0x512244: 200140f9  ldr x0, [x9]
//   0x512248: 0090231e  fmov s0, #7.00000000
//   0x51224c: 0170261e  fmov s1, #19.00000000
//   0x512250: 200c201e  fcsel s0, s1, s0, eq
//   0x512254: e1031f2a  mov w1, wzr
inline constexpr u32 PinItemTripleFrameWords[] = {0x93407c28, 0x7100083f, 0x9a9f3108, 0x8b080809, 0xb9420928, 0x7100091f, 0x5400004d, 0xd65f03c0, 0xb9421129, 0x51001d29, 0x7100313f, 0x54ffff88, 0x900050ca, 0x9115614a, 0xb8a97949, 0x8b0a0129, 0xd61f0120, 0xf940380b, 0xb940680a, 0x531f7829, 0x11001d29, 0x8b29cd6c, 0x6b09015f, 0x9a8b8189, 0x7100091f, 0xf9400120, 0x1e2703e0, 0x1e2e1001, 0x14000024, 0xf940380b, 0xb940680a, 0x531f7829, 0x11001d29, 0x8b29cd6c, 0x6b09015f, 0x9a8b8189, 0x7100091f, 0xf9400120, 0x1e211000, 0x1e263001, 0x14000018, 0xf940380b, 0xb940680a, 0x531f7829, 0x11001d29, 0x8b29cd6c, 0x6b09015f, 0x9a8b8189, 0x7100091f, 0xf9400120, 0x1e229000, 0x1e265001, 0x1400000c, 0xf940380b, 0xb940680a, 0x531f7829, 0x11001d29, 0x8b29cd6c, 0x6b09015f, 0x9a8b8189, 0x7100091f, 0xf9400120, 0x1e239000, 0x1e267001, 0x1e200c20, 0x2a1f03e1};
inline constexpr Pin PinItemTripleFrame{0x512150, PinItemTripleFrameWords, "ItemTripleFrame"};

// CoinKind: course coin kind from RI+0x1a0
//   0x87fac4: ffc300d1  sub sp, sp, #0x30
//   0x87fac8: f44f01a9  stp x20, x19, [sp, #0x10]
//   0x87facc: fd7b02a9  stp x29, x30, [sp, #0x20]
//   0x87fad0: fd830091  add x29, sp, #0x20
//   0x87fad4: f40300aa  mov x20, x0
//   0x87fad8: f30308aa  mov x19, x8
//   0x87fadc: 88a241b9  ldr w8, [x20, #0x1a0]
//   0x87fae0: 1fcd0071  cmp w8, #0x33
//   0x87fae4: 61000054  b.ne #0x87faf0
//   0x87fae8: e8030032  mov w8, #1
//   0x87faec: 0e000014  b #0x87fb24
//   0x87faf0: e0230091  add x0, sp, #8
//   0x87faf4: e80b00b9  str w8, [sp, #8]
//   0x87faf8: 9cfcff97  bl #0x87ed68
//   0x87fafc: 60000036  tbz w0, #0, #0x87fb08
//   0x87fb00: e8031f32  mov w8, #2
//   0x87fb04: 08000014  b #0x87fb24
//   0x87fb08: 88a241b9  ldr w8, [x20, #0x1a0]
//   0x87fb0c: 1f990171  cmp w8, #0x66
//   0x87fb10: e9179f1a  cset w9, eq
//   0x87fb14: 1f190171  cmp w8, #0x46
//   0x87fb18: e8070032  mov w8, #3
//   0x87fb1c: 29751e53  lsl w9, w9, #2
//   0x87fb20: 0801891a  csel w8, w8, w9, eq
//   0x87fb24: 680200b9  str w8, [x19]
//   0x87fb28: fd7b42a9  ldp x29, x30, [sp, #0x20]
//   0x87fb2c: f44f41a9  ldp x20, x19, [sp, #0x10]
//   0x87fb30: ffc30091  add sp, sp, #0x30
//   0x87fb34: c0035fd6  ret
inline constexpr u32 PinCoinKindWords[] = {0xd100c3ff, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xaa0003f4, 0xaa0803f3, 0xb941a288, 0x7100cd1f, 0x54000061, 0x320003e8, 0x1400000e, 0x910023e0, 0xb9000be8, 0x97fffc9c, 0x36000060, 0x321f03e8, 0x14000008, 0xb941a288, 0x7101991f, 0x1a9f17e9, 0x7101191f, 0x320007e8, 0x531e7529, 0x1a890108, 0xb9000268, 0xa9427bfd, 0xa9414ff4, 0x9100c3ff, 0xd65f03c0};
inline constexpr Pin PinCoinKind{0x87fac4, PinCoinKindWords, "CoinKind"};

// AnimalCourse: raw 0x34 + bits of 0x7001
//   0x87ed68: 080040b9  ldr w8, [x0]
//   0x87ed6c: 08d10051  sub w8, w8, #0x34
//   0x87ed70: 1f390071  cmp w8, #0xe
//   0x87ed74: c8000054  b.hi #0x87ed8c
//   0x87ed78: 08390012  and w8, w8, #0x7fff
//   0x87ed7c: 29008e52  mov w9, #0x7001
//   0x87ed80: 2825c81a  lsr w8, w9, w8
//   0x87ed84: 00010012  and w0, w8, #1
//   0x87ed88: c0035fd6  ret
inline constexpr u32 PinAnimalCourseWords[] = {0xb9400008, 0x5100d108, 0x7100391f, 0x540000c8, 0x12003908, 0x528e0029, 0x1ac82528, 0x12000100, 0xd65f03c0};
inline constexpr Pin PinAnimalCourse{0x87ed68, PinAnimalCourseWords, "AnimalCourse"};

// SystemEngine: SE = [[root+0x28]+0x190]
//   0x8bbf94: ff8300d1  sub sp, sp, #0x20
//   0x8bbf98: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x8bbf9c: fd430091  add x29, sp, #0x10
//   0x8bbfa0: b1eefc97  bl #0x7f7a64
//   0x8bbfa4: bfc31fb8  stur wzr, [x29, #-4]
//   0x8bbfa8: a8c39fb8  ldursw x8, [x29, #-4]
//   0x8bbfac: 1f290071  cmp w8, #0xa
//   0x8bbfb0: fd7b41a9  ldp x29, x30, [sp, #0x10]
//   0x8bbfb4: 08319f9a  csel x8, x8, xzr, lo
//   0x8bbfb8: 080c088b  add x8, x0, x8, lsl #3
//   0x8bbfbc: 00c940f9  ldr x0, [x8, #0x190]
//   0x8bbfc0: ff830091  add sp, sp, #0x20
//   0x8bbfc4: c0035fd6  ret
inline constexpr u32 PinSystemEngineWords[] = {0xd10083ff, 0xa9017bfd, 0x910043fd, 0x97fceeb1, 0xb81fc3bf, 0xb89fc3a8, 0x7100291f, 0xa9417bfd, 0x9a9f3108, 0x8b080c08, 0xf940c900, 0x910083ff, 0xd65f03c0};
inline constexpr Pin PinSystemEngine{0x8bbf94, PinSystemEngineWords, "SystemEngine"};

// PlayerManager: PM = [SE+0x800]
//   0x8bd33c: fd7bbfa9  stp x29, x30, [sp, #-0x10]!
//   0x8bd340: fd030091  mov x29, sp
//   0x8bd344: 14fbff97  bl #0x8bbf94
//   0x8bd348: 400000b4  cbz x0, #0x8bd350
//   0x8bd34c: 000044f9  ldr x0, [x0, #0x800]
//   0x8bd350: fd7bc1a8  ldp x29, x30, [sp], #0x10
//   0x8bd354: c0035fd6  ret
inline constexpr u32 PinPlayerManagerWords[] = {0xa9bf7bfd, 0x910003fd, 0x97fffb14, 0xb4000040, 0xf9440000, 0xa8c17bfd, 0xd65f03c0};
inline constexpr Pin PinPlayerManager{0x8bd33c, PinPlayerManagerWords, "PlayerManager"};

// LocalPlayerObj: PM+0x17c+4k, k<=3
//   0x87f10c: f30f1ef8  str x19, [sp, #-0x20]!
//   0x87f110: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x87f114: fd430091  add x29, sp, #0x10
//   0x87f118: f303002a  mov w19, w0
//   0x87f11c: 7f0e0071  cmp w19, #3
//   0x87f120: 69000054  b.ls #0x87f12c
//   0x87f124: 00008012  mov w0, #-1
//   0x87f128: 04000014  b #0x87f138
//   0x87f12c: 84f80094  bl #0x8bd33c
//   0x87f130: 08c8338b  add x8, x0, w19, sxtw #2
//   0x87f134: 007d41b9  ldr w0, [x8, #0x17c]
inline constexpr u32 PinLocalPlayerObjWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x2a0003f3, 0x71000e7f, 0x54000069, 0x12800000, 0x14000004, 0x9400f884, 0x8b33c808, 0xb9417d00};
inline constexpr Pin PinLocalPlayerObj{0x87f10c, PinLocalPlayerObjWords, "LocalPlayerObj"};

// RacerPlayerObj: PM+0x1cc+4i, i<=11
//   0x87edf8: f30f1ef8  str x19, [sp, #-0x20]!
//   0x87edfc: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x87ee00: fd430091  add x29, sp, #0x10
//   0x87ee04: f303002a  mov w19, w0
//   0x87ee08: 7f2e0071  cmp w19, #0xb
//   0x87ee0c: 69000054  b.ls #0x87ee18
//   0x87ee10: 00008012  mov w0, #-1
//   0x87ee14: 04000014  b #0x87ee24
//   0x87ee18: 49f90094  bl #0x8bd33c
//   0x87ee1c: 08c8338b  add x8, x0, w19, sxtw #2
//   0x87ee20: 00cd41b9  ldr w0, [x8, #0x1cc]
inline constexpr u32 PinRacerPlayerObjWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x2a0003f3, 0x71002e7f, 0x54000069, 0x12800000, 0x14000004, 0x9400f949, 0x8b33c808, 0xb941cd00};
inline constexpr Pin PinRacerPlayerObj{0x87edf8, PinRacerPlayerObjWords, "RacerPlayerObj"};

// UiRoot: D = [[[cell]]+8] (ADRP+LDR)
//   0x8c43a4: 085200f0  adrp x8, #0x1307000
//   0x8c43a8: 083d47f9  ldr x8, [x8, #0xe78]
//   0x8c43ac: 080140f9  ldr x8, [x8]
//   0x8c43b0: 000540f9  ldr x0, [x8, #8]
//   0x8c43b4: c0035fd6  ret
inline constexpr u32 PinUiRootWords[] = {0xf0005208, 0xf9473d08, 0xf9400108, 0xf9400500, 0xd65f03c0};
inline constexpr Pin PinUiRoot{0x8c43a4, PinUiRootWords, "UiRoot"};

// UiResource: R = [D+0x18+8k], k<13
//   0x8cf5c4: f30f1ef8  str x19, [sp, #-0x20]!
//   0x8cf5c8: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x8cf5cc: fd430091  add x29, sp, #0x10
//   0x8cf5d0: f30300aa  mov x19, x0
//   0x8cf5d4: 74d3ff97  bl #0x8c43a4
//   0x8cf5d8: 680240b9  ldr w8, [x19]
//   0x8cf5dc: a8c31fb8  stur w8, [x29, #-4]
//   0x8cf5e0: a8c39fb8  ldursw x8, [x29, #-4]
//   0x8cf5e4: fd7b41a9  ldp x29, x30, [sp, #0x10]
//   0x8cf5e8: 1f350071  cmp w8, #0xd
//   0x8cf5ec: 08319f9a  csel x8, x8, xzr, lo
//   0x8cf5f0: 080c088b  add x8, x0, x8, lsl #3
//   0x8cf5f4: 000d40f9  ldr x0, [x8, #0x18]
inline constexpr u32 PinUiResourceWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0x97ffd374, 0xb9400268, 0xb81fc3a8, 0xb89fc3a8, 0xa9417bfd, 0x7100351f, 0x9a9f3108, 0x8b080c08, 0xf9400d00};
inline constexpr Pin PinUiResource{0x8cf5c4, PinUiResourceWords, "UiResource"};

// UiTypeCheck: R3 used as [R3+0x30] after its type check
//   0x50cbb8: 880240f9  ldr x8, [x20]
//   0x50cbbc: 616f00f0  adrp x1, #0x12fb000
//   0x50cbc0: 080140f9  ldr x8, [x8]
//   0x50cbc4: 214047f9  ldr x1, [x1, #0xe80]
//   0x50cbc8: e00314aa  mov x0, x20
//   0x50cbcc: 00013fd6  blr x8
//   0x50cbd0: 1f000072  tst w0, #1
//   0x50cbd4: 88129f9a  csel x8, x20, xzr, ne
//   0x50cbd8: 151940f9  ldr x21, [x8, #0x30]
inline constexpr u32 PinUiTypeCheckWords[] = {0xf9400288, 0xf0006f61, 0xf9400108, 0xf9474021, 0xaa1403e0, 0xd63f0100, 0x7200001f, 0x9a9f1288, 0xf9401915};
inline constexpr Pin PinUiTypeCheck{0x50cbb8, PinUiTypeCheckWords, "UiTypeCheck"};

// DriverTableInit: DT = R3+0x298, pict table R3+0x350
//   0x402d0c: 60620a91  add x0, x19, #0x298
//   0x402d10: f73e0094  bl #0x4128ec
//   0x402d14: 60c20c91  add x0, x19, #0x330
//   0x402d18: 754f0094  bl #0x416aec
//   0x402d1c: 60420d91  add x0, x19, #0x350
inline constexpr u32 PinDriverTableInitWords[] = {0x910a6260, 0x94003ef7, 0x910cc260, 0x94004f75, 0x910d4260};
inline constexpr Pin PinDriverTableInit{0x402d0c, PinDriverTableInitWords, "DriverTableInit"};

// DriverName: name = names[id+1] (+0x80 char*[], count +0x78)
//   0x414d50: a80240b9  ldr w8, [x21]
//   0x414d54: e81f00b9  str w8, [sp, #0x1c]
//   0x414d58: e81f80b9  ldrsw x8, [sp, #0x1c]
//   0x414d5c: 8a4240f9  ldr x10, [x20, #0x80]
//   0x414d60: 897a40b9  ldr w9, [x20, #0x78]
//   0x414d64: 08050091  add x8, x8, #1
//   0x414d68: 4b0d088b  add x11, x10, x8, lsl #3
//   0x414d6c: 3f01086b  cmp w9, w8
//   0x414d70: 68818a9a  csel x8, x11, x10, hi
//   0x414d74: 180140f9  ldr x24, [x8]
inline constexpr u32 PinDriverNameWords[] = {0xb94002a8, 0xb9001fe8, 0xb9801fe8, 0xf940428a, 0xb9407a89, 0x91000508, 0x8b080d4b, 0x6b08013f, 0x9a8a8168, 0xf9400118};
inline constexpr Pin PinDriverName{0x414d50, PinDriverNameWords, "DriverName"};

// DriverMsg: msg = u32 [+0x90][id+1], count +0x88
//   0x412a28: a9c39fb8  ldursw x9, [x29, #-4]
//   0x412a2c: 6b4a40f9  ldr x11, [x19, #0x90]
//   0x412a30: 6a8a40b9  ldr w10, [x19, #0x88]
//   0x412a34: 29050091  add x9, x9, #1
//   0x412a38: 6c09098b  add x12, x11, x9, lsl #2
//   0x412a3c: 5f01096b  cmp w10, w9
//   0x412a40: 89818b9a  csel x9, x12, x11, hi
//   0x412a44: 280100b9  str w8, [x9]
inline constexpr u32 PinDriverMsgWords[] = {0xb89fc3a9, 0xf9404a6b, 0xb9408a6a, 0x91000529, 0x8b09096c, 0x6b09015f, 0x9a8b8189, 0xb9000128};
inline constexpr Pin PinDriverMsg{0x412a28, PinDriverMsgWords, "DriverMsg"};

// DriverSuffix: ids in mask 0x160000010010 get '0'+variant
//   0x414e74: a80240b9  ldr w8, [x21]
//   0x414e78: 1fb10071  cmp w8, #0x2c
//   0x414e7c: 080f0054  b.hi #0x41505c
//   0x414e80: e9030032  mov w9, #1
//   0x414e84: 2821c89a  lsl x8, x9, x8
//   0x414e88: 09c0c2d2  mov x9, #0x160000000000
//   0x414e8c: 2900a0f2  movk x9, #1, lsl #16
//   0x414e90: 090280f2  movk x9, #0x10
//   0x414e94: 0801098a  and x8, x8, x9
//   0x414e98: 280e00b4  cbz x8, #0x41505c
inline constexpr u32 PinDriverSuffixWords[] = {0xb94002a8, 0x7100b11f, 0x54000f08, 0x320003e9, 0x9ac82128, 0xd2c2c009, 0xf2a00029, 0xf2800209, 0x8a090108, 0xb4000e28};
inline constexpr Pin PinDriverSuffix{0x414e74, PinDriverSuffixWords, "DriverSuffix"};

// CoursePict: pict name = names[raw+1] (+0x80, count +0x78)
//   0x418dd0: a80240b9  ldr w8, [x21]
//   0x418dd4: e82700b9  str w8, [sp, #0x24]
//   0x418dd8: e82780b9  ldrsw x8, [sp, #0x24]
//   0x418ddc: 8a4240f9  ldr x10, [x20, #0x80]
//   0x418de0: 897a40b9  ldr w9, [x20, #0x78]
//   0x418de4: 08050091  add x8, x8, #1
//   0x418de8: 4b0d088b  add x11, x10, x8, lsl #3
//   0x418dec: 3f01086b  cmp w9, w8
//   0x418df0: 68818a9a  csel x8, x11, x10, hi
inline constexpr u32 PinCoursePictWords[] = {0xb94002a8, 0xb90027e8, 0xb98027e8, 0xf940428a, 0xb9407a89, 0x91000508, 0x8b080d4b, 0x6b08013f, 0x9a8a8168};
inline constexpr Pin PinCoursePict{0x418dd0, PinCoursePictWords, "CoursePict"};

// CupOfCourse: T = [R3+0x30]; {cup,slot} = T+0xc80+8*raw (raw<0x7b); cup name ptr table (ADRP+ADD)
//   0x50d158: 081940f9  ldr x8, [x8, #0x30]
//   0x50d15c: f76b00b9  str w23, [sp, #0x68]
//   0x50d160: e96b80b9  ldrsw x9, [sp, #0x68]
//   0x50d164: 3fed0171  cmp w9, #0x7b
//   0x50d168: 29319f9a  csel x9, x9, xzr, lo
//   0x50d16c: 180d098b  add x24, x8, x9, lsl #3
//   0x50d170: 08834cb9  ldr w8, [x24, #0xc80]
//   0x50d174: 1f050031  cmn w8, #1
//   0x50d178: 40340054  b.eq #0x50d800
//   0x50d17c: e82f00b9  str w8, [sp, #0x2c]
//   0x50d180: e82f40b9  ldr w8, [sp, #0x2c]
//   0x50d184: 1f5d0071  cmp w8, #0x17
//   0x50d188: e9028052  mov w9, #0x17
//   0x50d18c: 09b1891a  csel w9, w8, w9, lt
//   0x50d190: 1f010071  cmp w8, #0
//   0x50d194: a86b00f0  adrp x8, #0x1284000
//   0x50d198: 297d4093  sxtw x9, w9
//   0x50d19c: 08e10291  add x8, x8, #0xb8
//   0x50d1a0: e9b3899a  csel x9, xzr, x9, lt
inline constexpr u32 PinCupOfCourseWords[] = {0xf9401908, 0xb9006bf7, 0xb9806be9, 0x7101ed3f, 0x9a9f3129, 0x8b090d18, 0xb94c8308, 0x3100051f, 0x54003440, 0xb9002fe8, 0xb9402fe8, 0x71005d1f, 0x528002e9, 0x1a89b109, 0x7100011f, 0xf0006ba8, 0x93407d29, 0x9102e108, 0x9a89b3e9};
inline constexpr Pin PinCupOfCourse{0x50d158, PinCupOfCourseWords, "CupOfCourse"};

// CupMsg: cup name msg = u32 rodata table (ADRP+ADD)[cup], -1 when cup < 0
//   0x41fc00: 080040b9  ldr w8, [x0]
//   0x41fc04: 1f050031  cmn w8, #1
//   0x41fc08: 00010054  b.eq #0x41fc28
//   0x41fc0c: 080040b9  ldr w8, [x0]
//   0x41fc10: 0801f837  tbnz w8, #0x1f, #0x41fc30
//   0x41fc14: 090080b9  ldrsw x9, [x0]
//   0x41fc18: 285800f0  adrp x8, #0xf26000
//   0x41fc1c: 08513091  add x8, x8, #0xc14
//   0x41fc20: 007969b8  ldr w0, [x8, x9, lsl #2]
//   0x41fc24: c0035fd6  ret
inline constexpr u32 PinCupMsgWords[] = {0xb9400008, 0x3100051f, 0x54000100, 0xb9400008, 0x37f80108, 0xb9800009, 0xf0005828, 0x91305108, 0xb8697900, 0xd65f03c0};
inline constexpr Pin PinCupMsg{0x41fc00, PinCupMsgWords, "CupMsg"};

// DriverParamDb: driver param DB = [[[GOT (ADRP+LDR)]]+0xe8]
//   0x86ba7c: 745400f0  adrp x20, #0x12fa000
//   0x86ba80: 94ce46f9  ldr x20, [x20, #0xd98]
//   0x86ba84: 890240f9  ldr x9, [x20]
//   0x86ba88: e80301aa  mov x8, x1
//   0x86ba8c: 217540f9  ldr x1, [x9, #0xe8]
//   0x86ba90: f30300aa  mov x19, x0
//   0x86ba94: 60220091  add x0, x19, #8
//   0x86ba98: e20308aa  mov x2, x8
//   0x86ba9c: 7bffff97  bl #0x86b888
//   0x86baa0: 880240f9  ldr x8, [x20]
//   0x86baa4: 147540f9  ldr x20, [x8, #0xe8]
//   0x86baa8: 340300b4  cbz x20, #0x86bb0c
//   0x86baac: e00313aa  mov x0, x19
//   0x86bab0: e10314aa  mov x1, x20
//   0x86bab4: 04020094  bl #0x86c2c4
inline constexpr u32 PinDriverParamDbWords[] = {0xf0005474, 0xf946ce94, 0xf9400289, 0xaa0103e8, 0xf9407521, 0xaa0003f3, 0x91002260, 0xaa0803e2, 0x97ffff7b, 0xf9400288, 0xf9407514, 0xb4000334, 0xaa1303e0, 0xaa1403e1, 0x94000204};
inline constexpr Pin PinDriverParamDb{0x86ba7c, PinDriverParamDbWords, "DriverParamDb"};

// DriverParamRecord: record = [DB+0x30] + id*0x244 if 0 <= id < u16 DB+0xa2 (else DB+0xcc)
//   0x86c2f8: 291040f9  ldr x9, [x1, #0x20]
//   0x86c2fc: c90d00b4  cbz x9, #0x86c4b4
//   0x86c300: 291840f9  ldr x9, [x1, #0x30]
//   0x86c304: 690100b4  cbz x9, #0x86c330
//   0x86c308: aac35ab8  ldur w10, [x29, #-0x54]
//   0x86c30c: 2a01f837  tbnz w10, #0x1f, #0x86c330
//   0x86c310: aac35ab8  ldur w10, [x29, #-0x54]
//   0x86c314: 2b444179  ldrh w11, [x1, #0xa2]
//   0x86c318: 5f010b6b  cmp w10, w11
//   0x86c31c: aa000054  b.ge #0x86c330
//   0x86c320: aac39ab8  ldursw x10, [x29, #-0x54]
//   0x86c324: 8b488052  mov w11, #0x244
//   0x86c328: 5a250b9b  madd x26, x10, x11, x9
//   0x86c32c: 02000014  b #0x86c334
//   0x86c330: 3a300391  add x26, x1, #0xcc
inline constexpr u32 PinDriverParamRecordWords[] = {0xf9401029, 0xb4000dc9, 0xf9401829, 0xb4000169, 0xb85ac3aa, 0x37f8012a, 0xb85ac3aa, 0x7941442b, 0x6b0b015f, 0x540000aa, 0xb89ac3aa, 0x5280488b, 0x9b0b255a, 0x14000002, 0x9103303a};
inline constexpr Pin PinDriverParamRecord{0x86c2f8, PinDriverParamRecordWords, "DriverParamRecord"};

// EmblemFlag: suffix flag = bit 1 of u32 record+0x60 (-> +0xc94)
//   0x86c67c: 486340b9  ldr w8, [x26, #0x60]
//   0x86c680: 690a40b9  ldr w9, [x19, #8]
//   0x86c684: 08050153  ubfx w8, w8, #1, #1
//   0x86c688: 3fd10071  cmp w9, #0x34
//   0x86c68c: 68523239  strb w8, [x19, #0xc94]
inline constexpr u32 PinEmblemFlagWords[] = {0xb9406348, 0xb9400a69, 0x53010508, 0x7100d13f, 0x39325268};
inline constexpr Pin PinEmblemFlag{0x86c67c, PinEmblemFlagWords, "EmblemFlag"};

// EmblemCode: code = [[GOT (ADRP+LDR)]] + u32 record+0x10 (0 -> empty)
//   0x86ce3c: fa4740f9  ldr x26, [sp, #0x88]
//   0x86ce40: 29550090  adrp x9, #0x1310000
//   0x86ce44: 298544f9  ldr x9, [x9, #0x908]
//   0x86ce48: 481340b9  ldr w8, [x26, #0x10]
//   0x86ce4c: 290140f9  ldr x9, [x9]
//   0x86ce50: 743a46f9  ldr x20, [x19, #0xc70]
//   0x86ce54: 4a3300d0  adrp x10, #0xed6000
//   0x86ce58: 4ae12b91  add x10, x10, #0xaf8
//   0x86ce5c: 2901088b  add x9, x9, x8
//   0x86ce60: 1f010071  cmp w8, #0
//   0x86ce64: 4101899a  csel x1, x10, x9, eq
inline constexpr u32 PinEmblemCodeWords[] = {0xf94047fa, 0x90005529, 0xf9448529, 0xb9401348, 0xf9400129, 0xf9463a74, 0xd000334a, 0x912be14a, 0x8b080129, 0x7100011f, 0x9a890141};
inline constexpr Pin PinEmblemCode{0x86ce3c, PinEmblemCodeWords, "EmblemCode"};

// EmblemName: driver 0x34 / 0x1d (Mii) -> Emblem_Mii
//   0x86cef4: 680a40b9  ldr w8, [x19, #8]
//   0x86cef8: 77c20b91  add x23, x19, #0x2f0
//   0x86cefc: 1fd10071  cmp w8, #0x34
//   0x86cf00: 60000054  b.eq #0x86cf0c
//   0x86cf04: 1f750071  cmp w8, #0x1d
//   0x86cf08: e1040054  b.ne #0x86cfa4
inline constexpr u32 PinEmblemNameWords[] = {0xb9400a68, 0x910bc277, 0x7100d11f, 0x54000060, 0x7100751f, 0x540004e1};
inline constexpr Pin PinEmblemName{0x86cef4, PinEmblemNameWords, "EmblemName"};

// EmblemSuffix: Emblem_%s; if flag && driver != 0x10: + %02d(variant)
//   0x86cfa4: 683646f9  ldr x8, [x19, #0xc68]
//   0x86cfa8: 080d40f9  ldr x8, [x8, #0x18]
//   0x86cfac: 60a23191  add x0, x19, #0xc68
//   0x86cfb0: 00013fd6  blr x8
//   0x86cfb4: 623a46f9  ldr x2, [x19, #0xc70]
//   0x86cfb8: 01350090  adrp x1, #0xf0c000
//   0x86cfbc: 21a41d91  add x1, x1, #0x769
//   0x86cfc0: e00317aa  mov x0, x23
//   0x86cfc4: 63bff697  bl #0x61cd50
//   0x86cfc8: 68527239  ldrb w8, [x19, #0xc94]
//   0x86cfcc: 88faff34  cbz w8, #0x86cf1c
//   0x86cfd0: e83740f9  ldr x8, [sp, #0x68]
//   0x86cfd4: 080140b9  ldr w8, [x8]
//   0x86cfd8: a8c318b8  stur w8, [x29, #-0x74]
//   0x86cfdc: a8c358b8  ldur w8, [x29, #-0x74]
//   0x86cfe0: 1f410071  cmp w8, #0x10
//   0x86cfe4: c0f9ff54  b.eq #0x86cf1c
//   0x86cfe8: 620e40b9  ldr w2, [x19, #0xc]
//   0x86cfec: 01350090  adrp x1, #0xf0c000
//   0x86cff0: 21cc1d91  add x1, x1, #0x773
//   0x86cff4: e00317aa  mov x0, x23
inline constexpr u32 PinEmblemSuffixWords[] = {0xf9463668, 0xf9400d08, 0x9131a260, 0xd63f0100, 0xf9463a62, 0x90003501, 0x911da421, 0xaa1703e0, 0x97f6bf63, 0x39725268, 0x34fffa88, 0xf94037e8, 0xb9400108, 0xb818c3a8, 0xb858c3a8, 0x7100411f, 0x54fff9c0, 0xb9400e62, 0x90003501, 0x911dcc21, 0xaa1703e0};
inline constexpr Pin PinEmblemSuffix{0x86cfa4, PinEmblemSuffixWords, "EmblemSuffix"};

// PlayerInput: player input obj = [[SE+0x7f0]+0x58][p] + 0x38 (p < u32 [SE+0x7f0]+0x50)
//   0x8bd450: f30f1ef8  str x19, [sp, #-0x20]!
//   0x8bd454: fd7b01a9  stp x29, x30, [sp, #0x10]
//   0x8bd458: fd430091  add x29, sp, #0x10
//   0x8bd45c: f303002a  mov w19, w0
//   0x8bd460: cdfaff97  bl #0x8bbf94
//   0x8bd464: 600000b4  cbz x0, #0x8bd470
//   0x8bd468: 08f843f9  ldr x8, [x0, #0x7f0]
//   0x8bd46c: 02000014  b #0x8bd474
//   0x8bd470: e8031faa  mov x8, xzr
//   0x8bd474: 095140b9  ldr w9, [x8, #0x50]
//   0x8bd478: 3f01136b  cmp w9, w19
//   0x8bd47c: 89000054  b.ls #0x8bd48c
//   0x8bd480: 082d40f9  ldr x8, [x8, #0x58]
//   0x8bd484: 08d973f8  ldr x8, [x8, w19, sxtw #3]
//   0x8bd488: 02000014  b #0x8bd490
//   0x8bd48c: e8031faa  mov x8, xzr
//   0x8bd490: fd7b41a9  ldp x29, x30, [sp, #0x10]
//   0x8bd494: 00e10091  add x0, x8, #0x38
inline constexpr u32 PinPlayerInputWords[] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x2a0003f3, 0x97fffacd, 0xb4000060, 0xf943f808, 0x14000002, 0xaa1f03e8, 0xb9405109, 0x6b13013f, 0x54000089, 0xf9402d08, 0xf873d908, 0x14000002, 0xaa1f03e8, 0xa9417bfd, 0x9100e100};
inline constexpr Pin PinPlayerInput{0x8bd450, PinPlayerInputWords, "PlayerInput"};

// HornEvent: one-shot horn request u8 obj+0x35d: word +0x114 |= 0x100, then +0x35d = 0 (main+0x7d9260)
//   0x7d9200: 69764d39  ldrb w9, [x19, #0x35d]
//   0x7d9204: 681641b9  ldr w8, [x19, #0x114]
//   0x7d9208: 69000034  cbz w9, #0x7d9214
//   0x7d920c: 08011832  orr w8, w8, #0x100
//   0x7d9210: 681601b9  str w8, [x19, #0x114]
//   0x7d9214: 48001037  tbnz w8, #2, #0x7d921c
//   0x7d9218: 7f7a0d39  strb wzr, [x19, #0x35e]
//   0x7d921c: 6a7243b9  ldr w10, [x19, #0x370]
//   0x7d9220: aa000035  cbnz w10, #0x7d9234
//   0x7d9224: 6aea4679  ldrh w10, [x19, #0x374]
//   0x7d9228: 6a000035  cbnz w10, #0x7d9234
//   0x7d922c: 6a7a4d39  ldrb w10, [x19, #0x35e]
//   0x7d9230: aa050034  cbz w10, #0x7d92e4
//   0x7d9234: 69000035  cbnz w9, #0x7d9240
//   0x7d9238: 08791712  and w8, w8, #0xfffffeff
//   0x7d923c: 681601b9  str w8, [x19, #0x114]
//   0x7d9240: 89208052  mov w9, #0x104
//   0x7d9244: 0801090a  and w8, w8, w9
//   0x7d9248: 1f110071  cmp w8, #4
//   0x7d924c: 61000054  b.ne #0x7d9258
//   0x7d9250: e8030032  mov w8, #1
//   0x7d9254: 687a0d39  strb w8, [x19, #0x35e]
//   0x7d9258: 684643b9  ldr w8, [x19, #0x344]
//   0x7d925c: 1f090071  cmp w8, #2
//   0x7d9260: 7f760d39  strb wzr, [x19, #0x35d]
//   0x7d9264: 20030054  b.eq #0x7d92c8
inline constexpr u32 PinHornEventWords[] = {0x394d7669, 0xb9411668, 0x34000069, 0x32180108, 0xb9011668, 0x37100048, 0x390d7a7f, 0xb943726a, 0x350000aa, 0x7946ea6a, 0x3500006a, 0x394d7a6a, 0x340005aa, 0x35000069, 0x12177908, 0xb9011668, 0x52802089, 0x0a090108, 0x7100111f, 0x54000061, 0x320003e8, 0x390d7a68, 0xb9434668, 0x7100091f, 0x390d767f, 0x54000320};
inline constexpr Pin PinHornEvent{0x7d9200, PinHornEventWords, "HornEvent"};

// HornFill: kart input fill: horn byte (+7) = bit 8 of player action word +0x114
//   0x1797b8: a00a40b9  ldr w0, [x21, #8]
//   0x1797bc: f30301aa  mov x19, x1
//   0x1797c0: 240f1d94  bl #0x8bd450
//   0x1797c4: f40300aa  mov x20, x0
//   0x1797c8: 881641b9  ldr w8, [x20, #0x114]
//   0x1797cc: 08210853  ubfx w8, w8, #8, #1
//   0x1797d0: 681e0039  strb w8, [x19, #7]
inline constexpr u32 PinHornFillWords[] = {0xb9400aa0, 0xaa0103f3, 0x941d0f24, 0xaa0003f4, 0xb9411688, 0x53082108, 0x39001e68};
inline constexpr Pin PinHornFill{0x1797b8, PinHornFillWords, "HornFill"};

// HornBit: V+0x1cc bit 16 = u8 V+0x177 (horn)
//   0x173d44: 68de4539  ldrb w8, [x19, #0x177]
//   0x173d48: 69ce41b9  ldr w9, [x19, #0x1cc]
//   0x173d4c: 2a011032  orr w10, w9, #0x10000
//   0x173d50: 29790f12  and w9, w9, #0xfffeffff
//   0x173d54: 1f010071  cmp w8, #0
//   0x173d58: 4811891a  csel w8, w10, w9, ne
inline constexpr u32 PinHornBitWords[] = {0x3945de68, 0xb941ce69, 0x3210012a, 0x120f7929, 0x7100011f, 0x1a891148};
inline constexpr Pin PinHornBit{0x173d44, PinHornBitWords, "HornBit"};

// HornRequest: sound obj +0x16e7 = bit 0 of u16 V+0x1ce
//   0x12ffb8: 680640f9  ldr x8, [x19, #8]
//   0x12ffbc: 089d4379  ldrh w8, [x8, #0x1ce]
//   0x12ffc0: 08010012  and w8, w8, #1
//   0x12ffc4: e9dc8252  mov w9, #0x16e7
//   0x12ffc8: 08682938  strb w8, [x0, x9]
inline constexpr u32 PinHornRequestWords[] = {0xf9400668, 0x79439d08, 0x12000108, 0x5282dce9, 0x38296808};
inline constexpr Pin PinHornRequest{0x12ffb8, PinHornRequestWords, "HornRequest"};

// HornLatch: +0x16e8 = +0x16e7; +0x16e7 = 0
//   0x0ae9e0: e8dc8252  mov w8, #0x16e7
//   0x0ae9e4: 696a6838  ldrb w9, [x19, x8]
//   0x0ae9e8: 696a2a38  strb w9, [x19, x10]
//   0x0ae9ec: 7f6a2838  strb wzr, [x19, x8]
inline constexpr u32 PinHornLatchWords[] = {0x5282dce8, 0x38686a69, 0x382a6a69, 0x38286a7f};
inline constexpr Pin PinHornLatch{0xae9e0, PinHornLatchWords, "HornLatch"};

// HornPlay: play when +0x16e7 == 0 and +0x16e8 != 0 (release edge)
//   0x0af148: e8dc8252  mov w8, #0x16e7
//   0x0af14c: 686a6838  ldrb w8, [x19, x8]
//   0x0af150: c8000034  cbz w8, #0xaf168
//   0x0af154: fd7b50a9  ldp x29, x30, [sp, #0x100]
//   0x0af158: f44f4fa9  ldp x20, x19, [sp, #0xf0]
//   0x0af15c: fc574ea9  ldp x28, x21, [sp, #0xe0]
//   0x0af160: ff430491  add sp, sp, #0x110
//   0x0af164: c0035fd6  ret
//   0x0af168: 08dd8252  mov w8, #0x16e8
//   0x0af16c: 686a6838  ldrb w8, [x19, x8]
//   0x0af170: 28ffff34  cbz w8, #0xaf154
inline constexpr u32 PinHornPlayWords[] = {0x5282dce8, 0x38686a68, 0x340000c8, 0xa9507bfd, 0xa94f4ff4, 0xa94e57fc, 0x910443ff, 0xd65f03c0, 0x5282dd08, 0x38686a68, 0x34ffff28};
inline constexpr Pin PinHornPlay{0xaf148, PinHornPlayWords, "HornPlay"};

// CourseTokens: course enum text (ADRP+ADD)
//   0x01b54c: e9750090  adrp x9, #0xed7000
//   0x01b550: 29b51891  add x9, x9, #0x62d
inline constexpr u32 PinCourseTokensWords[] = {0x900075e9, 0x9118b529};
inline constexpr Pin PinCourseTokens{0x1b54c, PinCourseTokensWords, "CourseTokens"};

// CourseTokenIndex: token index = raw + 1
//   0x8af1e8: 00050011  add w0, w8, #1
//   0x8af1ec: aeb0dd97  bl #0x1b4a4
inline constexpr u32 PinCourseTokenIndexWords[] = {0x11000500, 0x97ddb0ae};
inline constexpr Pin PinCourseTokenIndex{0x8af1e8, PinCourseTokenIndexWords, "CourseTokenIndex"};
