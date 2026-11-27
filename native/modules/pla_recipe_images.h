// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Include in the module private namespace after the native Pokemon image helpers.
// Supplied recipe observations only; no guest access, native action or saved-data fallback.
#pragma once
struct RecipeRenderSnapshot {
    pla::RecipePageSnapshot data;
    unsigned selected_item = 0, list_start = 0;
};
void RecipeDashes(acnh::Image &im, int x, int y, int w, int h, Color color) {
    for (int n=0;n<w;n+=12) {
        DockRect(im,x+n,y,std::min(7,w-n),2,color);
        DockRect(im,x+n,y+h-2,std::min(7,w-n),2,color);
    }
    for (int n=0;n<h;n+=12) {
        DockRect(im,x,y+n,2,std::min(7,h-n),color);
        DockRect(im,x+w-2,y+n,2,std::min(7,h-n),color);
    }
}
acnh::Image RecipeIcon(Instance &i, const EdenDsmodHostApi &h, unsigned id) {
    const auto path=pla::ItemIcon(CachedRomfs(i,h,"bin/appli/res_item/list/item_info_list.bin"),id);
    return path.empty() ? acnh::Image{} : CachedBntxTexture(i,CachedRomfs(i,h,path.c_str()),path,"");
}
std::string RecipeRatio(std::optional<unsigned> have, unsigned need) {
    return (have ? std::to_string(*have) : "—")+" / "+std::to_string(need);
}
// Existing Satchel navigation anchors: place this 124x224 picture at (1110,780).
bool RenderRecipeControls(Instance &i, const EdenDsmodHostApi &h,
                          const RecipeRenderSnapshot &snapshot, acnh::Image &im) {
    const auto definitions=pla::ParseRecipeDefinitions(
        CachedRomfs(i,h,"bin/appli/table/making/makelist.bin"));
    const auto model=pla::ProjectRecipes(definitions,snapshot.data);
    if (!model.available || (!model.capacity && !snapshot.data.dock_available)) return false;
    acnh::Image capacity,filter;
    if (model.capacity && !LoadDockImage(i,h,"module:pla:bag_capacity",capacity)) return false;
    if (snapshot.data.dock_available && !LoadDockImage(i,h,
        snapshot.data.dock_only ? "module:pla:bag_filter:1" : "module:pla:bag_filter:0",filter))
        return false;
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i,h)) return false;
    im=acnh::Image(124,224);
    if (model.capacity) {
        DockBlit(im,capacity,34,2,56,56);
        PokemonText(im,i.font,std::to_string(*model.used_slots)+"/"+
                    std::to_string(*model.capacity),23,0,52,124,DockCream,2);
    }
    if (snapshot.data.dock_available) DockBlit(im,filter,0,112,124,112);
    return true;
}
bool DrawRecipeRow(Instance &i, const EdenDsmodHostApi &h,
                     const pla::RecipePageRow &row, bool on, acnh::Image &im) {
    acnh::Image disc,ring,end;
    if (!ThrowMaterials(i,h,disc,ring,end)) return false;
    const auto names=CachedRomfs(i,h,"bin/message/English/common/itemname.dat");
    constexpr int y=0;
    const auto fg=on ? DockInk : DockCream;
    im=acnh::Image(1080,88);
    NativePlate(im,h,on ? DockCream : DockSlate,24,&i);
    DockBlit(im,ThrowQuarter(disc,72,{78,80,81,235}),8,y+8,72,72);
    DockBlit(im,RecipeIcon(i,h,row.definition.result_item),10,y+10,68,68);
    PokemonText(im,i.font,pla::Message(names,row.definition.result_item),23,96,y+28,280,fg);
    // Four real ingredients do not fit the mock's three-chip band. Keep their
    // full names/counts in the independently scrollable detail, without shrinking text.
    bool compact=row.definition.materials.size()>3;
    for (unsigned slot=0;slot<row.have.size();++slot)
        compact=compact || DockTextWidth(i.font,
        RecipeRatio(row.have[slot],row.definition.materials[slot].count))>112;
    if (compact) {
        PokemonText(im,i.font,std::to_string(row.definition.materials.size())+
            (row.definition.materials.size()==1 ? " material" : " materials"),
            23,408,y+28,480,fg);
    } else for (unsigned slot=0;slot<row.definition.materials.size();++slot) {
        const int x=388+176*int(slot);
        const auto &material=row.definition.materials[slot];
        const bool missing=row.have[slot] && *row.have[slot]<material.count;
        pla::PokemonRound(im,x,y+24,164,40,10,missing ? Color{30,34,40,255} :
        on ? Color{226,216,170,255} : Color{221,213,178,255});
        if (missing) RecipeDashes(im,x+1,y+25,162,38,DockCream);
        DockBlit(im,RecipeIcon(i,h,material.id),x-6,y+15,58,58);
        PokemonText(im,i.font,RecipeRatio(row.have[slot],material.count),23,
            x+44,y+28,112,missing ? DockCream : DockInk,1);
    }
    const auto count=row.material_batches ? std::to_string(*row.material_batches) : "—";
    if (row.material_batches==0) RecipeDashes(im,918,y+4,136,80,fg);
    const bool small_count=DockTextWidth(i.hero,count)>136;
    PokemonText(im,small_count ? i.font : i.hero,count,small_count ? 23 : 37,
            918,y+(small_count ? 14 : 4),136,fg,2);
    PokemonText(im,i.secondary,"can craft",21,918,y+54,136,fg,2);
    return true;
}
// One bounded picture per repeated row, rather than an oversized all-recipes image.
bool RenderRecipeRow(Instance &i, const EdenDsmodHostApi &h,
                     const RecipeRenderSnapshot &snapshot, unsigned index, acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i,h)) return false;
    const auto definitions=pla::ParseRecipeDefinitions(
        CachedRomfs(i,h,"bin/appli/table/making/makelist.bin"));
    const auto model=pla::ProjectRecipes(definitions,snapshot.data);
    if (!model.available || index>=model.rows.size()) return false;
    const auto &row=model.rows[index];
    return DrawRecipeRow(i,h,row,row.definition.result_item==snapshot.selected_item,im);
}
bool RenderRecipePage(Instance &i, const EdenDsmodHostApi &h,
                       const RecipeRenderSnapshot &snapshot, acnh::Image &im) {
    acnh::Image tabs,cursor;
    if (!LoadDockImage(i,h,"module:pla:bag_tabs:2",tabs) ||
        !LoadDockImage(i,h,"module:pla:bag_cursor",cursor)) return false;
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i,h)) return false;
    const auto paper=NativePaper(h,&i);
    if (!paper.w) return false;
    im=PokemonCrop(paper,0,0,1104,1080);
    DockBlit(im,tabs,12,12,1080,88);
    const auto definitions=pla::ParseRecipeDefinitions(
        CachedRomfs(i,h,"bin/appli/table/making/makelist.bin"));
    const auto model=pla::ProjectRecipes(definitions,snapshot.data);
    if (!model.available) {
        PokemonText(im,i.font,"Recipes unavailable",23,48,356,992,DockInk,2);
        return true;
    }
    if (model.rows.empty()) {
        PokemonText(im,i.font,"No recipes",23,48,356,992,DockInk,2);
        return true;
    }
    acnh::Image disc,ring,end;
    if (!ThrowMaterials(i,h,disc,ring,end)) return false;
    const auto names=CachedRomfs(i,h,"bin/message/English/common/itemname.dat");
    const auto descriptions=CachedRomfs(i,h,"bin/message/English/common/iteminfo.dat");
    const auto selected=std::find_if(model.rows.begin(),model.rows.end(),[&](const auto &row) {
        return row.definition.result_item==snapshot.selected_item;
    });
    for (unsigned visible=0;visible<5 && snapshot.list_start+visible<model.rows.size();++visible) {
        const auto &row=model.rows[snapshot.list_start+visible];
        const bool on=selected!=model.rows.end() && &row==&*selected;
        const int y=112+100*int(visible);
        acnh::Image picture;
        if (!DrawRecipeRow(i,h,row,on,picture)) return false;
        DockBlit(im,picture,12,y,1080,88);
        if (on) DockBlit(im,cursor,-12,y+16,56,56);
    }
    if (selected==model.rows.end()) return true;
    const auto &row=*selected;
    pla::PokemonRound(im,5,605,1094,470,8,DockCream);
    DockBlit(im,ThrowQuarter(disc,132,{78,80,81,235}),24,626,132,132);
    DockBlit(im,RecipeIcon(i,h,row.definition.result_item),28,630,124,124);
    PokemonText(im,i.font,pla::Message(names,row.definition.result_item),23,176,638,560,DockInk);
    PokemonText(im,i.hero,row.material_batches ? std::to_string(*row.material_batches) : "—",
                37,898,630,180,DockInk,1);
    PokemonText(im,i.secondary,"can craft",21,898,678,180,DockInk,1);
    PokemonDescription(im,i.secondary,pla::Message(descriptions,row.definition.result_item,true),
                       176,690,740,3);
    for (int x=176;x<1076;x+=10) DockRect(im,x,800,3,3,{150,154,156,255});
    return true;
}
bool RenderRecipeMaterials(Instance &i, const EdenDsmodHostApi &h,
                           const RecipeRenderSnapshot &snapshot, acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i,h)) return false;
    const auto definitions=pla::ParseRecipeDefinitions(
        CachedRomfs(i,h,"bin/appli/table/making/makelist.bin"));
    const auto model=pla::ProjectRecipes(definitions,snapshot.data);
    if (!model.available) return false;
    const auto selected=std::find_if(model.rows.begin(),model.rows.end(),[&](const auto &row) {
        return row.definition.result_item==snapshot.selected_item;
    });
    if (selected==model.rows.end()) return false;
    const auto &row=*selected;
    im=acnh::Image(928,std::max(256,76*int(row.definition.materials.size())));
    const auto names=CachedRomfs(i,h,"bin/message/English/common/itemname.dat");
    for (unsigned slot=0;slot<row.definition.materials.size();++slot) {
        const auto &material=row.definition.materials[slot];
        const int y=76*int(slot);
        DockBlit(im,RecipeIcon(i,h,material.id),4,y,72,72);
        PokemonText(im,i.font,pla::Message(names,material.id),23,88,y+24,440,DockInk);
        const bool missing=row.have[slot] && *row.have[slot]<material.count;
        if (missing) {
            pla::PokemonRound(im,606,y+12,250,52,12,{30,34,40,255});
            RecipeDashes(im,607,y+13,248,50,DockCream);
        }
        PokemonText(im,i.font,RecipeRatio(row.have[slot],material.count),23,
                    606,y+24,242,missing ? DockCream : DockInk,1);
        for (int x=12;x<912;x+=10) DockRect(im,x,y+74,3,2,{150,154,156,255});
    }
    return true;
}
