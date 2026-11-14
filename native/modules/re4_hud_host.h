// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/mods/dsmod_module_extensions.h"
#include "../guest/re4_hud_abi.h"
#include <cstddef>
namespace re4 {
struct HudStatus {bool available=false,display=false,hidden=false,health_hidden=false;uint32_t heartbeat=0,error=0;};
class HudLease {
    EdenDsmodHostExtensions e{};
    uint32_t seq=0,last_heartbeat=0,last_epoch=0;
    uint32_t request_epoch=0,request_mask=0;
    uint64_t samples=0,changed_at=0;
    bool configured=false,advanced=false,has_request=false;
    bool Load(uint32_t o,uint32_t& v)const{return configured&&e.load_u32(e.userdata,o,&v);}
    bool Store(uint32_t o,uint32_t v)const{return configured&&e.store_u32(e.userdata,o,v);}
    bool Request(uint32_t epoch,uint32_t deadline,uint32_t mask) {
        // Keep a stable acknowledgment token while only extending the same
        // lease. Sampling can run more often than the native HUD callback.
        if(has_request && epoch==request_epoch && mask==request_mask) {
            if(Store(16,deadline))return true;
            has_request=false;return false;
        }
        const uint32_t next=(seq+2)?seq+2:2;
        if(!Store(8,next|1)||!Store(12,epoch)||!Store(16,deadline)||!Store(20,mask)||!Store(8,next)){
            has_request=false;return false;
        }
        seq=next;request_epoch=epoch;request_mask=mask;has_request=true;return true;
    }
public:
    ~HudLease(){Stop();}
    void Stop(){if(configured){uint32_t epoch=0,hb=0;Load(4,epoch);Load(0,hb);Request(epoch,hb+1,0);}advanced=false;}
    void Configure(const EdenDsmodHostExtensions* ext) {
        Stop();e={};configured=false;has_request=false;seq=last_heartbeat=last_epoch=0;samples=changed_at=0;
        if(!ext || ext->version!=EDEN_DSMOD_EXT_VERSION || ext->abi_hash!=EDEN_DSMOD_EXT_HASH ||
           ext->struct_size<sizeof(EdenDsmodHostExtensions) || !ext->mailbox_address ||
           ext->mailbox_size<sizeof(Re4HudMailbox) || !ext->load_u32 || !ext->store_u32)return;
        e=*ext;configured=true;Load(8,seq);seq&=~1u;
    }
    HudStatus Poll(bool aux,bool ammo,bool hide,bool health=false,bool hide_health=true) {
        HudStatus s;++samples;
        uint32_t hb=0,epoch=0,ack=0,mask=0,error=0;
        if(!Load(0,hb)||!Load(4,epoch)||!Load(24,ack)||!Load(28,mask)||!Load(32,error))return s;
        if(epoch!=last_epoch){last_epoch=epoch;advanced=false;last_heartbeat=hb;changed_at=samples;}
        if(hb!=last_heartbeat){advanced=true;last_heartbeat=hb;changed_at=samples;}
        s.available=advanced && samples-changed_at<=120 && !error;
        s.heartbeat=hb;s.error=error;
        s.hidden=s.available && ack==seq && (mask&1)!=0 && !error;
        s.health_hidden=s.available && ack==seq && (mask&2)!=0 && !error;
        s.display=aux && ammo && (!hide || s.hidden);
        if(!Request(epoch,hb+90,s.available && aux?((ammo&&hide?1u:0u)|(health&&hide_health?2u:0u)):0u))s={};
        return s;
    }
};
static_assert(offsetof(Re4HudMailbox,ack)==24 && sizeof(Re4HudMailbox)==72);
}
