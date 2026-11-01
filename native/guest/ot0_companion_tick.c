// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Called after the original EngineLoop Tick, on its game thread. Only native
// UI visibility and effect-free parameter getters are called; no recovery,
// attack, resource setters, input injection or menu opening exists here.
#include "ot0_companion_abi.h"
#include "ot0_heal_abi.h"
#include "ot0_stats_abi.h"
typedef uint64_t Addr;
typedef struct { Addr data; int32_t count,capacity; } Array;
// Defined in ot0_heal_query.c; one typed prototype so both sides must agree.
void ot0_heal_query(volatile Ot0HealMailbox *mailbox, Addr main);
void ot0_field_stats(volatile Ot0StatsMailbox *box, Addr main, uint32_t heartbeat);
#define PTR(p,o) (*(const Addr *)((Addr)(p)+(o)))
#define U32(p,o) (*(const uint32_t *)((Addr)(p)+(o)))
#define U8(p,o) (*(const uint8_t *)((Addr)(p)+(o)))
#define FN(o,R,...) ((R (*)(__VA_ARGS__))(main+(o)))
static int ptr(Addr p) { return p>=0x1000000 && p<((Addr)1<<39) && !(p&7); }
static int typed(Addr main,Addr p,Addr vt) {
  return ptr(p) && PTR(p,0)==main+vt && !(U32(p,8)&0x40000030);
}
static int array(Addr p,Array *out,int limit) {
  *out=*(const Array *)p;
  return out->count>=0 && out->count<=limit && out->capacity>=out->count &&
         out->capacity<=limit*2 && (!out->count || ptr(out->data));
}
static int equal_array(Array a,Array b) {
  return a.data==b.data && a.count==b.count && a.capacity==b.capacity;
}
// Exact UObject index/serial ownership, native weak-object layout45bdc0.
static int serial(Addr main,Addr p,uint32_t *index,uint32_t *number) {
  if(!ptr(p))return 0;
  uint32_t n=U32(p,0xc),count=U32(main+0x6ce97e8,0x24);
  Addr chunks=PTR(main+0x6ce97e8,0x10);
  if(n>=count || count>1000000 || !ptr(chunks))return 0;
  Addr chunk=PTR(chunks,(Addr)(n>>16)*8);
  if(!ptr(chunk))return 0;
  Addr item=chunk+(Addr)(n&65535)*20;
  uint32_t s=U32(item,0x10);
  if(PTR(item,0)!=p || U32(item,8)&0x30200000 || (int32_t)s<=0)return 0;
  *index=n;*number=s;return 1;
}
// UObject serial0 is legitimate until its first weak reference. Use the
// native atomic allocator through FWeakObjectPtr assignment, never a fake stamp.
static int ensure_serial(Addr main,Addr p,uint32_t *index,uint32_t *number) {
  if(serial(main,p,index,number))return 1;
  if(!ptr(p)||PTR(main,0x67d0420)!=main+0x6ce97e8)return 0;
  uint32_t n=U32(p,0xc),count=U32(main+0x6ce97e8,0x24);
  Addr chunks=PTR(main+0x6ce97e8,0x10);
  if(n>=count||count>1000000||!ptr(chunks))return 0;
  Addr chunk=PTR(chunks,(Addr)(n>>16)*8);
  if(!ptr(chunk))return 0;
  Addr item=chunk+(Addr)(n&65535)*20;
  if(PTR(item,0)!=p||(U32(item,8)&0x30200000)||U32(item,0x10)!=0)return 0;
  uint32_t weak[2]={0};
  FN(0x45bb64,void,uint32_t *,Addr)(weak,p);
  if(weak[0]!=n||(int32_t)weak[1]<=0||FN(0x45bdc0,Addr,uint32_t *)(weak)!=p)return 0;
  return serial(main,p,index,number)&&*index==weak[0]&&*number==weak[1];
}
static Addr root(Addr main) {
  Addr m=PTR(main,0x7cc9a18);
  if(!typed(main,m,0x63b6f18))return 0;
  Array nodes; if(!array(m+0x2f0,&nodes,256) || !nodes.count)return 0;
  uint32_t free=U32(m,0x324),bits_count=U32(m,0x318),hashes=U32(m,0x338);
  Addr bits=PTR(m,0x310),buckets=PTR(m,0x330);
  if(free>=(uint32_t)nodes.count || bits_count<(uint32_t)nodes.count ||
     bits_count>256 || !hashes || hashes>256 || (hashes&(hashes-1)))return 0;
  if(!bits)bits=m+0x300; if(!buckets) {if(hashes!=1)return 0;buckets=m+0x328;}
  if(!ptr(bits)||!ptr(buckets))return 0;
  int32_t at=*(const int32_t *)(buckets+(10&(hashes-1))*4);
  for(int steps=0;steps<256 && at!=-1;++steps) {
    if(at<0 || at>=nodes.count || !(U32(bits,(at/32)*4)&(1u<<(at%32))))return 0;
    Addr item=nodes.data+(Addr)at*24;
    if(U8(item,0)==10) {
      Addr ui=PTR(item,8);
      if(!typed(main,ui,0x63c0c48))return 0;
      Addr s=PTR(ui,0x108),c=PTR(ui,0x110);
      if(!ptr(s)||!ptr(c)||PTR(s,0)!=main+0x5d011e8||PTR(s,8)!=s||
         U32(c,8)<1||U32(c,8)>1000000||U8(s,0x185)!=1)return 0;
      return ui;
    }
    at=(int32_t)U32(item,0x10);
  }
  return 0;
}
static int live_deadline(uint32_t now,uint32_t until) {
  return (int32_t)(until-now)>=0 && until-now<=120;
}
#include "ot0_native_minimap.h"
static int stamp(Addr main,Addr actor,uint32_t *number,uint32_t *selector,uint64_t *hash) {
  uint32_t index;Array effects;
  if(!typed(main,actor,0x63f39b8)||!serial(main,actor,&index,number)||
     !array(actor+0x800,&effects,128))return 0;
  *selector=U8(actor,0x2a8);if(*selector>1)return 0;
  uint64_t value=14695981039346656037ULL;
  for(unsigned i=0;i<4;++i){value^=((uint32_t)effects.count>>(i*8))&255;value*=1099511628211ULL;}
  unsigned bytes=(unsigned)effects.count*0xe8;
  if(bytes && effects.data>=((Addr)1<<39)-bytes)return 0;
  for(unsigned i=0;i<bytes;++i){value^=U8(effects.data,i);value*=1099511628211ULL;}
  if(!equal_array(effects,*(const Array *)(actor+0x800))||U8(actor,0x2a8)!=*selector)return 0;
  *hash=value;return 1;
}
static void query(volatile Ot0CompanionMailbox *m,Addr main,uint32_t state) {
  uint32_t seq=__atomic_load_n(&m->query_seq,__ATOMIC_ACQUIRE);
  if((seq&1)||seq==m->query_ack)return;
  uint32_t mask=m->query_mask,epoch=m->query_epoch,deadline=m->query_deadline;
  if(__atomic_load_n(&m->query_seq,__ATOMIC_ACQUIRE)!=seq)return;
  uint32_t result=OT0_NATIVE_BAD_REQUEST;
  if(m->relocation_delta)result=OT0_NATIVE_UNSUPPORTED;
  else if(epoch!=m->epoch||!live_deadline(m->heartbeat,deadline))result=OT0_NATIVE_EXPIRED;
  else if(state!=26)result=OT0_NATIVE_NOT_BATTLE;
  else if(mask && !(mask&~0x3fffu) && __builtin_popcount(mask)<=2) {
    Addr bm=PTR(main,0x7cc5b78);Array front,back,enemies;
    result=OT0_NATIVE_UNAVAILABLE;
    if(ptr(bm)&&!(U32(bm,8)&0x40000030)&&
       array(bm+0x9b8,&front,4)&&array(bm+0x9c8,&back,4)&&array(bm+0xa98,&enemies,6)) {
      m->query_bm=bm;result=OT0_NATIVE_OK;
      for(unsigned i=0;i<OT0_COMPANION_ROWS;++i) {
        volatile Ot0NativeStatRow *r=&m->rows[i];r->valid_mask=0;r->error=0;r->actor=0;r->id=0;
        if(!(mask&(1u<<i)))continue;
        Array a=i<4?front:i<8?back:enemies;unsigned slot=i<4?i:i<8?i-4:i-8;
        if(slot>=(unsigned)a.count)continue;
        Addr actor=PTR(a.data,slot*8);uint32_t number,selector,team=i<8?0:1;
        uint64_t hash;uint32_t actor_index;
        if(!typed(main,actor,0x63f39b8)||U8(actor,0x293)!=team||!U32(actor,0x298)||
           !ensure_serial(main,actor,&actor_index,&number)) {
          r->error=OT0_NATIVE_UNAVAILABLE;continue;
        }
        if(!stamp(main,actor,&number,&selector,&hash)||U8(actor,0x293)!=team||!U32(actor,0x298)) {
          r->error=OT0_NATIVE_UNAVAILABLE;continue;
        }
        r->actor=actor;r->id=U32(actor,0x298);r->team=team;r->slot=i;r->serial=number;
        r->weapon_selector=selector;r->ailment_fingerprint=hash;
        uint32_t native_weapon=FN(0x3837718,uint32_t,Addr)(actor)&1;
        r->native_weapon=native_weapon;
        if(team==0 && U8(actor,0x294)==0 && native_weapon!=selector) {
          r->error=OT0_NATIVE_STALE;continue;
        }
        // Clear derived caches only. The game's active-weapon setter/recalc
        // can clamp HP/SP and must never be used for an information query.
        FN(0x38308b4,void,Addr)(actor);
        // Keep native max-resource cache entries available to existing enemy
        // readers. These are getters only; no HP/SP recalc/clamp setter.
        (void)FN(0x382e264,int32_t,Addr,uint32_t,Addr,uint32_t)(actor,1,0,0);
        (void)FN(0x382e264,int32_t,Addr,uint32_t,Addr,uint32_t)(actor,2,0,0);
        for(unsigned p=0;p<8;++p)
          r->values[p]=FN(0x382e264,int32_t,Addr,uint32_t,Addr,uint32_t)(actor,9+p,0,0);
        uint32_t number2,selector2;uint64_t hash2;
        if(PTR(a.data,slot*8)!=actor||!stamp(main,actor,&number2,&selector2,&hash2)||
           number2!=number||selector2!=selector||hash2!=hash||U8(actor,0x293)!=team||U32(actor,0x298)!=r->id||
           (team==0 && U8(actor,0x294)==0 && (FN(0x3837718,uint32_t,Addr)(actor)&1)!=selector)) {
          r->error=OT0_NATIVE_STALE;continue;
        }
        r->valid_mask=255;
      }
      if(PTR(main,0x7cc5b78)!=bm || !equal_array(front,*(const Array *)(bm+0x9b8)) ||
         !equal_array(back,*(const Array *)(bm+0x9c8)) || !equal_array(enemies,*(const Array *)(bm+0xa98)) ||
         __atomic_load_n(&m->query_seq,__ATOMIC_ACQUIRE)!=seq || epoch!=m->epoch)
        result=OT0_NATIVE_STALE;
    }
  }
  m->query_result=result;
  __atomic_store_n(&m->query_ack,seq,__ATOMIC_RELEASE);
}
__attribute__((visibility("hidden"),section(".text.entry"))) void
ot0_companion_tick(volatile Ot0CompanionMailbox *m,Addr main) {
  Addr app=PTR(main,0x7cc8cc0);uint32_t state=0;
  if(typed(main,app,0x6207610))state=U8(app,0x2f0);
  m->helper_version=OT0_COMPANION_ABI;m->app_state=state;
  ot0_native_minimap_tick(m,main,state);query(m,main,state);
  // Separate request area; the shipped map/stat ABI remains unchanged.
  volatile Ot0HealMailbox *heal=(volatile Ot0HealMailbox *)((Addr)m+2048);
  heal->heartbeat=m->heartbeat;heal->epoch=m->epoch;
  if(!m->relocation_delta)ot0_heal_query(heal,main);
  if(!m->relocation_delta)
    ot0_field_stats((volatile Ot0StatsMailbox *)((Addr)m+OT0_STATS_OFFSET),main,m->heartbeat);
}
