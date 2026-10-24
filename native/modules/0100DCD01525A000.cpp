// SPDX-License-Identifier: GPL-3.0-or-later
// Experimental read-only Persona 3 Portable companion. No guest input/writes.
#include "p3p_reader.h"
#include "p5r_romfs_assets.h"
#include "core/mods/dsmod_module_extensions.h"
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>

namespace {
using namespace p3p;
constexpr char Build[] = "663052C2C7E8C357BA6EAD36641EE2792397EB1F000000000000000000000000";
constexpr u64 Title = UINT64_C(0x0100DCD01525A000);
std::vector<std::string> Strings(std::span<const u8> b, size_t width) {
    if (!width || b.size() % width) throw std::runtime_error("invalid names table");
    std::vector<std::string> out;
    for (size_t at = 0; at < b.size(); at += width) {
        size_t n = 0; while (n < width && b[at+n]) ++n;
        if (n == width) throw std::runtime_error("unterminated name");
        std::string name(reinterpret_cast<const char*>(b.data()+at), n);
        // Initial release uses the English archive and built-in Latin font.
        for (unsigned char c : name) if (c < 32 || c > 126) throw std::runtime_error("unsupported name encoding");
        out.push_back(std::move(name));
    }
    return out;
}
struct Module {
    EdenDsmodHostApi host;
    Reader reader;
    unsigned ticks{};
    bool names_ready{};
    // Companion page (manifest ui_open: 1 status, 2 personas, 3 links, 4 skills, 5 equip,
    // 6 stats, 7 items) and the member selected on the per-member pages, read only while open.
    int page{1}, member{}, item_tab{}, persona_pick{}; // item_tab: native Use / Equip / Key
    std::vector<std::string> personas, links;
    explicit Module(const EdenDsmodHostApi& h) : host(h), reader(h) {}
    void I(const std::string& key, int64_t v) { host.publish_i64(host.userdata,key.c_str(),v); }
    void T(const std::string& key, const std::string& v) { host.publish_text(host.userdata,key.c_str(),v.c_str()); }
    static std::span<const u8> Find(std::span<const u8> b, const char* name) {
        std::vector<p5r_assets::PakEntry> entries;
        if (!p5r_assets::ParsePak(b,entries)) throw std::runtime_error("startup package unreadable");
        auto result = p5r_assets::PakFind(entries,name);
        if (result.empty()) throw std::runtime_error("startup member missing");
        return result;
    }
    void Names() {
        p5r_assets::Cpk cpk;
        if (!cpk.Open(p5r_assets::HostReader(host), "romfs:data_EN/umd0.cpk")) return;
        auto read = [&](const char* path) {
            p5r_assets::Cpk::Entry entry; Bytes b;
            if (!cpk.Find(path,entry) || entry.extract > 96*1024*1024 || !cpk.ReadEntry(entry,b))
                throw std::runtime_error("localized startup archive unreadable");
            return b;
        };
        auto startup = read("init_free.bin");
        auto msg = Find(startup,"battle/MSG.TBL");
        constexpr size_t sizes[]{672,11856,6384,4352};
        size_t at=0;
        for (int i=0;i<4;++i) {
            if (at+4>msg.size()) throw std::runtime_error("truncated MSG table");
            u32 n; std::memcpy(&n,msg.data()+at,4);
            if (n!=sizes[i] || n>msg.size()-at-4) throw std::runtime_error("unsupported MSG table");
            if (i==3) personas=Strings(msg.subspan(at+4,n),17);
            at=(at+4+n+15)&~size_t(15);
        }
        auto social = read("init_2F.bin");
        auto cmm=Find(social,"init/cmmF.bin");
        auto decode = [&](const char* name) {
            auto table=Find(cmm,name);
            if (table.size()!=2080) throw std::runtime_error("unsupported social table");
            u32 size,count; std::memcpy(&size,table.data(),4);std::memcpy(&count,table.data()+4,4);
            if(size!=2048||count!=32) throw std::runtime_error("unsupported social names");
            return Strings(table.subspan(8,2048),64);
        };
        links=decode("cmmNameF.dat");
        names_ready=personas.size()==256 && links.size()==32;
    }
    std::string MemberName(const Snapshot&, const Member& m) const {
        if(!m.name.empty())return m.name;
        return m.id==1 ? "Protagonist" : "Member "+std::to_string(m.id);
    }
    std::string PersonaName(int id) const {
        return names_ready&&id>=0&&size_t(id)<personas.size()?personas[id]:"Persona "+std::to_string(id);
    }
    bool Action(std::string_view action, int64_t arg) {
        if(action=="ui_open"&&arg>=1&&arg<=7){page=int(arg);return true;}
        if(action=="member_select"&&arg>=0&&arg<10){member=int(arg);return true;}
        if(action=="item_tab"&&arg>=0&&arg<3){item_tab=int(arg);return true;}
        if(action=="persona_select"&&arg>=0&&arg<12){persona_pick=int(arg);return true;}
        return false;
    }
    void Clear() {
        I("live.ready",0); T("live.date",""); T("live.money","");
        I("ui.page",page); I("ui.member",member);
        for (int i=0;i<10;++i) {
            auto p="member."+std::to_string(i);I(p+".present",0);I(p+".selected",0);T(p+".name","");T(p+".tab","");T(p+".stats","");
            for(auto k:{"hp","sp","max_hp","max_sp"}) I(p+"."+k,0);
        }
        I("skill.ready",0);I("skill.count",0);T("skill.header","");
        I("equip.ready",0);T("equip.header","");
        I("stat.ready",0);I("social.ready",0);T("stat.header","");T("stat.next","");
        I("item.ready",0);I("item.count",0);T("item.header","");I("item.tab",item_tab);
        I("pd.ready",0);I("ui.persona",persona_pick);
        for(int i=0;i<12;++i){auto p="persona."+std::to_string(i);I(p+".present",0);T(p+".line","");}
        for(int i=0;i<28;++i){auto p="link."+std::to_string(i);I(p+".present",0);T(p+".line","");}
    }
    void Sample() {
        ++ticks; // The host clears published values every sample; always republish.
        Clear();
        Snapshot v; std::string why;
        if(!reader.Sample(v,why)){T("status",why);return;}
        if(!names_ready && ticks%60==1) { try {Names();} catch(...) {} }
        int day=v.day%365, month=4;
        constexpr int months[]{4,5,6,7,8,9,10,11,12,1,2,3};
        constexpr int days[]{30,31,30,31,31,30,31,30,31,31,28,31};
        for(int i=0;i<12;++i){month=months[i];if(day<days[i])break;day-=days[i];}
        std::string time=v.time==5 ? (v.dark?"Dark Hour":"Evening") : "Time "+std::to_string(v.time);
        T("live.date",std::to_string(month)+" / "+std::to_string(day+1)+"   "+time);
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
        for(size_t i=0;i<v.links.size();++i) {
            auto m=v.links[i];auto p="link."+std::to_string(i);
            std::string name=names_ready&&v.female&&m.id>=0&&size_t(m.id)<links.size()?links[m.id]:"Social Link "+std::to_string(m.id);
            T(p+".line",name+"   Rank "+std::to_string(m.rank));I(p+".present",1);
        }
        T("status",names_ready?"Read-only companion":"Read-only companion - names unavailable");I("live.ready",1);
        if(page==2&&!v.personas.empty()&&!v.members.empty()) {
            // PERSONA -> Details for the picked stock Persona; fails closed on its own.
            try {
                auto d=reader.Details(v,v.members.front(),v.personas.at(std::min<size_t>(persona_pick,v.personas.size()-1)).slot);
                T("pd.name",(d.name.empty()?PersonaName(d.id):d.name)+"   Lv "+std::to_string(d.level));
                T("pd.arcana",d.arcana);
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
                    T(p+".name",s.name);I(p+".present",1);
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
const EdenDsmodModuleApi Api{EDEN_DSMOD_MODULE_ABI_VERSION,sizeof(EdenDsmodModuleApi),0,EDEN_DSMOD_MODULE_ABI_HASH,Title,"Persona 3 Portable (read-only preview)",EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN|EDEN_DSMOD_CAP_EXTENSIONS,&Supports,&Create,&Destroy,&Sample,&Tick};
const EdenDsmodModuleExtensions Extensions{EDEN_DSMOD_EXT_VERSION,sizeof(EdenDsmodModuleExtensions),EDEN_DSMOD_EXT_HASH,&Configure,&OnAction,nullptr};
}
extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t version,uint64_t hash){return version==EDEN_DSMOD_MODULE_ABI_VERSION&&hash==EDEN_DSMOD_MODULE_ABI_HASH?&Api:nullptr;}
extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleExtensions* eden_dsmod_get_extensions(uint32_t version,uint64_t hash){return version==EDEN_DSMOD_EXT_VERSION&&hash==EDEN_DSMOD_EXT_HASH?&Extensions:nullptr;}
