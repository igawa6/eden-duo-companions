// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Included after pla_dock_images.h, inside the module's private namespace.
// All inputs are owned snapshots/ROMFS bytes; this worker never reads guest memory.
#pragma once

acnh::Image PokemonCrop(const acnh::Image &src, int x, int y, int w, int h) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x > src.w - w || y > src.h - h)
        return {};
    acnh::Image out(w, h);
    for (int row = 0; row < h; ++row)
        std::memcpy(out.At(0, row), src.At(x, y + row), size_t(w) * 4);
    return out;
}
acnh::Image PokemonMask(const acnh::Image &src, Color color, unsigned channel = 0) {
    acnh::Image out(src.w, src.h);
    for (size_t j = 0; j < src.rgba.size(); j += 4) {
        const unsigned mask = channel == 3 ? src.rgba[j + 3]
            : unsigned(src.rgba[j + channel]) * src.rgba[j + 3] / 255;
        std::copy(color.begin(), color.end(), out.rgba.begin() + j);
        out.rgba[j + 3] = mask * color[3] / 255;
    }
    return out;
}
acnh::Image PokemonNine(const acnh::Image &src, int w, int h, int edge) {
    if (src.w <= edge * 2 || src.h <= edge * 2 || w <= edge * 2 || h <= edge * 2)
        return {};
    acnh::Image out(w, h);
    auto source = [edge](int p, int length, int original) {
        return p < edge ? p : p >= length - edge ? original - (length - p)
            : edge + (p - edge) * (original - edge * 2) / (length - edge * 2);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            std::memcpy(out.At(x, y), src.At(source(x, w, src.w), source(y, h, src.h)), 4);
    return out;
}
void PokemonText(acnh::Image &im, const pla::Font &font, std::string text, unsigned cap,
                 int x, int y, int w, Color color, unsigned align = 0) {
    text = DockFit(font, std::move(text), w);
    const int actual = std::min(w, DockTextWidth(font, text));
    if (align == 1) x += w - actual;
    if (align == 2) x += (w - actual) / 2;
    // DockText vertically centres cap ink in its image. These layout anchors
    // follow the accepted renderer's cap-top coordinates instead.
    DockText(im, font, std::move(text), cap, x, y - 9, std::max(1, actual + 1), cap + 18, color);
}
struct PokemonPageArt {
    Instance *cache = nullptr; // Owned by the caller; this art bundle is render-local.
    std::vector<uint8_t> card, moves, types;
    acnh::Image corner, brush, disc, dots, cursor, faint_pill, faint_mark;
    acnh::Image Texture(const std::vector<uint8_t> &bytes, const std::string &source,
                        const char *name) const {
        if (cache) return CachedBntxTexture(*cache, bytes, source, name);
        acnh::Image image;
        acnh::BntxDecode(bytes, name, image);
        return image;
    }
    static std::string Source(pla::PackedUiMember member) {
        return std::string(pla::PackedUiPath(member)) + ":member:" +
            std::to_string(static_cast<unsigned>(member));
    }
    acnh::Image Card(const char *name) const {
        return Texture(card, Source(pla::PackedUiMember::PokemonCard), name);
    }
    acnh::Image Move(const char *name) const {
        return Texture(moves, Source(pla::PackedUiMember::BattleMoves), name);
    }
    bool Load(const EdenDsmodHostApi &h, bool move_art, Instance *owner = nullptr) {
        cache = owner;
        const auto unpack = [&](pla::PackedUiMember member) {
            const auto path = pla::PackedUiPath(member);
            const auto load = [&] {
                return pla::SarcTexture(pla::DecodePackedUi(member,
                    cache ? CachedRomfs(*cache, h, path) : Romfs(h, path)));
            };
            return cache ? CachedBntx(*cache, std::string("packed-bntx:") + path + ":" +
                std::to_string(static_cast<unsigned>(member)), load) : load();
        };
        card = unpack(pla::PackedUiMember::PokemonCard);
        const auto task = unpack(pla::PackedUiMember::PokemonTask);
        constexpr auto type_path = "bin/appli/icon_type/bin/type_dummy_eng.arc";
        const auto type_load = [&] {
            return pla::SarcTexture(cache ? CachedRomfs(*cache, h, type_path) : Romfs(h, type_path));
        };
        types = cache ? CachedBntx(*cache, std::string("plain-bntx:") + type_path, type_load) : type_load();
        if (move_art) moves = unpack(pla::PackedUiMember::BattleMoves);
        brush = Card("detail_base_header_00^s");
        acnh::Image full_disc, full_corner;
        full_disc = Card("detail_list_unselect_00^q");
        full_corner = Texture(task, Source(pla::PackedUiMember::PokemonTask), "task_base_00^s");
        disc = PokemonCrop(full_disc, 0, 0, 126, 126);
        for (int y = 0; y < disc.h; ++y)
            for (int x = 0; x < disc.w; ++x)
                if ((x - 63) * (x - 63) + (y - 63) * (y - 63) > 61 * 61)
                    disc.At(x, y)[3] = 0;
        corner = PokemonCrop(full_corner, 22, 22, 16, 16);
        const auto plain = [&](const char *path, const char *name) {
            return cache ? CachedTexture(*cache, h, path, name) : PlainTexture(h, path, name);
        };
        dots = plain("bin/appli/report_backup/bin/report_top_00.arc", "report_bg_line_dot_02^s");
        cursor = plain("bin/appli/bag/bin/bag_top_00.arc", "cursor_00^t");
        faint_pill = plain("bin/appli/icon_sick/bin/dummy_eng.arc", "iconSick_hinshi_00^u");
        faint_mark = plain("bin/appli/icon_sick/bin/dummy.arc", "iconSickMark_hinshi_00^u");
        return !card.empty() && brush.w && disc.w && corner.w && !types.empty();
    }
    acnh::Image Type(unsigned id, bool glyph = false) const {
        constexpr const char *tags[] = {"normal", "kakutou", "hikou", "doku", "jimen", "iwa",
            "mushi", "ghost", "hagane", "honoo", "mizu", "kusa", "denki", "esper", "koori",
            "dragon", "aku", "yousei"};
        if (id >= 18) return {};
        char name[96];
        std::snprintf(name, sizeof name, glyph ? "typeIcon_%02u_%s_00^t" : "typeIconText_%02u_%s_00^u",
                      id, tags[id]);
        const auto image = Texture(types, "bin/appli/icon_type/bin/type_dummy_eng.arc", name);
        return glyph ? image : PokemonCrop(image, 13, 3, 40, 40);
    }
};
acnh::Image PokemonBrush(const acnh::Image &end, int w, int h, Color color) {
    if (!end.w || !end.h) return {};
    acnh::Image out(w, h);
    DockRect(out, 0, 0, w, h, color);
    const int ew = std::min(w / 2, end.w * h / end.h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < ew; ++x) {
            const auto *p = end.At(x * end.w / ew, y * end.h / h);
            const unsigned alpha = unsigned(p[0]) * p[3] / 255;
            out.At(x, y)[3] = out.At(w - 1 - x, y)[3] = alpha * color[3] / 255;
        }
    return out;
}
acnh::Image PokemonStatPlate(const PokemonPageArt &art, int w, int h) {
    acnh::Image out(w, h);
    DockRect(out, 0, 0, w, h, {231, 222, 178, 255});
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const auto *p = art.corner.At(x, y);
            const uint8_t alpha = unsigned(p[0]) * p[3] / 255;
            out.At(x, y)[3] = out.At(w - 1 - x, y)[3] =
                out.At(x, h - 1 - y)[3] = out.At(w - 1 - x, h - 1 - y)[3] = alpha;
        }
    return out;
}
void PokemonDots(acnh::Image &im, const PokemonPageArt &art, int x, int y, int w) {
    if (!art.dots.w || !art.dots.h) return;
    const auto dots = PokemonMask(art.dots, {190, 182, 152, 255});
    for (int at = 0; at < w; at += dots.w)
        DockBlit(im, PokemonCrop(dots, 0, 0, std::min(w - at, dots.w), dots.h),
                 x + at, y, std::min(w - at, dots.w), dots.h);
}
void PokemonHp(acnh::Image &im, int x, int y, int w, unsigned hp, unsigned maximum, bool selected) {
    auto bar = [&](int length, Color color) {
        for (int yy = 0; yy < 9; ++yy)
            DockRect(im, x, y + yy, std::max(0, length - yy * 5 / 9), 1, color);
    };
    bar(w, selected ? Color{196, 188, 162, 255} : Color{53, 64, 72, 255});
    if (maximum) bar(int(uint64_t(w) * std::min(hp, maximum) / maximum), {43, 216, 129, 255});
}
// The native art of some large Pokemon ends in a hard cut where the texture frame clips it
// (e.g. a long tail). Fade any edge that still has opaque pixels so it reads as framed art
// instead of a sliced, squashed figure.
void FeatherCutEdges(acnh::Image &im) {
    if (im.w < 64 || im.h < 64) return;
    constexpr int band = 28;
    const auto opaque_edge = [&](bool vertical, int at) {
        int hits = 0;
        const int n = vertical ? im.h : im.w;
        for (int k = 0; k < n; ++k)
            hits += (vertical ? im.At(at, k) : im.At(k, at))[3] > 128;
        return hits > n / 40;
    };
    const bool left = opaque_edge(true, 0), right = opaque_edge(true, im.w - 1);
    const bool top = opaque_edge(false, 0), bottom = opaque_edge(false, im.h - 1);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            int d = band;
            if (left) d = std::min(d, x);
            if (right) d = std::min(d, im.w - 1 - x);
            if (top) d = std::min(d, y);
            if (bottom) d = std::min(d, im.h - 1 - y);
            if (d < band) im.At(x, y)[3] = uint8_t(unsigned(im.At(x, y)[3]) * d / band);
        }
}
// A 3 px soft ink shadow on a detail card's left edge so a cream selected list row beside it
// does not merge into the cream card.
void CardEdge(acnh::Image &im, int x, int y, int h) {
    for (int k = 0; k < 3; ++k)
        for (int yy = y + 10; yy < y + h - 10 && yy < im.h; ++yy) {
            auto *p = im.At(x - 1 - k, yy);
            const unsigned a = 70 - k * 22;
            for (unsigned c = 0; c < 3; ++c) p[c] = uint8_t((p[c] * (255 - a) + DockInk[c] * a) / 255);
        }
}
acnh::Image PokemonPortrait(const EdenDsmodHostApi &h, const pla::Pokemon &mon, bool large,
                            const std::vector<uint8_t> &catalogue, Instance *cache = nullptr) {
    auto path = pla::PokemonIcon(catalogue, mon.species, mon.form, mon.gender, mon.shiny);
    // Battle foes carry no gender: fall back to whichever gender the species has an icon for.
    for (const unsigned gender : {0u, 2u, 1u})
        if (path.empty()) path = pla::PokemonIcon(catalogue, mon.species, mon.form, gender, mon.shiny);
    if (large && !path.empty()) {
        for (const auto &pair : {std::pair{"icon_pokemon_s", "icon_pokemon_l"},
                                 std::pair{"pokeicon_s", "pokeicon_l"}}) {
            const auto at = path.find(pair.first);
            if (at == std::string::npos) return {};
            path.replace(at, std::strlen(pair.first), pair.second);
        }
    }
    const std::string key = "pokemon-portrait:" + path + (large ? ":alpha-crop-gt8-v1" : ":full-v1");
    acnh::Image image;
    if (cache) {
        if (const auto hit = cache->asset_images.Find(key)) image = *hit;
    }
    if (!image.w && !path.empty()) {
        // One cache entry per portrait (steward spot-check M6): small portraits live in the
        // texture cache only; large ones decode uncached and keep just the cropped copy.
        if (cache && !large) image = CachedBntxTexture(*cache, CachedRomfs(*cache, h, path.c_str()), path, "");
        else acnh::BntxDecode(cache ? CachedRomfs(*cache, h, path.c_str()) : Romfs(h, path.c_str()), {}, image);
        if (large && image.w) {
            int left = image.w, right = 0, top = image.h, bottom = 0;
            for (int y = 0; y < image.h; ++y)
                for (int x = 0; x < image.w; ++x)
                    if (image.At(x, y)[3] > 8) {
                        left = std::min(left, x); right = std::max(right, x + 1);
                        top = std::min(top, y); bottom = std::max(bottom, y + 1);
                    }
            image = PokemonCrop(image, left, top, right - left, bottom - top);
        }
        if (cache && large && !image.Empty()) {
            const auto bytes = image.rgba.capacity();
            cache->asset_images.Insert(key, std::make_shared<const acnh::Image>(image), bytes);
        }
    }
    // Preserve native large portrait color; faint treatment is a render-local
    // copy for party rows and never mutates the cached source image.
    if (large) return image;
    if (mon.fainted)
        for (size_t j = 0; j < image.rgba.size(); j += 4) {
            const auto grey = (unsigned(image.rgba[j]) * 299 + unsigned(image.rgba[j + 1]) * 587 +
                               unsigned(image.rgba[j + 2]) * 114) / 1000;
            image.rgba[j] = image.rgba[j + 1] = image.rgba[j + 2] = uint8_t(grey);
            image.rgba[j + 3] = image.rgba[j + 3] * 3 / 4;
        }
    return image;
}
void PokemonChevron(acnh::Image &im, const PokemonPageArt &art, int x, int y) {
    DockBlit(im, PokemonMask(art.cursor, {242, 242, 92, 255}, 3), x, y, 56, 56);
}
int PokemonTypePill(acnh::Image &im, const PokemonPageArt &art, const pla::Font &font,
                    std::span<const uint8_t> names, unsigned id, int x, int y) {
    const auto name = pla::PokemonTypeName(names, id);
    const auto icon = art.Type(id);
    if (name.empty() || !icon.w) return 0;
    const int width = 74 + DockTextWidth(font, name);
    pla::PokemonRound(im, x, y, width, 48, 24, DockSlate);
    DockBlit(im, icon, x + 4, y + 4, 40, 40);
    PokemonText(im, font, name, 23, x + 58, y + 12, width - 64, DockCream);
    return width;
}
acnh::Image PokemonTypeDiamond(const PokemonPageArt &art, unsigned type) {
    const auto icon = art.Type(type), glyph = art.Type(type, true);
    const auto base = art.Move("btl_type_base_00^s");
    if (!icon.w || !glyph.w || !base.w) return {};
    std::array<unsigned, 32768> counts{};
    unsigned best = 0;
    for (size_t j = 0; j < icon.rgba.size(); j += 4) {
        const auto *p = icon.rgba.data() + j;
        if (p[3] <= 250 || (p[0] > 230 && p[1] > 230)) continue;
        const unsigned key = (unsigned(p[0] / 8) << 10) | (unsigned(p[1] / 8) << 5) | (p[2] / 8);
        if (++counts[key] > counts[best]) best = key;
    }
    const Color color{uint8_t(((best >> 10) & 31) * 8 + 4), uint8_t(((best >> 5) & 31) * 8 + 4),
                      uint8_t((best & 31) * 8 + 4), 255};
    acnh::Image out(60, 60);
    DockBlit(out, PokemonMask(base, color), 0, 0, 60, 60);
    // Native selectors put the approved glyph's G mask in decoded alpha.
    DockBlit(out, PokemonMask(glyph, {250, 248, 240, 255}, 3), 10, 10, 40, 40);
    return out;
}
bool PokemonFonts(Instance &i, const EdenDsmodHostApi &h) {
    if (!i.font.rgba.empty() && !i.secondary.rgba.empty() && !i.hero.rgba.empty()) return true;
    const auto bytes = Romfs(h, "bin/font/FOT-UDKakugoC80Pro-DB.BFOTF");
    return (!i.font.rgba.empty() || pla::BuildFont(bytes, i.font, 23)) &&
        (!i.secondary.rgba.empty() || pla::BuildFont(bytes, i.secondary, 21)) &&
        (!i.hero.rgba.empty() || pla::BuildFont(bytes, i.hero, 37));
}
void PokemonDescription(acnh::Image &im, const pla::Font &font, const std::string &description,
                        int x, int y, int width, unsigned lines) {
    std::string text = description;
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::string line;
    size_t at = 0;
    for (unsigned row = 0; row < lines && at < text.size(); ++row) {
        line.clear();
        if (row + 1 == lines) {
            line = text.substr(at);
            std::replace(line.begin(), line.end(), '\n', ' ');
        } else {
            while (at < text.size()) {
                const auto end = text.find_first_of(" \n", at);
                const auto word = text.substr(at, end == std::string::npos ? end : end - at);
                const auto next = line.empty() ? word : line + " " + word;
                if (!line.empty() && DockTextWidth(font, next) > width) break;
                line = next;
                if (end == std::string::npos) { at = text.size(); break; }
                at = end + 1;
            }
        }
        PokemonText(im, font, line, 21, x, y + int(row) * 34, width, DockInk);
    }
}
bool RenderPokemonPage(Instance &i, const EdenDsmodHostApi &h,
                        const pla::PokemonPageSnapshot &page, acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i, h)) return false;
    const auto paper = NativePaper(h, &i);
    if (!paper.w) return false;
    im = PokemonCrop(paper, 0, 0, 1104, 1080);
    PokemonPageArt art;
    if (!art.Load(h, page.tab == 1, &i)) {
        PokemonText(im, i.font, "Unavailable", 23, 480, 522, 596, DockInk);
        return true;
    }
    const auto card = art.Card("detail_base_like_passive_00^q");
    DockBlit(im, PokemonNine(card, 642, 1070, 14), 457, 5, 642, 1070);
    CardEdge(im, 457, 5, 1070);
    const auto catalogue = CachedRomfs(i, h, "bin/appli/res_pokemon/list/pokemon_info_list.bin");
    if (page.party.valid)
        for (unsigned slot = 0; slot < std::min(page.party.count, 6u); ++slot) {
            const auto &mon = page.party.slots[slot];
            if (!mon.valid) continue;
            const bool selected = slot == page.slot;
            const int y = 12 + int(slot) * 176;
            const Color color = selected ? DockCream : mon.fainted ? Color{82, 85, 90, 255}
                : Color{46, 52, 61, 240};
            acnh::Image row(416, 164);
            NativePlate(row, h, color, 28, &i);
            DockBlit(im, row, 36, y, 416, 164);
            DockBlit(im, art.disc, 44, y + 20, 124, 124);
            const auto portrait = PokemonPortrait(h, mon, false, catalogue, &i);
            DockBlit(im, portrait, 48, y + 24, 116, 116);
            const auto fg = selected ? DockInk : DockCream;
            const auto name = DockPokemonName(i.font, h, mon.species, mon.nicknamed, mon.nickname.c_str(), &i);
            PokemonText(im, i.font, name, 23, 182, y + 24, mon.fainted ? 166 : 240, fg);
            PokemonHp(im, 182, y + 72, 222, mon.hp, mon.max_hp, selected);
            PokemonText(im, i.font, "Lv. " + std::to_string(mon.level), 23, 312, y + 114, 112, fg, 1);
            if (mon.fainted) {
                DockBlit(im, PokemonMask(art.faint_pill, {52, 54, 58, 255}, 3), 174, y + 100, 150, 44);
                PokemonText(im, i.font, "FAINTED", 23, 174, y + 111, 150, DockCream, 2);
                DockBlit(im, art.faint_mark, 356, y + 14, 48, 48);
            } else {
                PokemonText(im, i.font, std::to_string(mon.hp) + "/" + std::to_string(mon.max_hp),
                            23, 182, y + 112, 142, fg);
            }
            if (selected) PokemonChevron(im, art, 6, y + 54);
        }
    if (!page.Valid()) {
        PokemonText(im, i.font, "Unavailable", 23, 480, 522, 596, DockInk);
        return true;
    }
    const auto &mon = page.party.slots[page.slot];
    const auto personal = pla::ReadPokemonPersonal(
        CachedRomfs(i, h, "bin/pml/personal/personal_data_total.perbin"), mon.species, mon.form);
    const auto type_names = CachedRomfs(i, h, "bin/message/English/common/typename.dat");
    DockBlit(im, PokemonBrush(art.brush, 612, 68, DockInk), 472, 18, 612, 68);
    PokemonText(im, i.hero, DockPokemonName(i.hero, h, mon.species, mon.nicknamed, mon.nickname.c_str(), &i),
                37, 508, 34, 418, DockCream);
    PokemonText(im, i.font, "Lv. " + std::to_string(mon.level), 23, 920, 42, 112, DockCream, 1);
    auto large = PokemonPortrait(h, mon, true, catalogue, &i);
    FeatherCutEdges(large);
    if (large.w && large.h) {
        const double scale = std::min(300.0 / large.w, 300.0 / large.h);
        const int width = int(large.w * scale), height = int(large.h * scale);
        DockBlit(im, large, 480 + (300 - width) / 2, 98 + (300 - height) / 2, width, height);
    }
    int type_y = 110, type_x = 790;
    if (personal.valid)
        for (unsigned type : {personal.type1, personal.type2}) {
            if (type == personal.type2 && type == personal.type1 && type_x != 790) break;
            const auto name = pla::PokemonTypeName(type_names, type);
            const int need = 74 + DockTextWidth(i.font, name);
            if (type_x + need > 1076) { type_x = 790; type_y += 60; }
            type_x += PokemonTypePill(im, art, i.font, type_names, type, type_x, type_y) + 10;
        }
    type_y += 72;
    PokemonText(im, i.secondary, "HP", 21, 792, type_y + 4, 38, DockInk);
    PokemonText(im, i.font, std::to_string(mon.hp) + "/" + std::to_string(mon.max_hp),
                23, 838, type_y, 230, DockInk);
    PokemonHp(im, 792, type_y + 40, 276, mon.hp, mon.max_hp, true);
    if (mon.fainted) {
        DockBlit(im, PokemonMask(art.faint_pill, {52, 54, 58, 255}, 3), 792, type_y + 72, 200, 48);
        DockBlit(im, art.faint_mark, 798, type_y + 74, 44, 44);
        PokemonText(im, i.font, "FAINTED", 23, 848, type_y + 84, 144, DockCream);
    }
    pla::PokemonRound(im, 480, 420, 596, 88, 14, DockSlate);
    const char *tabs[] = {"Stats", "Moves", "Weak"};
    for (unsigned tab = 0; tab < 3; ++tab) {
        const int x = 484 + int(tab) * 196;
        if (tab == page.tab) {
            pla::PokemonRound(im, x, 424, 196, 80, 10, {78, 84, 92, 255});
            const int width = DockTextWidth(i.font, tabs[tab]) + 20;
            DockRect(im, x + (196 - width) / 2, 490, width, 5, DockCream);
        }
        PokemonText(im, i.font, tabs[tab], 23, x, 450, 196, DockCream, 2);
    }
    if (page.tab == 2) return true; // Independently scrollable, owned Weak body image.
    if (!mon.detail_valid) {
        PokemonText(im, i.font, "Unavailable", 23, 484, 542, 588, DockInk);
        return true;
    }
    if (page.tab == 0) {
        const char *labels[] = {"HP", "Attack", "Defense", "Sp. Atk", "Sp. Def", "Speed"};
        const unsigned values[] = {mon.max_hp, mon.attack, mon.defense, mon.sp_attack, mon.sp_defense, mon.speed};
        for (unsigned stat = 0; stat < 6; ++stat) {
            const int x = 480 + int(stat % 2) * 302, y = 522 + int(stat / 2) * 128;
            DockBlit(im, PokemonStatPlate(art, 290, 112), x, y, 290, 112);
            PokemonText(im, i.secondary, labels[stat], 21, x + 18, y + 18, 200, DockInk);
            PokemonText(im, i.font, std::to_string(values[stat]), 23, x + 210, y + 16, 62, DockInk, 1);
            for (unsigned pip = 0; pip < 10; ++pip)
                pla::PokemonRound(im, x + 18 + int(pip) * 20, y + 64, 14, 26, 3,
                    mon.effort_valid && pip < mon.effort_levels[stat] ? Color{43, 216, 129, 255} : Color{198, 190, 160, 255});
            PokemonText(im, i.font, mon.effort_valid ? std::to_string(mon.effort_levels[stat]) : "—",
                        23, x + 234, y + 66, 38, DockInk, 1);
        }
        PokemonDots(im, art, 480, 910, 596);
        PokemonText(im, i.secondary, "Nature", 21, 484, 936, 96, DockInk);
        auto nature = pla::PokemonNatureName(CachedRomfs(i, h, "bin/message/English/common/seikaku.dat"), mon.nature);
        PokemonText(im, i.font, nature.empty() ? "Unavailable" : nature, 23, 592, 934, 240, DockInk);
        const int width = DockTextWidth(i.secondary, "Effort Levels");
        pla::PokemonRound(im, 1076 - width - 30, 932, 14, 26, 3, {43, 216, 129, 255});
        PokemonText(im, i.secondary, "Effort Levels", 21, 1076 - width, 936, width, DockInk);
        return true;
    }
    const auto names = CachedRomfs(i, h, "bin/message/English/common/wazaname.dat");
    const auto descriptions = CachedRomfs(i, h, "bin/message/English/common/wazainfo.dat");
    const auto learnset = pla::ReadPokemonLearnset(
        CachedRomfs(i, h, "bin/pml/waza_oboe/waza_oboe_total.wazaoboe"), mon.species, mon.form);
    std::array<pla::PokemonMove, 4> moves;
    acnh::Image row_end, pp_end;
    row_end = art.Move("btl_waza_base_00^s");
    pp_end = art.Move("btl_waza_pp_base_00^s");
    if (!row_end.w || !pp_end.w) {
        PokemonText(im, i.font, "Unavailable", 23, 484, 542, 588, DockInk);
        return true;
    }
    for (unsigned slot = 0; slot < 4; ++slot) {
        const int y = 522 + int(slot) * 100;
        acnh::Image row(596, 80);
        DockRect(row, 0, 0, 596, 80, slot == page.move ? Color{78, 84, 92, 242} : Color{52, 50, 58, 242});
        for (int yy = 0; yy < 80; ++yy)
            for (int xx = 0; xx < 56; ++xx) {
                const auto *p = row_end.At((55 - xx) * row_end.w / 56, yy * row_end.h / 80);
                row.At(540 + xx, yy)[3] = unsigned(p[0]) * p[3] / 255 * 242 / 255;
            }
        DockBlit(im, row, 480, y, 596, 80);
        char path[64]; std::snprintf(path, sizeof path, "bin/pml/waza/waza%04u.wazabin", mon.moves[slot]);
        moves[slot] = pla::ReadPokemonMove(CachedRomfs(i, h, path), mon.moves[slot]);
        if (!moves[slot].usable) moves[slot].valid = false;
        const auto &move = moves[slot];
        const auto name = pla::PokemonMoveName(names, mon.moves[slot]);
        if (move.valid) DockBlit(im, PokemonTypeDiamond(art, move.type), 486, y + 10, 60, 60);
        PokemonText(im, i.font, move.valid && !name.empty() ? name : "Unavailable", 23,
                    556, y + 28, 290, DockCream);
        const auto mastered = pla::PokemonMoveMastery(learnset, mon.mastered_record, mon.level, mon.moves[slot], mon.alpha_move);
        if (move.valid && mastered.has_value() && *mastered) {
            const auto mark = art.Move("btl_wazamode_icon_03^t");
            DockBlit(im, PokemonMask(mark, DockCream, 3), 852, y + 20, 44, 40);
        }
        const auto pp = PokemonBrush(pp_end, 150, 44, {30, 32, 36, 230});
        DockBlit(im, pp, 906, y + 18, 150, 44);
        const bool valid_pp = move.valid && mon.pp_ups[slot] == 0 && mon.pp[slot] <= move.pp;
        PokemonText(im, i.font, valid_pp ? std::to_string(mon.pp[slot]) + "/" + std::to_string(move.pp) : "—",
                    23, 906, y + 28, 150, DockCream, 2);
        if (slot == page.move) PokemonChevron(im, art, 450, y + 12);
    }
    const auto &move = moves[page.move];
    const auto name = pla::PokemonMoveName(names, mon.moves[page.move]);
    int y = 922;
    unsigned lines = 3;
    if (DockTextWidth(i.font, name) > 290) {
        PokemonText(im, i.font, name, 23, 484, y, 588, DockInk);
        y += 42; lines = 2;
    }
    if (!move.valid) {
        PokemonText(im, i.font, "Unavailable", 23, 484, y, 588, DockInk);
        return true;
    }
    int metadata_x = 484;
    const auto metric = [&](const char *label, const std::string &value) {
        const int label_width = DockTextWidth(i.secondary, label);
        PokemonText(im, i.secondary, label, 21, metadata_x, y + 2, label_width, DockInk);
        metadata_x += label_width + 8;
        const int width = DockTextWidth(i.font, value);
        PokemonText(im, i.font, value, 23, metadata_x, y, width, DockInk);
        metadata_x += width + 28;
    };
    metric("Pow", move.power ? std::to_string(move.power) : "—");
    metric("Acc", move.accuracy && move.accuracy <= 100 ? std::to_string(move.accuracy) : "—");
    const auto mastered = pla::PokemonMoveMastery(learnset, mon.mastered_record, mon.level, mon.moves[page.move], mon.alpha_move);
    if (move.can_style && mastered.has_value() && *mastered) {
        const auto agile = art.Move("btl_wazamode_icon_01^t");
        const auto strong = art.Move("btl_wazamode_icon_02^t");
        DockBlit(im, PokemonMask(agile, DockInk, 3), metadata_x, y - 8, 40, 40);
        PokemonText(im, i.font, move.agile_power ? std::to_string(move.agile_power) : "—",
                    23, metadata_x + 46, y, 68, DockInk);
        metadata_x += 132;
        DockBlit(im, PokemonMask(strong, DockInk, 3), metadata_x, y - 8, 40, 40);
        PokemonText(im, i.font, move.strong_power ? std::to_string(move.strong_power) : "—",
                    23, metadata_x + 46, y, 68, DockInk);
    }
    auto description = pla::PokemonMoveDescription(descriptions, mon.moves[page.move]);
    PokemonDescription(im, i.secondary, description.empty() ? "Unavailable" : description,
                        484, y + 48, 588, lines);
    return true;
}
bool RenderPokemonWeakBody(Instance &i, const EdenDsmodHostApi &h,
                            const pla::PokemonPageSnapshot &page, acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i, h)) return false;
    im = acnh::Image(596, 1600);
    PokemonPageArt art;
    if (!page.Valid() || !art.Load(h, false, &i)) {
        PokemonText(im, i.font, "Unavailable", 23, 4, 4, 588, DockInk);
        return true;
    }
    const auto &mon = page.party.slots[page.slot];
    const auto personal = pla::ReadPokemonPersonal(
        CachedRomfs(i, h, "bin/pml/personal/personal_data_total.perbin"), mon.species, mon.form);
    const auto match = page.chart_valid && personal.valid
        ? pla::PokemonTypeMatchups(page.chart, personal.type1, personal.type2) : pla::PokemonMatchups{};
    if (!match.valid) {
        PokemonText(im, i.font, "Unavailable", 23, 4, 4, 588, DockInk);
        return true;
    }
    const auto type_names = CachedRomfs(i, h, "bin/message/English/common/typename.dat");
    constexpr unsigned order[] = {0, 9, 10, 12, 11, 14, 1, 3, 4, 2, 13, 6, 5, 7, 15, 16, 8, 17};
    int y = 0;
    const char *labels[] = {"Weak to", "Resists", "No effect"};
    for (unsigned section = 0; section < 3; ++section) {
        std::vector<unsigned> types;
        for (const auto type : order) {
            const auto m = match.types[type];
            if ((section == 0 && m.numerator > m.denominator) ||
                (section == 1 && m.numerator && m.numerator < m.denominator) ||
                (section == 2 && !m.numerator)) types.push_back(type);
        }
        std::stable_sort(types.begin(), types.end(), [&](auto a, auto b) {
            const auto aa = match.types[a], bb = match.types[b];
            return section == 0 ? aa.numerator * bb.denominator > bb.numerator * aa.denominator
                                : aa.numerator * bb.denominator < bb.numerator * aa.denominator;
        });
        if (types.empty()) continue;
        PokemonText(im, i.secondary, labels[section], 21, 4, y + 4, 146, DockInk);
        PokemonDots(im, art, 150, y + 16, 446);
        y += 40;
        int x = 0;
        for (const auto type : types) {
            const auto m = match.types[type];
            const auto name = pla::PokemonTypeName(type_names, type);
            const std::string multiplier = !m.numerator ? "0×" : m.numerator == 5 && m.denominator == 2 ? "2.5×"
                : m.numerator == 2 && m.denominator == 5 ? "0.4×" : m.denominator == 2 ? "½×" : "2×";
            const int pill_width = 74 + DockTextWidth(i.font, name);
            const int need = pill_width + 30 + DockTextWidth(i.font, multiplier);
            if (x && x + need > 596) { x = 0; y += 60; }
            PokemonTypePill(im, art, i.font, type_names, type, x, y);
            PokemonText(im, i.font, multiplier, 23, x + pill_width + 8, y + 12,
                        need - pill_width - 8, section == 0 ? Color{168, 30, 36, 255} : DockInk);
            x += need;
        }
        y += 76;
    }
    return true;
}
