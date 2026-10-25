// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental read-only Persona 3 Portable companion. No guest input/writes.
#include "p3p_reader.h"
#include "p3p_font.h"
#include "p3p_art.h"
#include "core/mods/dsmod_module_extensions.h"
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>

namespace {
using namespace p3p;
constexpr char Build[] = "663052C2C7E8C357BA6EAD36641EE2792397EB1F000000000000000000000000";
constexpr u64 Title = UINT64_C(0x0100DCD01525A000);
// Camp skill-list icon (c_main_01.spr) for a SKILL.TBL element byte, as the native Skill menu
// draws it: 124-130 Slash..Wind, 131 Almighty, 132 Light, 133 Dark, 134 recovery. Categories
// whose icon is not confirmed against the native menu get none (0).
int SkillIcon(int element) {
    if(element>=0&&element<=6)return 124+element;
    switch(element){case 7:return 131;case 8:return 132;case 9:return 133;case 17:return 134;default:return 0;}
}
struct Module {
    EdenDsmodHostApi host;
    const EdenDsmodHostApi font_host; // the create-time copy decode_font reads romfs through
    p3p_art::Library art;             // camp-menu art for load_image (p3p_art.h)
    Reader reader;
    unsigned ticks{};
    // Companion page (manifest ui_open: 1 status, 2 personas, 3 links, 4 skills, 5 equip,
    // 6 stats, 7 items, 8 calendar) and per-page selections, read only while open.
    int page{1}, member{}, item_tab{}, persona_pick{}; // item_tab: native Use / Equip / Key
    int cal_offset{}; // calendar month shown, relative to the current month
    // Page 9 (battle) is entered automatically while a battle runs; Status is restored after.
    explicit Module(const EdenDsmodHostApi& h) : host(h), font_host(h), reader(h) {}
    void I(const std::string& key, int64_t v) { host.publish_i64(host.userdata,key.c_str(),v); }
    void T(const std::string& key, const std::string& v) { host.publish_text(host.userdata,key.c_str(),v.c_str()); }
    std::string MemberName(const Snapshot&, const Member& m) const {
        if(!m.name.empty())return m.name;
        return m.id==1 ? "Protagonist" : "Member "+std::to_string(m.id);
    }
    // Names come from the game's loaded tables, so they follow the language and protagonist.
    std::string PersonaName(int id) const {
        std::string name;
        try { name=reader.PersonaName(id); } catch(...) {}
        return name.empty() ? "Persona "+std::to_string(id) : name;
    }
    bool Action(std::string_view action, int64_t arg) {
        if(action=="ui_open"&&arg>=1&&arg<=8){page=int(arg);if(page==8)cal_offset=0;return true;}
        if(action=="member_select"&&arg>=0&&arg<10){member=int(arg);return true;}
        if(action=="item_tab"&&arg>=0&&arg<3){item_tab=int(arg);return true;}
        if(action=="persona_select"&&arg>=0&&arg<12){persona_pick=int(arg);return true;}
        if(action=="calendar_shift"&&(arg==0||arg==1)){cal_offset+=arg?1:-1;return true;}
        return false;
    }
    void Clear() {
        I("live.ready",0); T("live.date",""); T("live.money",""); I("ui.route",0); I("pd.id",0);
        I("live.lead",0); I("live.moon_spr",0); I("cal.month_spr",0);
        I("ui.page",page); I("ui.member",member);
        for (int i=0;i<10;++i) {
            auto p="member."+std::to_string(i);I(p+".present",0);I(p+".selected",0);T(p+".name","");T(p+".tab","");T(p+".stats","");
            for(auto k:{"hp","sp","max_hp","max_sp"}) I(p+"."+k,0);
        }
        I("skill.ready",0);I("skill.count",0);T("skill.header","");
        I("equip.ready",0);T("equip.header","");
        I("stat.ready",0);I("social.ready",0);T("stat.header","");T("stat.next","");
        I("item.ready",0);I("item.count",0);T("item.header","");I("item.tab",item_tab);
        I("pd.ready",0);I("ui.persona",persona_pick);I("cal.ready",0);
        I("battle.active",0);I("en.count",0);
        for(int i=0;i<12;++i){auto p="persona."+std::to_string(i);I(p+".present",0);T(p+".line","");}
        I("link.ready",0);I("link.count",0);
    }
    void Sample() {
        ++ticks; // The host clears published values every sample; always republish.
        Clear();
        Snapshot v; std::string why;
        if(!reader.Sample(v,why)){T("status",why);return;}
        // Follow battles: show the battle page while one runs, return to Status afterwards.
        if(v.battle) page=9; else if(page==9) page=1;
        I("ui.page",page);I("battle.active",v.battle);
        I("ui.route",v.female); // camp menu theme: 0 male lead (blue), 1 female lead (pink)
        // Camp art ids for the manifest's module:p3p: keys (p3p_art.h): the lead's camp portrait
        // (c_cha01a male, c_cha99a female) and tonight's moon (sprites 541.. = phases 0..28, 555 full).
        I("live.lead",v.female?99:1);
        try {
            int phase=MoonPhase(reader.MoonTable(),v.day);
            if(phase>=0&&phase<29)I("live.moon_spr",541+phase);
        } catch(...) {}
        auto [month,date]=MonthDay(v.day);
        const char* time=TimeLabel(v.time,v.dark,v.holiday);
        T("live.date",std::to_string(month)+" / "+std::to_string(date)+"   "+(time?time:"Time "+std::to_string(v.time)));
        T("live.money",std::to_string(v.money)+" yen");
        for(size_t i=0;i<v.members.size();++i) {
            const auto& m=v.members[i];auto p="member."+std::to_string(i);
            std::string name=MemberName(v,m);
            T(p+".name",name+"   Lv "+std::to_string(m.level));T(p+".tab",name);I(p+".selected",int(i)==member);
            T(p+".stats","HP "+std::to_string(m.hp)+" / "+std::to_string(m.max_hp)+"     SP "+std::to_string(m.sp)+" / "+std::to_string(m.max_sp));
            I(p+".hp",m.hp);I(p+".sp",m.sp);I(p+".max_hp",m.max_hp);I(p+".max_sp",m.max_sp);I(p+".present",1);
        }
        for(size_t i=0;i<v.personas.size();++i) {
            auto m=v.personas[i];auto p="persona."+std::to_string(i);I(p+".picked",int(i)==persona_pick);
            T(p+".line",(m.selected?"* ":"  ")+PersonaName(m.id)+"   Lv "+std::to_string(m.level));I(p+".present",1);
        }
        T("status","Read-only companion");I("live.ready",1);
        if(page==3) {
            // Native S.LINK list (fails closed on its own).
            try {
                auto links=reader.Links();
                static constexpr const char* status[]{"","MAX","Reversed","Broken"};
                for(size_t i=0;i<links.size();++i){
                    const auto& l=links[i];auto p="link."+std::to_string(i);
                    T(p+".arcana",l.arcana_name);T(p+".name",l.name.empty()?"Social Link "+std::to_string(l.id):l.name);
                    T(p+".partner",l.partner);I(p+".rank",l.rank);T(p+".status",status[l.status]);
                    T(p+".rank_text","Rank "+std::to_string(l.rank));I(p+".present",1);
                }
                I("link.count",int64_t(links.size()));I("link.ready",1);
            } catch(const std::exception& e) {T("status",std::string("Social Links unavailable: ")+e.what());}
        }
        if(page==2&&!v.personas.empty()&&!v.members.empty()) {
            // PERSONA -> Details for the picked stock Persona; fails closed on its own.
            try {
                auto d=reader.Details(v,v.members.front(),v.personas.at(std::min<size_t>(persona_pick,v.personas.size()-1)).slot);
                T("pd.name",(d.name.empty()?PersonaName(d.id):d.name)+"   Lv "+std::to_string(d.level));
                T("pd.arcana",d.arcana);I("pd.id",d.id);
                static constexpr const char* stat[5]{"St","Ma","En","Ag","Lu"};
                for(int s=0;s<5;++s){I("pd.stat."+std::to_string(s),d.stats[s]);(void)stat;}
                for(int e=0;e<9;++e)T("pd.aff."+std::to_string(e),d.affinity[e]);
                T("pd.next","NEXT EXP  "+std::to_string(d.next_exp));
                for(int k=0;k<8;++k)T("pd.skill."+std::to_string(k),d.skills[k]);
                T("pd.card",d.card_level>=0?"Skill card  Lv "+std::to_string(d.card_level)+"   "+d.card:"");
                T("pd.nextskill","Next skill  --");
                I("pd.ready",1);
            } catch(const std::exception& e) {T("status",std::string("Persona details unavailable: ")+e.what());}
        }
        if(page==9) {
            // Encounter enemies (party values are the member.* outputs above).
            try {
                auto enemies=reader.Enemies();
                for(size_t i=0;i<enemies.size();++i){
                    const auto& e=enemies[i];auto p="en."+std::to_string(i);
                    T(p+".name",e.name.empty()?"Enemy "+std::to_string(e.id):e.name);
                    T(p+".info","Lv "+std::to_string(e.level)+"   "+e.arcana);
                    T(p+".hp_text","HP "+std::to_string(e.hp)+" / "+std::to_string(e.max_hp));
                    I(p+".hp",e.hp);I(p+".max_hp",e.max_hp);I(p+".analyzed",e.analyzed);
                    for(int k=0;k<9;++k)T(p+".aff."+std::to_string(k),e.affinity[k]);
                    I(p+".present",1);
                }
                I("en.count",int64_t(enemies.size()));
            } catch(const std::exception& e) {T("status",std::string("Enemies unavailable: ")+e.what());}
        }
        if(page==8) {
            // Month grid (Sunday first) with today, holidays (0x1d2e30) and full moons (phase 14).
            try {
                const auto& moon=reader.MoonTable();
                auto [month,date]=MonthDay(v.day);
                int index=(month+8)%12;                       // April = 0
                index=std::clamp(index+cal_offset,0,11); cal_offset=index-(month+8)%12;
                static constexpr const char* names[]{"April","May","June","July","August","September","October","November","December","January","February","March"};
                static constexpr int lengths[]{30,31,30,31,31,30,31,30,31,31,28,31};
                int start=MonthStart(index)+(v.day/365)*365, lead=Weekday(start);
                T("cal.title",names[index]);
                I("cal.month_spr",584+(index+3)%12); // camp month lettering, 584 = January
                for(int c=0;c<42;++c){
                    auto p="cal."+std::to_string(c);int d=c-lead;
                    if(d<0||d>=lengths[index])continue;
                    int day=start+d;
                    T(p+".text",std::to_string(d+1));I(p+".present",1);
                    I(p+".today",day==v.day);I(p+".holiday",Holiday(day,reader.Holidays()));
                    I(p+".full",MoonPhase(moon,day)==14);
                }
                int next=DaysToFullMoon(moon,v.day);
                auto [nm,nd]=MonthDay(v.day+next);
                T("cal.next",next?"Next full moon in "+std::to_string(next)+(next==1?" day  (":" days  (")+std::to_string(nm)+" / "+std::to_string(nd)+")":"Full moon tonight");
                I("cal.ready",1);
            } catch(const std::exception& e) {T("status",std::string("Calendar unavailable: ")+e.what());}
        }
        if(page==4&&!v.members.empty()) {
            // A failed skill read hides only the skill list (fail closed per domain).
            try {
                const auto& m=v.members.at(std::min<size_t>(member,v.members.size()-1));
                auto skills=reader.Skills(v,m);
                // The protagonist's native list spans the whole Persona stock.
                T("skill.header",MemberName(v,m)+"   "+(m.id==1?"All Personas":PersonaName(m.persona)));
                // Rows the module does not publish are absent (the host clears every sample).
                for(size_t i=0;i<skills.size();++i){
                    const auto& s=skills[i];auto p="skill."+std::to_string(i);
                    T(p+".name",s.name);I(p+".present",1);I(p+".icon",SkillIcon(s.element));
                    if(s.type==1||s.type==2)T(p+".cost",(s.cost_known?std::to_string(s.cost):"?")+(s.type==1?" HP":" SP"));
                }
                I("skill.count",int64_t(skills.size()));I("skill.ready",1);
            } catch(const std::exception& e) {T("status",std::string("Skills unavailable: ")+e.what());}
        }
        if(page==5&&!v.members.empty()) {
            try {
                const auto& m=v.members.at(std::min<size_t>(member,v.members.size()-1));
                auto equip=reader.Equipment(m);
                T("equip.header",MemberName(v,m)+"   Lv "+std::to_string(m.level));
                auto n=[](int x){return std::to_string(x);};
                // The native Status panel's per-slot figures (cmpStatus.c 0x10bb80).
                const std::string stats[4]{"At "+n(equip[0].at)+"     Hit "+n(equip[0].hit),"Df "+n(equip[1].df),"Ev "+n(equip[2].ev),""};
                for(int i=0;i<4;++i){auto p="equip."+std::to_string(i);T(p+".name",equip[i].name);T(p+".stats",stats[i]);}
                I("equip.ready",1);
            } catch(const std::exception& e) {T("status",std::string("Equipment unavailable: ")+e.what());}
        }
        if(page==6&&!v.members.empty()) {
            try {
                const auto& m=v.members.at(std::min<size_t>(member,v.members.size()-1));
                auto st=reader.MemberStats(v,m);
                T("stat.header",MemberName(v,m)+"   Lv "+std::to_string(m.level)+"     "+PersonaName(m.persona));
                for(int i=0;i<5;++i){
                    auto p="stat."+std::to_string(i);I(p+".value",st.stats[i]);
                    T(p+".bonus",st.bonus[i]?"+"+std::to_string(st.bonus[i]):"");
                }
                T("stat.next",st.next_exp>=0?"NEXT EXP  "+std::to_string(st.next_exp):"NEXT EXP  --");
                for(size_t i=0;i<st.social.size();++i){
                    auto p="social."+std::to_string(i);I(p+".rank",st.social[i].rank);T(p+".name",st.social[i].name);
                }
                I("social.ready",st.social.empty()?0:1);I("stat.ready",1);
            } catch(const std::exception& e) {T("status",std::string("Stats unavailable: ")+e.what());}
        }
        if(page==7) {
            try {
                auto items=reader.Items(item_tab);
                constexpr size_t rows=256; // manifest rows; the native list allows 0x500
                size_t shown=std::min(items.size(),rows);
                for(size_t i=0;i<shown;++i){
                    auto p="item."+std::to_string(i);
                    T(p+".name",items[i].name);T(p+".count","x "+std::to_string(items[i].count));I(p+".present",1);
                }
                T("item.header",std::to_string(items.size())+(items.size()==1?" item":" items")+(items.size()>rows?"  (first "+std::to_string(rows)+" shown)":""));
                I("item.count",int64_t(shown));I("item.ready",1);
            } catch(const std::exception& e) {T("status",std::string("Items unavailable: ")+e.what());}
        }
    }
};
EdenDsmodBool Supports(const char* s){return s&&std::strcmp(s,Build)==0?EDEN_DSMOD_TRUE:EDEN_DSMOD_FALSE;}
void* Create(const EdenDsmodHostApi* h,const char*) {
    if(!h||h->title_id!=Title||!h->read_memory||!h->is_mapped||!h->publish_i64||!h->publish_text)return nullptr;
    constexpr u8 expected[]{0x66,0x30,0x52,0xc2,0xc7,0xe8,0xc3,0x57,0xba,0x6e,0xad,0x36,0x64,0x1e,0xe2,0x79,0x23,0x97,0xeb,0x1f,0,0,0,0,0,0,0,0,0,0,0,0};
    if(std::memcmp(h->build_id,expected,32))return nullptr;
    try{return new Module(*h);}catch(...){return nullptr;}
}
void Destroy(void* p){delete static_cast<Module*>(p);}
void Sample(void* p,const EdenDsmodHostApi* h){if(p&&h)try{auto m=static_cast<Module*>(p);m->host=*h;m->reader.UpdateHost(*h);m->Sample();}catch(...){auto m=static_cast<Module*>(p);m->Clear();m->T("status","Reader unavailable");}}
void Tick(void*,const EdenDsmodHostApi*){}
void Configure(void*,const EdenDsmodHostExtensions*){}
// Serialized with sample; page/member selection only, never guest input or writes.
EdenDsmodBool OnAction(void* p,const char* action,int64_t arg){try{return p&&action&&static_cast<Module*>(p)->Action(action,arg)?EDEN_DSMOD_TRUE:EDEN_DSMOD_FALSE;}catch(...){return EDEN_DSMOD_FALSE;}}
// The game's font (p3p_font.h): "module:p3p_font" is the atlas, and the package's `font` file
// (magic "P3PFONT") stands for the metrics of the same romfs file. "module:p3p:..." keys are the
// camp-menu art (p3p_art.h).
EdenDsmodBool LoadImage(void* p,const EdenDsmodHostApi* h,const char* key,void* receiver,EdenDsmodImageSink sink) {
    try {
        if(!h||!key||!sink||!h->read_romfs)return EDEN_DSMOD_FALSE;
        const std::string_view k{key};
        if(p&&k.starts_with("module:p3p:")) {
            p3p_art::Image image;
            if(!static_cast<Module*>(p)->art.Load(*h,k,image))return EDEN_DSMOD_FALSE;
            sink(receiver,image.w,image.h,image.rgba.data(),image.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if(k!="module:p3p_font")return EDEN_DSMOD_FALSE;
        p5r_font::Atlas atlas;
        if(!p3p_font::BuildAtlas(*h,atlas))return EDEN_DSMOD_FALSE;
        sink(receiver,atlas.width,atlas.height,atlas.rgba.data(),atlas.rgba.size());
        return EDEN_DSMOD_TRUE;
    } catch(...) {return EDEN_DSMOD_FALSE;}
}
EdenDsmodBool DecodeFont(void* p,const uint8_t* bytes,size_t size,void* receiver,EdenDsmodFontSink sink) {
    try {
        p5r_font::Atlas atlas;
        if(!p||!sink||!bytes||size<7||std::memcmp(bytes,"P3PFONT",7)||!static_cast<Module*>(p)->font_host.read_romfs||
           !p3p_font::BuildAtlas(static_cast<Module*>(p)->font_host,atlas))return EDEN_DSMOD_FALSE;
        std::vector<EdenDsmodFontGlyph> glyphs(atlas.glyphs.size());
        for(size_t i=0;i<glyphs.size();++i){const auto& g=atlas.glyphs[i];glyphs[i]={g.x,g.y,g.w,g.h,g.bearing_x,g.bearing_y,g.advance};}
        sink(receiver,atlas.line_height,p5r_font::FirstCodepoint,glyphs.data(),uint32_t(glyphs.size()));
        return EDEN_DSMOD_TRUE;
    } catch(...) {return EDEN_DSMOD_FALSE;}
}
const EdenDsmodModuleApi Api{EDEN_DSMOD_MODULE_ABI_VERSION,sizeof(EdenDsmodModuleApi),0,EDEN_DSMOD_MODULE_ABI_HASH,Title,"Persona 3 Portable (read-only preview)",EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN|EDEN_DSMOD_CAP_EXTENSIONS|EDEN_DSMOD_CAP_ROMFS_READ,&Supports,&Create,&Destroy,&Sample,&Tick};
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION,sizeof(EdenDsmodModuleExtensions),EDEN_DSMOD_EXT_HASH,&Configure,&OnAction,&LoadImage};
const EdenDsmodFontExtensions FontExtensions{EDEN_DSMOD_FONT_EXT_VERSION,sizeof(EdenDsmodFontExtensions),EDEN_DSMOD_FONT_EXT_HASH,&DecodeFont};
}
extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t version,uint64_t hash){return version==EDEN_DSMOD_MODULE_ABI_VERSION&&hash==EDEN_DSMOD_MODULE_ABI_HASH?&Api:nullptr;}
extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t version,uint64_t hash){return version==EDEN_DSMOD_EXT_VERSION&&hash==EDEN_DSMOD_EXT_HASH?&Extensions:nullptr;}
extern "C" __attribute__((visibility("default"))) const EdenDsmodFontExtensions* eden_dsmod_get_font_extensions(uint32_t version,uint64_t hash){return version==EDEN_DSMOD_FONT_EXT_VERSION&&hash==EDEN_DSMOD_FONT_EXT_HASH?&FontExtensions:nullptr;}
