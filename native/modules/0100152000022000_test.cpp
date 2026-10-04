// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// dsmod-mk8d-reader: synthetic-memory test of the 4.0.0 reader through the module ABI.
// A fake main image carries the build-pinned code words (mk8d_pins.inc) at their offsets and the
// GOT/rodata cells at the static 4.0.0 addresses (typed here independently of the reader),
// so the test also checks that the reader's ADRP/LDR/ADD decoding lands on those addresses.
// A fake heap carries one race. No Eden/Core dependency.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "mk8d_reader.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(std::uint32_t, std::uint64_t);
extern "C" const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(std::uint32_t, std::uint64_t);
extern "C" const EdenDsmodModuleWriteExtensions* eden_dsmod_get_write_extensions(std::uint32_t,
                                                                                 std::uint64_t);

namespace {
using Mk8dReader::Pin;
using namespace dsmod_sdk::int_types;
#include "mk8d_pins.inc"

constexpr std::array<const Pin*, 70> AllPins{&PinRaceInfoHolder,
                                             &PinRaceInfo,
                                             &PinActiveScene,
                                             &PinSceneKind,
                                             &PinPhase,
                                             &PinEngineClass,
                                             &PinMirror,
                                             &PinMapYScale,
                                             &PinMapYScaleMul,
                                             &PinRaceCount,
                                             &PinRaceRule,
                                             &PinLapsTotal,
                                             &PinTimer,
                                             &PinTimerMin,
                                             &PinTimerSec,
                                             &PinTimerMs,
                                             &PinCheckers,
                                             &PinCheckerRank,
                                             &PinCheckerCoins,
                                             &PinCheckerLap,
                                             &PinDriverEntry,
                                             &PinDriverVariant,
                                             &PinKartDirector,
                                             &PinKartUnit,
                                             &PinKartUnitPlayer,
                                             &PinKartUnitPos,
                                             &PinKartUnitMove,
                                             &PinItemDirector,
                                             &PinItemSlotItem,
                                             &PinItemSlotState,
                                             &PinItemSlotEmpty,
                                             &PinItemSlotCount,
                                             &PinItemFrame,
                                             &PinCoinKind,
                                             &PinAnimalCourse,
                                             &PinSystemEngine,
                                             &PinPlayerManager,
                                             &PinLocalPlayerObj,
                                             &PinRacerPlayerObj,
                                             &PinUiRoot,
                                             &PinUiResource,
                                             &PinUiTypeCheck,
                                             &PinDriverTableInit,
                                             &PinDriverName,
                                             &PinDriverMsg,
                                             &PinDriverSuffix,
                                             &PinCoursePict,
                                             &PinCupOfCourse,
                                             &PinCourseTokens,
                                             &PinCourseTokenIndex,
                                             &PinCupMsg,
                                             &PinDriverParamDb,
                                             &PinDriverParamRecord,
                                             &PinEmblemFlag,
                                             &PinEmblemCode,
                                             &PinEmblemName,
                                             &PinEmblemSuffix,
                                             &PinItemOwnerCreate,
                                             &PinItemOwnerCtor,
                                             &PinItemTripleFrame,
                                             &PinCheckerGoalCheck,
                                             &PinCheckerGoalSet,
                                             &PinCheckerGoalDone,
                                             &PinPlayerInput,
                                             &PinHornEvent,
                                             &PinHornFill,
                                             &PinHornBit,
                                             &PinHornRequest,
                                             &PinHornLatch,
                                             &PinHornPlay};

struct Host {
    std::map<u64, std::array<u8, 0x1000>> pages;
    std::unordered_map<std::string, std::int64_t> ints;
    std::unordered_map<std::string, double> floats;
    std::unordered_map<std::string, std::string> texts;
    u64 tick{};
    u64 reads{};
    // Torn-read simulation: after `mutate_after` reads, write mutate_value at mutate_at.
    u64 mutate_after{}, mutate_at{}, mutate_value{};
    void Map(u64 at, u64 n) {
        for (u64 p = at & ~u64{0xfff}; p < at + n; p += 0x1000)
            pages.try_emplace(p);
    }
    void Bytes(u64 at, const void* src, u64 n) {
        Map(at, n);
        const auto* b = static_cast<const u8*>(src);
        for (u64 i = 0; i < n; ++i)
            pages[(at + i) & ~u64{0xfff}][(at + i) & 0xfff] = b[i];
    }
    template <typename T>
    void Put(u64 at, const T& v) {
        Bytes(at, &v, sizeof(T));
    }
    void Str(u64 at, const std::string& s) {
        Bytes(at, s.c_str(), s.size() + 1);
        Map(at, s.size() + 128); // the reader reads strings in 64-byte chunks
    }
};
EdenDsmodBool Mapped(void* p, u64 at, u64 n) {
    auto& h = *static_cast<Host*>(p);
    for (u64 q = at & ~u64{0xfff}; q < at + n; q += 0x1000)
        if (!h.pages.contains(q))
            return 0;
    return 1;
}
EdenDsmodBool ReadMem(void* p, u64 at, void* out, size_t n) {
    auto& h = *static_cast<Host*>(p);
    if (!Mapped(p, at, n))
        return 0;
    if (h.mutate_after && ++h.reads == h.mutate_after)
        h.Put(h.mutate_at, h.mutate_value);
    auto* b = static_cast<u8*>(out);
    for (size_t i = 0; i < n; ++i)
        b[i] = h.pages[(at + i) & ~u64{0xfff}][(at + i) & 0xfff];
    return 1;
}
u64 Tick(void* p) {
    return static_cast<Host*>(p)->tick;
}
void I64(void* p, const char* n, std::int64_t v) {
    static_cast<Host*>(p)->ints[n] = v;
}
void F64(void* p, const char* n, double v) {
    static_cast<Host*>(p)->floats[n] = v;
}
void Text(void* p, const char* n, const char* v) {
    static_cast<Host*>(p)->texts[n] = v;
}
size_t NoRomfs(void*, const char*, u64, void*, size_t) {
    return 0;
}

constexpr u64 Base = 0x8000000000, MainSize = 0x1370000;
constexpr u8 Build[16] = {0x2C, 0x33, 0x6A, 0x9B, 0xCF, 0x79, 0xC3, 0x04,
                          0x0C, 0xE5, 0x06, 0xCD, 0xD3, 0x91, 0xB5, 0x78};

EdenDsmodHostApi Api(Host& h) {
    EdenDsmodHostApi api{};
    api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    api.struct_size = sizeof(api);
    api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    api.userdata = &h;
    api.title_id = UINT64_C(0x0100152000022000);
    std::memcpy(api.build_id, Build, sizeof(Build));
    api.main_base = Base;
    api.main_size = MainSize;
    api.get_tick = Tick;
    api.is_mapped = Mapped;
    api.read_memory = ReadMem;
    api.publish_i64 = I64;
    api.publish_f64 = F64;
    api.publish_text = Text;
    api.read_romfs = NoRomfs;
    return api;
}

// Heap layout.
constexpr u64 Root = 0x100000, SceneHolder = 0x101000, Scene = 0x102000, Oe = 0x103000;
constexpr u64 Rd = 0x104000, Seq = 0x105000, Lm = 0x105100, Tm = 0x105200, CkArr = 0x105300;
constexpr u64 Ck0 = 0x106000; // + 0x100 * i
constexpr u64 Engine = 0x108000, RiHolder = 0x109000, Ri = 0x10a000, Se = 0x10b000;
constexpr u64 Pm = 0x10c000, Kd = 0x10d000, Ku0 = 0x10e000, Veh0 = 0x110000, Move0 = 0x112000;
constexpr u64 Id = 0x114000, Owners = 0x114100, Io = 0x114200, S0 = 0x114400, S1 = 0x114600;
constexpr u64 UiVarObj = 0x116000, UiD = 0x116100, R3 = 0x117000, CupT = 0x119000;
constexpr u64 NamesArr = 0x11c000, MsgArr = 0x11c400, PictArr = 0x11c800, Strs = 0x11d000;

void BuildGame(Host& h, int n) {
    for (const Pin* p : AllPins)
        h.Bytes(Base + p->offset, p->words.data(), p->words.size() * 4);
    // GOT slots / rodata / data at the static 4.0.0 addresses.
    h.Put(Base + 0x12fc388, Base + 0x135b170); // root GOT slot -> &root var
    h.Put(Base + 0x135b170, Root);
    h.Put(Base + 0x1307e78, Base + 0x1360000); // ui GOT slot -> &ui var
    h.Put(Base + 0x1360000, UiVarObj);
    h.Put(UiVarObj + 8, UiD);
    h.Put(UiD + 0x30, R3);
    std::string tokens = "Invalid ,Gu_Menu ,Test ";
    for (int i = 1; i <= 14; ++i)
        tokens += ",Reserved" + std::string(i < 10 ? "0" : "") + std::to_string(i) + " ";
    tokens += ",Gu_MarioCircuit ,Gu_DossunIseki ,Gu_City ,Gu_Cake ,Gu_HorrorHouse ,Gu_Expert "
              ",Gu_Desert ,Gu_Cloud ,Gu_SnowMountain ,Gu_Techno ,Gu_Airport ,Gu_FirstCircuit";
    h.Str(Base + 0xed762d, tokens); // token[0x1b + 1] = Gu_FirstCircuit
    const std::array<u8, 23> frames{3,  5,  7,  0,  9,  11, 16, 2, 13, 15, 14, 12,
                                    10, 22, 20, 21, 24, 4,  6,  8, 23, 27, 28};
    h.Bytes(Base + 0xf26c9c, frames.data(), frames.size());
    std::array<s32, 24> cupmsg{};
    for (int c = 0; c < 24; ++c)
        cupmsg[static_cast<size_t>(c)] = c < 12 ? 1301 + c : 11601 + c - 12;
    h.Put(Base + 0xf26c14, cupmsg);
    const float yscale = 6.2f;
    h.Put(Base + 0xed66e4, yscale);
    const char* cups[] = {"Mushroom", "Flower", "Star", "Special"};
    for (int c = 0; c < 24; ++c) {
        const u64 s = Base + 0x1290000 + 0x40 * static_cast<u64>(c);
        h.Str(s, c < 4 ? cups[c] : "Charger" + std::to_string(c));
        h.Put(Base + 0x12840b8 + 8 * static_cast<u64>(c), s);
    }
    // UI resource R3: driver table (+0x298), picture table (+0x350), cup table [+0x30].
    const std::vector<std::string> drivers{"Invalid", "Mario", "Luigi", "Peach", "Daisy", "Yoshi"};
    h.Put(R3 + 0x298 + 0x78, static_cast<u32>(drivers.size()));
    h.Put(R3 + 0x298 + 0x80, NamesArr);
    for (size_t i = 0; i < drivers.size(); ++i) {
        h.Str(Strs + 0x40 * i, drivers[i]);
        h.Put(NamesArr + 8 * i, Strs + 0x40 * i);
    }
    const std::vector<s32> msgs{-1, 1001, 1002, 1003, 1013, 1004};
    h.Put(R3 + 0x298 + 0x88, static_cast<u32>(msgs.size()));
    h.Put(R3 + 0x298 + 0x90, MsgArr);
    h.Bytes(MsgArr, msgs.data(), msgs.size() * 4);
    std::vector<std::string> picts(0x1d, "Random_00");
    picts[0x1b + 1] = "Gu_FirstCircuit_00";
    h.Put(R3 + 0x350 + 0x78, static_cast<u32>(picts.size()));
    h.Put(R3 + 0x350 + 0x80, PictArr);
    for (size_t i = 0; i < picts.size(); ++i) {
        h.Str(Strs + 0x1000 + 0x40 * i, picts[i]);
        h.Put(PictArr + 8 * i, Strs + 0x1000 + 0x40 * i);
    }
    h.Put(R3 + 0x30, CupT);
    const std::array<s32, 2> cup_slot{0, 0};
    h.Put(CupT + 0xc80 + 8 * 0x1b, cup_slot);
    const std::array<s32, 3> entry{0x1b, -1, 1401};
    h.Put(CupT, entry);

    // Driver param DB (emblem codes): GOT main+0x12fad98 -> var -> obj, DB = [obj+0xe8];
    // string pool GOT main+0x1310908 -> var -> pool.
    constexpr u64 DbObj = 0x130000, Db = 0x131000, Recs = 0x132000, Pool = 0x140000;
    h.Put(Base + 0x12fad98, Base + 0x1361000);
    h.Put(Base + 0x1361000, DbObj);
    h.Put(DbObj + 0xe8, Db);
    h.Put(Db + 0x20, u64{1});
    h.Put(Db + 0x30, Recs);
    h.Put(Db + 0xa2, u16{0x35});
    h.Put(Base + 0x1310908, Base + 0x1362000);
    h.Put(Base + 0x1362000, Pool);
    const char* codes[] = {"Mro", "Lig", "Pch", "Dsy", "Ysi"};
    for (int d = 0; d < 5; ++d) {
        h.Str(Pool + 0x10 + 0x10 * static_cast<u64>(d), codes[d]);
        h.Put(Recs + 0x244 * static_cast<u64>(d) + 0x10, u32(0x10 + 0x10 * d));
        h.Put(Recs + 0x244 * static_cast<u64>(d) + 0x60, u32(d == 4 ? 2 : 0));
    }

    // Player input objects [SE+0x7f0]: count +0x50, array +0x58; obj = entry + 0x38.
    constexpr u64 Inputs = 0x150000, InArr = 0x150100, Entry0 = 0x151000;
    h.Put(Se + 0x7f0, Inputs);
    h.Put(Inputs + 0x50, u32{1});
    h.Put(Inputs + 0x58, InArr);
    h.Put(InArr, Entry0);
    h.Put(Entry0 + 0x38 + 0x35d, u8{0}); // horn request byte

    // Framework.
    h.Put(Root + 0x20, SceneHolder);
    h.Put(SceneHolder + 8, Scene);
    h.Put(Scene + 0x180, u32{4});
    h.Put(Scene + 0x184, u32{4}); // Race
    h.Put(Scene + 0x1b0, Oe);
    h.Put(Oe + 0x218, Rd);
    h.Put(Oe + 0x238, Kd);
    h.Put(Oe + 0x240, Id);
    h.Put(Root + 0x28, Engine);
    h.Put(Engine + 0x240, RiHolder);
    h.Put(Engine + 0x190, Se);
    h.Put(Se + 0x800, Pm);
    h.Put(RiHolder + 0x10, Ri);
    h.Map(Ri, 0x1a4);
    h.Put(Ri + 0x08, u32{0});
    h.Put(Ri + 0x10, u32{2}); // 150cc
    h.Put(Ri + 0x26, u8{0});
    h.Put(Ri + 0x180, s32{n});
    h.Put(Ri + 0x1a0, u32{0x1b});
    h.Put(Rd + 0x50, Seq);
    h.Put(Seq + 0x38, u32{5});
    h.Put(Rd + 0x58, Lm);
    h.Put(Lm + 0x64, u8{3});
    h.Put(Rd + 0x60, u32(n));
    h.Put(Rd + 0x68, CkArr);
    h.Put(Rd + 0xe0, Tm);
    const std::array<u8, 8> time{0, 0, 0, 0, 1, 23, 0x2d, 0x01}; // 1:23.301
    h.Put(Tm + 0x40, time);
    h.Put(Kd + 0xc0, s32{n});
    h.Put(Pm + 0x17c, s32{7}); // local controller 0 -> player object 7
    h.Put(Inputs + 0x50, u32{8});
    h.Put(InArr + 8 * 7, Entry0);
    for (int i = 0; i < n; ++i) {
        const u64 ck = Ck0 + 0x100 * static_cast<u64>(i);
        h.Put(CkArr + 8 * static_cast<u64>(i), ck);
        h.Put(ck + 0x40, static_cast<u16>((i + 3) % n)); // rank index
        h.Put(ck + 0x44, u32(i == 2 ? 1 : 0));
        h.Put(ck + 0x50, static_cast<u8>(i));
        h.Put(ck + 0x3c, u32(i == 9 ? 5 : 0)); // racer 9 finished
        h.Put(Ri + 0x30 + 0x1c * static_cast<u64>(i) + 0x0c, u32(i % 5));
        h.Put(Ri + 0x30 + 0x1c * static_cast<u64>(i) + 0x10, u8(i == 4 ? 3 : 0));
        const u64 ku = Ku0 + 0x100 * static_cast<u64>(i), veh = Veh0 + 0x100 * static_cast<u64>(i),
                  mv = Move0 + 0x100 * static_cast<u64>(i);
        h.Put(Kd + 0x50 + 8 * static_cast<u64>(i), ku);
        h.Put(ku + 0x48, veh);
        h.Put(ku + 0x50, mv);
        h.Put(veh + 0xa8, u32(i));
        const std::array<float, 3> pos{10.0f * static_cast<float>(i), 0.0f, 5.0f};
        h.Put(mv + 0x2c, pos);
        h.Put(Pm + 0x1cc + 4 * static_cast<u64>(i), s32(i == 2 ? 7 : 100 + i)); // racer 2 = me
    }
    // Items of racer 2.
    h.Put(Id + 0xe0, u32(n));
    h.Put(Id + 0xe8, Owners);
    for (int i = 0; i < n; ++i) {
        // Owner i (Owner ctor stores i at +0x40); racer 2's owner is Io with slots S0/S1.
        const u64 io = i == 2 ? Io : 0x120000 + 0x400 * static_cast<u64>(i);
        const u64 s0 = i == 2 ? S0 : io + 0x100, s1 = i == 2 ? S1 : io + 0x200;
        h.Put(Owners + 8 * static_cast<u64>(i), io);
        h.Put(io + 0x40, u32(i));
        h.Put(io + 0x60, s0);
        h.Put(io + 0x68, s1);
        h.Map(s0, 0x100);
        h.Map(s1, 0x100);
    }
    // Racer 7 holds a banana (item 0) in slot 0.
    h.Put(0x120000 + 0x400 * 7 + 0x100 + 0x41, u8{3});
    h.Put(0x120000 + 0x400 * 7 + 0x100 + 0xa0, u32{1});
    h.Put(S0 + 0x41, u8{3});
    h.Put(S0 + 0xa0, u32{3});
    h.Put(S0 + 0xc4, u32{7});
    h.Put(S0 + 0xc8, u32{7}); // Mush3
    h.Put(S1 + 0x41, u8{1});  // roulette
    h.Put(S1 + 0xc8, u32{4});
}

const EdenDsmodModuleApi& Module() {
    const auto* m =
        eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(m);
    return *m;
}

void Step(const EdenDsmodModuleApi& m, void* inst, Host& h, const EdenDsmodHostApi& api) {
    h.tick += 2;
    m.sample(inst, &api);
}

void TestRace() {
    Host h;
    BuildGame(h, 12);
    const auto api = Api(h);
    const auto& m = Module();
    assert(m.supports_build("2C336A9BCF79C3040CE506CDD391B57800000000000000000000000000000000"));
    assert(!m.supports_build("FE941ED5BA14BE5D505698DA1BBF4FE700000000000000000000000000000000"));
    void* inst = m.create(&api, "{}");
    assert(inst);
    Step(m, inst, h, api);
    Step(m, inst, h, api);
    if (h.ints["mk.ready"] != 1)
        std::fprintf(stderr, "diag: %s\n", h.texts["mk.diag"].c_str());
    assert(h.ints["mk.ready"] == 1);
    assert(h.ints["mk.scene"] == 4);
    assert(h.ints["mk.phase"] == 5);
    assert(h.ints["mk.course"] == 0x1b);
    assert(h.ints["mk.cc"] == 150);
    assert(h.ints["mk.mode"] == 0);
    assert(h.ints["mk.laps_total"] == 3);
    assert(h.ints["mk.time_ms"] == 83301);
    assert(h.ints["mk.cup"] == 0);
    assert(h.texts["mk.course_folder"] == "Gu_FirstCircuit");
    assert(h.texts["mk.map_key"] == "module:mk8d:map/Gu_FirstCircuit");
    assert(h.texts["mk.pict_key"] == "module:mk8d:coursepict/Gu_FirstCircuit");
    assert(h.texts["mk.cup_key"] == "module:mk8d:cup/Mushroom");
    assert(h.ints["racers.count"] == 12);
    assert(h.ints["r0.valid"] == 1);
    assert(h.ints["r0.rank"] == 4); // index 3 -> 4th
    assert(h.ints["r2.lap"] == 2);  // raw 1 -> "LAP 2"
    assert(h.ints["r5.coins"] == 5);
    assert(h.ints["r9.finished"] == 1 && h.ints["r5.finished"] == 0 && h.ints["me.finished"] == 0);
    assert(h.ints["r1.driver"] == 1);
    assert(h.texts["r1.icon"] == "module:mk8d:chara/Luigi");
    assert(h.ints["r4.driver"] == 4 && h.ints["r4.variant"] == 3);
    assert(h.texts["r4.icon"] == "module:mk8d:chara/Yoshi03"); // variant-mask driver 4
    assert(h.texts["r4.emblem_key"] == "module:mk8d:emblem/Ysi03");
    assert(h.texts["r1.emblem_key"] == "module:mk8d:emblem/Lig");
    assert(h.texts["me.emblem_key"] == "module:mk8d:emblem/Pch"); // racer 2 = driver 2
    assert(h.ints["me.slot"] == 2);
    assert(h.ints["r2.is_me"] == 1 && h.ints["r0.is_me"] == 0);
    assert(h.ints["me.rank"] == 6 && h.ints["me.lap"] == 2 && h.ints["me.coins"] == 2);
    assert(h.ints["me.item0"] == 7 && h.ints["me.item0_count"] == 3);
    assert(h.ints["me.item1"] == 4 && h.ints["me.item1_roulette"] == 1);
    assert(h.ints["me.item_roulette"] == 0);
    assert(h.ints["r2.item0"] == 7 && h.ints["r2.item_state"] == 2 &&
           h.ints["r2.item0_count"] == 3);
    assert(h.ints["r2.item1"] == 4 && h.ints["r2.item1_state"] == 1);
    assert(h.ints["r7.item0"] == 0 && h.ints["r7.item_state"] == 2 && h.ints["r7.item1"] == -1);
    assert(h.ints["r5.item0"] == -1 && h.ints["r5.item_state"] == 0);
    // No romfs here: no map camera, so no map positions (fail closed).
    assert(h.ints["r0.map_ok"] == 0 && h.floats["r0.map_x"] == -1.0);

    // Torn read: the race director pointer changes in the middle of a sample.
    h.reads = 0;
    h.mutate_after = 40;
    h.mutate_at = Oe + 0x218;
    h.mutate_value = Rd + 0x10;
    Step(m, inst, h, api);
    assert(h.ints["mk.ready"] == 0);
    assert(h.texts["mk.diag"].find("torn") != std::string::npos);
    h.mutate_after = 0;
    h.Put(Oe + 0x218, Rd);
    Step(m, inst, h, api);
    assert(h.ints["mk.ready"] == 1);

    // Phase 0 (loading): checkers not initialised yet -> no racers, no timer.
    h.Put(Seq + 0x38, u32{0});
    Step(m, inst, h, api);
    assert(h.ints["mk.ready"] == 1 && h.ints["mk.phase"] == 0);
    assert(h.ints["racers.count"] == 0 && h.ints["r0.valid"] == 0 && h.ints["me.slot"] == -1);
    assert(h.ints["mk.time_ms"] == -1);
    h.Put(Seq + 0x38, u32{5});
    Step(m, inst, h, api);
    assert(h.ints["racers.count"] == 12 && h.ints["mk.time_ms"] == 83301);

    // Not in a race scene: context only, no racers.
    h.Put(Scene + 0x184, u32{3});
    Step(m, inst, h, api);
    assert(h.ints["mk.ready"] == 1 && h.ints["mk.phase"] == -1 && h.ints["racers.count"] == 0);
    assert(h.ints["me.slot"] == -1 && h.ints["me.item0"] == -1);
    m.destroy(inst);
}

std::vector<std::string> g_write_log;
EdenDsmodBool FakeBatch(void* p, const EdenDsmodWriteOp* ops, std::uint32_t n) {
    auto& h = *static_cast<Host*>(p);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::vector<u8> now(ops[i].size);
        if (!ReadMem(p, ops[i].address, now.data(), now.size()))
            return 0;
        if (ops[i].expect && std::memcmp(now.data(), ops[i].expect, now.size()) != 0)
            return 0;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        h.Bytes(ops[i].address, ops[i].value, ops[i].size);
        g_write_log.push_back(std::to_string(ops[i].address));
    }
    return 1;
}

void TestHorn() {
    Host h;
    BuildGame(h, 12);
    h.Put(Seq + 0x38, u32{6}); // racing
    const auto api = Api(h);
    const auto& m = Module();
    void* inst = m.create(&api, "{}");
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    const auto* wext =
        eden_dsmod_get_write_extensions(EDEN_DSMOD_WRITE_EXT_VERSION, EDEN_DSMOD_WRITE_EXT_HASH);
    assert(ext && wext);
    Step(m, inst, h, api);
    assert(h.ints["mk.horn_ok"] == 0); // no write extension yet
    assert(!ext->on_action(inst, "mk.horn_write", 0));
    EdenDsmodHostWriteApi w{EDEN_DSMOD_WRITE_EXT_VERSION, sizeof(w), EDEN_DSMOD_WRITE_EXT_HASH, &h,
                            &FakeBatch};
    wext->configure(inst, &w);
    Step(m, inst, h, api);
    assert(h.ints["mk.horn_ok"] == 1);
    const u64 req = 0x151000 + 0x38 + 0x35d; // horn request byte
    h.Map(req, 1);
    const auto rd = [&](u64 at) {
        u8 v{};
        ReadMem(&h, at, &v, 1);
        return v;
    };
    assert(ext->on_action(inst, "mk.horn_write", 0));
    Step(m, inst, h, api); // request written (press starts)
    assert(rd(req) == 1 && h.ints["mk.horn_busy"] == 1);
    h.Put(req, u8{0}); // the game consumes it (main+0x7d9260) ...
    Step(m, inst, h, api);
    assert(rd(req) == 1); // ... and the module keeps the press held
    for (int i = 0; i < 5; ++i)
        Step(m, inst, h, api); // hold ends: the unconsumed request is taken back
    assert(rd(req) == 0 && h.ints["mk.horn_busy"] == 0 && h.ints["mk.horn_count"] == 1);
    // Guard: an unexpected value is never touched.
    h.Put(req, u8{7});
    assert(ext->on_action(inst, "mk.horn_write", 0));
    Step(m, inst, h, api);
    assert(rd(req) == 7);
    // Not racing: refused.
    h.Put(req, u8{0});
    h.Put(Seq + 0x38, u32{7});
    Step(m, inst, h, api);
    assert(!ext->on_action(inst, "mk.horn_write", 0));
    m.destroy(inst);
}

/// The press ends without any write when the request byte stops being the local player's
/// (input object rebuilt / scene left) while the press is held: the old address may be reused.
void TestHornObjectGone() {
    Host h;
    BuildGame(h, 12);
    h.Put(Seq + 0x38, u32{6});
    const auto api = Api(h);
    const auto& m = Module();
    void* inst = m.create(&api, "{}");
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    const auto* wext =
        eden_dsmod_get_write_extensions(EDEN_DSMOD_WRITE_EXT_VERSION, EDEN_DSMOD_WRITE_EXT_HASH);
    EdenDsmodHostWriteApi w{EDEN_DSMOD_WRITE_EXT_VERSION, sizeof(w), EDEN_DSMOD_WRITE_EXT_HASH, &h,
                            &FakeBatch};
    wext->configure(inst, &w);
    Step(m, inst, h, api);
    const u64 old_req = 0x151000 + 0x38 + 0x35d, entry1 = 0x152000;
    const auto rd = [&](u64 at) {
        u8 v{};
        ReadMem(&h, at, &v, 1);
        return v;
    };
    assert(ext->on_action(inst, "mk.horn_write", 0));
    Step(m, inst, h, api);
    assert(rd(old_req) == 1);
    // The game rebuilds player 7's input object; the old byte now belongs to something else.
    h.Put(entry1 + 0x38 + 0x35d, u8{0});
    h.Put(0x150100 + 8 * 7, entry1);
    g_write_log.clear();
    for (int i = 0; i < 8; ++i)
        Step(m, inst, h, api);
    assert(g_write_log.empty() && rd(old_req) == 1 && rd(entry1 + 0x38 + 0x35d) == 0);
    assert(h.ints["mk.horn_busy"] == 0);
    // Scene left mid-press (race -> menu): no take-back write at the old address either.
    h.Put(old_req, u8{0});
    h.Put(0x150100 + 8 * 7, u64{0x151000});
    Step(m, inst, h, api);
    assert(ext->on_action(inst, "mk.horn_write", 0));
    Step(m, inst, h, api);
    assert(rd(old_req) == 1);
    h.Put(Scene + 0x184, u32{3});
    h.Put(Se + 0x7f0, u64{0}); // the menu scene has no race input objects
    g_write_log.clear();
    for (int i = 0; i < 8; ++i)
        Step(m, inst, h, api);
    assert(g_write_log.empty() && rd(old_req) == 1 && h.ints["mk.horn_busy"] == 0);
    assert(!ext->on_action(inst, "mk.horn_write", 0));
    m.destroy(inst);
    assert(g_write_log.empty()); // Shutdown does not touch it either
}

/// The UI resource is not there yet when the race is first read: the course keys follow as soon
/// as the name tables resolve (the course is loaded again, not remembered as "no folder").
void TestTablesLate() {
    Host h;
    BuildGame(h, 12);
    h.Put(Base + 0x1360000, u64{0}); // ui variable still null
    const auto api = Api(h);
    const auto& m = Module();
    void* inst = m.create(&api, "{}");
    Step(m, inst, h, api);
    assert(h.ints["mk.ready"] == 1 && h.texts["mk.course_folder"].empty());
    h.Put(Base + 0x1360000, UiVarObj);
    for (int i = 0; i < 70; ++i) // tables retry every 120 ticks
        Step(m, inst, h, api);
    assert(h.texts["mk.course_folder"] == "Gu_FirstCircuit");
    assert(h.texts["mk.map_key"] == "module:mk8d:map/Gu_FirstCircuit");
    assert(h.texts["r1.icon"] == "module:mk8d:chara/Luigi");
    m.destroy(inst);
}

/// Code patched after the first check (CTGP-DX hooks main from its plugin): the next scene change
/// re-checks the pins, and the table domain whose pin now fails stays closed -- it must not come
/// back from the addresses the earlier check decoded.
void TestTablePinRecheck() {
    Host h;
    BuildGame(h, 12);
    const auto api = Api(h);
    const auto& m = Module();
    void* inst = m.create(&api, "{}");
    Step(m, inst, h, api);
    assert(h.texts["r1.icon"] == "module:mk8d:chara/Luigi");
    h.Put(Base + PinDriverName.offset, u32{0}); // patched
    h.Put(Scene + 0x184, u32{3});
    Step(m, inst, h, api); // scene change seen
    Step(m, inst, h, api); // pins re-checked
    h.Put(Scene + 0x184, u32{4});
    for (int i = 0; i < 70; ++i)
        Step(m, inst, h, api);
    assert(h.texts["mk.diag"].find("DriverName") != std::string::npos);
    assert(h.texts["r1.icon"].empty() && h.texts["me.emblem_key"].empty());
    assert(h.texts["mk.course_folder"].empty() && h.texts["mk.cup_key"].empty());
    assert(h.ints["racers.count"] == 12 && h.ints["r5.coins"] == 5); // other domains unaffected
    m.destroy(inst);
}

void TestPinMismatch() {
    Host h;
    BuildGame(h, 12);
    // One changed instruction word in the race-info accessor: everything fails closed.
    h.Put(Base + 0x7f7aa0, u32{0xf9412500});
    const auto api = Api(h);
    const auto& m = Module();
    void* inst = m.create(&api, "{}");
    Step(m, inst, h, api);
    assert(h.ints["mk.ready"] == 0);
    assert(h.texts["mk.diag"].find("RaceInfoHolder") != std::string::npos);
    assert(h.ints["racers.count"] == 0);
    m.destroy(inst);
}

void TestWrongBuild() {
    Host h;
    auto api = Api(h);
    api.build_id[0] ^= 1;
    assert(Module().create(&api, "{}") == nullptr);
}

// ---- 3.0.3 (AArch32, 4-byte pointers): the same race through the 3.0.3 profile ---------------
// Offsets typed here independently of the reader's profile; GOT/rodata cells at the static 3.0.3
// addresses the literal decoders must land on.
namespace v303 {
#include "mk8d_pins303.inc"
constexpr std::array<const Pin*, 82> AllPins{&PinEngineHolder,     &PinRaceInfoHolder,
                                             &PinRaceInfo,         &PinActiveScene,
                                             &PinSceneKind,        &PinPhase,
                                             &PinEngineClass,      &PinMirror,
                                             &PinMapYScale,        &PinMapYScaleMul,
                                             &PinMapYScaleConst,   &PinMapProjection,
                                             &PinRaceCount,        &PinRaceRule,
                                             &PinLapsTotal,        &PinTimer,
                                             &PinTimerMin,         &PinTimerSec,
                                             &PinTimerMs,          &PinCheckers,
                                             &PinCheckerRank,      &PinCheckerCoins,
                                             &PinCheckerLap,       &PinCheckerGoalCheck,
                                             &PinCheckerGoalSet,   &PinCheckerGoalDone,
                                             &PinDriverEntryBase,  &PinDriverEntry,
                                             &PinDriverVariant,    &PinDriverEntryStride,
                                             &PinKartDirector,     &PinKartUnit,
                                             &PinKartUnitPlayer,   &PinKartUnitPos,
                                             &PinKartUnitMove,     &PinItemDirector,
                                             &PinItemOwnerCreate,  &PinItemOwnerCtor,
                                             &PinItemSlotItem,     &PinItemSlotState,
                                             &PinItemSlotEmpty,    &PinItemSlotCount,
                                             &PinItemSlotLayout,   &PinItemFrame,
                                             &PinItemTripleFrame,  &PinCoinKind,
                                             &PinAnimalCourse,     &PinSystemEngine,
                                             &PinPlayerManager,    &PinLocalPlayerObj,
                                             &PinRacerPlayerObj,   &PinUiRoot,
                                             &PinUiResource,       &PinUiTypeCheck,
                                             &PinDriverTableInit,  &PinDriverTableAlloc,
                                             &PinDriverName,       &PinDriverMsg,
                                             &PinDriverSuffix,     &PinCoursePict,
                                             &PinPictTableAlloc,   &PinCupOfCourse,
                                             &PinCupNameTableLit,  &PinCupTableInit,
                                             &PinCupMsg,           &PinDriverParamDb,
                                             &PinDriverParamDbLit, &PinDriverParamRecord,
                                             &PinEmblemFlag,       &PinEmblemCode,
                                             &PinEmblemName,       &PinEmblemSuffix,
                                             &PinPlayerInput,      &PinHornEvent,
                                             &PinHornFill,         &PinHornBit,
                                             &PinHornRequest,      &PinHornLatch,
                                             &PinHornPlay,         &PinCourseTokens,
                                             &PinCourseTokensLit,  &PinCourseTokenIndex};

constexpr u64 Base = 0x00200000, MainSize = 0xf5c000;
constexpr u8 Build[16] = {0x6A, 0x85, 0x26, 0x2F, 0x21, 0xB9, 0x03, 0x64,
                          0x9B, 0xD7, 0xC6, 0x26, 0x28, 0xD2, 0x6E, 0x43};
constexpr u64 Inputs = 0x150000, InArr = 0x150100, Entry0 = 0x151000;

EdenDsmodHostApi Api(Host& h) {
    EdenDsmodHostApi api = ::Api(h);
    std::memcpy(api.build_id, Build, sizeof(Build));
    api.main_base = Base;
    api.main_size = MainSize;
    return api;
}
void P32(Host& h, u64 at, u64 v) {
    h.Put(at, static_cast<u32>(v));
}

void BuildGame303(Host& h, int n) {
    for (const Pin* p : AllPins)
        h.Bytes(Base + p->offset, p->words.data(), p->words.size() * 4);
    P32(h, Base + 0xf12c68, Base + 0xf50b1c); // root GOT cell -> &root var
    P32(h, Base + 0xf50b1c, Root);
    P32(h, Base + 0xf18a08, Base + 0xf53290); // ui GOT cell -> &ui var
    P32(h, Base + 0xf53290, UiVarObj);
    P32(h, UiVarObj + 4, UiD);
    P32(h, UiD + 0x18, R3);
    std::string tokens = "Invalid ,Gu_Menu ,Test ";
    for (int i = 1; i <= 14; ++i)
        tokens += ",Reserved" + std::string(i < 10 ? "0" : "") + std::to_string(i) + " ";
    tokens += ",Gu_MarioCircuit ,Gu_DossunIseki ,Gu_City ,Gu_Cake ,Gu_HorrorHouse ,Gu_Expert "
              ",Gu_Desert ,Gu_Cloud ,Gu_SnowMountain ,Gu_Techno ,Gu_Airport ,Gu_FirstCircuit";
    h.Str(Base + 0xd96188, tokens);
    const std::array<u8, 23> frames{3,  5,  7,  0,  9,  11, 16, 2, 13, 15, 14, 12,
                                    10, 22, 20, 21, 24, 4,  6,  8, 23, 27, 28};
    h.Bytes(Base + 0xda01b4, frames.data(), frames.size());
    std::array<s32, 24> cupmsg{};
    for (int c = 0; c < 24; ++c)
        cupmsg[static_cast<size_t>(c)] = c < 12 ? 1301 + c : 11601 + c - 12;
    h.Put(Base + 0xda012c, cupmsg);
    const char* cups[] = {"Mushroom", "Flower", "Star", "Special"};
    for (int c = 0; c < 24; ++c) {
        const u64 s = Base + 0xe00000 + 0x40 * static_cast<u64>(c);
        h.Str(s, c < 4 ? cups[c] : "Charger" + std::to_string(c));
        P32(h, Base + 0xec2628 + 4 * static_cast<u64>(c), s);
    }
    // R3: driver table +0x170 (names +0x40/+0x44, msgs +0x48/+0x4c), picture table +0x1d0,
    // cup table [+0x1c].
    const std::vector<std::string> drivers{"Invalid", "Mario", "Luigi", "Peach", "Daisy", "Yoshi"};
    h.Put(R3 + 0x170 + 0x40, static_cast<u32>(drivers.size()));
    P32(h, R3 + 0x170 + 0x44, NamesArr);
    for (size_t i = 0; i < drivers.size(); ++i) {
        h.Str(Strs + 0x40 * i, drivers[i]);
        P32(h, NamesArr + 4 * i, Strs + 0x40 * i);
    }
    const std::vector<s32> msgs{-1, 1001, 1002, 1003, 1013, 1004};
    h.Put(R3 + 0x170 + 0x48, static_cast<u32>(msgs.size()));
    P32(h, R3 + 0x170 + 0x4c, MsgArr);
    h.Bytes(MsgArr, msgs.data(), msgs.size() * 4);
    std::vector<std::string> picts(0x1d, "Random_00");
    picts[0x1b + 1] = "Gu_FirstCircuit_00";
    h.Put(R3 + 0x1d0 + 0x40, static_cast<u32>(picts.size()));
    P32(h, R3 + 0x1d0 + 0x44, PictArr);
    for (size_t i = 0; i < picts.size(); ++i) {
        h.Str(Strs + 0x1000 + 0x40 * i, picts[i]);
        P32(h, PictArr + 4 * i, Strs + 0x1000 + 0x40 * i);
    }
    P32(h, R3 + 0x1c, CupT);
    const std::array<s32, 2> cup_slot{0, 0};
    h.Put(CupT + 0xc80 + 8 * 0x1b, cup_slot);
    const std::array<s32, 3> entry{0x1b, -1, 1401};
    h.Put(CupT, entry);
    // Driver param DB: GOT 0xf1217c -> var -> obj, DB = [obj+0x78]; records [DB+0x1c], present
    // [DB+0x14], count u16 DB+0x56; code = the char* u32 rec+0x10.
    constexpr u64 DbObj = 0x130000, Db = 0x131000, Recs = 0x132000, Codes = 0x140000;
    P32(h, Base + 0xf1217c, Base + 0xf50dec);
    P32(h, Base + 0xf50dec, DbObj);
    P32(h, DbObj + 0x78, Db);
    P32(h, Db + 0x14, 1);
    P32(h, Db + 0x1c, Recs);
    h.Put(Db + 0x56, u16{0x35});
    const char* codes[] = {"Mro", "Lig", "Pch", "Dsy", "Ysi"};
    for (int d = 0; d < 5; ++d) {
        h.Str(Codes + 0x10 * static_cast<u64>(d), codes[d]);
        P32(h, Recs + 0x244 * static_cast<u64>(d) + 0x10, Codes + 0x10 * static_cast<u64>(d));
        h.Put(Recs + 0x244 * static_cast<u64>(d) + 0x60, u32(d == 4 ? 2 : 0));
    }
    // Player input objects [SE+0x520]: count +0x2c, array +0x34; obj = entry + 0x1c.
    P32(h, Se + 0x520, Inputs);
    h.Put(Inputs + 0x2c, u32{8});
    P32(h, Inputs + 0x34, InArr);
    P32(h, InArr + 4 * 7, Entry0);
    h.Put(Entry0 + 0x1c + 0x321, u8{0});

    // Framework.
    P32(h, Root + 0x10, SceneHolder);
    P32(h, SceneHolder + 4, Scene);
    h.Put(Scene + 0xd4, u32{4});
    h.Put(Scene + 0xd8, u32{4}); // Race
    P32(h, Scene + 0xf0, Oe);
    P32(h, Oe + 0x11c, Rd);
    P32(h, Oe + 0x12c, Kd);
    P32(h, Oe + 0x130, Id);
    P32(h, Root + 0x14, Engine);
    P32(h, Engine + 0x140, RiHolder);
    P32(h, Engine + 0xe0, Se);
    P32(h, Se + 0x528, Pm);
    P32(h, RiHolder + 0x08, Ri);
    h.Map(Ri, 0x1a4);
    h.Put(Ri + 0x08, u32{0});
    h.Put(Ri + 0x10, u32{2}); // 150cc
    h.Put(Ri + 0x26, u8{0});
    h.Put(Ri + 0x180, s32{n});
    h.Put(Ri + 0x1a0, u32{0x1b});
    P32(h, Rd + 0x2c, Seq);
    h.Put(Seq + 0x1c, u32{5});
    P32(h, Rd + 0x30, Lm);
    h.Put(Lm + 0x34, u8{3});
    h.Put(Rd + 0x34, u32(n));
    P32(h, Rd + 0x3c, CkArr);
    P32(h, Rd + 0xac, Tm);
    const std::array<u8, 8> time{0, 0, 0, 0, 1, 23, 0x2d, 0x01}; // 1:23.301
    h.Put(Tm + 0x20, time);
    h.Put(Kd + 0x64, s32{n});
    h.Put(Pm + 0xd0, s32{7}); // local controller 0 -> player object 7
    for (int i = 0; i < n; ++i) {
        const u64 ck = Ck0 + 0x100 * static_cast<u64>(i);
        P32(h, CkArr + 4 * static_cast<u64>(i), ck);
        h.Put(ck + 0x24, static_cast<u16>((i + 3) % n)); // rank index
        h.Put(ck + 0x28, u32(i == 2 ? 1 : 0));
        h.Put(ck + 0x34, static_cast<u8>(i));
        h.Put(ck + 0x20, u32(i == 9 ? 5 : 0)); // racer 9 finished
        h.Put(Ri + 0x30 + 0x1c * static_cast<u64>(i) + 0x0c, u32(i % 5));
        h.Put(Ri + 0x30 + 0x1c * static_cast<u64>(i) + 0x10, u8(i == 4 ? 3 : 0));
        const u64 ku = Ku0 + 0x100 * static_cast<u64>(i), veh = Veh0 + 0x100 * static_cast<u64>(i),
                  mv = Move0 + 0x100 * static_cast<u64>(i);
        P32(h, Kd + 0x2c + 4 * static_cast<u64>(i), ku);
        P32(h, ku + 0x24, veh);
        P32(h, ku + 0x28, mv);
        h.Put(veh + 0x54, u32(i));
        const std::array<float, 3> pos{10.0f * static_cast<float>(i), 0.0f, 5.0f};
        h.Put(mv + 0x28, pos);
        h.Put(Pm + 0x120 + 4 * static_cast<u64>(i), s32(i == 2 ? 7 : 100 + i)); // racer 2 = me
    }
    h.Put(Id + 0x74, u32(n));
    P32(h, Id + 0x7c, Owners);
    for (int i = 0; i < n; ++i) {
        const u64 io = i == 2 ? Io : 0x120000 + 0x400 * static_cast<u64>(i);
        const u64 s0 = i == 2 ? S0 : io + 0x100, s1 = i == 2 ? S1 : io + 0x200;
        P32(h, Owners + 4 * static_cast<u64>(i), io);
        h.Put(io + 0x24, u32(i));
        P32(h, io + 0x34, s0);
        P32(h, io + 0x38, s1);
        h.Map(s0, 0x100);
        h.Map(s1, 0x100);
    }
    h.Put(0x120000 + 0x400 * 7 + 0x100 + 0x21, u8{3});
    h.Put(0x120000 + 0x400 * 7 + 0x100 + 0x5c, u32{1});
    h.Put(S0 + 0x21, u8{3});
    h.Put(S0 + 0x5c, u32{3});
    h.Put(S0 + 0x78, u32{7});
    h.Put(S0 + 0x7c, u32{7}); // Mush3
    h.Put(S1 + 0x21, u8{1});  // roulette
    h.Put(S1 + 0x7c, u32{4});
}

void TestRace() {
    Host h;
    BuildGame303(h, 12);
    const auto api = v303::Api(h);
    const auto& m = Module();
    assert(m.supports_build("6A85262F21B903649BD7C62628D26E4300000000000000000000000000000000"));
    assert(!m.supports_build("6A85262F21B903649BD7C62628D26E4300000000000000000000000000000001"));
    void* inst = m.create(&api, "{}");
    assert(inst);
    Step(m, inst, h, api);
    Step(m, inst, h, api);
    if (h.ints["mk.ready"] != 1 || h.ints["racers.count"] != 12)
        std::fprintf(stderr, "303 diag: %s\n", h.texts["mk.diag"].c_str());
    assert(h.texts["mk.diag"].find("mismatch") == std::string::npos);
    assert(h.ints["mk.ready"] == 1 && h.ints["mk.scene"] == 4 && h.ints["mk.phase"] == 5);
    assert(h.ints["mk.course"] == 0x1b && h.ints["mk.cc"] == 150 && h.ints["mk.mode"] == 0);
    assert(h.ints["mk.laps_total"] == 3 && h.ints["mk.time_ms"] == 83301);
    assert(h.ints["mk.cup"] == 0);
    assert(h.texts["mk.course_folder"] == "Gu_FirstCircuit");
    assert(h.texts["mk.pict_key"] == "module:mk8d:coursepict/Gu_FirstCircuit");
    assert(h.texts["mk.cup_key"] == "module:mk8d:cup/Mushroom");
    assert(h.ints["racers.count"] == 12);
    assert(h.ints["r0.valid"] == 1 && h.ints["r0.rank"] == 4 && h.ints["r2.lap"] == 2);
    assert(h.ints["r5.coins"] == 5);
    assert(h.ints["r9.finished"] == 1 && h.ints["r5.finished"] == 0);
    assert(h.texts["r1.icon"] == "module:mk8d:chara/Luigi");
    assert(h.texts["r4.icon"] == "module:mk8d:chara/Yoshi03");
    assert(h.texts["r4.emblem_key"] == "module:mk8d:emblem/Ysi03");
    assert(h.texts["me.emblem_key"] == "module:mk8d:emblem/Pch");
    assert(h.ints["me.slot"] == 2 && h.ints["r2.is_me"] == 1);
    assert(h.ints["me.rank"] == 6 && h.ints["me.lap"] == 2 && h.ints["me.coins"] == 2);
    assert(h.ints["me.item0"] == 7 && h.ints["me.item0_count"] == 3);
    assert(h.ints["me.item1"] == 4 && h.ints["me.item1_roulette"] == 1);
    assert(h.ints["r7.item0"] == 0 && h.ints["r7.item_state"] == 2 && h.ints["r7.item1"] == -1);
    // Phase 0: no racers.
    h.Put(Seq + 0x1c, u32{0});
    Step(m, inst, h, api);
    assert(h.ints["racers.count"] == 0 && h.ints["mk.time_ms"] == -1);
    m.destroy(inst);
}

void TestHorn() {
    Host h;
    BuildGame303(h, 12);
    h.Put(Seq + 0x1c, u32{6}); // racing
    const auto api = v303::Api(h);
    const auto& m = Module();
    void* inst = m.create(&api, "{}");
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    const auto* wext =
        eden_dsmod_get_write_extensions(EDEN_DSMOD_WRITE_EXT_VERSION, EDEN_DSMOD_WRITE_EXT_HASH);
    EdenDsmodHostWriteApi w{EDEN_DSMOD_WRITE_EXT_VERSION, sizeof(w), EDEN_DSMOD_WRITE_EXT_HASH, &h,
                            &FakeBatch};
    wext->configure(inst, &w);
    Step(m, inst, h, api);
    assert(h.ints["mk.horn_ok"] == 1);
    const u64 req = Entry0 + 0x1c + 0x321;
    const auto rd = [&](u64 at) {
        u8 v{};
        ReadMem(&h, at, &v, 1);
        return v;
    };
    assert(ext->on_action(inst, "mk.horn_write", 0));
    Step(m, inst, h, api);
    assert(rd(req) == 1);
    h.Put(req, u8{0});
    Step(m, inst, h, api);
    assert(rd(req) == 1);
    for (int i = 0; i < 5; ++i)
        Step(m, inst, h, api);
    assert(rd(req) == 0 && h.ints["mk.horn_count"] == 1);
    m.destroy(inst);
}

/// CTGP-DX rewrites 8 bytes at known sites with `ldr pc,[pc,#-4]` + target: pins overlapping a
/// window still pass; the same jump anywhere else, or a jump into main, fails the pin.
void TestCtgpWindow() {
    Host h;
    BuildGame303(h, 12);
    const auto api = v303::Api(h);
    const auto& m = Module();
    h.Put(Base + 0x55d32c, std::array<u32, 2>{0xe51ff004, 0x7100a000}); // CheckerCoins window
    void* inst = m.create(&api, "{}");
    Step(m, inst, h, api);
    assert(h.texts["mk.diag"].find("CheckerCoins") == std::string::npos);
    assert(h.ints["racers.count"] == 12 && h.ints["r5.coins"] == 5);
    m.destroy(inst);
    h.Put(Base + 0x55d32c, std::array<u32, 2>{0xe51ff004, Base + 0x1000}); // target in main
    inst = m.create(&api, "{}");
    Step(m, inst, h, api);
    assert(h.texts["mk.diag"].find("CheckerCoins") != std::string::npos);
    assert(h.ints["racers.count"] == 0);
    m.destroy(inst);
    h.Put(Base + 0x55d32c, std::array<u32, 2>{0xe51ff004, 0x7100a000});
    h.Put(Base + 0x55d338, u32{0xe51ff004}); // outside the window
    inst = m.create(&api, "{}");
    Step(m, inst, h, api);
    assert(h.texts["mk.diag"].find("CheckerCoins") != std::string::npos);
    m.destroy(inst);
    // Inline hook form: one `b` to a trampoline outside main (HornBit 0x18cd6c).
    h.Put(Base + 0x55d338, u32{0xe5d15034});
    h.Put(Base + 0x18cd6c, u32{0xea800000});
    inst = m.create(&api, "{}");
    Step(m, inst, h, api);
    assert(h.texts["mk.diag"].find("HornBit") == std::string::npos);
    m.destroy(inst);
    h.Put(Base + 0x18cd6c, u32{0xea000010}); // a branch inside main: not the plugin's hook
    inst = m.create(&api, "{}");
    Step(m, inst, h, api);
    assert(h.texts["mk.diag"].find("HornBit") != std::string::npos);
    m.destroy(inst);
}
} // namespace v303

} // namespace

int main() {
    TestRace();
    TestPinMismatch();
    TestWrongBuild();
    TestHorn();
    TestHornObjectGone();
    TestTablesLate();
    TestTablePinRecheck();
    v303::TestRace();
    v303::TestHorn();
    v303::TestCtgpWindow();
    std::puts("dsmod-mk8d-reader: passed");
    return 0;
}
