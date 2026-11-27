// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Immutable native settings page; the Enabled/Disabled toggles are separate flag-gated images.
bool LoadSettingsImage(Instance &i,const EdenDsmodHostApi &h,acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    PokemonPageArt art;if (!PokemonFonts(i,h)||!art.Load(h,false,&i)) return false;
    const auto half=art.Card("pokedex_deco_page_01^t");
    const auto hex=art.Card("pokedex_base_lack_of_research_00^q");
    const auto ornament=CachedTexture(i,h,"bin/appli/report_backup/bin/report_top_00.arc","report_bg_deco_frame_00^t");
    if (half.Empty()||hex.Empty()||ornament.Empty()) return false;
    im=acnh::Image(1104,1080);
    acnh::Image ball(400,400);
    for(int y=0;y<400;++y)for(int x=0;x<400;++x){
        const auto *s=half.At(x*half.w/400,(y<200?y:399-y)*half.h/200);auto *d=ball.At(x,y);
        d[0]=222;d[1]=214;d[2]=184;d[3]=unsigned(s[1])*s[3]*55/25500;
    }
    DockBlit(im,ball,650,640,400,400);
    constexpr Color rule{207,205,186,255};
    for(int inset : {0,9}){
        const int x=24+inset,y=24+inset,w=1057-2*inset,ht=1033-2*inset,t=inset?3:6;
        DockRect(im,x,y,w,t,rule);DockRect(im,x,y+ht-t,w,t,rule);
        DockRect(im,x,y,t,ht,rule);DockRect(im,x+w-t,y,t,ht,rule);
    }
    auto corner=PokemonMask(ornament,DockCream,1);
    for(int y=0;y<corner.h;++y)for(int x=0;x<corner.w;++x){
        const auto *s=ornament.At(x,y);auto *d=corner.At(x,y);const unsigned mix=255-s[0];
        for(unsigned k=0;k<3;++k)d[k]=(DockCream[k]*(255-mix)+rule[k]*mix)/255;
    }
    for(int y:{24,1056})for(int x:{24,1080})DockBlit(im,corner,x-18,y-18,56,56);
    const char *labels[]={"Follow player by default","Reduced motion","Haptics","Show throw selector on top"};
    const char *descriptions[]={"The map recentres on you after loading","No page fades or map gliding","Vibrate on taps and selection changes","Native item and Pokemon ring on the main screen"};
    for(int row=0;row<4;++row){
        const int y=36+124*row;
        PokemonText(im,i.font,labels[row],23,64,y+18,620,DockInk);
        PokemonText(im,i.secondary,descriptions[row],21,64,y+62,640,{70,90,110,255});
        PokemonDots(im,art,64,y+112,1000);
    }
    DockRect(im,64,532,1000,2,{120,128,136,255}); // solid rule: preferences above, about below
    PokemonText(im,i.secondary,"Language",21,64,558,220,{70,90,110,255});
    PokemonText(im,i.font,"English",23,300,554,720,DockInk);
    PokemonText(im,i.secondary,"Version",21,64,626,220,{70,90,110,255});
    PokemonText(im,i.font,"Companion 1.0.0  /  Game 1.1.1",23,300,622,720,DockInk);
    return true;
}

// One Enabled/Disabled pair (350x52): the selected word sits on the native hex plate.
bool LoadSettingsToggle(Instance &i,const EdenDsmodHostApi &h,bool on,acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    PokemonPageArt art;if (!PokemonFonts(i,h)||!art.Load(h,false,&i)) return false;
    const auto hex=art.Card("pokedex_base_lack_of_research_00^q");
    if (hex.Empty()) return false;
    im=acnh::Image(350,52);
    for(int option=0;option<2;++option){
        const bool selected=(option==0)==on;const int px=180*option;
        if(selected){
            auto plate=PokemonMask(hex,DockInk,3);const int sw=plate.w*52/plate.h;
            acnh::Image scaled(170,52);
            for(int yy=0;yy<52;++yy)for(int xx=0;xx<170;++xx){
                const int sx=xx<20?xx:xx>=150?sw-(170-xx):20+(xx-20)*(sw-40)/130;
                std::memcpy(scaled.At(xx,yy),plate.At(std::clamp(sx*plate.w/sw,0,plate.w-1),yy*plate.h/52),4);
            }
            DockBlit(im,scaled,px,0,170,52);
        }
        PokemonText(im,i.font,option?"Disabled":"Enabled",23,px,15,170,selected?DockCream:Color{72,84,96,255},2);
    }
    return true;
}

// The Arc Phone loading emblem; Settings shows it at 55 %, the loading state at full strength.
bool LoadSettingsEmblem(Instance &i,const EdenDsmodHostApi &h,acnh::Image &im,unsigned opacity=55) {
    constexpr auto path="bin/appli/load/bin/load_01.arc";
    const auto cloud=CachedTexture(i,h,path,"load_cloud^t");
    const auto glow=CachedTexture(i,h,path,"load_clack_glow^q");
    const auto base=CachedTexture(i,h,path,"load_clack_base^q");
    if(cloud.Empty()||glow.Empty()||base.Empty())return false;
    im=acnh::Image(128,128);
    // Native BRTI selectors already move the cloud mask into alpha (RGB=0).
    DockBlit(im,PokemonMask(cloud,{18,18,20,255},3),0,0,128,128);
    DockBlit(im,glow,0,0,128,128);DockBlit(im,base,0,0,128,128);
    for(size_t n=3;n<im.rgba.size();n+=4)im.rgba[n]=unsigned(im.rgba[n])*opacity/100;
    return true;
}
