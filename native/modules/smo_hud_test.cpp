// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "smo_hud.h"
#include <cassert>
#include <cstring>
#include <unordered_map>

namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
struct Memory {
    std::unordered_map<u64,u8> bytes;
    bool aux = true, reject = false;
    unsigned writes = 0;
    template<class T> void Put(u64 at, T value) {
        const auto* p = reinterpret_cast<const u8*>(&value);
        for (size_t i = 0; i < sizeof value; ++i) bytes[at+i] = p[i];
    }
    static EdenDsmodBool Mapped(void* raw, u64 at, u64 size) {
        auto& m = *static_cast<Memory*>(raw);
        for (u64 i=0;i<size;++i) if (!m.bytes.contains(at+i)) return EDEN_DSMOD_FALSE;
        return EDEN_DSMOD_TRUE;
    }
    static EdenDsmodBool Read(void* raw, u64 at, void* out, u64 size) {
        if (!Mapped(raw,at,size)) return EDEN_DSMOD_FALSE;
        auto& m = *static_cast<Memory*>(raw);
        for(u64 i=0;i<size;++i) static_cast<u8*>(out)[i] = m.bytes.at(at+i);
        return EDEN_DSMOD_TRUE;
    }
    static EdenDsmodBool Write(void* raw, const EdenDsmodWriteOp* ops, std::uint32_t count) {
        auto& m = *static_cast<Memory*>(raw);
        if (m.reject) return EDEN_DSMOD_FALSE;
        for(unsigned i=0;i<count;++i) {
            const auto& op=ops[i];
            for(unsigned j=0;j<op.size;++j)
                if (!m.bytes.contains(op.address+j) || !op.expect ||
                    m.bytes.at(op.address+j) != static_cast<const u8*>(op.expect)[j])
                    return EDEN_DSMOD_FALSE;
        }
        for(unsigned i=0;i<count;++i) {
            const auto& op=ops[i];
            for(unsigned j=0;j<op.size;++j) m.bytes[op.address+j]=static_cast<const u8*>(op.value)[j];
        }
        ++m.writes;
        return EDEN_DSMOD_TRUE;
    }
};
constexpr u64 base=0x800000000, scene=0x2100000000, holder=scene+0x1000;
constexpr u64 actor=scene+0x2000, keeper=scene+0x3000, layout=scene+0x4000, pane=scene+0x5000;
}
int main() {
    Memory mem;
    EdenDsmodHostApi host{};
    host.userdata=&mem;host.main_base=base;host.read_memory=Memory::Read;host.is_mapped=Memory::Mapped;
    host.get_i64=[](void* raw,const char* key,std::int64_t fallback)->std::int64_t {
        return std::strcmp(key,"__aux_present")==0 ? static_cast<Memory*>(raw)->aux : fallback;
    };
    EdenDsmodHostWriteApi write{};
    write.version=EDEN_DSMOD_WRITE_EXT_VERSION;write.struct_size=sizeof write;
    write.abi_hash=EDEN_DSMOD_WRITE_EXT_HASH;write.userdata=&mem;write.write_batch=Memory::Write;
    mem.Put<u64>(scene,base+SmoReader::kStageSceneVtableOffset);
    mem.Put<u64>(scene+0x308,holder);mem.Put<u64>(holder,base+0x1212CA8);
    mem.Put<u64>(holder+0x18,actor);mem.Put<u64>(actor,base+0x12089E0);
    mem.Put<u64>(actor+0xE0,keeper);mem.Put<u64>(keeper+0x10,layout);
    mem.Put<u64>(layout+0x18,pane);mem.Put<u64>(pane,base+0x135EDC8);
    mem.Put<u8>(pane+0x58,0x13);
    SmoReader::Snapshot snap;
    snap.stage_scene=scene;snap.chain_ready=true;snap.scene_mode_valid=true;
    snap.scene_mode=SmoReader::SceneMode::Playing;
    SmoHud::Controller ctrl;ctrl.SetWriteApi(write);
    ctrl.Update(host,snap,true);
    assert(ctrl.HiddenCount()==1 && mem.bytes.at(pane+0x58)==0x12);
    assert(ctrl.GetterStage()==17 && ctrl.CandidateCount()==1 && ctrl.PaneCount()==1);
    assert(ctrl.VisibleCount()==1 && ctrl.WriteAttempts()==1 && ctrl.FailedWrites()==0);
    assert(ctrl.ProbePane()==pane && ctrl.ProbeFlags()==0x13);
    mem.Put<u8>(pane+0x58,0x92); // unrelated game flag changed while hidden
    mem.aux=false;ctrl.Update(host,snap,true);
    assert(ctrl.HiddenCount()==0 && mem.bytes.at(pane+0x58)==0x93);
    assert(ctrl.GetterStage()==2);
    mem.aux=true;mem.Put<u8>(pane+0x58,0x12);ctrl.Update(host,snap,true);ctrl.Restore(host);
    assert(ctrl.HiddenCount()==0 && mem.bytes.at(pane+0x58)==0x12); // naturally invisible
    mem.Put<u8>(pane+0x58,0x13);mem.reject=true;ctrl.Update(host,snap,true);
    assert(ctrl.HiddenCount()==0 && mem.bytes.at(pane+0x58)==0x13);
    assert(ctrl.GetterStage()==16 && ctrl.WriteAttempts()==1 && ctrl.FailedWrites()==1);
    mem.reject=false;ctrl.Update(host,snap,true);
    assert(ctrl.HiddenCount()==1);mem.Put<u64>(actor,base+0x1234);ctrl.Restore(host);
    assert(ctrl.HiddenCount()==0 && mem.bytes.at(pane+0x58)==0x12); // recycled owner untouched
    mem.Put<u64>(actor,base+0x12089E0);mem.Put<u8>(pane+0x58,0x13);
    snap.scene_mode=SmoReader::SceneMode::Paused;ctrl.Update(host,snap,true);
    assert(ctrl.HiddenCount()==0 && mem.bytes.at(pane+0x58)==0x13);
    snap.scene_mode=SmoReader::SceneMode::Playing;
    mem.Put<u64>(pane,base+0x123456);ctrl.Update(host,snap,true);
    assert(ctrl.GetterStage()==12 && ctrl.PaneCount()==0 && ctrl.WriteAttempts()==0);
    mem.Put<u64>(pane,base+0x132B248);ctrl.Update(host,snap,true); // native base Pane accepted too
    assert(ctrl.HiddenCount()==1);
    ctrl.Update(host,snap,false);
    assert(ctrl.HiddenCount()==0 && mem.bytes.at(pane+0x58)==0x13);
    // MapMini owns the compass, and separately draws its MapMiniPlayer arrow.
    // PlayGuideMenu is another holder child. Restore all three independently.
    constexpr u64 guide=scene+0x6000, guide_keeper=scene+0x7000;
    constexpr u64 guide_layout=scene+0x8000, guide_pane=scene+0x9000;
    mem.Put<u64>(holder+0x50,guide);mem.Put<u64>(guide,base+0x120D370);
    mem.Put<u64>(guide+0xE0,guide_keeper);mem.Put<u64>(guide_keeper+0x10,guide_layout);
    mem.Put<u64>(guide_layout+0x18,guide_pane);mem.Put<u64>(guide_pane,base+0x135EDC8);
    mem.Put<u8>(guide_pane+0x58,0x93);
    constexpr u64 menu=scene+0xA000, mini_player=scene+0xE000;
    constexpr u64 mini_icon=scene+0x12000, icons=scene+0x16000;
    const auto add_pane=[&](u64 a,u64 vt){
        mem.Put<u64>(a,base+vt);mem.Put<u64>(a+0xE0,a+0x1000);
        mem.Put<u64>(a+0x1010,a+0x2000);mem.Put<u64>(a+0x2018,a+0x3000);
        mem.Put<u64>(a+0x3000,base+0x135EDC8);mem.Put<u8>(a+0x3058,0x93);
    };
    add_pane(menu,0x1308A70);add_pane(mini_player,0x12ECB70);
    add_pane(mini_icon,0x12ECB70);mem.Put<std::int32_t>(guide+0x148,1);
    mem.Put<u64>(guide+0x150,icons);mem.Put<u64>(icons,mini_icon);
    mem.Put<u64>(holder+0x68,menu);mem.Put<u64>(guide+0x140,mini_player);
    ctrl.Update(host,snap,true);
    assert(ctrl.HiddenCount()==5 && mem.bytes.at(guide_pane+0x58)==0x92);
    assert(mem.bytes.at(menu+0x3058)==0x92 && mem.bytes.at(mini_player+0x3058)==0x92);
    assert(mem.bytes.at(mini_icon+0x3058)==0x92);
    mem.aux=false;ctrl.Update(host,snap,true);
    assert(ctrl.HiddenCount()==0 && mem.bytes.at(guide_pane+0x58)==0x93);
    assert(mem.bytes.at(menu+0x3058)==0x93 && mem.bytes.at(mini_player+0x3058)==0x93);
    assert(mem.bytes.at(mini_icon+0x3058)==0x93);
    mem.aux=true;ctrl.Update(host,snap,true);
    mem.Put<u64>(guide,base+0x1234);ctrl.Restore(host);
    assert(mem.bytes.at(guide_pane+0x58)==0x92);
    mem.Put<u64>(guide,base+0x120D370);mem.Put<std::int32_t>(guide+0x148,11);
    ctrl.Update(host,snap,true);
    assert(mem.bytes.at(mini_icon+0x3058)==0x93); // reject an oversized child array
    ctrl.Restore(host);
}
