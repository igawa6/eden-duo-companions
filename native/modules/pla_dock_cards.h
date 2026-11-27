// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Included after Pokemon page art. Immutable images only; never reads guest state.
#pragma once

acnh::Image DockNativePlaque(const PokemonPageArt &art) {
    const auto source=art.Card("detail_deco_rd_00^q");
    if (source.Empty()) return {};
    const int scaled_width=source.w*124/source.h;
    if (scaled_width<=84) return {};
    acnh::Image image(324,124);
    for (int y=0;y<124;++y)
        for (int x=0;x<324;++x) {
            const int sx=x<70 ? x : x>=310 ? scaled_width-(324-x)
                : 70+(x-70)*(scaled_width-84)/240;
            std::memcpy(image.At(x,y),source.At(sx*source.w/scaled_width,y*source.h/124),4);
        }
    return image;
}
bool LoadDockCard(Instance &i,const EdenDsmodHostApi &h,const char *key,acnh::Image &image) {
    const std::string_view source(key);
    const bool item=source.starts_with("module:pla:hero_item_card:");
    const bool hero=source.starts_with("module:pla:party_hero:");
    const bool cell=source.starts_with("module:pla:party_cell:");
    if ((!item&&!hero&&!cell)||source.size()>255) return false;
    pla::Pokemon mon;unsigned selected=0,nicknamed=0,shiny=0,id=0,count=0;int offset=0;
    if (item) {
        if (std::sscanf(key,"module:pla:hero_item_card:%u:%u%n",&id,&count,&offset)!=2 ||
            size_t(offset)!=source.size() || !id || id>4095 || count>999) return false;
    } else {
        const char *format=hero ? "module:pla:party_hero:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%n"
            : "module:pla:party_cell:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%n";
        if (std::sscanf(key,format,&mon.species,&mon.form,&mon.gender,&shiny,&mon.level,
                &mon.hp,&mon.max_hp,&mon.status,&selected,&nicknamed,&offset)!=10 || !offset ||
            !mon.species || mon.species>1025 || mon.form>255 || mon.gender>2 || shiny>1 ||
            !mon.level || mon.level>100 || !mon.max_hp || mon.max_hp>65535 ||
            mon.hp>mon.max_hp || selected>1 || nicknamed>1 || source.size()-offset>96) return false;
        mon.valid=true;mon.shiny=shiny;mon.nicknamed=nicknamed;mon.nickname=key+offset;mon.fainted=!mon.hp;
    }
    std::lock_guard lock(i.font_mutex);
    PokemonPageArt art;
    if (!PokemonFonts(i,h) || !art.Load(h,false,&i)) return false;
    if (item || hero) {
        image=DockNativePlaque(art);if (image.Empty()) return false;
        if (item) {
            char path[96];std::snprintf(path,sizeof path,"bin/appli/res_item/icon_item/item_%04u.bntx",id);
            const auto icon=CachedBntxTexture(i,CachedRomfs(i,h,path),path,"");
            acnh::Image disc,ring,end;if (icon.Empty() || !ThrowMaterials(i,h,disc,ring,end)) return false;
            DockBlit(image,ThrowQuarter(disc,60,{78,80,81,235}),6,32,60,60);
            DockBlit(image,ThrowQuarter(ring,52,DockCream,true),10,36,52,52);
            DockBlit(image,ThrowItemIcon(icon,52),10,36,52,52);
            auto name=pla::Message(CachedRomfs(i,h,"bin/message/English/common/itemname.dat"),id);
            if (name.empty()) name="Unavailable";
            auto split=name.find(' ',name.size()/2);
            if (split==std::string::npos) split=name.find_last_of(' ');
            if (DockTextWidth(i.font,name)>248 && split!=std::string::npos) {
                PokemonText(image,i.font,name.substr(0,split),23,66,20,248,DockInk);
                PokemonText(image,i.font,name.substr(split+1),23,66,60,140,DockInk);
            } else PokemonText(image,i.font,name,23,66,20,248,DockInk);
            PokemonText(image,i.hero,std::to_string(count),37,66,56,240,DockInk,1);
        } else {
            const auto catalogue=CachedRomfs(i,h,"bin/appli/res_pokemon/list/pokemon_info_list.bin");
            const auto portrait=PokemonPortrait(h,mon,false,catalogue,&i);
            constexpr auto ball_path="bin/appli/icon_ball/icon_ball_1710.bntx";
            const auto ball=CachedBntxTexture(i,CachedRomfs(i,h,ball_path),ball_path,"");
            if (portrait.Empty() || ball.Empty()) return false;
            const auto hp=std::to_string(mon.hp)+"/"+std::to_string(mon.max_hp);
            DockBlit(image,ball,16,DockTextWidth(i.hero,hp)>290 ? 8 : 40,40,44);
            DockBlit(image,portrait,60,4,60,60);
            PokemonText(image,i.font,DockPokemonName(i.font,h,mon.species,mon.nicknamed,
                mon.nickname.c_str(),&i),23,126,20,184,DockInk);
            PokemonText(image,i.secondary,"HP",21,74,DockTextWidth(i.hero,hp)>184 ? 96 : 72,64,DockInk);
            if (DockTextWidth(i.hero,hp)>290) {
                PokemonText(image,i.hero,std::to_string(mon.hp),37,16,56,166,DockInk);
                PokemonText(image,i.font,"/"+std::to_string(mon.max_hp),23,180,70,130,DockInk,1);
            } else PokemonText(image,i.hero,hp,37,16,56,290,DockInk,1);
        }
        return true;
    }
    image=acnh::Image(170,168);
    const Color color=selected ? DockCream : mon.fainted ? Color{78,84,92,255} : DockSlate;
    NativePlate(image,h,color,10,&i);
    if (!selected && !mon.fainted)
        for (size_t n=3;n<image.rgba.size();n+=4) image.rgba[n]=unsigned(image.rgba[n])*94/100;
    DockBlit(image,art.disc,4,6,64,64);
    const auto catalogue=CachedRomfs(i,h,"bin/appli/res_pokemon/list/pokemon_info_list.bin");
    const auto portrait=PokemonPortrait(h,mon,false,catalogue,&i);
    if (portrait.Empty()) return false;
    DockBlit(image,portrait,6,8,60,60);
    const auto fg=selected ? DockInk : DockCream;
    // One line, as in the approved card; three-digit levels drop the space before wrapping.
    auto level="Lv. "+std::to_string(mon.level);
    // Keep clear of the card's torn right edge (x>=158).
    if (DockTextWidth(i.secondary,level)>92) level="Lv."+std::to_string(mon.level);
    PokemonText(image,i.secondary,level,21,66,22,92,fg,1);
    PokemonText(image,i.font,DockPokemonName(i.font,h,mon.species,mon.nicknamed,
        mon.nickname.c_str(),&i),23,5,84,164,fg);
    auto bar=[&](int width,Color c){for(int y=0;y<7;++y)DockRect(image,12,116+y,std::max(0,width-y*5/7),1,c);};
    bar(128,selected ? Color{196,188,162,255} : Color{53,64,72,255});
    if (mon.hp) bar(int(uint64_t(128)*mon.hp/mon.max_hp),{43,216,129,255});
    if (mon.fainted) {
        DockBlit(image,PokemonMask(art.faint_pill,{52,54,58,255},3),2,124,164,40);
        PokemonText(image,i.font,"FAINTED",23,0,133,166,DockCream,2);
        DockBlit(image,art.faint_mark,86,36,56,56);
    } else {
        PokemonText(image,i.font,std::to_string(mon.hp)+"/"+std::to_string(mon.max_hp),23,12,132,144,fg);
        const char *kind=(mon.status&0x88) ? "doku" : (mon.status&0x10) ? "yakedo"
            : (mon.status&0x20) ? "koori" : (mon.status&0x40) ? "mahi" : (mon.status&7) ? "nemuri" : nullptr;
        if (kind) {
            const auto name=std::string("iconSickMark_")+kind+"_00^u";
            const auto mark=CachedTexture(i,h,"bin/appli/icon_sick/bin/dummy.arc",name.c_str());
            if (mark.Empty()) return false;
            DockBlit(image,mark,100,44,40,40); // under the one-line level, above the name
        } else if (mon.status) PokemonText(image,i.secondary,"STATUS",21,68,44,98,fg,1);
    }
    return true;
}
