// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Native SP recovery, Switch1.0.5. Runs only from the game's own Tick hook.
// QUERY is effect-free. APPLY validates the same native rules again and binds
// to a fresh query's resources, save/avatar and actual party identities.
#include "ot0_heal_abi.h"
#include "ot0_stats_abi.h"
typedef uint64_t Addr;
typedef struct {
  uint32_t *data;
  int32_t count, capacity;
} NativeArray;
#define FN(off, result, ...) ((result (*)(__VA_ARGS__))(main + (off)))
#define U8(p, off) (*(const uint8_t *)((Addr)(p) + (off)))
#define U32(p, off) (*(const uint32_t *)((Addr)(p) + (off)))
#define PTR(p, off) (*(const Addr *)((Addr)(p) + (off)))
static int valid(Addr main, Addr object) {
  return object && object < ((Addr)1 << 39) && !(object & 7) &&
         FN(0x42e6b4, int, Addr, int)(object, 1);
}
// Source-correlated cached CommonDialog visibility, same native route as the
// host reader: MenuUIManager2a8 ->UWidget108/110 weak cached outer Slate.
static int common_dialog_hidden(Addr main, Addr menus) {
  Addr dialog = PTR(menus, 0x2a8);
  if (!valid(main, dialog))
    return -1;
  Addr slate = PTR(dialog, 0x108), weak = PTR(dialog, 0x110);
  if (!slate || !weak || slate >= (((Addr)1 << 39) - 0x186) ||
      weak >= (((Addr)1 << 39) - 12) || (slate & 7) || (weak & 7))
    return -1;
  int refs = (int)U32(weak, 8);
  if (refs < 1 || refs > 1000000 || PTR(slate, 8) != slate ||
      U8(slate, 0x185) != 1)
    return -1;
  uint8_t visibility = U8(slate, 0x184);
  if (visibility == 2 || visibility == 4)
    return 1;
  if (visibility == 1 || visibility == 0x11 || visibility == 0x19)
    return 0;
  return -1;
}
static int has(const uint32_t *ids, int count, uint32_t id) {
  for (int i = 0; i < count; ++i)
    if (ids[i] == id)
      return 1;
  return 0;
}
static int learned(Addr main, uint32_t caster, uint32_t skill) {
  Addr db = FN(0x38eb770, Addr, uint32_t)(caster);
  if (db) {
    const uint32_t *ids = (const uint32_t *)PTR(db, 0x10);
    int count = (int)U32(db, 0x18);
    if (ids && count >= 0 && count <= 256)
      for (int i = 0; i < count; ++i)
        if (ids[i] == skill &&
            (FN(0x3af8b68, int, uint32_t, uint32_t)(caster, (uint32_t)i) & 1))
          return 1;
  }
  NativeArray granted = {0};
  FN(0x3a81e48, int, NativeArray *, int, int, int, int)(&granted, (int)caster,
                                                        1, 0, 0);
  int found = granted.data && granted.count >= 0 && granted.count <= 256 &&
              has(granted.data, granted.count, skill);
  if (granted.data)
    FN(0xa2444, void, void *)(granted.data);
  return found;
}
static int target_allowed(Addr main, Addr ability, uint32_t target) {
  const NativeArray *effects =
      FN(0x3b01084, const NativeArray *, uint32_t)(U32(ability, 8));
  if (!effects || !effects->data || effects->count < 1 || effects->count > 256)
    return 0;
  // Exact3c76910 eligibility, over an actual current-party target rather than
  // the menu's encoded slot. Native target identity/party membership is
  // checked.
  int alive = FN(0x3a7bf38, int, uint32_t)(target) & 1;
  int missing = FN(0x3a7bdd4, int, uint32_t)(target) & 1;
  for (int i = 0; i < effects->count; ++i) {
    Addr effect = FN(0x3af9ee0, Addr, uint32_t)(effects->data[i]);
    if (!effect)
      continue;
    uint8_t type = U8(effect, 0x14);
    int dead_type = FN(0x3b0d74c, int, uint32_t)(U32(effect, 0x14)) & 1;
    if (((((dead_type | (U8(effect, 0x18) == 7)) ^ alive) & 1) ||
         type == 0x19) &&
        U8(effect, 0x1c) == 5 && missing)
      return 1;
  }
  return 0;
}
static int supported_hp(Addr main, Addr ability) {
  const NativeArray *effects=FN(0x3b01084,const NativeArray *,uint32_t)(U32(ability,8));
  if(!effects || !effects->data || effects->count<1 || effects->count>256)return 0;
  for(int i=0;i<effects->count;++i) {
    Addr effect=FN(0x3af9ee0,Addr,uint32_t)(effects->data[i]);
    // Native3c76af0 itself ignores every category except5. Other entries
    // must not reject a legitimate native skill.
    if(effect && (FN(0x3afb9ac,int,Addr)(effect)&255)==5)return 1;
  }
  return 0;
}
static uint32_t party_identity(const uint32_t *ids,int count,uint32_t id) {
  if(id<900 || id>909)return id;
  uint32_t found=0;
  for(int i=0;i<count;++i)if(ids[i]>=900 && ids[i]<=909) {
    if(found)return 0;
    found=ids[i];
  }
  return found;
}
// A KO'd target takes only revival skills (native effect target mode 7); a living target never
// does, matching the menu's own split of AnyDead/AnyAliveMissingHP gates.
static void consider(Addr main,uint32_t caster,uint32_t skill,uint32_t target,int dead,
                     uint32_t *best_caster,uint32_t *best_skill,int32_t *best_cost) {
  if(!skill || FN(0x3a743a0,int,uint32_t)(caster)<=0)return;
  Addr ability=FN(0x3af9c00,Addr,uint32_t)(skill);
  if(!ability || U32(ability,0xe4)!=3 || !U8(ability,0xfc) ||
     (FN(0x3b0c130,int,uint32_t,int)(skill,7)&1)!=dead ||
     !supported_hp(main,ability) || !target_allowed(main,ability,target))return;
  int32_t cost=FN(0x3b08d28,int32_t,uint32_t,uint32_t)(skill,caster);
  if(cost<0 || cost>FN(0x3a74844,int32_t,uint32_t)(caster))return;
  // Lowest native SP cost, stable native party/skill order for ties.
  if(!*best_skill || cost<*best_cost) {
    *best_caster=caster;*best_skill=skill;*best_cost=cost;
  }
}
static void find_skill(Addr main,const uint32_t *ids,int count,uint32_t target,int dead,
                       uint32_t *caster,uint32_t *skill) {
  int32_t best_cost=0;*caster=0;*skill=0;
  for(int p=0;p<count;++p) {
    Addr db=FN(0x38eb770,Addr,uint32_t)(ids[p]);
    if(db) {
      NativeArray a=*(const NativeArray *)(db+0x10);
      if(a.data && a.count>=0 && a.count<=256)
        for(int i=0;i<a.count;++i)if(a.data[i] &&
           (FN(0x3af8b68,int,uint32_t,uint32_t)(ids[p],(uint32_t)i)&1))
          consider(main,ids[p],a.data[i],target,dead,caster,skill,&best_cost);
    }
    NativeArray granted={0};
    FN(0x3a81e48,int,NativeArray *,int,int,int,int)(&granted,(int)ids[p],1,0,0);
    if(granted.data && granted.count>=0 && granted.count<=256)
      for(int i=0;i<granted.count;++i)
        consider(main,ids[p],granted.data[i],target,dead,caster,skill,&best_cost);
    if(granted.data)FN(0xa2444,void,void *)(granted.data);
  }
}
__attribute__((visibility("hidden"))) void
ot0_heal_query(volatile Ot0HealMailbox *mailbox, Addr main) {
  mailbox->version=2;
  uint32_t seq = __atomic_load_n(&mailbox->seq, __ATOMIC_ACQUIRE);
  if ((seq & 1) || seq == mailbox->ack)
    return;
  uint32_t result = OT0_HEAL_BAD_REQUEST;
  uint32_t caster = mailbox->caster, skill = mailbox->skill,
           target = mailbox->target;
  if (__atomic_load_n(&mailbox->seq, __ATOMIC_ACQUIRE) != seq)
    return;
  int32_t cost = 0, hp = 0, sp = 0, max_hp = 0;
  mailbox->native_eligible = 0;
  mailbox->skill_text_id = 0;
  mailbox->app_state = 0;
  uint32_t op=mailbox->op;
  if ((op != OT0_HEAL_QUERY && op != OT0_HEAL_APPLY && op != OT0_HEAL_FIND) ||
      !target || (op!=OT0_HEAL_FIND && (!caster || !skill)))
    goto done;
  if (mailbox->expected_epoch != mailbox->epoch ||
      (int32_t)(mailbox->deadline - mailbox->heartbeat) < 0 ||
      mailbox->deadline - mailbox->heartbeat > 120) {
    result = OT0_HEAL_EXPIRED;
    goto done;
  }
  Addr app = PTR(main, 0x7cc8cc0);
  if (!valid(main, app) || U8(app, 0x2f0) != 23) {
    result = OT0_HEAL_NOT_FIELD;
    goto done;
  }
  mailbox->app_state = 23;
  Addr menus = PTR(main, 0x7cc9a18);
  if (!valid(main, menus)) {
    result = OT0_HEAL_NATIVE_UNAVAILABLE;
    goto done;
  }
  if (U8(menus, 0x38a) != 0) {
    result = OT0_HEAL_MENU_OPEN;
    goto done;
  }
  int hidden = common_dialog_hidden(main, menus);
  if (hidden < 0) {
    result = OT0_HEAL_NATIVE_UNAVAILABLE;
    goto done;
  }
  if (!hidden) {
    result = OT0_HEAL_MODAL_OPEN;
    goto done;
  }
  if (FN(0x3942d94, int, void)() & 1) {
    result = OT0_HEAL_EVENT_PLAYING;
    goto done;
  }
  Addr characters = PTR(main, 0x7cc6288);
  if (!valid(main, characters)) {
    result = OT0_HEAL_NATIVE_UNAVAILABLE;
    goto done;
  }
  Addr avatar = PTR(characters, 0x290);
  if (!valid(main, avatar) || !valid(main, PTR(avatar, 0x2c8))) {
    result = OT0_HEAL_NATIVE_UNAVAILABLE;
    goto done;
  }
  // Native reflected IsSystemDialogOpen resolves World's GameInstance388
  // SystemDialog and tests its native open flag via3be21dc.
  if (FN(0x39db094, int, Addr)(avatar) & 1) {
    result = OT0_HEAL_MODAL_OPEN;
    goto done;
  }
  // Reflected Pawn.IsMoveInputIgnored: exec297ce04->virtual898;
  // KSPlayerCharacter VT6416cf0[898]=297f790 ->Controller virtual860.
  if (FN(0x297f790, int, Addr)(avatar) & 1) {
    result = OT0_HEAL_CONTROL_LOCKED;
    goto done;
  }
  NativeArray *party = FN(0x3a70e10, NativeArray *, int, int, int, int, int,
                          int)(-1, 1, 1, 1, 0, 0);
  uint32_t ids[8];
  int count = party ? party->count : 0;
  if (count < 1 || count > 8 || !party->data) {
    result = OT0_HEAL_NATIVE_UNAVAILABLE;
    goto done;
  }
  for (int i = 0; i < count; ++i)
    ids[i] = party->data[i];
  uint64_t hash=14695981039346656037ULL;
  for(int i=0;i<count;++i)for(int j=0;j<4;++j) {
    hash^=(ids[i]>>(j*8))&255;hash*=1099511628211ULL;
  }
  Addr saves=PTR(main,0x7cc8540);
  if(!valid(main,saves) || !valid(main,PTR(saves,0x2c8))) {
    result=OT0_HEAL_NATIVE_UNAVAILABLE;goto done;
  }
  Addr save=PTR(saves,0x2c8);
  target=party_identity(ids,count,target);
  if(!has(ids,count,target)) {result=OT0_HEAL_NOT_IN_PARTY;goto done;}
  // Native alive is HP-derived (3a7bf38); a dead member (HP 0) is a revival target.
  int dead=!(FN(0x3a7bf38,int,uint32_t)(target)&1);
  hp=FN(0x3a743a0,int,uint32_t)(target);
  max_hp=FN(0x3a742f8,int,uint32_t)(target);
  if(dead ? hp!=0 || max_hp<=0 : hp<=0 || hp>=max_hp) {result=OT0_HEAL_NO_TARGET;goto done;}
  if(op==OT0_HEAL_FIND) {
    find_skill(main,ids,count,target,dead,&caster,&skill);
    if(!skill) {result=OT0_HEAL_NO_TARGET;goto done;}
  }
  caster=party_identity(ids,count,caster);
  if (!has(ids, count, caster)) {
    result = OT0_HEAL_NOT_IN_PARTY;
    goto done;
  }
  if (!learned(main, caster, skill)) {
    result = OT0_HEAL_NOT_LEARNED;
    goto done;
  }
  Addr ability = FN(0x3af9c00, Addr, uint32_t)(skill);
  if (!ability || U32(ability, 0xe4) != 3 || !U8(ability, 0xfc)) {
    result = OT0_HEAL_NOT_RECOVERY;
    goto done;
  }
  if(!supported_hp(main,ability)) {result=OT0_HEAL_UNSUPPORTED;goto done;}
  cost = FN(0x3b08d28, int, uint32_t, uint32_t)(skill, caster);
  sp = FN(0x3a74844, int, uint32_t)(caster);
  if (cost < 0 || sp < cost || FN(0x3a743a0, int, uint32_t)(caster) <= 0) {
    result = OT0_HEAL_UNAFFORDABLE;
    goto done;
  }
  int revive = FN(0x3b0c130, int, uint32_t, int)(skill, 7) & 1;
  if(revive!=dead) {result=OT0_HEAL_NO_TARGET;goto done;}
  int eligible =
      revive ? FN(0x3a7c240, int, int)(-1) : FN(0x3a7bd40, int, int)(-1);
  if (!(eligible & 1) || !target_allowed(main, ability, target)) {
    result = OT0_HEAL_NO_TARGET;
    goto done;
  }
  hp = FN(0x3a743a0, int, uint32_t)(target);
  max_hp = FN(0x3a742f8, int, uint32_t)(target);
  mailbox->native_eligible = 1;
  // Same AbilityDB name-text ID used by native Healing Skills3c75ae0.
  mailbox->skill_text_id=U32(ability,0xc);
  result = OT0_HEAL_QUERY_OK;
  if(op==OT0_HEAL_APPLY) {
    if(mailbox->expected_save!=save || mailbox->expected_avatar!=avatar ||
       mailbox->expected_party_hash!=hash || mailbox->expected_hp!=hp ||
       mailbox->expected_sp!=sp || mailbox->expected_cost!=cost ||
       mailbox->expected_max_hp!=max_hp ||
       (int32_t)(seq-mailbox->executed_seq)<=0 ||
       __atomic_load_n(&mailbox->seq,__ATOMIC_ACQUIRE)!=seq) {
      result=OT0_HEAL_STALE;goto done;
    }
    // Claim before effects; a retried/duplicate request cannot spend twice.
    mailbox->executed_seq=seq;
    NativeArray targets={&target,1,1};
    FN(0x3c76af0,void,Addr,NativeArray *,Addr,uint32_t)(0,&targets,ability,caster);
    int32_t remaining=FN(0x3a74844,int32_t,uint32_t)(caster)-
                      FN(0x3b08d28,int32_t,uint32_t,uint32_t)(skill,caster);
    FN(0x3a748c4,void,uint32_t,int32_t)(caster,remaining);
    mailbox->result_hp=FN(0x3a743a0,int32_t,uint32_t)(target);
    mailbox->result_sp=FN(0x3a74844,int32_t,uint32_t)(caster);
    result=OT0_HEAL_APPLIED;
  } else {
    mailbox->caster=caster;mailbox->skill=skill;mailbox->target=target;
    mailbox->expected_hp=hp;mailbox->expected_sp=sp;
    mailbox->expected_cost=cost;mailbox->expected_max_hp=max_hp;
    mailbox->expected_save=save;mailbox->expected_avatar=avatar;
    mailbox->expected_party_hash=hash;
  }
done:
  mailbox->cost = cost;
  mailbox->target_hp = hp;
  mailbox->caster_sp = sp;
  mailbox->target_max_hp = max_hp;
  mailbox->result = result;
  mailbox->result_heartbeat=mailbox->heartbeat;
  __atomic_store_n(&mailbox->ack, seq, __ATOMIC_RELEASE);
}

// Field Status values for one party member (ABI3). Party membership and the Leader's job alias
// are resolved through the same native party list as healing; nothing is written to the game.
__attribute__((visibility("hidden"))) void
ot0_field_stats(volatile Ot0StatsMailbox *box, Addr main, uint32_t heartbeat) {
  box->version = OT0_STATS_VERSION;
  uint32_t request = box->request_id;
  if (!request || (box->result_id == request && heartbeat % 15))
    return;
  Addr app = PTR(main, 0x7cc8cc0);
  if (!valid(main, app) || U8(app, 0x2f0) != 23)
    return;
  // Same readiness as healing before any native party call: field world loaded (characters,
  // player avatar and its component valid), no menu open and no event playing. State 23 alone
  // is already reported while a save is still loading.
  Addr menus = PTR(main, 0x7cc9a18), characters = PTR(main, 0x7cc6288);
  if (!valid(main, menus) || U8(menus, 0x38a) != 0 || !valid(main, characters))
    return;
  Addr avatar = PTR(characters, 0x290);
  if (!valid(main, avatar) || !valid(main, PTR(avatar, 0x2c8)) || (FN(0x3942d94, int, void)() & 1))
    return;
  Addr saves = PTR(main, 0x7cc8540);
  if (!valid(main, saves) || !valid(main, PTR(saves, 0x2c8)))
    return;
  NativeArray *party = FN(0x3a70e10, NativeArray *, int, int, int, int, int, int)(-1, 1, 1, 1, 0, 0);
  int count = party ? party->count : 0;
  if (count < 1 || count > 8 || !party->data)
    return;
  uint32_t ids[8];
  for (int i = 0; i < count; ++i)
    ids[i] = party->data[i];
  uint32_t member = party_identity(ids, count, request);
  if (!has(ids, count, member))
    return;
  int32_t values[8], a[7];
  for (int i = 0; i < 7; ++i)
    a[i] = box->args[i];
  for (int i = 0; i < 8; ++i)
    values[i] = a[6] == 2
        ? FN(0x3a738b0, int32_t, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
             int32_t)(member, 9 + i, a[0], a[1], a[2], a[3], a[4], a[5])
        : FN(0x3a7ede0, int32_t, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
             int32_t, uint8_t)(member, 9 + i, a[0], a[1], a[2], a[3], a[4], a[5], (uint8_t)a[6]);
  if (box->exclude_weapon) {
    int32_t weapon = FN(0x3a76c64, int32_t, uint32_t, int32_t)(member, a[0] & 1);
    if (weapon > 0)
      for (int i = 0; i < 8; ++i)
        values[i] -= FN(0x39be1a8, int32_t, uint64_t, int32_t, uint32_t, uint64_t)(9 + i, weapon, member, 0);
  }
  if (box->request_id != request)
    return; // retargeted meanwhile; the next tick answers the new member
  for (int i = 0; i < 8; ++i)
    box->values[i] = values[i];
  box->stamp = heartbeat;
  __atomic_store_n(&box->result_id, request, __ATOMIC_RELEASE);
}
