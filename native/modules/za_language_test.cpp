// SPDX-FileCopyrightText: Copyright 2026 Sickops
// SPDX-License-Identifier: GPL-3.0-or-later
// Real-archive locale check. Never changes a game or save.
#include "za_assets.h"
#include "za_save.h"
#include "za_ui_catalog.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <set>

int main(int argc,char**argv){
    if(argc<2)return 77;
    std::string root=argv[1];za::Assets assets;
    assets.SetReader(&root,+[](void*p,const char*path,std::uint64_t offset,void*out,std::size_t size)->std::size_t{
        std::ifstream f(*static_cast<std::string*>(p)+"/"+path,std::ios::binary|std::ios::ate);if(!f)return 0;
        const auto length=std::size_t(f.tellg());if(!out)return length;if(offset>=length)return 0;
        size=std::min(size,length-std::size_t(offset));f.seekg(offset);f.read(static_cast<char*>(out),size);return std::size_t(f.gcount());
    });
    const char* expected[]={"","フシギダネ","Bulbasaur","Bulbizarre","Bulbasaur","Bisasam","","Bulbasaur","이상해씨","妙蛙种子","妙蛙種子","Bulbasaur"};
    const char* poison_type[]={"","どく","Poison","Poison","Veleno","Gift","","Veneno","독","毒","毒","Veneno"};
    std::vector<std::string> phrases;if(argc>2){std::ifstream f(argv[2]);for(std::string s;std::getline(f,s);)if(!s.empty())phrases.push_back(s);}
    std::set<std::string> ui;for(const auto& spec:za::ui::specs){if(*spec.source)ui.insert(spec.source);for(const auto name:spec.names)if(*name)ui.insert(name);}
    int missing=0;
    std::ofstream corpus;if(argc>3)corpus.open(argv[3],std::ios::binary);
    for(int lang=1;lang<=11;++lang){if(lang==6)continue;assets.SetLanguage(lang);
        if(!assets.EnsureNames()){std::printf("FAIL locale %d: no names\n",lang);return 1;}
        const auto levels=assets.ResearchLevels();
        const auto ranks=assets.TrainerRanks();
        if(ranks.size()!=27||ranks[0].name!="Z"||ranks[25].name!="A"||ranks[26].name!="∞"||ranks[1].points!=1000||ranks[26].points!=260000)return 11;
        if(levels.size()!=50||levels[0].points!=0||levels[1].points!=100||levels[40].points!=68400||levels[41].points!=70900||levels[49].points!=90900||
           levels[40].item!=365||levels[40].count!=1||assets.ItemName(levels[40].item).empty())return 8;
        za::Picture missions;
        if(!assets.MenuIcon(4,missions)||!missions.Valid())return 9;
        std::size_t transparent=0,covered=0;
        for(std::size_t p=3;p<missions.rgba.size();p+=4){transparent+=missions.rgba[p]==0;covered+=missions.rgba[p]>128;}
        if(transparent<missions.width*missions.height/4||covered<missions.width*missions.height/10)return 10;
        const auto lucario=assets.Learnset(448,0);
        if(std::none_of(lucario.begin(),lucario.end(),[](const auto& row){return row.id==370&&row.level<=81;}))return 5;
        const auto machines=assets.MachineMoves(448,0);
        if(std::find(machines.begin(),machines.end(),14)==machines.end())return 6;
        const auto close_combat=assets.MoveParameters(370),aura_sphere=assets.MoveParameters(396);
        if(close_combat.category!=1||close_combat.power!=120||close_combat.accuracy!=100||
           aura_sphere.category!=2||aura_sphere.power!=80||assets.MoveDescription(370).empty())return 7;
        const std::string name=assets.SpeciesName(1);
        std::printf("locale %d: %s | type %s | nature %s\n",lang,name.c_str(),assets.TypeName(0).c_str(),assets.NatureName(0).c_str());
        if(name!=expected[lang]||assets.MoveName(1).empty()||assets.ItemName(1).empty()||assets.TypeName(0).empty()||assets.NatureName(0).empty())return 2;
        if(assets.TypeName(3)!=poison_type[lang]){
            std::printf("FAIL locale %d: unexpected Poison type name %s\n",lang,assets.TypeName(3).c_str());
            return 12;
        }
        const auto antidote=assets.ItemUsageInfo(18),paralyze_heal=assets.ItemUsageInfo(22);
        const auto potion=assets.ItemUsageInfo(17);
        const unsigned paralyzed=static_cast<unsigned>(za::PartyStatus::Paralysis);
        const unsigned poisoned=static_cast<unsigned>(za::PartyStatus::Poison);
        if(!paralyze_heal.IsMedicine()||!paralyze_heal.CanHeal(10,10,paralyzed)||
           paralyze_heal.CanHeal(9,10,poisoned)||paralyze_heal.CanHeal(9,10,0)||
           paralyze_heal.CanHeal(0,10,paralyzed)||!antidote.CanHeal(10,10,poisoned)||
           potion.HpAfterMedicine(195,258)!=215||potion.HpAfterMedicine(250,258)!=258||
           potion.HpAfterMedicine(0,258)!=0||paralyze_heal.HpAfterMedicine(195,258)!=195){
            std::printf("FAIL locale %d: medicine eligibility does not match item data\n",lang);
            return 13;
        }
        for(const auto& phrase:phrases)std::printf("UI %d\t%s\t%s\n",lang,phrase.c_str(),assets.UiText(phrase).c_str());
        for(const auto& phrase:ui){bool explicit_translation=false;for(const auto& row:za::ui::phrases)if(phrase==row.source)explicit_translation=true;
            if(!explicit_translation&&assets.UiText(phrase).empty()){++missing;std::printf("MISSING %d\t%s\n",lang,phrase.c_str());}}
        if(corpus){
            for(int i=0;i<18;++i)corpus<<assets.TypeName(i)<<'\n';
            for(int i=0;i<25;++i)corpus<<assets.NatureName(i)<<'\n';
            for(const auto& cafe:assets.Cafes())corpus<<cafe.name<<'\n';
            for(const auto& center:assets.Centers())corpus<<center.name<<'\n';
            for(int i=1;i<=1025;++i)corpus<<assets.SpeciesName(i)<<'\n'<<assets.SpeciesDescription(i)<<'\n';
            for(int i=0;i<3000;++i)corpus<<assets.ItemName(i)<<'\n'<<assets.ItemDescription(i)<<'\n';
            for(int i=0;i<1000;++i)corpus<<assets.MoveName(i)<<'\n'<<assets.MoveDescription(i)<<'\n';
            for(int category=0;category<3;++category)for(const auto& q:assets.Missions(category))corpus<<q.title<<'\n'<<q.description<<'\n'<<q.objectives<<'\n';
            for(const auto& phrase:ui)corpus<<assets.UiText(phrase)<<'\n';
            for(const auto& phrase:za::ui::phrases)corpus<<phrase.text[lang]<<'\n';
        }
    }
    std::printf("za-language: ten languages verified, %d missing UI translations\n",missing);return missing?3:0;
}
