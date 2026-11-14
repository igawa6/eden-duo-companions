// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "re4_reader.h"
#include "re4_case_art.h"
#include "re4_case_layout.h"
#include "re4_names.h"
#include "re4_keys_metadata.h"
#include <algorithm>
#include <string>
#include <tuple>
namespace re4 {
struct InventorySelection {
    std::array<size_t,2> key_pages{};
    uint16_t key_selected=UINT16_MAX,selected_slot=UINT16_MAX;
    bool selected=false;
    void ResetForSave() {
        key_pages={};key_selected=selected_slot=UINT16_MAX;
        // The new save starts with no selection, including on later steady ticks.
        selected=true;
    }
};
// Fixed row keys are built once, rather than concatenated for every sample.
template<size_t Rows,size_t Fields>
inline auto BindKeys(const char* prefix,const std::array<const char*,Fields>& suffixes) {
    std::array<std::array<std::string,Fields>,Rows> keys;
    for(size_t row=0;row<Rows;++row)for(size_t field=0;field<Fields;++field)
        keys[row][field]=std::string(prefix)+std::to_string(row)+suffixes[field];
    return keys;
}
inline uint32_t AssignSlot(uint32_t slots,size_t index,uint8_t id) {
    return (slots & ~(uint32_t{255}<<(index*8))) | (uint32_t{id}<<(index*8));
}
inline uint32_t AssignUnique(uint32_t slots,size_t index,uint8_t id) {
    if(id)for(size_t n=0;n<4;++n)if(n!=index && static_cast<uint8_t>(slots>>(n*8))==id)
        slots=AssignSlot(slots,n,0);
    return AssignSlot(slots,index,id);
}
inline int64_t CommitSlots(uint32_t slots,int64_t previous,bool reset_empty=false) {
    if(reset_empty && !slots)return -1;
    // One cosmetic transaction bit makes selecting the current item close the picker too.
    return static_cast<int64_t>(uint64_t{slots}|((((static_cast<uint64_t>(previous)>>32)^1)&1)<<32));
}
inline uint32_t SwapSlots(uint32_t slots,size_t a,size_t b) {
    const auto av=static_cast<uint8_t>(slots>>(a*8));
    const auto bv=static_cast<uint8_t>(slots>>(b*8));
    return AssignSlot(AssignSlot(slots,a,bv),b,av);
}
inline std::string ItemArt(uint16_t id,const char* prefix="weapon-") {
    constexpr char hex[]="0123456789abcdef";
    if(id>255)return {};
    std::string s="module:re4/";
    const std::string_view family(prefix);
    s+=family=="case-art-"?"case/":family=="slot-art-"||family=="slot-missing-"?"slot/":family=="key-art-"?"icon/":"weapon/";
    s+=hex[id>>4];s+=hex[id&15];
    if(family=="case-art-")s+="/120";
    if(family=="slot-missing-")s+="/missing";
    return s;
}
inline const char* WeaponName(uint16_t id,const RuntimeNames* names=nullptr) {
    return names?names->Get(id):"";
}
inline std::string SlotAmmo(const PlacedItem& item) {
    switch(item.id) {
    case 1:case 2:case 14:return std::to_string(item.count);
    default: {
        const auto rounds=LoadedRounds(item);
        return rounds?std::to_string(*rounds):std::string{};
    }
    }
}
inline std::optional<std::array<int,4>> CaseRect(const PlacedItem& item,const std::array<uint8_t,2>& capacity) {
    auto size=CaseSize(item.id);if(item.orientation)std::swap(size[0],size[1]);
    const int w=size[0],h=size[1];
    const int x2=item.centre_x2+1-w,y2=item.centre_y2+1-h;
    if(!w || !h || x2<0 || y2<0 || (x2&1) || (y2&1) ||
       x2/2+w>capacity[0] || y2/2+h>capacity[1])return {};
    return std::array<int,4>{x2/2,y2/2,w,h};
}
inline void PublishCaseSelection(const EdenDsmodHostApi& h,const Snapshot& s,uint16_t slot,const RuntimeNames* names=nullptr) {
    const PlacedItem* selected=nullptr;
    if(s.placed_items)for(const auto& item:*s.placed_items)if(item.slot==slot)selected=&item;
    const auto rect=selected && s.case_capacity?CaseRect(*selected,*s.case_capacity):std::nullopt;
    if(!rect)selected=nullptr;
    const auto id=selected?selected->id:UINT16_MAX;
    const auto* text=WeaponName(id,names);
    h.publish_text(h.userdata,"re4.case.name",text);
    h.publish_text(h.userdata,"re4.case.category","");
    // Two native herb names contain the multiplication glyph, outside the ASCII font adapter.
    const char* name_art=*text && id==0x12?"module:re4/case/name/012":*text && id==0x13?"module:re4/case/name/013":"";
    h.publish_text(h.userdata,"re4.case.name.art",name_art);
    h.publish_i64(h.userdata,"re4.case.name.image.available",*name_art!=0);
    h.publish_i64(h.userdata,"re4.case.name.text.available",*name_art==0);
    auto size=CaseSize(id);
    if(selected && selected->orientation)std::swap(size[0],size[1]);
    const bool visible=selected && s.case_capacity && size[0];
    const auto grid=s.case_capacity?CaseGeometry(*s.case_capacity):CaseLayout{0,0,0};
    h.publish_i64(h.userdata,"re4.case.selection.shape",visible?size[0]*16+size[1]:0);
    h.publish_i64(h.userdata,"re4.case.selection.x",visible?grid.x+(selected->centre_x2+1-static_cast<int>(size[0]))*grid.cell/2:0);
    h.publish_i64(h.userdata,"re4.case.selection.y",visible?grid.y+(selected->centre_y2+1-static_cast<int>(size[1]))*grid.cell/2:0);
    const auto loaded=selected?LoadedRounds(*selected):std::nullopt;
    const bool ammo=loaded.has_value();
    h.publish_i64(h.userdata,"re4.case.ammo.available",ammo);
    h.publish_i64(h.userdata,"re4.case.badge.x",visible?grid.x+(selected->centre_x2+1+static_cast<int>(size[0]))*grid.cell/2-108:0);
    h.publish_i64(h.userdata,"re4.case.badge.y",visible?grid.y+(selected->centre_y2+1+static_cast<int>(size[1]))*grid.cell/2-46:0);
    const auto rounds=ammo?std::to_string(*loaded):"";
    h.publish_text(h.userdata,"re4.case.ammo",rounds.c_str());
}
inline std::vector<OwnedItem> KeyItems(const Snapshot& s,int category) {
    std::vector<OwnedItem> result;
    if(s.owned_items)for(const auto& item:*s.owned_items)
        if(KeyCategory(item.id)==category)result.push_back(item);
    return result;
}
inline void PublishKeys(const EdenDsmodHostApi& h,const Snapshot& s,std::array<size_t,2>& pages,uint16_t selected,const RuntimeNames* names=nullptr) {
    static const auto warm_keys=BindKeys<384>("re4.keys.warm.",std::array{".art"});
    size_t warm_count=0;
    if(s.owned_items)for(const auto& item:*s.owned_items)
        if(item.count && KeyCategory(item.id) && warm_count<warm_keys.size()) {
            const auto art=ItemArt(item.id,"key-art-");
            h.publish_text(h.userdata,warm_keys[warm_count++][0].c_str(),art.c_str());
        }
    h.publish_i64(h.userdata,"re4.keys.warm.count",warm_count);
    static const auto more_keys=BindKeys<3>("re4.keys.",std::array{".more"});
    static const auto item_keys=[] {
        std::array<decltype(BindKeys<8>("",std::array{".available",".art",".count"})),2> keys;
        for(size_t c=0;c<2;++c)keys[c]=BindKeys<8>(("re4.keys."+std::to_string(c+1)+".").c_str(),std::array{".available",".art",".count"});
        return keys;
    }();
    for(int category=1;category<=2;++category) {
        const auto items=KeyItems(s,category);auto& page=pages[category-1];
        if(page*8>=items.size())page=0;
        h.publish_i64(h.userdata,more_keys[category][0].c_str(),items.size()>8);
        for(size_t n=0;n<8;++n) {
            const auto& key=item_keys[category-1][n];const auto at=page*8+n;
            const bool present=at<items.size();
            h.publish_i64(h.userdata,key[0].c_str(),present);
            const auto art=present?ItemArt(items[at].id,"key-art-"):"";
            const auto count=present && items[at].count>1?std::to_string(items[at].count):"";
            h.publish_text(h.userdata,key[1].c_str(),art.c_str());
            h.publish_text(h.userdata,key[2].c_str(),count.c_str());
        }
    }
    // Names verified in the native menu. Undecoded names stay blank.
    const char* name="";
    if(s.owned_items)for(const auto& item:*s.owned_items)if(item.slot==selected) {
        if(item.id==0xa2 || item.id==0xb9)name=WeaponName(item.id,names);
    }
    h.publish_text(h.userdata,"re4.keys.name",name);
}
// flag_key names the per-save-bucket persisted flag to read (e.g. "@flag:slots.assignment.2",
// see PER-SAVE SLOT ISOLATION in 010099A00BC1E000.cpp); callers that have no live save
// identity yet pass "@flag:slots.assignment.0" (bucket 0 doubles as the pre-isolation
// default, keeping existing persisted data meaningful instead of orphaning it).
inline void PublishSlots(const EdenDsmodHostApi& h,const Snapshot& s,const std::string& flag_key,const RuntimeNames* names=nullptr) {
    const auto i=[&](std::string_view k,int64_t v){h.publish_i64(h.userdata,k.data(),v);};
    const auto t=[&](std::string_view k,const std::string& v){h.publish_text(h.userdata,k.data(),v.c_str());};
    static const auto slot_keys=BindKeys<4>("re4.slot.",std::array{".present",".not_present",".missing",".equipped",".art",".missing_art",".ammo",".ammo.available",".badge",".digit.0",".digit.0.x",".digit.1",".digit.1.x",".digit.2",".digit.2.x",".digit.3",".digit.3.x",".digit.4",".digit.4.x"});
    static const auto picker_keys=BindKeys<30>("re4.picker.",std::array{".available",".art",".name",".ammo",".ammo.available",".selected.0",".selected.1",".selected.2",".selected.3"});
    static const auto assignment_keys=BindKeys<4>("re4.assign.",std::array{".clear",".0",".1",".2",".3",".4",".5",".6",".7",".8",".9",".10",".11",".12",".13",".14",".15",".16",".17",".18",".19",".20",".21",".22",".23",".24",".25",".26",".27",".28",".29"});
    static const auto swap_keys=BindKeys<4>("re4.swap.",std::array{".0",".1",".2",".3"});
    std::vector<PlacedItem> owned;
    if(s.placed_items)for(const auto& v:*s.placed_items)if(StripWeapon(v.id) && std::none_of(owned.begin(),owned.end(),[&](const auto& item){return item.id==v.id;}))owned.push_back(v);
    uint32_t slots=0;
    const auto config=h.get_i64?h.get_i64(h.userdata,flag_key.c_str(),-1):-1;
    if(config<0)for(size_t n=0;n<std::min<size_t>(4,owned.size());++n)slots=AssignSlot(slots,n,owned[n].id);
    else slots=static_cast<uint32_t>(config);
    i("re4.slots.ready",s.placed_items.has_value());
    for(size_t n=0;n<4;++n) {
        const auto& k=slot_keys[n];const uint8_t id=slots>>(n*8);
        auto it=std::find_if(owned.begin(),owned.end(),[&](const auto& v){return v.id==id;});
        const bool present=id && it!=owned.end();
        i(k[0],present);i(k[1],!present);i(k[2],id && !present && s.placed_items.has_value());
        i(k[3],present && s.weapon && id==*s.weapon);
        t(k[4],id?ItemArt(id,"slot-art-"):"");
        t(k[5],id?ItemArt(id,"slot-missing-"):"");
        const auto ammo=present?SlotAmmo(*it):"";t(k[6],ammo);i(k[7],!ammo.empty());
        t(k[8],ammo.empty()?"":"file:assets/r5-badge-"+std::to_string(ammo.size())+".png");
        for(size_t d=0;d<5;++d) {
            t(k[9+d*2],d<ammo.size()?"module:re4/digit/"+ammo.substr(d,1):"");
            i(k[10+d*2],123-12-static_cast<int>(ammo.size()-std::min(d,ammo.size()))*19);
        }
        i(assignment_keys[n][0],CommitSlots(AssignSlot(slots,n,0),config,true));
        for(size_t source=0;source<4;++source)
            i(swap_keys[n][source],CommitSlots(SwapSlots(slots,n,source),config));
    }
    // A closed bound: the supported equippable set has 30 distinct types. Clear all stale rows.
    for(size_t k=0;k<30;++k) {
        const auto& key=picker_keys[k];const bool present=k<owned.size();
        i(key[0],present);t(key[1],present?ItemArt(owned[k].id):"");
        t(key[2],present?WeaponName(owned[k].id,names):"");
        const auto rounds=present?SlotAmmo(owned[k]):"";
        t(key[3],rounds);i(key[4],!rounds.empty());
        for(size_t n=0;n<4;++n)i(key[5+n],present && static_cast<uint8_t>(slots>>(n*8))==owned[k].id);
        for(size_t n=0;n<4;++n)i(assignment_keys[n][k+1],
            present?CommitSlots(AssignUnique(slots,n,owned[k].id),config):config);
    }
    i("re4.picker.height",std::max<size_t>(1,owned.size())*120);
    const auto case_grid=s.case_capacity?CaseGeometry(*s.case_capacity):CaseLayout{0,0,0};
    h.publish_f64(h.userdata,"re4.case.scale",case_grid.cell*1000.0/120);
    std::array<unsigned,144> shape_counts{};
    for(size_t n=0;n<384;++n) {
        const bool present=s.placed_items && s.case_capacity && n<s.placed_items->size() && CaseRect((*s.placed_items)[n],*s.case_capacity).has_value();
        // Missing availability is false in the per-tick-cleared host snapshot.
        if(!present)continue;
        const auto& item=(*s.placed_items)[n];
        // One repeat per native shape, bounded by its actual item count. The total
        // expanded widget count stays equal to inventory size, not shapes * 384.
        const auto size=CaseSize(item.id);
        const auto shape=std::to_string(size[0])+"x"+std::to_string(size[1]);
        const auto prefix="re4.case.shape."+shape+"."+std::to_string(shape_counts[size[0]*9+size[1]]++);
        t(prefix+".art",ItemArt(item.id,"case-art-"));
        i(prefix+".x",case_grid.x+(item.centre_x2+1)*case_grid.cell/2-540);
        i(prefix+".y",case_grid.y+(item.centre_y2+1)*case_grid.cell/2-360);
        i(prefix+".rotation",item.orientation?90:0);
    }
    for(unsigned w=1;w<=15;++w)for(unsigned hgt=1;hgt<=8;++hgt)
        if(shape_counts[w*9+hgt])i("re4.case.shape."+std::to_string(w)+"x"+std::to_string(hgt)+".count",shape_counts[w*9+hgt]);
    const auto hp=s.health?std::clamp<int>((14*(*s.health)[0]+(*s.health)[1]-1)/(*s.health)[1],0,14):0;
    i("re4.hp.level",hp);
    t("re4.hp.art","module:re4/ui/hp-"+std::to_string(hp));
    t("re4.case.art",s.case_capacity?"module:re4/case/background/"+std::to_string((*s.case_capacity)[0]):"");
    const auto command=s.ashley_command.value_or(0);
    // The single word names the next native command, not the current order.
    t("re4.ashley.bubble",command==1?"module:re4/ui/wait":command==2?"module:re4/ui/follow":"module:re4/ui/both");
}
// Cache computed bindings, but replay them every tick: the host clears its snapshot.
class CachedBindings {
    struct Integer {std::string key;int64_t value;};
    struct Float {std::string key;double value;};
    struct Text {std::string key,value;};
    std::vector<Integer> integers;
    std::vector<Float> floats;
    std::vector<Text> texts;
public:
    size_t string_builds=0,rebuilds=0;
    template<class Build> void Rebuild(const EdenDsmodHostApi& host,Build build) {
        integers.clear();floats.clear();texts.clear();++rebuilds;
        struct Capture {CachedBindings& cache;const EdenDsmodHostApi& host;} capture{*this,host};
        EdenDsmodHostApi h{};h.userdata=&capture;
        h.get_i64=[](void* p,const char* k,int64_t fallback) {
            const auto& h=static_cast<Capture*>(p)->host;
            return h.get_i64?h.get_i64(h.userdata,k,fallback):fallback;
        };
        h.publish_i64=[](void* p,const char* k,int64_t v) {
            auto& c=static_cast<Capture*>(p)->cache;c.integers.push_back({k,v});++c.string_builds;
        };
        h.publish_f64=[](void* p,const char* k,double v) {
            auto& c=static_cast<Capture*>(p)->cache;c.floats.push_back({k,v});++c.string_builds;
        };
        h.publish_text=[](void* p,const char* k,const char* v) {
            auto& c=static_cast<Capture*>(p)->cache;c.texts.push_back({k,v});c.string_builds+=2;
        };
        build(h);
    }
    void Publish(const EdenDsmodHostApi& h) const {
        for(const auto& row:integers)h.publish_i64(h.userdata,row.key.c_str(),row.value);
        for(const auto& row:floats)h.publish_f64(h.userdata,row.key.c_str(),row.value);
        for(const auto& row:texts)h.publish_text(h.userdata,row.key.c_str(),row.value.c_str());
    }
};
class InventoryPublisher {
    Snapshot last;
    bool ready=false,names_ready=false;
    const RuntimeNames* last_names=nullptr;
    int64_t assignment=-1;
    std::array<size_t,2> key_pages{};
    uint16_t key_selected=UINT16_MAX,case_selected=UINT16_MAX;
    CachedBindings slots,keys,selection,sample;
    static auto SlotInputs(const Snapshot& s) {
        return std::tie(s.placed_items,s.case_capacity,s.weapon,s.health,s.ashley_command);
    }
    static auto TextInputs(const Snapshot& s) {
        return std::tie(s.save_identity,s.health,s.case_capacity,s.placed_items,s.ammo,
                        s.reserve_ammo,s.stage,s.character);
    }
public:
    std::string merc_flag_key;
    size_t StringBuilds() const {
        return slots.string_builds+keys.string_builds+selection.string_builds+sample.string_builds;
    }
    void Publish(const EdenDsmodHostApi& h,const Snapshot& s,const std::string& flag_key,
                 InventorySelection& selected,const RuntimeNames* names=nullptr) {
        const auto config=h.get_i64?h.get_i64(h.userdata,flag_key.c_str(),-1):-1;
        const bool names_changed=last_names!=names || names_ready!=(names && names->Ready());
        // Exact inventory comparisons are a lossless fingerprint, including availability,
        // order, count, magazine, capacity level, slot identity and placement.
        const bool slot_dirty=!ready || names_changed || assignment!=config || SlotInputs(s)!=SlotInputs(last);
        const bool key_dirty=!ready || names_changed || s.owned_items!=last.owned_items ||
            key_pages!=selected.key_pages || key_selected!=selected.key_selected;
        const bool case_dirty=!ready || names_changed || s.placed_items!=last.placed_items ||
            s.case_capacity!=last.case_capacity || case_selected!=selected.selected_slot;
        const bool text_dirty=!ready || TextInputs(s)!=TextInputs(last);
        if(slot_dirty)slots.Rebuild(h,[&](const auto& out){PublishSlots(out,s,flag_key,names);});
        if(key_dirty)keys.Rebuild(h,[&](const auto& out){PublishKeys(out,s,selected.key_pages,selected.key_selected,names);});
        if(case_dirty)selection.Rebuild(h,[&](const auto& out){PublishCaseSelection(out,s,selected.selected_slot,names);});
        if(text_dirty)sample.Rebuild(h,[&](const auto& out) {
            const auto t=[&](const char* k,const std::string& v){out.publish_text(out.userdata,k,v.c_str());};
            t("re4.save.fingerprint",s.save_identity?std::to_string((*s.save_identity)[0])+":"+
                std::to_string((*s.save_identity)[1])+":"+std::to_string((*s.save_identity)[2]):"");
            t("re4.hp.text",s.health?std::to_string((*s.health)[0])+" / "+std::to_string((*s.health)[1]):"");
            t("re4.case.capacity.text",s.case_capacity?std::to_string((*s.case_capacity)[0])+" x "+
                std::to_string((*s.case_capacity)[1])+" slots":"Capacity not available");
            std::string positions;
            if(s.placed_items)for(const auto& item:*s.placed_items)
                positions+=std::to_string(item.slot)+":"+std::to_string(item.id)+":"+
                    std::to_string(item.count)+":"+std::to_string(item.centre_x2)+":"+
                    std::to_string(item.centre_y2)+":"+std::to_string(item.orientation)+";";
            t("re4.inventory.positions",positions);
            t("re4.ammo.text",s.ammo?std::to_string(*s.ammo):"");
            t("re4.ammo.reserve.text",s.reserve_ammo?std::to_string(*s.reserve_ammo):"");
            merc_flag_key="@flag:merc.best."+std::to_string(s.stage.value_or(0))+"."+
                std::to_string(s.character.value_or(0));
        });
        if(slot_dirty || key_dirty || case_dirty || text_dirty)last=s;
        assignment=config;key_pages=selected.key_pages;key_selected=selected.key_selected;
        case_selected=selected.selected_slot;last_names=names;names_ready=names && names->Ready();ready=true;
        slots.Publish(h);keys.Publish(h);selection.Publish(h);sample.Publish(h);
    }
};

}
