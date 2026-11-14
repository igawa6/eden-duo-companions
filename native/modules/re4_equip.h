// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "re4_reader.h"
#include "../guest/re4_equip_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include <cstddef>
#include <cstdint>
#include <optional>
// Host-side mirror of re4_equip_tick.c's guard (the equip-guards research notes,
// ANSWERS.md). Drives the UI (dim slots exactly when the native case could not open right
// now) and decides whether a mailbox request is even worth sending; the guest helper
// re-checks every bit itself at call time against live memory, per the owner's instruction --
// this host copy is never trusted as the final word, only as a pre-filter.
//
// FLAGS-ONLY guard (owner decision 2026-11-01, accepted risk): g.safe mirrors
// RE4_EQUIP_GUARD_ALL_KNOWN exactly, with no ROUTINE_CALM requirement -- see
// re4_equip_abi.h's own comment on RE4_EQUIP_GUARD_ALL_KNOWN for the risk this accepts
// (a reload or knife-swing tap is not detected by any bit here).
namespace re4 {
struct EquipGuard { uint32_t bits=0; bool safe=false; };
// Require the sampled room id to settle after a crossing as well as the fresh native
// GameTask guard below. Room id alone changes before room-init has finished.
class RoomStability {
    std::optional<uint16_t> last_room; uint32_t stable_samples=0;
public:
    bool Update(std::optional<uint16_t> room) {
        if(!room){last_room.reset();stable_samples=0;return false;}
        if(last_room!=room){last_room=room;stable_samples=0;return false;}
        if(stable_samples<60)++stable_samples;
        return stable_samples>=30;
    }
};
inline EquipGuard PollEquipGuard(const EdenDsmodHostApi& h,const char* build_id,
                                  const Snapshot& s,RoomStability& stability) {
    EquipGuard g;
    Resolver resolver(h,build_id);
    const auto pG=resolver.Global();
    if(!pG)return g;
    // Re-read at action time; a prior Snapshot can predate the door press. Unreadable or
    // non-main-loop routines fail closed, including all three loading phases (4/1/2).
    if(auto v=resolver.Read<uint8_t>(*pG+0x28))if(*v==3)g.bits|=RE4_EQUIP_GUARD_GAME_MAINLOOP;
    // ---- Q0: SubScreenCall's own permission predicate, re-implemented as pure reads ----
    if(s.health && (*s.health)[0]>0)g.bits|=RE4_EQUIP_GUARD_LEON_ALIVE;
    // SubScreenCall also blocks on "Ashley carried + dead" (pSUB->id==3 && ashley_life<=0);
    // no live Ashley-HP reader exists, so this uses Leon's own already-driven game-over
    // state as a conservative stand-in, never a substitute for the real check.
    // Also refused inside a shooting-range session: r22c itemSave/gameEnd swap the whole
    // inventory for the range weapon, so a quick-slot tap must never equip there.
    if(s.state!=State::GameOver && s.state!=State::ShootingRange)g.bits|=RE4_EQUIP_GUARD_NOT_GAMEOVER;
    if(auto v=resolver.Read<uint32_t>(*pG+0x5090))if(*v&0x02000000u)g.bits|=RE4_EQUIP_GUARD_SCREEN_ENABLED;
    // RE4_EQUIP_GUARD_ROUTINE_CALM (subScrCheck's r_no_0/1/2 read) is deliberately never set
    // here: Leon's r_no_0/1/2 Switch byte offset from pPL is not located (ANSWERS.md Q1,
    // same gap flagged across 11+ prior dedicated sessions). It is excluded from
    // RE4_EQUIP_GUARD_ALL_KNOWN, so this has no effect on g.safe -- see the FLAGS-ONLY note
    // above and re4_equip_tick.c's matching RE4_EQUIP_ROUTINE_OFFSET gate, the single place
    // both sides would need to change if that gap ever closes.
    // ---- Equip-specific extras beyond Q0 (ANSWERS.md "Conclusion for the owner's heuristic") ----
    // main+0x4AF630's own early-exit tests this byte against exactly 1, not merely "nonzero"
    // (ANSWERS.md's raw-byte observation) -- match that exact native check.
    if(auto v=resolver.Read<uint8_t>(*pG+0x503c))if(*v!=1)g.bits|=RE4_EQUIP_GUARD_BUSY_CLEAR;
    if(resolver.Single(0xB26C98))g.bits|=RE4_EQUIP_GUARD_SUBSCREEN_LIVE;
    // ---- Q2: event / cutscene / suspend / scripted-freeze ----
    if(auto v=resolver.Read<uint32_t>(*pG+0x5098)) {
        if((*v&0x00090000u)==0)g.bits|=RE4_EQUIP_GUARD_NO_EVENT;
        // STA_SSCRN_REQUEST: case/menu already mid-open -- refuse a second tap-to-equip.
        if((*v&0x04000000u)==0)g.bits|=RE4_EQUIP_GUARD_NOT_REQUESTED;
    }
    if(auto v=resolver.Read<uint32_t>(*pG+0x5094))if((*v&0x10000000u)==0)g.bits|=RE4_EQUIP_GUARD_NO_SUSPEND;
    if(auto v=resolver.Read<uint32_t>(*pG+0x1a8))if((*v&0x80000000u)==0)g.bits|=RE4_EQUIP_GUARD_NO_SCRIPT_FREEZE;
    if(auto v=resolver.Read<uint32_t>(*pG+0x509c))if((*v&0x00002000u)==0)g.bits|=RE4_EQUIP_GUARD_NOT_LOADING_STA;
    // ---- Q3: loading (both candidates unverified against a real door load; see above) ----
    if(auto v=resolver.Read<uint32_t>(h.main_base+0x1242E10))if(*v==0)g.bits|=RE4_EQUIP_GUARD_NOT_LOADING_UI;
    if(stability.Update(s.room))g.bits|=RE4_EQUIP_GUARD_ROOM_STABLE;
    g.safe=(g.bits&RE4_EQUIP_GUARD_ALL_KNOWN)==RE4_EQUIP_GUARD_ALL_KNOWN;
    return g;
}
// Item eligibility mirrors arm()'s own internal checks (ANSWERS.md: manager+0x1b!=1 i.e. not
// viewing Ashley's items, item count!=0, category in {1,3,6}). Both are already structurally
// guaranteed for anything this package's UI ever offers to tap: re4_slots.h only ever
// populates a quick-slot from StripWeapon()-eligible ids (the practical category gate), and
// RE4 has no Ashley weapon-slot UI at all (Ashley never carries a case). This function exists
// so a future per-item dimming pass has a single named place to call rather than
// re-deriving the rule -- it is not required for the native arm() call's own safety, which is
// the authoritative, final check (error=4 on its own refusal) regardless of this result.
inline bool ItemEquipEligible(const PlacedItem& item) {
    return StripWeapon(item.id) && item.count>0;
}
// Host-side driver for the round trip: Configure() once (mirrors HudLease), TryEquip(slot,
// host_flags) to send a request (slot = PlacedItem.slot, the manager-array index -- NOT the
// UI quickbar position), Poll() once per sample to collect the guest's ack/ok/error. Uses the
// SAME EdenDsmodHostExtensions mailbox as the HUD helper, offset by
// RE4_EQUIP_MAILBOX_OFFSET -- the two mailboxes never overlap and never block each other.
class EquipLease {
    EdenDsmodHostExtensions e{};
    bool configured=false, waiting=false;
    uint32_t seq=0;
    bool Load(uint32_t o,uint32_t& v)const{return configured&&e.load_u32(e.userdata,RE4_EQUIP_MAILBOX_OFFSET+o,&v);}
    bool Store(uint32_t o,uint32_t v)const{return configured&&e.store_u32(e.userdata,RE4_EQUIP_MAILBOX_OFFSET+o,v);}
public:
    struct Result { bool done=false,ok=false; uint32_t error=0,guard_bits=0; };
    void Configure(const EdenDsmodHostExtensions* ext) {
        e={};configured=false;waiting=false;seq=0;
        if(!ext || ext->version!=EDEN_DSMOD_EXT_VERSION || ext->abi_hash!=EDEN_DSMOD_EXT_HASH ||
           ext->struct_size<sizeof(EdenDsmodHostExtensions) || !ext->mailbox_address ||
           ext->mailbox_size<RE4_EQUIP_MAILBOX_OFFSET+sizeof(Re4EquipMailbox) ||
           !ext->load_u32 || !ext->store_u32)return;
        e=*ext;configured=true;
        uint32_t s=0;Load(8,s);seq=s&~1u;
    }
    bool Present() const { return configured; }
    bool Busy() const { return waiting; }
    // Returns true once the request is accepted for send -- per the extensions ABI contract,
    // this is NOT proof arm()/refresh actually ran; that outcome arrives later via Poll().
    bool TryEquip(uint32_t slot,uint32_t host_flags) {
        if(!configured || waiting)return false;
        uint32_t epoch=0,hb=0;
        if(!Load(4,epoch) || !Load(0,hb))return false;
        const uint32_t next=(seq+2)?seq+2:2;
        const bool ok=Store(8,next|1) && Store(12,epoch) && Store(16,hb+90) &&
                      Store(20,slot) && Store(24,host_flags) && Store(8,next);
        if(!ok)return false;
        seq=next;waiting=true;issued_heartbeat=hb;return true;
    }
    // Once per sample. `done` is true exactly the sample a result (or a stall timeout) lands.
    Result Poll() {
        Result r;
        if(!configured || !waiting)return r;
        uint32_t hb=0;
        if(Load(0,hb) && (int32_t)(hb-issued_heartbeat)>180) {
            // The hook never served this request within 3x the normal 90-tick lease window
            // (should not happen outside a genuine pause/unload) -- stop waiting on it so a
            // later TryEquip() is not blocked forever.
            waiting=false;r.done=true;r.ok=false;r.error=1;return r;
        }
        uint32_t ack=0;
        if(!Load(28,ack) || ack!=seq)return r;
        waiting=false;
        uint32_t ok=0,err=0,gb=0;Load(32,ok);Load(36,err);Load(40,gb);
        r.done=true;r.ok=ok!=0;r.error=err;r.guard_bits=gb;return r;
    }
private:
    uint32_t issued_heartbeat=0;
};
// EquipLease's Load/Store offsets above are hand-written against Re4EquipMailbox's field
// order; catch any future reordering at compile time instead of a silent ABI mismatch, the
// same guard re4_hud_host.h already keeps for Re4HudMailbox.
static_assert(offsetof(Re4EquipMailbox,ack)==28 && offsetof(Re4EquipMailbox,ok)==32 &&
              offsetof(Re4EquipMailbox,error)==36 && offsetof(Re4EquipMailbox,guard_bits)==40 &&
              sizeof(Re4EquipMailbox)==44);
}
