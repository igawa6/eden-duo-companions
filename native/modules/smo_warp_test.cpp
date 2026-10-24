// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_warp.h"
#include <cassert>
#include <cstdio>
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
constexpr U64 Base = 0x800000000, Scene = 0x200000000, Holder = Scene + 0x1000;
constexpr U64 Game = Scene + 0x2000, Menu = Scene + 0x5000, State = Scene + 0x6000;
constexpr U64 Map = Scene + 0x7000, Spine = Scene + 0x8000, Worlds = Scene + 0x9000;
constexpr U64 Checkpoints = Scene + 0xA000, Icons = Scene + 0xD000, Records = Scene + 0xE000;
constexpr U64 StageText = Scene + 0xF000, IdText = Scene + 0xF100;
constexpr U64 SceneSpine = Scene + 0x11000;
void Text(Memory& m, U64 at, const char* text) {
    do { m.Put(at++, std::uint8_t(*text)); } while (*text++);
}
Memory Ready() {
    Memory m;
    m.Put(Scene, Base + 0x12C4DC8); m.Put(Scene + 0x2E0, Holder);
    m.Put(Holder + 0x20, Game); m.Put(Holder + 0x238, std::uint8_t(1));
    m.Put(Game + 0xA38, std::int32_t(2)); m.Put(Game + 0x330, StageText);
    Text(m, StageText, "SandWorldHomeStage"); Text(m, IdText, "obj222");
    m.Put(Game + 0xA8C, std::int32_t(0)); m.Put(Game + 0x624, std::int32_t(0));
    m.Put(Game + 0x628, std::int32_t(0)); m.Put(Game + 0xA70, std::int32_t(-1));
    m.Put(Game + 0xA6C, std::int32_t(5)); m.Put(Game + 0xA30, Worlds);
    m.Put(Worlds + 16, Checkpoints);
    for (unsigned i = 0; i < 17; ++i) m.Put(Checkpoints + i * 0x148 + 0x130, std::uint32_t(0));
    // Physical record 3 becomes filtered index 0; native filtering order matters.
    m.Put(Checkpoints + 3 * 0x148 + 0x130, std::uint32_t(1 << 5));
    m.Put(Checkpoints + 3 * 0x148 + 8, StageText);
    m.Put(Checkpoints + 3 * 0x148 + 0xA0, IdText);
    m.Put(Checkpoints + 3 * 0x148 + 0x140, std::uint8_t(1));
    m.Put(Scene + 0x170, Menu); m.Put(Menu + 0x38, State);
    m.Put(State, Base + 0x12CB708); m.Put(State + 0x18, Scene);
    m.Put(State + 0x10, std::uint8_t(0)); m.Put(State + 0x20, Map);
    m.Put(State + 8, Spine); m.Put(State + 0x28, U64(0)); m.Put(Spine, State);
    m.Put(Spine + 8, Base + 0x12CBB08); m.Put(Spine + 0x10, U64(0));
    m.Put(Spine + 0x18, std::int32_t(100));
    m.Put(Map + 0x178, Icons); m.Put(Map + 0x180, std::int32_t(16));
    m.Put(Map + 0x210, Records); m.Put(Map + 0x218, std::int32_t(1));
    m.Put(Icons, std::int32_t(0)); m.Put(Icons + 4, std::int32_t(0));
    m.Put(Records, Icons); m.Put(Records + 8, std::uint8_t(1));
    m.Put(Records + 0xC, std::int32_t(0));
    return m;
}
auto Plan(Memory& m, bool touched = true) {
    auto h = m.Host(); h.main_base = Base;
    return SmoWarp::Prepare(h, Scene, Game, 2, "SandWorldHomeStage", "obj222", touched);
}
} // namespace
int main() {
    auto m = Ready(); auto p = Plan(m);
    auto diagnostic_host = m.Host(); diagnostic_host.main_base = Base;
    unsigned reason = 99;
    assert(SmoWarp::Prepare(diagnostic_host, Scene, Game, 2, "SandWorldHomeStage", "obj222", true, &reason));
    assert(reason == 0);
    assert(!SmoWarp::Prepare(diagnostic_host, Scene, Game, 2, "SandWorldHomeStage", "obj222", false, &reason));
    assert(reason == 1);
    assert(p && p->checkpoint_index == 0 && p->record == Records);
    assert(p->writes[0].address == State + 0x28 && p->writes[0].before == 0 && p->writes[0].after == Records);
    assert(p->writes[1].address == Spine + 0x10 && p->writes[1].after == Base + 0x12CBB38);
    assert(p->writes[2].address == Spine + 0x18 && p->writes[2].after == UINT32_MAX && p->writes[2].size == 4);
    assert(!Plan(m, false));
    m.Put(Checkpoints + 3 * 0x148 + 0x140, std::uint8_t(0)); assert(!Plan(m));
    assert(!SmoWarp::Prepare(diagnostic_host, Scene, Game, 2, "SandWorldHomeStage", "obj222", true, &reason));
    assert(reason == 7);
    m = Ready();
    m.Put(Spine + 8, Base + 0x12CBB30); assert(Plan(m)); // Native WaitAdsorb.
    m.Put(Spine + 0x10, Base + 0x12CBB38); assert(!Plan(m));
    m = Ready(); m.Put(State + 0x10, std::uint8_t(1)); assert(!Plan(m));
    m = Ready(); m.Put(Records + 8, std::uint8_t(0)); assert(!Plan(m));
    m = Ready(); m.Put(Records + 0xC, std::int32_t(2)); assert(!Plan(m)); // Odyssey is excluded.
    m = Ready(); m.Put(Holder + 0x238, std::uint8_t(0)); assert(!Plan(m));
    m = Ready(); m.Put(Game + 0x624, std::int32_t(1)); assert(!Plan(m));
    m = Ready(); m.Put(Game + 0xA8C, std::int32_t(1)); assert(!Plan(m));
    m = Ready(); m.Put(Game + 0xA38, std::int32_t(3)); assert(!Plan(m));
    m = Ready(); Text(m, IdText, "obj999"); assert(!Plan(m));
    m = Ready(); m.Put(Game + 0xA70, std::int32_t(4)); assert(!Plan(m));
    m = Ready(); m.Put(State + 0x18, Scene + 8); assert(!Plan(m));
    m = Ready(); m.tear = State + 0x28; assert(!Plan(m));
    m = Ready(); m.data.erase(Checkpoints + 16 * 0x148 + 0x130); assert(!Plan(m));
    // Closed-map checkpoint records are already populated before the first map
    // opening. Direct travel changes the scene nerve, never the map's nerve.
    const auto direct_ready=[] {
        auto v=Ready();
        v.Put(State+0x10,std::uint8_t(1));v.Put(Scene+8,SceneSpine);
        v.Put(SceneSpine,Scene);v.Put(SceneSpine+8,Base+0x12C5EB8);
        v.Put(SceneSpine+0x10,U64(0));v.Put(SceneSpine+0x18,std::int32_t(100));
        return v;
    };
    const auto direct=[](Memory& v) {
        auto h=v.Host();h.main_base=Base;
        return SmoWarp::Prepare(h,Scene,Game,2,"SandWorldHomeStage","obj222",true,nullptr,true);
    };
    m=direct_ready();p=direct(m);assert(p);
    assert(p->writes[0].address==State+0x28 && p->writes[0].after==Records);
    assert(p->writes[1].address==SceneSpine+0x10 && p->writes[1].after==Base+0x12C5F48);
    assert(p->writes[2].address==SceneSpine+0x18 && p->writes[2].after==UINT32_MAX);
    assert(!Plan(m)); // Closed map is not an active native-map request.
    m.Put(Records+8,std::uint8_t(0));m.Put(Records+0xC,std::int32_t(19));
    assert(direct(m)); // Preallocated record before any native map opening.
    m.Put(Records+8,std::uint8_t(1));assert(!direct(m)); // Reject a partial UI update.
    m=direct_ready();
    m.Put(SceneSpine+8,Base+0x12C5E48);assert(!direct(m)); // Pause.
    m=direct_ready();m.Put(State+0x10,std::uint8_t(0));assert(!direct(m));
    m=direct_ready();m.Put(SceneSpine+0x10,Base+0x12C5F48);assert(!direct(m));
    m=direct_ready();m.Put(SceneSpine,State);assert(!direct(m));
    m=direct_ready();m.tear=Scene+8;assert(!direct(m));
    m=direct_ready();m.Put(Checkpoints+3*0x148+0x140,std::uint8_t(0));assert(!direct(m));
    // NCE can relocate singleton objects away from host.main_base. Match the
    // validated live vtable base for both idle detection and the requested nerve.
    m=direct_ready();constexpr U64 delta=0x400000;
    m.Put(Scene,Base+delta+0x12C4DC8);m.Put(State,Base+delta+0x12CB708);
    m.Put(SceneSpine+8,Base+delta+0x12C5EB8);
    auto relocated=m.Host();relocated.main_base=Base;
    relocated.get_i64=[](void*,const char* key,std::int64_t fallback)->std::int64_t {
        return std::string_view{key}=="__relocation_delta"?delta:fallback;
    };
    p=SmoWarp::Prepare(relocated,Scene,Game,2,"SandWorldHomeStage","obj222",true,nullptr,true);
    assert(p && p->writes[1].after==Base+delta+0x12C5F48);
    std::puts("smo_warp: exact native checkpoint IDs, idle state and expected write plan passed");
}
