// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_reader.h"
#include "re4_assets.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "re4_hud_host.h"
#include "re4_slots.h"
#include "re4_map.h"
#include "re4_map_publish.h"
#include "re4_equip.h"
#include <algorithm>
#include <charconv>
#include <string>
namespace {
using namespace re4;
// PER-SAVE SLOT ISOLATION. The generic persisted-flag mechanism persists one value per flag
// NAME per title package (no per-save-file parameter), so true isolation needs a small fixed
// set of pre-declared flags (manifest "slots.assignment.0".."slots.assignment.3") and a
// deterministic save -> bucket hash, rather than one global flag. Bucket 0 is also the
// fallback when no save identity is available yet (preserves pre-isolation persisted data
// instead of orphaning it). See the r5-save-identity research notes
// for why {chapter, save_cnt, terminal_no} was chosen and its confidence (candidate, not an
// independently live-confirmed Switch offset).
constexpr size_t SaveBuckets=4;
inline size_t SaveBucket(const std::optional<std::array<uint32_t,3>>& identity) {
    if(!identity)return 0;
    uint64_t h=1469598103934665603ull; // FNV-1a 64
    for(uint32_t v:*identity)for(int b=0;b<4;++b){h^=(v>>(b*8))&0xFF;h*=1099511628211ull;}
    return static_cast<size_t>(h%SaveBuckets);
}
struct Module : InventorySelection {
    std::string build; HudLease hud; Snapshot last;
    std::optional<int64_t> assignment; std::optional<size_t> last_bucket;
    GeometryCache geometry; std::mutex geometry_jobs; unsigned geometry_stage=1; uint64_t geometry_epoch=0;
    std::atomic<uint64_t> geometry_request{1}; std::atomic<bool> geometry_failed{false};
    std::chrono::steady_clock::time_point geometry_retry{};
    MapReadCache map_reads; RuntimeNames names; assets::Cache asset_cache;
    int64_t font_epoch=0; // Sample/LoadFont share the host publication thread.
    MapLegendPublisher legend; InventoryPublisher inventory;
    RoomStability equip_stability; EquipLease equip; EquipGuard last_equip_guard;
    explicit Module(std::string id):build(std::move(id)){}
};
EdenDsmodBool Supports(const char* id){return SupportsBuild(id);}
void* Create(const EdenDsmodHostApi* h,const char*) {
    if(!dsmod_sdk::HostAbiMatches(h)||h->title_id!=Title||!h->read_memory||!h->is_mapped||
       !h->publish_i64||!h->publish_f64||!h->publish_text)return nullptr;
    constexpr char hex[]="0123456789ABCDEF";
    std::string id;
    for(auto b:h->build_id){id+=hex[b>>4];id+=hex[b&15];}
    if(!SupportsBuild(id.c_str()))return nullptr;
    return new Module{id};
}
void Configure(void* p,const EdenDsmodHostExtensions* e){
    if(!p)return;auto& m=*static_cast<Module*>(p);m.hud.Configure(e);m.equip.Configure(e);
}
EdenDsmodBool LoadImage(void* p,const EdenDsmodHostApi* h,const char* key,void* receiver,EdenDsmodImageSink sink){
    if(!p)return EDEN_DSMOD_FALSE;
    auto& module=*static_cast<Module*>(p);
    if(key&&std::string_view(key).starts_with("module:re4/geometry/")){
        const std::string_view tail=std::string_view(key).substr(20);
        if(tail.size()<3||tail[1]!='/'||(tail[0]!='1'&&tail[0]!='2'&&tail[0]!='3'&&tail[0]!='5'))return false;
        uint64_t epoch=0;const auto parsed=std::from_chars(tail.data()+2,tail.data()+tail.size(),epoch);
        if(parsed.ec!=std::errc{}||parsed.ptr!=tail.data()+tail.size()||epoch>UINT64_MAX/8)return false;
        const unsigned stage=tail[0]-'0';
        std::lock_guard lock(module.geometry_jobs);
        if(module.geometry_request.load(std::memory_order_acquire)==epoch*8+stage &&
           !module.geometry.Warm(h,stage)){module.geometry_failed.store(true,std::memory_order_release);return false;}
        const uint8_t blank[4]={};sink(receiver,1,1,blank,4);return true;
    }
    return assets::LoadCached(module.asset_cache,h,key,receiver,sink);
}
EdenDsmodBool DecodeFont(void* p,const uint8_t* bytes,size_t size,void* receiver,EdenDsmodFontSink sink){
    if(!p)return EDEN_DSMOD_FALSE;
    return static_cast<Module*>(p)->asset_cache.font.Decode(bytes,size,receiver,sink);
}
void Destroy(void* p){delete static_cast<Module*>(p);}
void Sample(void* p,const EdenDsmodHostApi* h){
    if(!p||!h)return;
    auto& module=*static_cast<Module*>(p);
    module.asset_cache.ReleaseIdle();
    const char* page=h->get_text?h->get_text(h->userdata,"__page"):nullptr;
    // The current page still names standby/Case on the sample that enters Field.
    // Native state, not the previous page, decides whether map data is ready.
    const auto s=Resolver(*h,module.build.c_str()).Poll(&module.map_reads);
    const auto i=[&](const char* k,int64_t v){h->publish_i64(h->userdata,k,v);};
    const auto t=[&](const char* k,const char* v){h->publish_text(h->userdata,k,v);};
    const bool font_installed=module.asset_cache.font.installed.load(std::memory_order_acquire);
    // Recover from exhausted host retries when the worker becomes ready late. Do not
    // request a redundant redecode after installation: it would drop live font pages.
    if(!font_installed&&module.asset_cache.font.ready.load(std::memory_order_acquire))module.font_epoch=1;
    i("__font_epoch",module.font_epoch);
    i("re4.font.installed",font_installed);
    module.last=s;
    const std::string_view image_area=s.room?MapArea(*s.room,s.castle_entered):"";
    const unsigned image_stage=image_area.starts_with("village-")?1:image_area.starts_with("castle-")?2:
        image_area.starts_with("island-")?3:image_area.starts_with("stage5-")?5:0;
    if(image_stage)module.asset_cache.stage.store(image_stage,std::memory_order_relaxed);
    if(image_stage&&image_stage!=module.geometry_stage){module.geometry_stage=image_stage;++module.geometry_epoch;}
    const auto geometry_now=std::chrono::steady_clock::now();
    if(module.geometry_failed.load(std::memory_order_acquire)&&geometry_now>=module.geometry_retry){
        module.geometry_failed.store(false,std::memory_order_release);++module.geometry_epoch;
        module.geometry_retry=geometry_now+std::chrono::seconds(1);
    }
    module.geometry_request.store(module.geometry_epoch*8+module.geometry_stage,std::memory_order_release);
    const auto geometry=module.geometry.Get(module.geometry_stage);
    i("re4.geometry.ready",bool(geometry));
    const std::span<const MapMesh> meshes=geometry?std::span<const MapMesh>(geometry->meshes):std::span<const MapMesh>{};
    const std::string geometry_key="module:re4/geometry/"+std::to_string(module.geometry_stage)+"/"+std::to_string(module.geometry_epoch);
    t("re4.geometry.art",geometry_key.c_str());
    const auto bucket=SaveBucket(s.save_identity);
    static const auto flag_keys=BindKeys<SaveBuckets>("@flag:slots.assignment.",std::array{""});
    const auto& flag_key=flag_keys[bucket][0];
    const auto assignment=h->get_i64?h->get_i64(h->userdata,flag_key.c_str(),-1):-1;
    // A bucket change (switched save) must never read as "the player just committed a
    // picker selection" -- that would spuriously close an open picker page.
    const bool bucket_changed=module.last_bucket && *module.last_bucket!=bucket;
    if(bucket_changed)module.ResetForSave();
    i("re4.slots.committed",!bucket_changed && module.assignment && *module.assignment!=assignment && page && std::string_view(page).starts_with("picker_"));
    module.assignment=assignment;module.last_bucket=bucket;
    i("re4.save.bucket",static_cast<int64_t>(bucket));
    t("re4.ui.page",page?page:"");
    if(!module.selected && s.placed_items && !s.placed_items->empty()) {
        module.selected_slot=s.placed_items->front().slot;module.selected=true;
    }
    module.names.StageChanged(s.stage.value_or(0));
    if(s.placed_items || s.owned_items)module.names.Load(h);
    module.inventory.Publish(*h,s,flag_key,module,&module.names);
    i("re4.money.available",s.money.has_value());i("re4.money",s.money.value_or(0));
    i("re4.pause.available",s.paused.has_value());i("re4.paused",s.paused.value_or(false));
    i("re4.position.available",s.position.has_value());
    i("re4.facing.available",s.facing.has_value());
    i("re4.room.available",s.room.has_value());i("re4.room",s.room.value_or(0));
    // Raw room high byte; the separate native castle-entry flag resolves r200.
    i("re4.stage.available",s.stage.has_value());i("re4.stage",s.stage.value_or(0));
    i("re4.ammo.available",s.ammo.has_value());i("re4.ammo",s.ammo.value_or(0));
    i("re4.ammo.reserve.available",s.state!=State::ShootingRange && s.reserve_ammo.has_value());
    i("re4.ammo.reserve",s.reserve_ammo.value_or(0));
    i("re4.weapon.available",s.weapon.has_value());i("re4.weapon",s.weapon.value_or(0));
    i("re4.ashley.presence.available",s.ashley_escort.has_value());
    i("re4.ashley.escort",s.ashley_escort.value_or(false));
    i("re4.ashley.hp.available",s.ashley_health.has_value());
    i("re4.ashley.hp",s.ashley_health?(*s.ashley_health)[0]:0);
    i("re4.ashley.hp.max",s.ashley_health?(*s.ashley_health)[1]:0);
    h->publish_f64(h->userdata,"re4.ashley.hp.ratio",
        s.ashley_health?double((*s.ashley_health)[0])/(*s.ashley_health)[1]:0);
    const bool command=s.ashley_command && (*s.ashley_command==1 || *s.ashley_command==2) &&
        s.paused && !*s.paused && s.health && (*s.health)[0]>0;
    i("re4.ashley_command.available",command);
    // Until the independently reviewed Ashley HUD restore protocol exists,
    // expose the moved order readout only with the explicit re-show preference.
    const bool command_display=command && h->get_i64 &&
        h->get_i64(h->userdata,"@flag:opt.hide_ashley",1)==0;
    i("re4.ashley.command.display_available",command_display);
    i("re4.ashley.command.state",s.ashley_command.value_or(0));
    i("re4.ashley.waiting",command_display && *s.ashley_command==2);
    i("re4.ashley.following",command_display && *s.ashley_command==1);
    i("re4.hp.available",s.health.has_value());
    i("re4.hp",s.health?(*s.health)[0]:0);i("re4.hp.max",s.health?(*s.health)[1]:0);
    h->publish_f64(h->userdata,"re4.hp.ratio",s.health?double((*s.health)[0])/(*s.health)[1]:0);
    i("re4.case.capacity.available",s.case_capacity.has_value());
    i("re4.case.width",s.case_capacity?(*s.case_capacity)[0]:0);
    i("re4.case.height",s.case_capacity?(*s.case_capacity)[1]:0);
    i("re4.inventory.positions.available",s.placed_items.has_value());
    i("re4.inventory.positions.count",s.placed_items?s.placed_items->size():0);
    const bool aux=h->get_i64&&h->get_i64(h->userdata,"__aux_present",0)!=0;
    const bool hide=!h->get_i64||h->get_i64(h->userdata,"@flag:opt.hide_ammo",1)!=0;
    const auto hud=static_cast<Module*>(p)->hud.Poll(aux,s.ammo.has_value(),hide,s.health.has_value(),!h->get_i64 || h->get_i64(h->userdata,"@flag:opt.hide_leon",1)!=0);
    i("re4.ammo.display_available",hud.display);
    i("re4.health_hide.hidden",hud.health_hidden);
    i("re4.hud_hide.available",hud.available);i("re4.hud_hide.hidden",hud.hidden);
    i("re4.hud_hide.heartbeat",hud.heartbeat);i("re4.hud_hide.error",hud.error);
    t("re4.hud_hide.status",hud.available?"HUD helper ready":"HUD hide not available");
    for(int n=0;n<3;++n){const char* keys[]={"re4.x","re4.y","re4.z"};
        h->publish_f64(h->userdata,keys[n],s.position?(*s.position)[n]:0);}
    h->publish_f64(h->userdata,"re4.facing.a",s.facing?(*s.facing)[0]:0);
    h->publish_f64(h->userdata,"re4.facing.b",s.facing?(*s.facing)[1]:0);
    i("re4.state",static_cast<int>(s.state));
    i("re4.progress.castle_entered.available",s.castle_entered.has_value());
    i("re4.progress.castle_entered",s.castle_entered.value_or(false));
    const char* area=s.room?MapArea(*s.room,s.castle_entered):"";
    double map_height=0;
    const bool sw_area=std::string_view(area).starts_with("stage5-");
    const bool field=s.state==State::Unknown || s.state==State::SeparateWays;
    const bool map_sample=field || s.state==State::Loading || s.state==State::Title;
    const auto pose=map_sample && s.position && s.facing && *area?
        MapPose(area,*s.position,*s.facing,sw_area?nullptr:&map_height,meshes):std::nullopt;
    const bool map=field && pose.has_value();
    const std::string_view area_sv=area;
    const bool is_castle=area_sv.starts_with("castle-"), is_island=area_sv.starts_with("island-");
    // Separate Ways (stage5) reuses the main campaign's village/castle/island geography, and
    // the native pause Map plate shows that same native label there too (native plate
    // selector by area, live-verified per map file, analysis/20261109-sw-label-and-zip.md).
    // stage5-* area keys carry no file letter, so Stage5Label() looks the room up directly.
    const bool is_stage5=area_sv.starts_with("stage5-");
    const char stage5_label=is_stage5 && s.room?Stage5Label(*s.room):0;
    const auto native_area=pose?NativeMapArea(s,area_sv):std::nullopt;
    i("re4.map.warm.ready",native_area.has_value() && s.map_flags && s.map_passed);
    // Treasure Map ownership is per stage (village 0xA9, castle 0x54, island 0x55): owning
    // one never implies owning another. Separate Ways uses one item per native area
    // (scf_check_treasure main+0x4C21F0, table +0xA1287C): 18 0x10B, 19 0x10C, 20 0x10D,
    // 21-23 0x10E, 24-25 0x10F, and native draws marks only where mask 0x01BE7A5E selects
    // the area's own model (never 22 or 25).
    std::optional<bool> sw_owned;
    if(is_stage5 && native_area && *native_area>=18 && *native_area<=25 && s.sw_treasure_maps) {
        static constexpr uint8_t bit[8]={0,1,2,3,3,3,4,4};
        sw_owned=((*s.sw_treasure_maps>>bit[*native_area-18])&1)!=0;
    }
    const std::optional<bool> treasure_owned=is_stage5?sw_owned:is_castle?s.castle_treasure_map:
        (is_island || area_sv.starts_with("stage4-"))?s.island_treasure_map:s.village_treasure_map;
    const bool sw_drawn=is_stage5 && native_area && ((0x01BE7A5Eu>>*native_area)&1);
    i("re4.map.available",map);
    i("re4.map.solo",map && !s.ashley_escort.value_or(false));
    i("re4.map.escort",map && s.ashley_escort.value_or(false));
    i("re4.map.village",map && native_area!=0 && (is_stage5?stage5_label=='v':(!is_castle && !is_island)));
    i("re4.map.castle",map && (is_stage5?stage5_label=='c':is_castle));
    i("re4.map.island",map && (is_stage5?stage5_label=='i':is_island));
    i("re4.treasure.map.available",treasure_owned.has_value());
    i("re4.treasure.map.owned",treasure_owned.value_or(false));
    i("re4.treasure.available",map && treasure_owned.has_value());
    const bool spoilers=h->get_i64 && h->get_i64(h->userdata,"@flag:opt.map_spoilers",0)!=0;
    i("re4.treasure.show",map && native_area!=0 && (!is_stage5 || sw_drawn) && (spoilers || treasure_owned.value_or(false)));
    const bool village=area_sv.starts_with("village-") && native_area==1;
    const bool medal_gate=s.medallion_note.value_or(false) || s.merchant_contact.value_or(false);
    i("re4.medallion.gate",medal_gate);
    i("re4.medallion.state.available",s.medallions_unbroken.has_value());
    i("re4.medallion.show",map && village && (spoilers || medal_gate) && s.medallions_unbroken.has_value());
    static const auto medallion_keys=BindKeys<15>("re4.medallion.",std::array{".shot",".show"});
    for(int n=0;n<15;++n) {
        i(medallion_keys[n][0].c_str(),s.medallions_unbroken && !(*s.medallions_unbroken&(1u<<n)));
        i(medallion_keys[n][1].c_str(),map && village && (spoilers || medal_gate) &&
            s.medallions_unbroken && (*s.medallions_unbroken&(1u<<n)));
    }
    // Non-village sub-mission parts have no shot-state predicate in markCoinDisp.
    i("re4.submission.show",map && (is_castle || is_island));
    const double floor_y=map_height/100.;
    const auto native_floor=pose?(sw_area?MapFloor(area_sv,*s.position,meshes):
        std::optional<int>(static_cast<int>(floor_y+(floor_y>0?.5:-.5)))):std::nullopt;
    module.legend.Publish(*h,s,native_area,native_floor);
    t("re4.map.area",map?area:"");
    h->publish_f64(h->userdata,"re4.map.x",map?(*pose)[0]:0);
    h->publish_f64(h->userdata,"re4.map.y",map?(*pose)[1]:0);
    h->publish_f64(h->userdata,"re4.map.heading",map?(*pose)[2]:0);
    // Both viewports follow Leon at the original local scale; pan/zoom can explore
    // the full placed overview without shrinking the default view to the whole stage.
    if(map) {
        const auto camera=MapCamera(area,(*pose)[0],(*pose)[1],s.ashley_escort.value_or(false));
        if(camera) {
            h->publish_f64(h->userdata,"re4.camera.x0",(*camera)[0]);
            h->publish_f64(h->userdata,"re4.camera.x1",(*camera)[2]);
            h->publish_f64(h->userdata,"re4.camera.y0",(*camera)[1]);
            h->publish_f64(h->userdata,"re4.camera.y1",(*camera)[3]);
        }
    }
    t("re4.status",s.state==State::Pause?"Menu open":"State not available");
    t("re4.page",Select(s.state).page);
    // Mercenaries round stats -- investigated live 2026-11-03/04 (analysis/20261103-merc.md,
    // 20261104-merc-cheats.md, 20261104-merc-kills.md). re4.state reaches MercRound (re4_reader.h,
    // zero new readers, reuses the already-shipped room field, now restricted to exactly
    // stage==4 -- see the SeparateWays fix in re4_reader.h) so the page itself activates via the
    // manifest's own page_binds the moment a round is detected. Score/combo/stage-timer/
    // combo-window were NOT found despite an extensive live value-search across many boots
    // (findi/findf over the whole heap, isnap/idiff brackets of pG and of the static anchor
    // cluster main+0xB25D78..0xB28900, a structural probe of a candidate object at
    // main+0xB25F68, and -- 2026-11-04 -- live gunfire/melee across 8+ rounds with the decoded
    // Infinite HP cheat active, which removed the death risk entirely but never landed a
    // confirmed kill to anchor main+0x124f6a8/+0x124f6ac, the two Main-NSO-direct words that DO
    // show combat-correlated activity after firing). Publishing hide-never-dash
    // .available=false for all five rather than publishing a value against an unconfirmed
    // address (this project's standing rule) -- names/types match what the Mercenaries page
    // already expects (analysis/20261102-fix-b.md "Coordination"). All five are now explicitly
    // GATED to a live round (re4.state==27/MercRound) rather than unconditionally false as
    // before, per the task's ".available only in re4.state 27": structurally correct and ready
    // for a future session to flip the `false`/0 operands on once a kill-confirmed offset
    // exists, without having to restructure this gating.
    const bool merc_live=s.state==State::MercRound;
    // Literal keys, not a per-tick concatenation loop: these four names are fixed and the
    // value is always false (no kill-confirmed offset exists yet, see above).
    i("re4.merc.time.available",false);
    i("re4.merc.combo.available",false);
    i("re4.merc.combo_window.available",false);
    i("re4.merc.score.available",false);
    t("re4.merc.time","");
    i("re4.merc.combo",0);
    i("re4.merc.combo_window",0);
    i("re4.merc.score",0);
    // re4.merc.best: companion-side persistence per (stage, character), same split already
    // documented in analysis/20261103-merc.md section 3 as the one `re4.slots.assignment`
    // (PER-SAVE SLOT ISOLATION comment above) already uses -- this module only ever READS the
    // persisted flag via get_i64; a manifest `enforce` action (tools/resident-evil-4/*, out of
    // this file's scope) would be the thing that actually COMMITS a new best to the flag on a
    // round-end transition. re4.merc.best_candidate is published for that future action to
    // compare against and write: today it always equals the persisted value (no live score
    // exists yet to raise it, same honest gap as every merc.* field above), so nothing is
    // fabricated -- only the read-side plumbing (flag key, per-stage/character bucketing,
    // state==27 gating) is new this round.
    const auto merc_best=h->get_i64?h->get_i64(h->userdata,module.inventory.merc_flag_key.c_str(),0):0;
    i("re4.merc.best.available",merc_live);
    i("re4.merc.best",merc_live?merc_best:0);
    i("re4.merc.best_candidate",merc_live?merc_best:0);
    // TAP-TO-EQUIP guard (direct, no native case-open round trip; owner decision 2026-11-01,
    // FLAGS-ONLY, accepted risk -- see re4_equip.h/re4_equip_tick.c). re4.equip.safe drives
    // slot dimming and gates whether Action() below even sends a mailbox request; the guest
    // helper re-checks every bit itself at call time against live memory regardless.
    const auto equip_guard=PollEquipGuard(*h,module.build.c_str(),s,module.equip_stability);
    i("re4.equip.safe",equip_guard.safe);
    i("re4.equip.guard_bits",equip_guard.bits);
    module.last_equip_guard=equip_guard;
    const auto equip_result=module.equip.Poll();
    if(equip_result.done) {
        i("re4.equip.result.ok",equip_result.ok);
        i("re4.equip.result.error",equip_result.error);
    }
}
EdenDsmodBool Action(void* p,const char* action,int64_t argument){
    if(!p || !action)return EDEN_DSMOD_FALSE;
    auto& keys=*static_cast<Module*>(p);
    for(int category=1;category<=2;++category) {
        if(std::string(action)=="key_next_"+std::to_string(category)) {
            ++keys.key_pages[category-1];keys.key_selected=UINT16_MAX;return EDEN_DSMOD_TRUE;
        }
        if(std::string(action)=="key_select_"+std::to_string(category)) {
            const auto items=KeyItems(keys.last,category);const auto at=keys.key_pages[category-1]*8+argument;
            if(argument<0 || argument>=8 || at>=items.size())return EDEN_DSMOD_FALSE;
            keys.key_selected=items[at].slot;return EDEN_DSMOD_TRUE;
        }
    }
    // TAP-TO-EQUIP (direct, owner decision 2026-11-01, FLAGS-ONLY guard): the field page's
    // occupied-quickbar-slot tap target has always been wired to this action name (the stub
    // that used to always refuse). `argument` is the UI quickbar index (0..3, re4.slot.N),
    // not the manager-array slot -- resolved below via the same id->PlacedItem lookup
    // re4_slots.h's PublishSlots already uses, so the two never drift apart.
    if(std::strcmp(action,"equip_unavailable")==0) {
        auto& m=*static_cast<Module*>(p);const auto& s=m.last;
        if(argument<0 || argument>3 || !m.last_equip_guard.safe)return EDEN_DSMOD_FALSE;
        std::vector<PlacedItem> owned;
        if(s.placed_items)for(const auto& v:*s.placed_items)
            if(StripWeapon(v.id) && std::none_of(owned.begin(),owned.end(),
                [&](const auto& it){return it.id==v.id;}))owned.push_back(v);
        const int64_t config=m.assignment.value_or(-1);
        uint32_t slots=0;
        if(config<0)for(size_t n=0;n<std::min<size_t>(4,owned.size());++n)slots=AssignSlot(slots,n,owned[n].id);
        else slots=static_cast<uint32_t>(config);
        const uint8_t id=static_cast<uint8_t>(slots>>(argument*8));
        auto it=std::find_if(owned.begin(),owned.end(),[&](const auto& v){return v.id==id;});
        if(!id || it==owned.end() || !ItemEquipEligible(*it))return EDEN_DSMOD_FALSE;
        const uint32_t host_flags=((m.last_equip_guard.bits&RE4_EQUIP_GUARD_ROOM_STABLE)?1u:0u)|
                                   ((m.last_equip_guard.bits&RE4_EQUIP_GUARD_NOT_GAMEOVER)?2u:0u);
        return m.equip.TryEquip(it->slot,host_flags)?EDEN_DSMOD_TRUE:EDEN_DSMOD_FALSE;
    }
    if(std::strcmp(action,"case_select")!=0)return EDEN_DSMOD_FALSE;
    auto& m=*static_cast<Module*>(p);const auto& s=m.last;
    if(!s.placed_items || !s.case_capacity)return EDEN_DSMOD_FALSE;
    const auto packed=static_cast<uint64_t>(argument);
    const auto point=CaseTapPoint(packed);const int x=point[0],y=point[1];
    const auto grid=CaseGeometry(*s.case_capacity);
    m.selected=true;m.selected_slot=UINT16_MAX;
    for(const auto& item:*s.placed_items) {
        const auto rect=CaseRect(item,*s.case_capacity);if(!rect)continue;
        const int left=grid.x+(*rect)[0]*grid.cell,top=grid.y+(*rect)[1]*grid.cell;
        if(x>=left && x<left+(*rect)[2]*grid.cell && y>=top && y<top+(*rect)[3]*grid.cell){m.selected_slot=item.slot;break;}
    }
    return EDEN_DSMOD_TRUE;
}
void Tick(void*,const EdenDsmodHostApi*){}
const EdenDsmodModuleApi Api{EDEN_DSMOD_MODULE_ABI_VERSION,sizeof(EdenDsmodModuleApi),0,
 EDEN_DSMOD_MODULE_ABI_HASH,Title,"Resident Evil 4",EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN|EDEN_DSMOD_CAP_EXTENSIONS,
 Supports,Create,Destroy,Sample,Tick};
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION,sizeof(EdenDsmodModuleExtensions),
 EDEN_DSMOD_EXT_HASH,Configure,Action,LoadImage};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,sizeof(EdenDsmodFontExtensions),
 EDEN_DSMOD_FONT_EXT_HASH,DecodeFont};
}
DSMOD_SDK_EXPORT_MODULE(Api)
DSMOD_SDK_EXPORT_EXTENSIONS(Extensions)

DSMOD_SDK_EXPORT_FONT_EXTENSIONS(FontExtensions)
