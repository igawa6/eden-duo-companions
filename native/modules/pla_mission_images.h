// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Include inside the module private namespace after Pokemon image helpers.
// Pure owned received-list observations; no guest access or native action.
#pragma once

struct MissionPageRenderSnapshot {
    pla::MissionPageSnapshot data;
    bool request = false, active_only = false;
    uint64_t selected_hash = 0;
    unsigned list_start = 0;
};
struct MissionPageArt {
    acnh::Image paper, badge, tracked, complete, main_icon, sub_icon, symbol, stamp, cursor;
    bool Load(Instance &i, const EdenDsmodHostApi &h) {
        const char *left = "bin/appli/mssn/bin/mssn_list_left_00.arc";
        const auto get = [&](const char *name) { return CachedTexture(i, h, left, name); };
        paper = get("mssn_bg_pattern_00^o");
        badge = get("mssn_list_btn_base_00^t");
        tracked = get("mssn_icon_pguide_00^t");
        complete = get("mssn_list_icon_complete_00^t");
        main_icon = get("mssn_list_icon_mmssn_00^q");
        sub_icon = get("mssn_list_icon_smssn_00^q");
        symbol = get("mssn_base_symbol_00^s");
        stamp = CachedTexture(i, h, "bin/appli/mssn/bin/mssn_list_right_00_eng.arc",
                              "grptxt_mssn_icon_complete_00^t");
        cursor = CachedTexture(i, h, "bin/appli/bag/bin/bag_top_00.arc", "cursor_00^t");
        return cursor.w && paper.w && badge.w && tracked.w && complete.w && main_icon.w &&
               sub_icon.w && symbol.w && stamp.w;
    }
};
// Native BC5 component selection sends raw G to alpha for these ^t masks.
acnh::Image MissionBadge(const acnh::Image &src, int w, int h, bool focus) {
    if (!src.w || !src.h || w <= 0 || h <= 0) return {};
    const int end = std::min(w / 2, std::max(1, src.w * h / src.h));
    acnh::Image scaled(end, h), out(w, h);
    DockBlit(scaled, PokemonMask(src, focus ? Color{222,216,92,255}
                                                    : Color{92,96,100,255}, 3), 0, 0, end, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (x < end) std::memcpy(out.At(x,y), scaled.At(x,y), 4);
            else if (x >= w - end) std::memcpy(out.At(x,y), scaled.At(w-x-1,y), 4);
            else {
                const Color c = focus ? Color{222,216,92,255} : Color{92,96,100,255};
                std::copy(c.begin(), c.end(), out.At(x,y));
            }
        }
    return out;
}
unsigned MissionLines(acnh::Image &im, const pla::Font &font, std::string text,
                       int x, int y, int width, unsigned limit, int pitch, Color color) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    size_t at = 0;
    unsigned line = 0;
    for (; line < limit && at < text.size(); ++line) {
        std::string value;
        if (line + 1 == limit) { value = text.substr(at); at = text.size(); }
        else while (at < text.size()) {
            while (at < text.size() && text[at] == ' ') ++at;
            if (at == text.size()) break;
            const auto end = text.find(' ', at);
            const auto word = text.substr(at, end == std::string::npos ? end : end-at);
            const auto next = value.empty() ? word : value + " " + word;
            if (!value.empty() && DockTextWidth(font, next) > width) break;
            value = next;
            if (end == std::string::npos) { at = text.size(); break; }
            at = end + 1;
        }
        PokemonText(im, font, value, 23, x, y + int(line)*pitch, width, color);
    }
    return line;
}
void MissionDots(acnh::Image &im, int x, int y, int width) {
    for (int p = 0; p < width; p += 10) DockRect(im, x+p, y, 3, 3, {150,154,156,255});
}
void MissionWatermark(acnh::Image &im, const acnh::Image &quarter) {
    if (!quarter.w || !quarter.h) return;
    acnh::Image full(quarter.w*2, quarter.h*2);
    for (int y=0; y<full.h; ++y)
        for (int x=0; x<full.w; ++x) {
            const int sx=x<quarter.w?x:full.w-x-1, sy=y<quarter.h?y:full.h-y-1;
            const auto *p=quarter.At(sx,sy);
            auto *q=full.At(x,y);
            q[0]=q[1]=q[2]=255;
            q[3]=unsigned(p[0])*p[3]*18/(255*255);
        }
    DockBlit(im, full, 642, 600, 420, 420);
}
bool RenderMissionPage(Instance &i, const EdenDsmodHostApi &h,
                        const MissionPageRenderSnapshot &snapshot, acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i,h)) return false;
    const auto paper=NativePaper(h,&i);
    if (!paper.w) return false;
    im=PokemonCrop(paper,0,0,1104,1080);
    MissionPageArt art;
    if (!art.Load(i,h)) {
        PokemonText(im,i.font,"Unavailable",23,480,150,596,DockInk);
        return true;
    }
    const auto main=pla::ParseMissionDefinitions(CachedRomfs(i,h,"bin/event/event_progress/quest_list_main.bin"),false);
    const auto sub=pla::ParseMissionDefinitions(CachedRomfs(i,h,"bin/event/event_progress/quest_list_sub.bin"),true);
    const auto model=pla::BuildMissionPage(main,sub,snapshot.data);
    const auto main_dat=CachedRomfs(i,h,"bin/message/English/common/questlist_main.dat");
    const auto main_tbl=CachedRomfs(i,h,"bin/message/English/common/questlist_main.tbl");
    const auto sub_dat=CachedRomfs(i,h,"bin/message/English/common/questlist_sub.dat");
    const auto sub_tbl=CachedRomfs(i,h,"bin/message/English/common/questlist_sub.tbl");
    const pla::MissionTextTables tables{{main_dat,main_tbl},{sub_dat,sub_tbl}};
    const auto ui_dat=CachedRomfs(i,h,"bin/message/English/common/mssn.dat");
    const auto ui_tbl=CachedRomfs(i,h,"bin/message/English/common/mssn.tbl");
    const auto ui=[&](pla::MissionUiLabel label) {
        const auto value=pla::MissionUiLabelText(label,{ui_dat,ui_tbl});
        return value.empty()?std::string("Unavailable"):value;
    };
    std::vector<const pla::MissionPageRow*> rows;
    std::array<unsigned,2> active{};
    for (const auto &row:model.rows) {
        if (row.status==pla::MissionReceivedStatus::Active) ++active[row.request];
        if (row.request==snapshot.request &&
            (!snapshot.active_only || row.status==pla::MissionReceivedStatus::Active)) rows.push_back(&row);
    }
    const auto id=[&](const pla::MissionPageRow &row) {
        const auto &defs=row.request?sub:main;
        for (const auto &q:defs.quests) if (q.state_hash==row.state_hash) return q.id;
        return uint32_t(0);
    };
    std::sort(rows.begin(),rows.end(),[&](const auto *a,const auto *b) {return id(*a)<id(*b);});
    const pla::MissionPageRow *selected=nullptr;
    for (const auto *row:rows) if (snapshot.selected_hash && row->state_hash==snapshot.selected_hash) selected=row;
    const Color row_ink{46,52,61,255}, card{58,62,64,250};
    pla::PokemonRound(im,12,12,1080,100,14,{46,52,61,245});
    for (unsigned category=0; category<2; ++category) {
        const int x=16+int(category)*540;
        if (bool(category)==snapshot.request) pla::PokemonRound(im,x,16,532,92,10,{78,84,92,255});
        DockBlit(im,category?art.sub_icon:art.main_icon,x+120,36,48,48);
        const auto label=ui(category?pla::MissionUiLabel::Requests:pla::MissionUiLabel::Missions);
        const int width=DockTextWidth(i.font,label);
        PokemonText(im,i.font,label,23,x+180,34,180,DockCream);
        const auto count=model.available && !model.unavailable[category]
            ?std::to_string(active[category])+" active":std::string("—");
        PokemonText(im,i.font,count,23,x+180+width+18,34,std::max(1,330-width),DockCream);
        if (bool(category)==snapshot.request) DockRect(im,x+120,92,392,5,DockCream);
    }
    // Rounded native paper list panel, matching the accepted r12 anchors.
    acnh::Image panel(416,944);
    for (int y=0;y<panel.h;y+=art.paper.h)
        for (int x=0;x<panel.w;x+=art.paper.w) DockBlit(panel,art.paper,x,y,art.paper.w,art.paper.h);
    acnh::Image mask(416,944);
    pla::PokemonRound(mask,0,0,416,944,16,{255,255,255,255});
    for (size_t p=0;p<panel.rgba.size();p+=4) panel.rgba[p+3]=mask.rgba[p+3];
    pla::PokemonRound(im,36,124,416,944,16,row_ink);
    DockBlit(im,panel,40,128,408,936);
    const size_t start=std::min<size_t>(snapshot.list_start,rows.size()>8?rows.size()-8:0);
    for (unsigned slot=0;slot<8 && start+slot<rows.size();++slot) {
        const auto &row=*rows[start+slot];
        const int y=136+int(slot)*100;
        const bool focus=&row==selected, done=row.status==pla::MissionReceivedStatus::Completed;
        const bool tracked=model.tracked_row && &model.rows[*model.tracked_row]==&row;
        pla::PokemonRound(im,48,y,392,88,44,focus?DockCream:done?Color{216,204,172,235}:Color{232,222,192,235});
        if (tracked || done) DockBlit(im,PokemonMask(tracked?art.tracked:art.complete,
            tracked?Color{200,170,40,255}:Color{150,130,40,255},3),56,y+22,44,44);
        DockBlit(im,MissionBadge(art.badge,58,38,focus),102,y+25,58,38);
        PokemonText(im,i.font,std::to_string(id(row)),23,102,y+32,58,focus?DockInk:DockCream,2);
        const auto words=pla::MissionRowText(row,main,sub,snapshot.data,tables);
        // Long titles wrap to two lines so rows sharing a prefix ("The Plate of ...") stay
        // distinguishable (steward spot-check M5); short ones stay centred on one line.
        const std::string title=words.title.empty()?"Unavailable":words.title;
        const Color title_ink=done?Color{76,68,56,255}:DockInk;
        if (DockTextWidth(i.font,title)<=260) PokemonText(im,i.font,title,23,170,y+32,260,title_ink);
        else MissionLines(im,i.font,title,170,y+15,260,2,30,title_ink);
        if (focus) DockBlit(im, PokemonMask(art.cursor, {222,216,92,255}, 3),
                            2, y+16, 56, 56);
    }
    if (rows.size()>8) {
        const int height=int(800*8/rows.size()), y=138+int(800*start/rows.size());
        pla::PokemonRound(im,442,y,4,height,2,{120,110,90,255});
    }
    if (model.available && !model.unavailable[snapshot.request] &&
        std::none_of(model.rows.begin(), model.rows.end(), [&](const auto &row) {
            return row.request == snapshot.request;
        }))
        MissionLines(im, i.font, ui(pla::MissionUiLabel::NoneReceived),
                     60, 220, 368, 2, 38, DockInk);
    pla::PokemonRound(im,48,960,392,92,14,{46,52,61,240});
    PokemonText(im,i.font,ui(pla::MissionUiLabel::NumericalOrder),23,60,995,368,DockCream,2);
    pla::PokemonRound(im,464,124,628,944,18,card);
    MissionWatermark(im,art.symbol);
    if (!model.available || !selected) {
        PokemonText(im,i.font,"Unavailable",23,492,220,572,DockCream);
        return true;
    }
    const auto words=pla::MissionRowText(*selected,main,sub,snapshot.data,tables);
    DockBlit(im,snapshot.request?art.sub_icon:art.main_icon,492,146,56,56);
    DockBlit(im,MissionBadge(art.badge,90,44,true),556,152,90,44);
    PokemonText(im,i.font,std::to_string(id(*selected)),23,556,162,90,DockInk,2);
    int y=220+int(MissionLines(im,i.font,words.title.empty()?"Unavailable":words.title,
                             492,220,572,2,38,DockCream))*38;
    MissionDots(im,492,y+6,572); y+=32;
    const bool done=selected->status==pla::MissionReceivedStatus::Completed;
    if (!words.summary.empty()) {
        y+=int(MissionLines(im,i.font,words.summary,492,y,572,5,40,DockCream))*40+28;
    }
    if (done) {
        DockBlit(im,PokemonMask(art.stamp,{226,56,52,255},3),492,y+4,380,100);
    } else {
        PokemonText(im,i.secondary,ui(pla::MissionUiLabel::CurrentObjective),21,492,y,572,DockCream);
        MissionDots(im,492,y+32,572); y+=52;
        MissionLines(im,i.font,words.purpose.empty()?"Unavailable":words.purpose,
                     492,y,572,unsigned(std::max(1,(1032-y)/40)),40,DockCream);
    }
    return true;
}
