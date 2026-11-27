// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside the module's private namespace, after Romfs().
using Color = std::array<uint8_t, 4>;
constexpr Color DockInk{51, 77, 103, 255}, DockCream{244, 239, 214, 255},
    DockSlate{46, 52, 61, 255};
void DockRect(acnh::Image &im, int x, int y, int w, int h, Color c) {
    for (int yy = std::max(y, 0); yy < std::min(y + h, im.h); ++yy)
        for (int xx = std::max(x, 0); xx < std::min(x + w, im.w); ++xx)
            std::copy(c.begin(), c.end(), im.At(xx, yy));
}
void DockBlit(acnh::Image &dst, const acnh::Image &src, int x, int y, int w, int h,
              bool dim = false) {
    if (!src.w || !src.h || w <= 0 || h <= 0)
        return;
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            if (x + xx < 0 || y + yy < 0 || x + xx >= dst.w || y + yy >= dst.h)
                continue;
            const auto *s = src.At(xx * src.w / w, yy * src.h / h);
            auto *d = dst.At(x + xx, y + yy);
            const unsigned a = dim ? s[3] / 3 : s[3], da = d[3], out = a + da * (255 - a) / 255;
            for (int k = 0; k < 3; ++k)
                d[k] = out ? (s[k] * a + d[k] * da * (255 - a) / 255) / out : 0;
            d[3] = out;
        }
}
void DockCircle(acnh::Image &im, int cx, int cy, int r, Color c) {
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x)
            if (x >= 0 && y >= 0 && x < im.w && y < im.h &&
                (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
                std::copy(c.begin(), c.end(), im.At(x, y));
}
int DockTextWidth(const pla::Font &font, std::string_view text) {
    int w = 0;
    for (size_t j = 0; j < text.size();) {
        unsigned cp = uint8_t(text[j++]);
        if (cp >= 0xc0) {
            unsigned n = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
            cp &= (1u << (6 - n)) - 1;
            if (n > text.size() - j)
                break;
            while (n--)
                cp = (cp << 6) | (uint8_t(text[j++]) & 63);
        }
        if (cp >= 32 && cp - 32 < font.glyphs.size())
            w += font.glyphs[cp - 32].advance;
    }
    return w;
}
std::string DockFit(const pla::Font &font, std::string text, int width) {
    if (DockTextWidth(font, text) <= width)
        return text;
    while (!text.empty() && DockTextWidth(font, text + "…") > width) {
        size_t j = text.size() - 1;
        while (j && (uint8_t(text[j]) & 0xc0) == 0x80)
            --j;
        text.resize(j);
    }
    return text + "…";
}
void DockText(acnh::Image &dst, const pla::Font &font, std::string text, unsigned cap, int x, int y,
              int w, int h, Color c) {
    text = DockFit(font, std::move(text), w);
    auto ink = pla::TextImage(font, text, cap, w + 16, h, true);
    for (size_t j = 0; j < ink.rgba.size(); j += 4)
        for (unsigned k = 0; k < 3; ++k)
            ink.rgba[j + k] = c[k];
    // TextImage's left padding is 16. Drop it, preserving exact glyph pixels.
    acnh::Image cropped(w, h);
    for (int yy = 0; yy < h; ++yy)
        std::memcpy(cropped.At(0, yy), ink.At(16, yy), w * 4);
    DockBlit(dst, cropped, x, y, w, h);
}
std::string DockPokemonName(const pla::Font &font, const EdenDsmodHostApi &h, unsigned id,
                            bool nicknamed, const char *nickname, Instance *cache = nullptr) {
    const auto names = [&] {
        constexpr auto path = "bin/message/English/common/monsname.dat";
        return cache ? CachedRomfs(*cache, h, path) : Romfs(h, path);
    };
    auto text = nicknamed ? std::string(nickname)
                          : pla::Message(names(), id);
    bool unsupported = false;
    for (size_t j = 0; j < text.size();) {
        unsigned cp = uint8_t(text[j++]);
        if (cp >= 0xc0) {
            unsigned n = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
            cp &= (1u << (6 - n)) - 1;
            if (n > text.size() - j) {
                unsupported = true;
                break;
            }
            while (n--)
                cp = (cp << 6) | (uint8_t(text[j++]) & 63);
        }
        if (cp < 32 || cp - 32 >= font.glyphs.size() || !font.glyphs[cp - 32].advance)
            unsupported = true;
    }
    if (unsupported)
        text = pla::Message(names(), id);
    return text.empty() ? "Unavailable" : text;
}
acnh::Image CachedTexture(Instance &i, const EdenDsmodHostApi &h,
                           const char *archive, const char *name) {
    const auto bntx = CachedBntx(i, std::string("sarc:") + archive, [&] {
        return pla::SarcTexture(CachedRomfs(i, h, archive));
    });
    return CachedBntxTexture(i, bntx, archive, name);
}
acnh::Image PlainTexture(const EdenDsmodHostApi &h, const char *archive, const char *name,
                          Instance *cache = nullptr) {
    if (cache) return CachedTexture(*cache, h, archive, name);
    acnh::Image image;
    acnh::BntxDecode(pla::SarcTexture(Romfs(h, archive)), name, image);
    return image;
}
void NativePlate(acnh::Image &im, const EdenDsmodHostApi &h, Color color, int tear,
                   Instance *cache = nullptr) {
    const auto texture = PlainTexture(h, "bin/appli/bag/bin/bag_top_over_00.arc",
                                      "bag_base_poke_info_unseting^t", cache);
    if (texture.w < 96 || texture.h < 2) return;
    DockRect(im, 0, 0, im.w, im.h, color);
    // This BC5 texture selects raw R into RGB and raw G into native alpha.
    // Its shape is raw R alone (the approved PNG's alpha was opaque), not
    // R*G. Saturate above 12 and retain the native torn right end.
    for (int y = 0; y < im.h; ++y) {
        const int sy = std::max(1, y * texture.h / im.h);
        for (int x = im.w - tear; x < im.w; ++x) {
            const int sx = texture.w - 96 + (x - im.w + tear) * 96 / tear;
            const auto *pixel = texture.At(sx, sy);
            const unsigned mask = pixel[0];
            im.At(x, y)[3] = color[3] * (mask >= 12 ? 255 : std::min(255u, mask * 20)) / 255;
        }
    }
}
acnh::Image NativePaper(const EdenDsmodHostApi &h, Instance *cache = nullptr) {
    constexpr auto key = "derived:paper:v1";
    if (cache) {
        if (const auto found = cache->asset_images.Find(key)) return *found;
        ++cache->asset_paper_builds;
    }
    const auto tile = PlainTexture(h, "bin/appli/report_backup/bin/report_top_00.arc",
                                   "report_pattern_00^o", cache);
    const auto title = PlainTexture(h, "bin/appli/title_demo/bin/title_demo_01_eng.arc",
                                    "title_deco_base^q", cache);
    if (tile.w != 32 || tile.h != 32 || title.w != 1000 || title.h != 1080) return {};
    acnh::Image paper(1240,1080), layer(1240,1080), right(700,1080);
    for (int y=0; y<1080; ++y) {
        for (int x=0; x<1240; ++x) {
            std::memcpy(paper.At(x,y),tile.At(x%32,y%32),4);
            if (x<700) std::memcpy(layer.At(x,y),title.At(999-x,y),4);
        }
        for (int x=0; x<700; ++x) {
            std::memcpy(right.At(x,y),title.At(300+x,y),4);
            if (x<160) right.At(x,y)[3] = unsigned(right.At(x,y)[3])*x/159;
        }
    }
    DockBlit(layer,right,540,0,700,1080);
    for (size_t j=3; j<layer.rgba.size(); j+=4) layer.rgba[j]=unsigned(layer.rgba[j])*72/100;
    DockBlit(paper,layer,0,0,1240,1080);
    if (cache) {
        const auto cost = paper.rgba.capacity();
        return *cache->asset_images.Insert(key, std::make_shared<const acnh::Image>(std::move(paper)), cost);
    }
    return paper;
}
acnh::Image ThrowQuarter(const acnh::Image &quarter, int diameter, Color color, bool widen = false) {
    if (quarter.Empty() || diameter <= 0) return {};
    acnh::Image image(diameter, diameter);
    for (int y = 0; y < diameter; ++y)
        for (int x = 0; x < diameter; ++x) {
            unsigned mask = 0;
            // Area sampling preserves the native soft mask at the smaller dock
            // size; nearest sampling visibly stair-steps these round materials.
            for (int dy=0;dy<4;++dy)
                for (int dx=0;dx<4;++dx) {
                    int sx=(x*4+dx)*quarter.w*2/(diameter*4);
                    int sy=(y*4+dy)*quarter.h*2/(diameter*4);
                    if (sx>=quarter.w) sx=quarter.w*2-1-sx;
                    if (sy>=quarter.h) sy=quarter.h*2-1-sy;
                    const auto *source=quarter.At(sx,sy);
                    mask+=unsigned(source[0])*source[3];
                }
            auto *pixel = image.At(x, y);
            std::copy(color.begin(), color.end(), pixel);
            pixel[3] = (mask/16) * color[3] / (255 * 255);
        }
    if (widen) {
        const auto original = image;
        for (int y = 0; y < diameter; ++y)
            for (int x = 0; x < diameter; ++x)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        image.At(x,y)[3] = std::max(image.At(x,y)[3],
                            original.At(std::clamp(x+dx,0,diameter-1),
                                        std::clamp(y+dy,0,diameter-1))[3]);
    }
    return image;
}
bool ThrowMaterials(Instance &i, const EdenDsmodHostApi &h, acnh::Image &disc,
                    acnh::Image &ring, acnh::Image &end) {
    constexpr auto member = pla::PackedUiMember::HudArcPhone;
    const auto path = pla::PackedUiPath(member);
    const auto bntx = CachedBntx(i, std::string("packed-bntx:") + path + ":" +
        std::to_string(static_cast<unsigned>(member)), [&] {
            return pla::SarcTexture(pla::DecodePackedUi(member, CachedRomfs(i,h,path)));
        });
    const auto source = std::string(path) + ":member:" + std::to_string(static_cast<unsigned>(member));
    disc = CachedBntxTexture(i,bntx,source,"fieldinfo_icon_item_base_00^s");
    ring = CachedBntxTexture(i,bntx,source,"fieldinfo_icon_item_frame_00^s");
    end = CachedBntxTexture(i,bntx,source,"fieldinfo_bg_item_01^s");
    return disc.w==60 && disc.h==60 && ring.w==32 && ring.h==32 && end.w==34 && end.h==38;
}
acnh::Image ThrowCountPlate(const acnh::Image &end) {
    if (end.w!=34 || end.h!=38) return {};
    acnh::Image image(84,35);
    constexpr int edge = 16; // 34 * 35 / (38 * 2), native mirrored cap proportion.
    for (int y=0;y<35;++y)
        for (int x=0;x<84;++x) {
            auto *pixel=image.At(x,y);
            constexpr Color slate{78,80,81,255};
            std::copy(slate.begin(),slate.end(),pixel);
            unsigned mask=255;
            if (x<edge || x>=84-edge) {
                const int sx=(x<edge ? x : 83-x)*end.w/edge;
                int sy=y*end.h*2/35;
                if (sy>=end.h) sy=end.h*2-1-sy;
                const auto *source=end.At(sx,sy);
                mask=unsigned(source[0])*source[3]/255;
            }
            pixel[3]=mask*234/255;
        }
    return image;
}
acnh::Image ThrowDockMark(const acnh::Image &disc, const acnh::Image &ring, int size) {
    auto image=ThrowQuarter(disc,size,{78,80,81,255});
    DockBlit(image,ThrowQuarter(ring,size-8,DockCream,true),4,4,size-8,size-8);
    const int core=size/2-2, inset=(size-core)/2;
    DockBlit(image,ThrowQuarter(disc,core,DockCream),inset,inset,core,core);
    return image;
}
acnh::Image ThrowItemIcon(const acnh::Image &source, int diameter) {
    if (source.Empty()) return {};
    acnh::Image image(diameter,diameter);
    for (int y=0;y<diameter;++y)
        for (int x=0;x<diameter;++x) {
            unsigned alpha=0,rgb[3]{};
            for (int dy=0;dy<4;++dy)
                for (int dx=0;dx<4;++dx) {
                    const auto *p=source.At((x*4+dx)*source.w/(diameter*4),
                                             (y*4+dy)*source.h/(diameter*4));
                    alpha+=p[3];
                    for (int k=0;k<3;++k) rgb[k]+=unsigned(p[k])*p[3];
                }
            auto *p=image.At(x,y);
            for (int k=0;k<3;++k) p[k]=alpha ? rgb[k]/alpha : 0;
            p[3]=alpha/16;
        }
    return image;
}
bool LoadDockImage(Instance &i, const EdenDsmodHostApi &h, const char *key, acnh::Image &im) {
    const std::string_view source(key);
    if (source == "module:pla:paper") {
        im = NativePaper(h, &i);
        return im.w != 0;
    }
    auto native = [&](const std::string &path) {
        return CachedBntxTexture(i, CachedRomfs(i, h, path.c_str()), path, "");
    };
    unsigned id = 0, count = 0, selected = 0, form = 0, hp = 0, max_hp = 0, nick = 0;
    int offset = 0;
    unsigned gender = 0, shiny = 0;
    if (std::sscanf(key, "module:pla:pokemon:%u:%u:%u:%u%n", &id, &form, &gender, &shiny, &offset) == 4 &&
        size_t(offset) == source.size() && shiny <= 1) {
        const auto path = pla::PokemonIcon(
            CachedRomfs(i, h, "bin/appli/res_pokemon/list/pokemon_info_list.bin"), id, form, gender, shiny);
        if (path.empty())
            return false;
        auto icon = native(path);
        if (!icon.w)
            return false;
        im = acnh::Image(64, 64);
        DockBlit(im, icon, 0, 0, 64, 64);
        return true;
    }
    if (source.starts_with("module:pla:plate:")) {
        const bool selected = source.ends_with("selected"), fainted = source.ends_with("fainted");
        im = acnh::Image(170, 168);
        const Color fill = selected ? DockCream : fainted ? Color{82, 85, 90, 255} : DockSlate;
        for (int y = 0; y < 168; ++y)
            DockRect(im, 0, y, 160 + (y % 13 < 6 ? 5 : 10), 1, fill);
        NativePlate(im, h, fill, 10);
        if (selected) {
            DockRect(im, 0, 0, 160, 3, DockInk);
            DockRect(im, 0, 0, 3, 168, DockInk);
            DockRect(im, 0, 165, 160, 3, DockInk);
        }
        DockCircle(im, 36, 38, 32, {28, 34, 39, 255});
        DockCircle(im, 36, 38, 29, {68, 74, 78, 255});
        return true;
    }
    if (source == "module:pla:edge" || source == "module:pla:edge:left") {
        im = acnh::Image(24, 84);
        for (int x = 0; x < 24; ++x)
            DockRect(im, x, 0, 1, 84,
                     {241, 235, 208, uint8_t((source.ends_with("left") ? 23 - x : x) * 210 / 23)});
        return true;
    }
    if (source == "module:pla:bag_static") {
        std::lock_guard lock(i.font_mutex);
        if (i.font.rgba.empty() || i.secondary.rgba.empty() || i.hero.rgba.empty()) {
            const auto font = Romfs(h, "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF");
            if ((i.font.rgba.empty() && !pla::BuildFont(font, i.font, 23)) ||
                (i.secondary.rgba.empty() && !pla::BuildFont(font, i.secondary, 21)) ||
                (i.hero.rgba.empty() && !pla::BuildFont(font, i.hero, 37))) return false;
        }
        for (const auto path : {"bin/message/English/common/itemname.dat",
                "bin/message/English/common/iteminfo.dat",
                "bin/appli/res_item/list/item_info_list.bin"})
            if (CachedRomfs(i,h,path).empty()) return false;
        const auto plate = CachedTexture(i,h,"bin/appli/bag/bin/bag_top_over_00.arc",
                                        "bag_base_poke_info_unseting^t");
        acnh::Image disc,ring,end;
        if (plate.w < 96 || plate.h < 2 || !ThrowMaterials(i,h,disc,ring,end)) return false;
        // Immutable art only: no current item snapshot or guest reads.
        im = acnh::Image(1,1);
        return true;
    }
    if (source == "module:pla:bag_cursor") {
        im=PlainTexture(h,"bin/appli/bag/bin/bag_top_00.arc","cursor_00^t",&i);
        if (im.Empty()) return false;
        for (size_t n=0;n<im.rgba.size();n+=4) {
            im.rgba[n+3]=unsigned(im.rgba[n+1])*im.rgba[n+3]/255;
            im.rgba[n]=im.rgba[n+1]=242;im.rgba[n+2]=92;
        }
        return true;
    }
    if (source == "module:pla:bag_capacity") {
        im=PlainTexture(h,"bin/appli/slidelist/bin/slidemenu_menulist_01.arc","sm_menu_icon_bag_00^t",&i);
        if (im.w!=90 || im.h!=90) return false;
        // The capacity display uses the native G silhouette, uniformly cream;
        // navigation uses the separate two-colour tab material.
        for (size_t n=0;n<im.rgba.size();n+=4)
            for (unsigned c=0;c<3;++c) im.rgba[n+c]=DockCream[c];
        return true;
    }
    if (source.starts_with("module:pla:bag_tabs:") || source.starts_with("module:pla:bag_filter:")) {
        unsigned active = 0;
        const bool tabs = std::sscanf(key, "module:pla:bag_tabs:%u", &active) == 1;
        if ((!tabs && std::sscanf(key, "module:pla:bag_filter:%u", &active) != 1) || active > (tabs ? 2u : 1u))
            return false;
        std::lock_guard lock(i.font_mutex);
        if (i.font.rgba.empty() && !pla::BuildFont(
                Romfs(h, "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF"), i.font, 23)) return false;
        im = acnh::Image(tabs ? 1080 : 124, tabs ? 88 : 112);
        pla::PokemonRound(im,0,0,im.w,im.h,14,DockSlate);
        if (tabs) {
            pla::PokemonRound(im,4+int(active)*360,4,352,80,10,{78,84,92,255});
            const char *titles[]={"Everyday Items","Key Items","Recipes"};
            for (unsigned tab=0;tab<3;++tab) {
                const std::string title = titles[tab];
                const int width=DockTextWidth(i.font,title);
                DockText(im,i.font,title,23,int(tab)*360+(360-width)/2,10,width,64,DockCream);
            }
            const int width=DockTextWidth(i.font,titles[active])+20;
            DockRect(im,int(active)*360+(360-width)/2,72,width,5,DockCream);
        } else {
            pla::PokemonRound(im,1,1,122,110,15,{78,80,81,255});
            acnh::Image disc,ring,end;
            if (!ThrowMaterials(i,h,disc,ring,end)) return false;
            DockBlit(im,ThrowDockMark(disc,ring,64),30,24,64,64);
            if (active) DockRect(im,12,104,100,5,{200,180,96,255});
        }
        return true;
    }
    // Satchel is inspection only. A row never invokes a native item action.
    if (source.starts_with("module:pla:bag_")) {
        unsigned dock = 0;
        const bool row = std::sscanf(key, "module:pla:bag_row:%u:%u:%u:%u", &id, &count,
                                     &selected, &dock) == 4;
        if (!row && std::sscanf(key, "module:pla:bag_detail:%u:%u:%u", &id, &count, &dock) != 3)
            return false;
        if (!id || id > 4095 || count > 999) return false;
        std::lock_guard lock(i.font_mutex);
        if (i.font.rgba.empty() && !pla::BuildFont(
                Romfs(h, "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF"), i.font, 23)) return false;
        const auto title = pla::Message(CachedRomfs(i, h, "bin/message/English/common/itemname.dat"), id);
        const auto path=pla::ItemIcon(CachedRomfs(i,h,
            "bin/appli/res_item/list/item_info_list.bin"),id);
        const auto icon=path.empty() ? acnh::Image{} : native(path);
        im = acnh::Image(row ? 528 : 1080, row ? 88 : 256);
        const auto fg = row && !selected ? DockCream : DockInk;
        if (row) NativePlate(im, h, selected ? DockCream : DockSlate, 24, &i);
        else pla::PokemonRound(im,0,0,im.w,im.h,8,DockCream);
        acnh::Image disc,ring,end;
        if (!ThrowMaterials(i,h,disc,ring,end)) return false;
        const int diameter=row ? 72 : 132;
        DockBlit(im,ThrowQuarter(disc,diameter,{78,80,81,235}),row ? 8 : 12,row ? 8 : 14,diameter,diameter);
        DockBlit(im, icon, row ? 10 : 16, row ? 10 : 18, row ? 68 : 124, row ? 68 : 124);
        DockText(im, i.font, title.empty() ? "Unavailable" : title, 23,
                 row ? 96 : 164, 16, row ? (dock ? 290 : 340) : 600, 48, fg);
        if (!row && i.hero.rgba.empty() && !pla::BuildFont(
                Romfs(h, "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF"), i.hero, 37)) return false;
        if (!row && i.secondary.rgba.empty() && !pla::BuildFont(
                Romfs(h, "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF"), i.secondary, 21)) return false;
        const auto count_text=std::to_string(count);
        const int count_width=DockTextWidth(row ? i.font : i.hero,count_text);
        DockText(im, row ? i.font : i.hero, count_text, row ? 23 : 37,
                 (row ? 506 : 1064)-count_width, row ? 16 : 8, count_width, row ? 48 : 68, fg);
        if (dock) {
            DockBlit(im,ThrowDockMark(disc,ring,40),row ? 390 : 758,row ? 24 : 18,40,40);
            if (!row) DockText(im,i.secondary,"Throw dock",21,806,8,188,60,DockInk);
        }
        if (!row) {
            auto description = pla::Message(CachedRomfs(i, h, "bin/message/English/common/iteminfo.dat"), id, true);
            if (description.empty()) description = "Description unavailable";
            std::replace(description.begin(),description.end(),'\n',' ');
            // Word wrapping at the measured native glyph width; final line ellipsizes.
            std::string line;
            int y = 70;
            size_t start = 0;
            while (start < description.size() && y <= 206) {
                const auto end = description.find_first_of(" \n", start);
                const auto word = description.substr(start, end == std::string::npos ? end : end - start);
                const auto candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && DockTextWidth(i.secondary, candidate) > 900) {
                    DockText(im, i.secondary, line, 21, 164, y, 900, 44, DockInk);
                    line.clear(); y += 34;
                }
                line += line.empty() ? word : " " + word;
                if (end == std::string::npos) { start = description.size(); break; }
                start = end + 1;
                if (description[end] == '\n') {
                    DockText(im, i.secondary, line, 21, 164, y, 900, 44, DockInk);
                    line.clear(); y += 34;
                }
                if (y >= 206 && start < description.size()) {
                    line += " " + description.substr(start); break;
                }
            }
            if (!line.empty() && y <= 206)
                DockText(im, i.secondary, line, 21, 164, y, 900, 44, DockInk);
        }
        return true;
    }
    const bool item = std::sscanf(key, "module:pla:item:%u:%u:%u", &id, &count, &selected) == 3;
    const bool hero_item =
        !item && std::sscanf(key, "module:pla:hero_item:%u:%u", &id, &count) == 2;
    const bool name = !item && !hero_item &&
                      std::sscanf(key, "module:pla:name:%u:%u:%n", &id, &nick, &offset) == 2 &&
                      offset;
    const bool hero_pokemon = !item && !hero_item && !name &&
                              std::sscanf(key, "module:pla:hero_pokemon:%u:%u:%u:%u:%n", &id, &hp,
                                          &max_hp, &nick, &offset) == 4 &&
                              offset;
    const bool hero_slot = !item && !hero_item && !name && !hero_pokemon &&
                           std::sscanf(key, "module:pla:hero_slot:%u", &id) == 1 && id >= 1 && id <= 6;
    if (!item && !hero_item && !name && !hero_pokemon && !hero_slot)
        return false;
    std::lock_guard lock(i.font_mutex);
    if (i.font.rgba.empty()) {
        const auto bytes = Romfs(h, "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF");
        if (!pla::BuildFont(bytes, i.font, 23))
            return false;
    }
    if (name) {
        const auto text = DockPokemonName(i.font, h, id, nick, key + offset);
        im = acnh::Image(164, 40);
        DockText(im, i.font, text, 23, 0, 0, 164, 40, {255, 255, 255, 255});
        return true;
    }
    char path[96];
    std::snprintf(path, sizeof path, "bin/appli/res_item/icon_item/item_%04u.bntx", id);
    if (item) {
        if (selected > 2) return false;
        const bool active = selected == 1;
        acnh::Image disc,ring,end;
        if (!ThrowMaterials(i,h,disc,ring,end)) return false;
        // Twelve extra pixels above the ordinary cell preserve the approved
        // raised88px selection without clipping it at the scroll viewport.
        im = acnh::Image(96, 124);
        const int diameter=active ? 88 : 76, x=active ? 4 : 10, y=active ? 0 : 12;
        DockBlit(im,ThrowQuarter(disc,diameter,
            {78,80,81,uint8_t(active?229:count?209:89)}),
            x,y,diameter,diameter);
        if (active)
            DockBlit(im,ThrowQuarter(ring,78,DockCream,true),9,5,78,78);
        if (selected == 2) {
            DockBlit(im,ThrowQuarter(ring,82,DockInk,true),7,9,82,82);
            DockBlit(im,ThrowQuarter(ring,76,DockInk),10,12,76,76);
        }
        auto icon = native(path);
        if (!count)
            for (size_t n=0;n<icon.rgba.size();n+=4) {
                const auto grey=(unsigned(icon.rgba[n])*299+unsigned(icon.rgba[n+1])*587+
                                 unsigned(icon.rgba[n+2])*114)/1000;
                icon.rgba[n]=icon.rgba[n+1]=icon.rgba[n+2]=uint8_t(grey);
                icon.rgba[n+3]=unsigned(icon.rgba[n+3])*45/100;
            }
        DockBlit(im, ThrowItemIcon(icon,active ? 80 : 70), active ? 8 : 13, active ? 4 : 15,
                 active ? 80 : 70, active ? 80 : 70);
        if (active) {
            DockBlit(im,ThrowCountPlate(end),6,89,84,35);
        }
        const auto text = std::to_string(count);
        const int w = DockTextWidth(i.font, text);
        DockText(im, i.font, text, 23, active ? (96 - w) / 2 : 92 - w,
                 active ? 89 : 88, w + 2, 35,
                 active ? DockCream : count ? DockInk : Color{78,84,92,255});
        auto cell=std::move(im);
        im=acnh::Image(122,124);
        DockBlit(im,cell,26,0,96,124);
        if (active) {
            auto cursor=PlainTexture(h,"bin/appli/bag/bin/bag_top_00.arc","cursor_00^t",&i);
            for (size_t n=0;n<cursor.rgba.size();n+=4) {
                cursor.rgba[n+3]=unsigned(cursor.rgba[n+1])*cursor.rgba[n+3]/255;
                cursor.rgba[n]=cursor.rgba[n+1]=242;cursor.rgba[n+2]=92;
            }
            DockBlit(im,cursor,0,14,56,56);
        }
        return true;
    }
    im = acnh::Image(324, 124);
    for (int y = 0; y < 124; ++y) {
        const int edge = std::abs(y - 62) * 52 / 62;
        DockRect(im, edge, y, 324 - edge, 1, {200, 180, 96, 255});
        if (y >= 3 && y < 121)
            DockRect(im, edge + 3, y, 318 - edge, 1, DockCream);
        if (y >= 7 && y < 117)
            DockRect(im, edge + 7, y, 310 - edge, 1, {251, 248, 232, 255});
    }
    std::string text = hero_item
                           ? pla::Message(CachedRomfs(i, h, "bin/message/English/common/itemname.dat"), id)
                           : hero_slot ? "Pokemon " + std::to_string(id)
                                       : DockPokemonName(i.font, h, id, nick, key + offset);
    if (text.empty())
        text = "Unavailable";
    DockText(im, i.font, text, 23, 66, 4, 244, 44, DockInk);
    if (hero_item) {
        auto icon = native(path);
        DockCircle(im, 32, 61, 30, {78, 80, 81, 255});
        DockCircle(im, 32, 61, 26, DockCream);
        DockBlit(im, icon, 8, 37, 48, 48);
    }
    if (hero_slot) {
        DockText(im, i.font, "Details unavailable", 23, 20, 60, 290, 44, DockInk);
        return true;
    }
    if (i.hero.rgba.empty()) {
        const auto bytes = Romfs(h, "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF");
        if (!pla::BuildFont(bytes, i.hero, 37))
            return false;
    }
    const auto value =
        hero_item ? std::to_string(count) : std::to_string(hp) + "/" + std::to_string(max_hp);
    const int w = std::min(240, DockTextWidth(i.hero, value));
    DockText(im, i.hero, value, 37, 310 - w, 48, w, 68, DockInk);
    return true;
}
