// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Include after Pokemon image helpers. Supplied observations; no guest access/actions.
#pragma once
struct BattlePageArt {
    PokemonPageArt pokemon;
    std::vector<uint8_t> timeline, header;
    acnh::Image selected_disc;
    acnh::Image Timeline(Instance &i, const char *name) const {
        return CachedBntxTexture(i,timeline,"battle-timeline",name);
    }
    acnh::Image Header(Instance &i, const char *name) const {
        return CachedBntxTexture(i,header,"battle-header",name);
    }
    bool Load(Instance &i, const EdenDsmodHostApi &h) {
        if (!pokemon.Load(h,true,&i)) return false;
        const auto unpack=[&](pla::PackedUiMember member) {
            const auto path=pla::PackedUiPath(member);
            return CachedBntx(i,std::string("packed-bntx:")+path+":"+
                std::to_string(unsigned(member)),[&] {
                return pla::SarcTexture(pla::DecodePackedUi(member,CachedRomfs(i,h,path)));
            });
        };
        timeline=unpack(pla::PackedUiMember::BattleTimeline);
        header=unpack(pla::PackedUiMember::BattleHeader);
        selected_disc=PokemonCrop(pokemon.Card("detail_list_select_00^q"),0,0,126,126);
        if (selected_disc.w) for (int y=0;y<126;++y) for (int x=0;x<126;++x)
            if ((x-63)*(x-63)+(y-63)*(y-63)>61*61) selected_disc.At(x,y)[3]=0;
        return !timeline.empty() && !header.empty() && selected_disc.w;
    }
};
pla::PokemonMove BattleMoveMetadata(Instance &i, const EdenDsmodHostApi &h, unsigned id) {
    char path[64]; std::snprintf(path,sizeof path,"bin/pml/waza/waza%04u.wazabin",id);
    return pla::ReadPokemonMove(CachedRomfs(i,h,path),id);
}
void BattlePlate(Instance &i, const EdenDsmodHostApi &h, acnh::Image &im,
                 int x,int y,int w,int height,Color color,int tear=24) {
    acnh::Image plate(w,height);NativePlate(plate,h,color,tear,&i);
    DockBlit(im,plate,x,y,w,height);
}
void BattleCondition(Instance &i,const EdenDsmodHostApi &h,acnh::Image &im,
                       pla::BattleConditionIcon icon,int x,int y) {
    const auto tag=pla::BattleConditionTag(icon);if (!tag) return;
    const auto name=std::string("iconSickMark_")+tag+"_00^u";
    DockBlit(im,CachedTexture(i,h,"bin/appli/icon_sick/bin/dummy_eng.arc",name.c_str()),x,y,66,66);
}
void BattleHp(acnh::Image &im,int x,int y,int w,unsigned hp,unsigned maximum) {
    const auto bar=[&](int length,Color color) {
        for(int yy=0;yy<14;++yy) DockRect(im,x,y+yy,std::max(0,length-yy/2),1,color);
    };
    bar(w,{196,188,162,255});
    if(maximum) bar(int(uint64_t(w)*std::min(hp,maximum)/maximum),{43,216,129,255});
}
void BattleActorCard(Instance &i,const EdenDsmodHostApi &h,const BattlePageArt &art,
    acnh::Image &im,const std::optional<pla::BattleActor> &actor,int x,bool enemy,
    const std::vector<uint8_t> &catalogue) {
    DockBlit(im,PokemonBrush(art.pokemon.brush,enemy?528:540,48,DockInk),x,12,enemy?528:540,48);
    if (!actor) {
        PokemonText(im,i.font,enemy?"Opponent unavailable":"Pokemon unavailable",23,x+24,25,490,DockCream);
        return;
    }
    const auto &p=actor->pokemon;
    const auto name=DockPokemonName(i.font,h,p.species,p.nicknamed,p.nickname.c_str(),&i);
    PokemonText(im,i.font,name,23,x+24,25,365,DockCream);
    PokemonText(im,i.font,"Lv. "+std::to_string(p.level),23,x+386,25,122,DockCream,1);
    DockBlit(im,art.selected_disc,x+6,66,92,92);
    DockBlit(im,PokemonPortrait(h,p,false,catalogue,&i),x+10,70,84,84);
    const int bx=x+104;
    if (enemy) {
        if (actor->hp_percent) BattleHp(im,bx,74,310,*actor->hp_percent,100);
        PokemonText(im,i.font,actor->hp_percent?std::to_string(*actor->hp_percent)+"%":"—",
            23,x+426,70,86,DockInk,1);
    } else {
        BattleHp(im,bx,74,200,p.hp,p.max_hp);
        const auto value=p.hp?std::to_string(p.hp)+"/"+std::to_string(p.max_hp):"FAINTED";
        const bool split=DockTextWidth(i.hero,value)>226;
        PokemonText(im,i.hero,split?std::to_string(p.hp):value,37,x+300,65,226,DockInk,1);
        if (split) PokemonText(im,i.font,"/ "+std::to_string(p.max_hp),23,x+300,113,226,DockInk,1);
        const int left=x+526-DockTextWidth(i.hero,split?std::to_string(p.hp):value);
        if (p.hp && left>x+350) PokemonText(im,i.secondary,"HP",21,left-56,82,44,DockInk,1);
    }
    const auto personal=pla::ReadPokemonPersonal(CachedRomfs(i,h,
        "bin/pml/personal/personal_data_total.perbin"),p.species,p.form);
    int tx=bx;
    if (personal.valid) for (auto type:{personal.type1,personal.type2}) {
        if (type==personal.type1 && tx!=bx) break;
        DockBlit(im,art.pokemon.Type(type),tx,102,50,50);tx+=58;
    }
    if (enemy) PokemonText(im,i.font,"Research Lv. "+
        (actor->research_level?std::to_string(*actor->research_level):"—"),23,tx+10,120,x+516-tx-10,DockInk);
    BattleCondition(i,h,im,actor->condition,x+38,106);
}
const char *BattleAffinityText(pla::BattleAffinity affinity) {
    switch (affinity) {
    case pla::BattleAffinity::Immune:return "No effect";
    case pla::BattleAffinity::Resisted:return "Not very effective";
    case pla::BattleAffinity::Effective:return "Effective";
    case pla::BattleAffinity::SuperEffective:return "Super effective";
    default:return "—";
    }
}
bool RenderBattlePage(Instance &i,const EdenDsmodHostApi &h,
                       const pla::BattlePageInput &input,acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i,h)) return false;
    im=PokemonCrop(NativePaper(h,&i),0,0,1104,1080);
    if (!im.w) return false;
    const auto model=pla::ProjectBattle(input);
    if (!model.available) {
        PokemonText(im,i.font,"Battle unavailable",23,12,522,1080,DockInk,2);return true;
    }
    BattlePageArt art;if (!art.Load(i,h)) return false;
    const auto catalogue=CachedRomfs(i,h,"bin/appli/res_pokemon/list/pokemon_info_list.bin");
    BattleActorCard(i,h,art,im,model.foe,12,true,catalogue);
    BattleActorCard(i,h,art,im,model.own,552,false,catalogue);
    const auto names=CachedRomfs(i,h,"bin/message/English/common/wazaname.dat");
    const auto row_end=art.pokemon.Move("btl_waza_base_00^s");
    const auto pp_end=art.pokemon.Move("btl_waza_pp_base_00^s");
    if (!row_end.w || !pp_end.w) return false;
    std::array<pla::PokemonMove,4> metadata{};
    for (unsigned slot=0;slot<4;++slot) {
        const int y=176+100*int(slot);
        const bool selected=model.inspected_move==slot;
        const auto fg=selected?DockInk:DockCream;
        const auto &move=model.moves[slot];
        acnh::Image row(1036,84);
        DockRect(row,0,0,1036,84,selected?DockCream:Color{71,68,74,255});
        for (int yy=0;yy<84;++yy) for (int xx=0;xx<84;++xx) {
            const auto *p=row_end.At((83-xx)*row_end.w/84,yy*row_end.h/84);
            row.At(952+xx,yy)[3]=unsigned(p[0])*p[3]/255;
        }
        DockBlit(im,row,56,y,1036,84);
        if (model.phase==pla::BattlePhase::ForcedSwitch) {
            // The active Pokemon fainted: the game asks for the next one on the top screen.
            if (!slot) PokemonText(im,i.font,"Choose your next Pokemon",23,124,y+30,770,fg);
            continue;
        }
        if (!model.moves_available || !move.id) {
            PokemonText(im,i.font,model.moves_available?"—":"Unavailable",23,124,y+30,770,fg);continue;
        }
        metadata[slot]=BattleMoveMetadata(i,h,move.id);
        const auto &m=metadata[slot];
        if (!m.valid || !m.usable) {
            PokemonText(im,i.font,"Unavailable",23,124,y+30,770,fg);continue;
        }
        DockBlit(im,PokemonTypeDiamond(art.pokemon,m.type),45,y+9,66,66);
        PokemonText(im,i.font,pla::PokemonMoveName(names,move.id),23,124,y+30,316,fg);
        if (move.affinity!=pla::BattleAffinity::Unknown) {
            unsigned id=0;
            if (move.affinity==pla::BattleAffinity::Effective) id=1;
            if (move.affinity==pla::BattleAffinity::SuperEffective) id=2;
            if (move.affinity==pla::BattleAffinity::Resisted) id=3;
            char name[64];std::snprintf(name,sizeof name,"type_affinity_icon_%02u^u",id);
            const auto mark=CachedTexture(i,h,"bin/appli/icon_type_affinity/bin/dummy_eng.arc",name);
            DockBlit(im,PokemonMask(mark,move.affinity==pla::BattleAffinity::SuperEffective?Color{227,50,52,255}:fg),448,y+22,40,40);
        }
        PokemonText(im,i.font,BattleAffinityText(move.affinity),23,494,y+30,244,fg);
        if (move.mastered==true) DockBlit(im,PokemonMask(art.pokemon.Move("btl_wazamode_icon_03^t"),fg,3),736,y+12,66,60);
        DockBlit(im,PokemonBrush(pp_end,170,44,selected?Color{84,82,76,230}:Color{40,42,48,230}),832,y+20,170,44);
        PokemonText(im,i.font,std::to_string(move.pp)+"/"+std::to_string(move.max_pp),23,832,y+30,170,DockCream,2);
        if (selected) PokemonChevron(im,art.pokemon,16,y+12);
    }
    if (model.inspected_move && metadata[*model.inspected_move].valid && metadata[*model.inspected_move].usable) {
        const auto slot=*model.inspected_move;const auto &m=metadata[slot];const auto &move=model.moves[slot];
        DockBlit(im,PokemonBrush(art.pokemon.brush,1080,44,DockInk),12,584,1080,44);
        DockBlit(im,PokemonMask(art.Header(i,"btl_icon_state_00^t"),DockCream,1),36,586,40,40);
        PokemonText(im,i.font,pla::PokemonMoveName(names,move.id),23,88,595,900,DockCream);
        constexpr const char *categories[]={"icon_waza_class_02^s","icon_waza_class_00^s","icon_waza_class_01^s"};
        if (m.category<3) DockBlit(im,PokemonMask(art.pokemon.Move(categories[m.category]),DockCream),1032,588,36,36);
        const auto value=[](unsigned v){return v?std::to_string(v):"—";};
        const auto metrics="Pow "+value(m.power)+"   Acc "+value(m.accuracy);
        const auto pp="PP "+std::to_string(move.pp)+"/"+std::to_string(move.max_pp);
        const bool wrap=DockTextWidth(i.font,metrics+"   "+pp)>424;
        PokemonText(im,i.font,wrap?metrics:metrics+"   "+pp,23,36,658,424,DockInk);
        if (wrap) PokemonText(im,i.font,pp,23,36,704,424,DockInk);
        if (m.can_style && move.mastered==true && move.style_locked==false && move.pp) {
            for (unsigned style=0;style<2;++style) {
                const int x=490+304*int(style);
                DockBlit(im,PokemonStatPlate(art.pokemon,292,108),x,644,292,108);
                DockBlit(im,PokemonMask(art.pokemon.Move(style?"btl_wazamode_icon_02^t":"btl_wazamode_icon_01^t"),DockInk,1),x+14,658,44,44);
                PokemonText(im,i.font,style?"Strong":"Agile",23,x+68,662,104,DockInk);
                const auto power=value(style?m.strong_power:m.agile_power);
                const int label=DockTextWidth(i.secondary,"Pow");
                PokemonText(im,i.secondary,"Pow",21,x+180,666,label,DockInk);
                if (DockTextWidth(i.font,power)<=104-label-8)
                    PokemonText(im,i.font,power,23,x+188+label,662,104-label-8,DockInk);
                else PokemonText(im,i.font,power,23,x+180,704,104,DockInk,1);
            }
        }
    }
    PokemonDots(im,art.pokemon,12,760,1080);
    DockRect(im,16,832,774,2,{190,182,152,255});
    const auto pointed=art.Timeline(i,"btl_tl_base_00_act^s");
    if (model.order_available && !model.order.empty() && pointed.w) {
        for (unsigned n=0;n<model.order.size();++n) {
            const auto &actor=model.order[n];const bool own=model.own && actor.id==model.own->id;
            const int x=24+128*int(n),y=own?838:774;
            const Color color=own?Color{65,98,112,255}:Color{165,68,64,255};
            acnh::Image plate(116,48);DockRect(plate,24,0,92,48,color);
            DockBlit(plate,PokemonMask(pointed,color),0,0,48,48);
            DockBlit(im,plate,x,y,116,48);
            DockBlit(im,art.selected_disc,x+64,y-2,52,52);
            DockBlit(im,PokemonPortrait(h,actor.pokemon,false,catalogue,&i),x+66,y,48,48);
            PokemonText(im,i.font,std::to_string(n+1),23,x+12,y+12,56,DockCream,2);
            if (!n) PokemonChevron(im,art.pokemon,x-24,y-4);
        }
    } else PokemonText(im,i.font,model.phase==pla::BattlePhase::ForcedSwitch?"Choose a Pokemon":"Order unavailable",23,24,800,744,DockInk,2);
    BattlePlate(i,h,im,804,776,288,112,DockSlate);
    DockBlit(im,PokemonMask(art.Header(i,"btl_icon_tl_00^t"),DockCream,1),816,808,48,48);
    PokemonText(im,i.font,"Action Order",23,872,821,208,DockCream);
    if (model.party.valid) for (unsigned n=0;n<model.party.count;++n) {
        const auto &p=model.party.slots[n];if (!p.valid) continue;
        const int x=12+182*int(n);
        BattlePlate(i,h,im,x,900,168,168,DockSlate,10);
        if (model.party_out_markers[n]==true) pla::PokemonRound(im,x+8,896,65,15,4,DockCream);
        DockBlit(im,art.pokemon.disc,x+6,908,60,60);
        DockBlit(im,PokemonPortrait(h,p,false,catalogue,&i),x+8,910,56,56);
        // Same fit as the Map party cards: secondary type, one line, clear of the torn edge.
        auto level="Lv. "+std::to_string(p.level);
        if (DockTextWidth(i.secondary,level)>92) level="Lv."+std::to_string(p.level);
        PokemonText(im,i.secondary,level,21,x+66,914,92,DockCream,1);
        if (!p.hp) DockBlit(im,art.pokemon.faint_mark,x+76,932,66,66);
        else BattleCondition(i,h,im,model.party_conditions[n],x+76,932);
        PokemonText(im,i.font,DockPokemonName(i.font,h,p.species,p.nicknamed,p.nickname.c_str(),&i),23,x+4,982,156,DockCream);
        PokemonHp(im,x+6,1016,148,p.hp,p.max_hp,false);
        if (!p.hp) DockBlit(im,PokemonMask(art.pokemon.faint_pill,{52,54,58,255},3),x+2,1024,162,40);
        PokemonText(im,i.font,p.hp?std::to_string(p.hp)+"/"+std::to_string(p.max_hp):"FAINTED",23,x+4,1032,156,DockCream,2);
    }
    return true;
}
// Static explanatory legend, not native action controls. Place at(1110,768).
bool RenderBattleLegend(Instance &i,const EdenDsmodHostApi &h,
                         const pla::BattlePageInput &input,acnh::Image &im) {
    const auto model=pla::ProjectBattle(input);
    if (!model.moves_available) return false;
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i,h)) return false;
    PokemonPageArt art;if (!art.Load(h,true,&i)) return false;
    im=acnh::Image(124,296);
    for (unsigned n=0;n<2;++n) {
        const int y=152*int(n);
        DockBlit(im,PokemonStatPlate(art,124,144),0,y,124,144);
        DockBlit(im,PokemonMask(art.Move(n?"btl_wazamode_icon_02^t":"btl_wazamode_icon_01^t"),DockInk,1),44,y+6,36,36);
        PokemonText(im,i.font,n?"Strong":"Agile",23,0,y+46,124,DockInk,2);
        PokemonText(im,i.secondary,n?"Later,":"Sooner,",21,0,y+80,124,DockInk,2);
        PokemonText(im,i.secondary,n?"stronger":"weaker",21,0,y+110,124,DockInk,2);
    }
    return true;
}
