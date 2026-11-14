// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_hud_abi.h"
typedef unsigned long Addr;
#ifndef FN
#define FN(off,R,...) ((R (*)(__VA_ARGS__))(main+(off)))
#endif
static int sane(Addr p) { return p>=0x10000 && p<((Addr)1<<39) && !(p&7); }
// Called through the native HUD updater's restored-frame tail branch. Never
// retain native UI pointers: the game's original functions own their lifecycle.
__attribute__((section(".text.entry")))
void re4_hud_tick(Addr main,volatile Re4HudMailbox *m) {
    FN(0x204c0,void,void)(); // original reticle update, unconditionally
    uint32_t now=m->heartbeat+1;
    __atomic_store_n(&m->heartbeat,now,__ATOMIC_RELEASE);
    m->applied_mask=0;m->error=0;
    uint32_t seq=__atomic_load_n(&m->seq,__ATOMIC_ACQUIRE);
    uint32_t epoch=m->request_epoch,deadline=__atomic_load_n(&m->deadline,__ATOMIC_ACQUIRE),mask=m->mask;
    // Invalid/torn/expired requests stop hiding and restore the non-self-healing needle.
    if((seq&1) || __atomic_load_n(&m->seq,__ATOMIC_ACQUIRE)!=seq ||
       epoch!=m->epoch || !re4_hud_lease_live(now,deadline) || (mask&~3u))mask=0;
    Addr manager=*(const Addr *)(main+0xB25D98);
    if(!sane(manager)){m->error=2;__atomic_store_n(&m->ack,seq,__ATOMIC_RELEASE);return;}
    Addr sticky[2]={0,0};
    if((mask&2) || m->saved_nodes[0] || m->saved_nodes[1]) {
        // FE is the non-self-healing needle; 0 is the native static frame/master
        // identified by its own show/hide dispatcher at main+20920.
        const unsigned ids[2]={0xFE,0};
        for(unsigned n=0;n<2;++n) {
            sticky[n]=FN(0x28c8b0,Addr,Addr,unsigned,unsigned)(manager,ids[n],0x21);
            if(!sane(sticky[n])){m->error=3;__atomic_store_n(&m->ack,seq,__ATOMIC_RELEASE);return;}
            // Compare identities only: never dereference a retained native pointer.
            if(m->saved_nodes[n] && (m->saved_epoch!=m->epoch || m->saved_manager!=manager || m->saved_nodes[n]!=sticky[n])) {
                m->saved_nodes[n]=0;m->saved_bits[n]=0;
            }
        }
        if(!(mask&2)) {
            for(unsigned n=0;n<2;++n)if(m->saved_nodes[n]) {
                volatile unsigned char *flags=(volatile unsigned char *)sticky[n];
                *flags=(unsigned char)((*flags&~8u)|m->saved_bits[n]);
                m->saved_nodes[n]=0;m->saved_bits[n]=0;
            }
            m->saved_manager=0;
        }
    }
    Addr nodes[7];unsigned count=0;
    if(mask&1) {
        const unsigned ids[4]={0xB,0xA,0x17,0x3F};
        for(unsigned n=0;n<4;++n)nodes[count++]=FN(0x28c8b0,Addr,Addr,unsigned,unsigned)(manager,ids[n],0x21);
    }
    if(mask&2)for(unsigned id=7;id<=9;++id)
        nodes[count++]=FN(0x28c8b0,Addr,Addr,unsigned,unsigned)(manager,id,0x21);
    for(unsigned n=0;n<count;++n)if(!sane(nodes[n])){m->error=3;__atomic_store_n(&m->ack,seq,__ATOMIC_RELEASE);return;}
    if(__atomic_load_n(&m->seq,__ATOMIC_ACQUIRE)!=seq)return;
    if(mask&2)for(unsigned n=0;n<2;++n) {
        volatile unsigned char *flags=(volatile unsigned char *)sticky[n];
        if(!m->saved_nodes[n]) {
            m->saved_bits[n]=*flags&8u;m->saved_nodes[n]=sticky[n];
        }
        *flags=(unsigned char)(*flags&~8u);
        m->saved_manager=manager;m->saved_epoch=m->epoch;
    }
    // These nodes are reasserted by the native updater before every helper call.
    for(unsigned n=0;n<count;++n) {
        volatile unsigned char *flags=(volatile unsigned char *)nodes[n];
        *flags=(unsigned char)(*flags&~8u);
    }
    m->applied_mask=mask;
    __atomic_store_n(&m->ack,seq,__ATOMIC_RELEASE);
}
