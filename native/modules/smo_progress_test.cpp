// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_progress.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace {
using U64 = std::uint64_t;
struct Memory {
    std::unordered_map<U64, std::uint8_t> data;
    U64 tear = 0;
    unsigned tears = 0;
    template <class T> void Put(U64 at, T value) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(&value);
        for (unsigned i = 0; i < sizeof(T); ++i) data[at + i] = b[i];
    }
    static EdenDsmodBool Mapped(void* p, U64 at, U64 size) {
        auto& m = *static_cast<Memory*>(p);
        for (U64 i = 0; i < size; ++i) if (!m.data.contains(at + i)) return false;
        return true;
    }
    static EdenDsmodBool Read(void* p, U64 at, void* out, size_t size) {
        auto& m = *static_cast<Memory*>(p);
        if (!Mapped(p, at, size)) return false;
        for (size_t i = 0; i < size; ++i) static_cast<std::uint8_t*>(out)[i] = m.data.at(at + i);
        if (at == m.tear && ++m.tears == 1) m.data[at] ^= 1;
        return true;
    }
    EdenDsmodHostApi Host() {
        EdenDsmodHostApi h{};
        h.userdata = this; h.is_mapped = Mapped; h.read_memory = Read;
        return h;
    }
};
constexpr U64 Game = 0x200000000, Bank = Game + 0x1000;
constexpr U64 Scenario = Game + 0x2000, Main = Game + 0x3000;
constexpr U64 Acquired = Game + 0x4000;
constexpr U64 Progress = Game + 0x5000, Unlocked = Game + 0x6000, First = Game + 0x7000;
constexpr U64 Purple = Game + 0x8000, Spent = Game + 0x9000;
constexpr U64 Holder = Game + 0xA000, Files = Game + 0xB000;
Memory Sand() {
    Memory m;
    m.Put(Game + SmoProgress::BankedArray, Bank);
    m.Put(Game + SmoProgress::ScenarioArray, Scenario);
    m.Put(Game + SmoProgress::MainShineArray, Main);
    m.Put(Game + SmoProgress::AcquiredArray, Acquired);
    m.Put(Game + SmoProgress::CurrentWorld, std::int32_t(2));
    m.Put(Game + 0xA48, Purple); m.Put(Game + 0x5E8, Spent);
    m.Put(Purple + 8, std::int32_t(23)); m.Put(Spent + 8, std::int32_t(7));
    m.Put(Holder + 0x20, Game); m.Put(Holder + 0x18, Files);
    m.Put(Holder + 0x30, std::int32_t(0));
    for (unsigned i = 0; i < 5; ++i) m.Put(Files + i * 8, Game + i * 0x10000);
    m.Put(Game + 0x6A0, Progress);
    m.Put(Progress + 0x20, Unlocked); m.Put(Progress + 0x48, First);
    m.Put(Unlocked + 14, std::uint8_t(0)); m.Put(First + 14, std::uint8_t(1));
    for (unsigned i = 0; i < SmoProgress::BankedSlots; ++i) m.Put(Bank + i * 4, std::int32_t(0));
    for (unsigned i = 0; i < SmoProgress::BankedSlots; ++i) m.Put(Acquired + i * 4, std::int32_t(0));
    m.Put(Bank + 4, std::int32_t(5)); // Native early-save banked total 5, despite 21 acquired units.
    m.Put(Acquired + 4, std::int32_t(5));
    m.Put(Acquired + 8, std::int32_t(16));
    m.Put(Scenario + 8, std::int32_t(5));
    m.Put(Main + 8, std::int32_t(4));
    return m;
}
} // namespace
int main() {
    auto m = Sand();
    auto h = m.Host();
    auto s = SmoProgress::Read(h, Game, 2);
    assert(s.global_valid && s.banked_total == 5);
    assert(s.scenario_valid && s.scenario == 5);
    assert(s.main_shine_valid && s.main_shine == 4);
    assert(s.regional_valid && s.regional_wallet == 16 && s.regional_collected == 23);
    assert(SmoProgress::ActiveSaveFile(h, Holder, Game) == 1);
    assert(s.unbanked_valid && s.world_unbanked == 16 && s.all_worlds_unbanked == 16 && s.acquired_units == 21);
    assert(s.moon_row_valid && s.moon_row == 16 && !s.moon_row_global);
    // Native travel requirement: five needed, one pending -> four dotted slots.
    constexpr U64 Info=Game+0xC000, Groups=Game+0xD000, Group=Game+0xE000, Needs=Game+0xF000;
    m.Put(Game+0x898,Info);m.Put(Info+0xF0,std::int32_t(1));m.Put(Info+0xF8,Groups);
    m.Put(Groups,Group);m.Put(Group,Needs);m.Put(Group+8,std::int32_t(17));m.Put(Group+0xC,std::uint8_t(0));
    for(unsigned i=0;i<17;++i){m.Put(Unlocked+i,std::uint8_t(0));m.Put(First+i,std::uint8_t(1));m.Put(Needs+i*4,std::int32_t(5));}
    m.Put(Acquired+8,std::int32_t(1));
    s=SmoProgress::Read(h,Game,2);
    assert(s.moon_requirement_valid && s.moon_missing==4 && s.moon_row==1);
    m.Put(Acquired+8,std::int32_t(2));
    assert(SmoProgress::Read(h,Game,2).moon_missing==3);
    m.tear=Needs+8;m.tears=0;
    assert(!SmoProgress::Read(h,Game,2).moon_requirement_valid);
    m.tear=0;
    // Live flags update before the next disk save. A missing/torn record is unknown.
    constexpr U64 Worlds=Game+0x10000, Entries=Game+0x11000, Stage=Game+0x13000, Id=Game+0x14000;
    m.Put(Game+0xA30,Worlds);m.Put(Worlds+16,Entries);
    for(unsigned i=0;i<17;++i){m.Put(Entries+i*0x148+8,U64(0));m.Put(Entries+i*0x148+0xA0,U64(0));}
    m.Put(Entries+8,Stage);m.Put(Entries+0xA0,Id);
    const char stage[]="SandWorldHomeStage",id[]="obj1";
    for(unsigned i=0;i<sizeof(stage);++i)m.Put(Stage+i,std::uint8_t(stage[i]));
    for(unsigned i=0;i<sizeof(id);++i)m.Put(Id+i,std::uint8_t(id[i]));
    m.Put(Entries+0x140,std::uint8_t(0));
    assert(SmoProgress::CheckpointTouched(h,Game,2,stage,id)==false);
    m.Put(Entries+0x140,std::uint8_t(1));
    assert(SmoProgress::CheckpointTouched(h,Game,2,stage,id)==true);
    m.tear=Entries+0x140;m.tears=0;
    assert(!SmoProgress::CheckpointTouched(h,Game,2,stage,id).has_value());
    m.tear=0;
    assert(!SmoProgress::CheckpointTouched(h,Game,1,stage,id).has_value());
    assert(!SmoProgress::CheckpointTouched(h,Game,2,stage,"missing").has_value());
    m=Sand();h=m.Host();
    m.Put(Unlocked + 14, std::uint8_t(1)); m.Put(First + 14, std::uint8_t(0));
    m.Put(Acquired + 16 * 4, std::int32_t(3));
    m.Put(Acquired + 19 * 4, std::int32_t(7));
    s = SmoProgress::Read(h, Game, 2);
    assert(s.moon_row_valid && s.moon_row == 19 && s.moon_row_global && s.acquired_units == 31);
    // The twentieth native slot contributes; do not silently truncate to 17 worlds.
    m.Put(Bank + 19 * 4, std::int32_t(1007));
    s = SmoProgress::Read(h, Game, 2);
    assert(s.global_valid && s.banked_total == 999);
    m.Put(Bank + 19 * 4, std::int32_t(-1));
    s = SmoProgress::Read(h, Game, 2);
    assert(!s.global_valid && s.scenario_valid);
    m = Sand(); h = m.Host(); m.tear = Bank;
    s = SmoProgress::Read(h, Game, 2);
    assert(!s.global_valid && s.main_shine_valid);
    m = Sand(); h = m.Host(); m.tear = Game + SmoProgress::BankedArray;
    assert(!SmoProgress::Read(h, Game, 2).global_valid);
    m = Sand(); h = m.Host(); m.tear = Scenario + 8;
    s = SmoProgress::Read(h, Game, 2);
    assert(s.global_valid && !s.scenario_valid && s.main_shine_valid);
    m = Sand(); h = m.Host(); m.tear = Acquired;
    s = SmoProgress::Read(h, Game, 2);
    assert(s.global_valid && !s.unbanked_valid);
    m = Sand(); h = m.Host(); m.Put(Game + SmoProgress::CurrentWorld, std::int32_t(3));
    s = SmoProgress::Read(h, Game, 2);
    assert(s.global_valid && !s.unbanked_valid);
    m = Sand(); h = m.Host(); m.Put(Scenario + 8, std::int32_t(0));
    assert(!SmoProgress::Read(h, Game, 2).scenario_valid);
    m.Put(Main + 8, std::int32_t(-1));
    assert(SmoProgress::Read(h, Game, 2).main_shine_valid);
    m.data.erase(Bank + 79);
    assert(!SmoProgress::Read(h, Game, 2).global_valid);
    assert(!SmoProgress::Read(h, Game, -1).global_valid);
    assert(!SmoProgress::Read(h, Game, 17).global_valid);
    assert(!SmoProgress::Read(h, UINT64_MAX, 2).global_valid);
    assert(!SmoProgress::Read({}, Game, 2).global_valid);
    m = Sand(); h = m.Host(); m.Put(Spent + 8, std::int32_t(24));
    assert(!SmoProgress::Read(h, Game, 2).regional_valid);
    m = Sand(); h = m.Host(); m.tear = Purple + 8;
    assert(!SmoProgress::Read(h, Game, 2).regional_valid);
    m = Sand(); h = m.Host();
    m.Put(Holder + 0x30, std::int32_t(4));
    m.Put(Files, Game + 0x10000); m.Put(Files + 4 * 8, Game);
    assert(SmoProgress::ActiveSaveFile(h, Holder, Game) == 5);
    m.Put(Files + 4 * 8, Game + 0x20000);
    assert(SmoProgress::ActiveSaveFile(h, Holder, Game) == 0);
    m = Sand(); h = m.Host(); m.tear = Holder + 0x30;
    assert(SmoProgress::ActiveSaveFile(h, Holder, Game) == 0);
    m = Sand(); h = m.Host(); m.Put(Holder + 0x30, std::int32_t(5));
    assert(SmoProgress::ActiveSaveFile(h, Holder, Game) == 0);
    std::puts("smo_progress: native banked/scenario bounds and torn reads passed");
}
