// SPDX-License-Identifier: GPL-3.0-or-later
// Compile and execute the actual freestanding helper with native functions replaced by fixtures.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
namespace {
uintptr_t node_base=0,manager_address=0;
unsigned reticle_calls=0,invalid_node=UINT32_MAX;
void NativeReticle(){++reticle_calls;}
unsigned long NativeNode(unsigned long manager,unsigned id,unsigned group){
    assert(group==0x21 && manager==manager_address && id<256);
    return id==invalid_node?0:node_base+id*32;
}
#define FN(off,R,...) reinterpret_cast<R(*)(__VA_ARGS__)>( \
    off==0x204c0 ? reinterpret_cast<uintptr_t>(&NativeReticle) : reinterpret_cast<uintptr_t>(&NativeNode))
#include "../guest/re4_hud_tick.c"
#undef FN
}
int main(){
    constexpr size_t size=0xB26000;
    auto* memory=static_cast<uint8_t*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0));
    assert(memory!=MAP_FAILED);const auto main=reinterpret_cast<Addr>(memory);
    node_base=main+0x1000;manager_address=main+0x4000;
    *reinterpret_cast<Addr*>(memory+0xB25D98)=manager_address;
    auto& flags=*reinterpret_cast<uint8_t*>(node_base+0xFE*32);
    auto& frame=*reinterpret_cast<uint8_t*>(node_base);frame=0x78;
    Re4HudMailbox m{};m.epoch=m.request_epoch=1;m.seq=2;m.deadline=90;m.mask=2;
    flags=0xAB;re4_hud_tick(main,&m);assert(frame==0x70);assert(flags==0xA3 && m.saved_bits[0]==8 && m.applied_mask==2);
    re4_hud_tick(main,&m);assert(m.saved_bits[0]==8); // Never recapture our own cleared bit.
    flags|=0x40;m.mask=0;re4_hud_tick(main,&m);assert(flags==0xEB && !m.saved_nodes[0]);assert(frame==0x78 && !m.saved_nodes[1]);
    for(unsigned failure=0;failure<4;++failure) {
        m={};m.epoch=m.request_epoch=1;m.seq=2;m.deadline=90;m.mask=2;flags=0x08;frame=0x78;
        re4_hud_tick(main,&m);assert(flags==0 && frame==0x70);
        if(failure==0)m.deadline=m.heartbeat; // expiry
        if(failure==1)m.request_epoch=2;
        if(failure==2)m.seq=3; // torn host write
        if(failure==3)m.mask=4; // unsupported request
        re4_hud_tick(main,&m);assert(flags==8 && !m.saved_nodes[0] && frame==0x78 && !m.saved_nodes[1]);
    }
    m={};m.epoch=m.request_epoch=1;m.seq=2;m.deadline=90;m.mask=2;flags=0x80;
    re4_hud_tick(main,&m);m.mask=0;re4_hud_tick(main,&m);assert(flags==0x80); // Originally invisible stays invisible.
    m={};m.epoch=m.request_epoch=1;m.seq=2;m.deadline=90;m.mask=2;flags=8;
    re4_hud_tick(main,&m);m.mask=0;node_base+=0x5000;
    auto& replacement=*reinterpret_cast<uint8_t*>(node_base+0xFE*32);replacement=0x40;
    re4_hud_tick(main,&m);assert(replacement==0x40 && !m.saved_nodes[0]); // No writes through old identities.
    m={};m.epoch=m.request_epoch=1;m.seq=2;m.deadline=90;m.mask=2;replacement=8;
    re4_hud_tick(main,&m);assert(replacement==0 && m.saved_nodes[0]);
    *reinterpret_cast<Addr*>(memory+0xB25D98)=0;m.seq=8;
    re4_hud_tick(main,&m);assert(m.error==2 && m.ack==8 && replacement==0 && m.saved_nodes[0]);
    *reinterpret_cast<Addr*>(memory+0xB25D98)=manager_address;m.mask=0;
    re4_hud_tick(main,&m);assert(replacement==8 && !m.saved_nodes[0] && !m.error);
    m={};m.epoch=m.request_epoch=1;m.seq=2;m.deadline=90;m.mask=2;replacement=8;
    re4_hud_tick(main,&m);assert(replacement==0);
    // A new mailbox generation must not write an old bit into reused addresses.
    m.epoch=m.request_epoch=2;m.mask=0;replacement=0x40;
    auto& new_frame=*reinterpret_cast<uint8_t*>(node_base);new_frame=0x20;
    re4_hud_tick(main,&m);assert(replacement==0x40 && new_frame==0x20 && !m.saved_nodes[0] && !m.saved_nodes[1]);
    // Cover both sticky lookup failures and ordinary nodes from each supported mask.
    for(unsigned id:{0xFEu,0u,0xBu,7u}) {
        m={};m.epoch=m.request_epoch=1;m.seq=10;m.deadline=90;
        m.mask=id==0xB?1:2;invalid_node=id;
        re4_hud_tick(main,&m);assert(m.error==3 && m.ack==10 && !m.applied_mask);
        invalid_node=UINT32_MAX;m.mask=0;
        re4_hud_tick(main,&m);assert(!m.error && m.ack==10 && !m.applied_mask);
    }
    assert(reticle_calls==28);munmap(memory,size);
    std::cout<<"guest helper stop/expiry/epoch/torn/invalid/identity restoration passed\n";
}
#else
int main(){std::cout<<"guest helper fixture requires Linux x86_64 MAP_32BIT\n";}
#endif
