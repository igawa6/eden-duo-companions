// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Included after pla_pokemon_images.h, inside the module's private namespace.
// Pure owned Pokédex snapshot + native ROMFS art/text; no guest read or action.
#pragma once

struct PokedexPageText {
    std::vector<uint8_t> ui_dat, ui_tbl, names, category_dat, category_tbl;
    std::vector<uint8_t> full_dat, full_tbl, ongoing_dat, ongoing_tbl;
    std::vector<uint8_t> area_dat, area_tbl, move_dat, move_tbl, type_dat, type_tbl;
    void Load(Instance &i, const EdenDsmodHostApi &h, bool comments = false, bool task_words = false) {
        ui_dat = CachedRomfs(i, h, "bin/message/English/common/pokedex.dat");
        ui_tbl = CachedRomfs(i, h, "bin/message/English/common/pokedex.tbl");
        names = CachedRomfs(i, h, "bin/message/English/common/monsname.dat");
        type_dat = CachedRomfs(i, h, "bin/message/English/common/typename.dat");
        type_tbl = CachedRomfs(i, h, "bin/message/English/common/typename.tbl");
        if (comments) {
            category_dat = CachedRomfs(i, h, "bin/message/English/common/zkn_type.dat");
            category_tbl = CachedRomfs(i, h, "bin/message/English/common/zkn_type.tbl");
            full_dat = CachedRomfs(i, h, "bin/message/English/common/zukan_comment_A.dat");
            full_tbl = CachedRomfs(i, h, "bin/message/English/common/zukan_comment_A.tbl");
            ongoing_dat = CachedRomfs(i, h, "bin/message/English/common/zukan_comment_B.dat");
            ongoing_tbl = CachedRomfs(i, h, "bin/message/English/common/zukan_comment_B.tbl");
            area_dat = CachedRomfs(i, h, "bin/message/English/script/place_name.dat");
            area_tbl = CachedRomfs(i, h, "bin/message/English/script/place_name.tbl");
        }
        if (task_words) {
            move_dat = CachedRomfs(i, h, "bin/message/English/common/wazaname.dat");
            move_tbl = CachedRomfs(i, h, "bin/message/English/common/wazaname.tbl");
        }
    }
    pla::PokedexTextTable Ui() const { return {ui_dat, ui_tbl}; }
    std::string Ui(pla::PokedexUiLabel label) const {
        return pla::PokedexUiLabelText(label, Ui());
    }
    std::string Literal(const char *label) const {
        return pla::PokedexMessageLabel(Ui(), label, "msg_pokedex_max");
    }
};
struct PokedexPageArt {
    PokemonPageArt pokemon;
    std::vector<uint8_t> task, areas;
    acnh::Image plaque, height, weight, ball, dex_icon, check, double_box;
    bool Load(Instance &i, const EdenDsmodHostApi &h) {
        if (!pokemon.Load(h, false, &i)) return false;
        const auto unpack = [&](pla::PackedUiMember member) {
            const auto path = pla::PackedUiPath(member);
            return CachedBntx(i, std::string("packed-bntx:") + path + ":" +
                std::to_string(static_cast<unsigned>(member)), [&] {
                    return pla::SarcTexture(pla::DecodePackedUi(member, CachedRomfs(i, h, path)));
                });
        };
        task = unpack(pla::PackedUiMember::PokemonTask);
        areas = unpack(pla::PackedUiMember::PokedexAreas);
        const auto hud = unpack(pla::PackedUiMember::HudArcPhone);
        plaque = pokemon.Card("detail_deco_rd_00^q");
        height = pokemon.Card("detail_icon_hight^s");
        weight = pokemon.Card("detail_icon_weight^s");
        check = pokemon.Texture(task, PokemonPageArt::Source(pla::PackedUiMember::PokemonTask), "task_icon_check^s");
        double_box = pokemon.Texture(task, PokemonPageArt::Source(pla::PackedUiMember::PokemonTask), "task_icon_b^t");
        dex_icon = pokemon.Texture(hud, PokemonPageArt::Source(pla::PackedUiMember::HudArcPhone), "fieldinfo_icon_pokedex_00^q");
        constexpr auto ball_path = "bin/appli/icon_ball/icon_ball_1710.bntx";
        ball = CachedBntxTexture(i, CachedRomfs(i, h, ball_path), ball_path, "");
        return plaque.w && height.w && weight.w && !task.empty() && !areas.empty();
    }
};
// Some approved BC5 materials deliberately use the raw R plane, while their
// G plane becomes native alpha. A second alpha multiplication changes the art.
acnh::Image PokedexRawMask(const acnh::Image &src, Color color, unsigned channel) {
    acnh::Image out(src.w, src.h);
    if (channel > 3) return {};
    for (size_t j = 0; j < src.rgba.size(); j += 4) {
        std::copy(color.begin(), color.end(), out.rgba.begin() + j);
        out.rgba[j + 3] = unsigned(src.rgba[j + channel]) * color[3] / 255;
    }
    return out;
}
std::string PokedexNumber(unsigned number) {
    char text[16];
    std::snprintf(text, sizeof text, "%03u", number);
    return text;
}
unsigned PokedexTextLines(acnh::Image &im, const pla::Font &font, std::string text,
                          unsigned cap, int x, int y, int width, unsigned lines,
                          int pitch, Color color) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    size_t at = 0;
    unsigned rendered = 0;
    for (; rendered < lines && at < text.size(); ++rendered) {
        std::string line;
        if (rendered + 1 == lines) {
            line = text.substr(at);
            at = text.size();
        } else {
            while (at < text.size()) {
                while (at < text.size() && text[at] == ' ') ++at;
                if (at == text.size()) break;
                const auto end = text.find(' ', at);
                const auto word = text.substr(at, end == std::string::npos ? end : end - at);
                const auto next = line.empty() ? word : line + " " + word;
                if (!line.empty() && DockTextWidth(font, next) > width) break;
                line = next;
                if (end == std::string::npos) { at = text.size(); break; }
                at = end + 1;
            }
        }
        PokemonText(im, font, line, cap, x, y + int(rendered) * pitch, width, color);
    }
    return rendered;
}
std::optional<pla::Pokemon> PokedexPortraitVariant(const pla::PokedexFormState &form,
                                                 std::span<const uint8_t> catalogue) {
    if (!form.valid || !form.revealed || !form.species) return std::nullopt;
    const unsigned observed = form.obtained_flags | form.seen_flags;
    // Primary statistics flags: gender1 adds1, alpha adds2, shiny adds4.
    // Genderless uses gender0's flag; native icon metadata disambiguates it.
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (!(observed & (1u << bit))) continue;
        pla::Pokemon mon;
        mon.valid = true;
        mon.species = form.species;
        mon.form = form.form;
        mon.shiny = bit & 4;
        if (bit & 1) {
            if (pla::PokemonIcon(catalogue, mon.species, mon.form, 1, mon.shiny).empty()) continue;
            mon.gender = 1;
        } else {
            const auto male = pla::PokemonIcon(catalogue, mon.species, mon.form, 0, mon.shiny);
            const auto genderless = pla::PokemonIcon(catalogue, mon.species, mon.form, 2, mon.shiny);
            if (male.empty() && genderless.empty()) continue;
            if (!male.empty() && !genderless.empty() && male != genderless) return std::nullopt;
            mon.gender = male.empty() ? 2 : 0;
        }
        return mon;
    }
    // A revealed species keeps a list portrait even when the observed variant has no icon of
    // its own (e.g. only an alpha or a female sighting): fall back to the base male/genderless art.
    for (unsigned gender : {0u, 2u, 1u}) {
        if (pla::PokemonIcon(catalogue, form.species, form.form, gender, false).empty()) continue;
        pla::Pokemon mon;
        mon.valid = true;
        mon.species = form.species;
        mon.form = form.form;
        mon.gender = gender;
        return mon;
    }
    return std::nullopt;
}
pla::PokedexFormState PokedexListForm(const pla::PokedexPageSnapshot &page,
                                     const pla::PokedexEntry &entry) {
    if (!entry.valid || !entry.revealed) return {};
    if (entry.selected_form_revealed)
        return pla::ReadPokedexForm(page.data, page.index, entry.species, entry.selected_form);
    for (const auto &mapping : page.index.species[entry.species].forms) {
        const auto form = pla::ReadPokedexForm(page.data, page.index, entry.species, mapping.form);
        if (form.valid && form.revealed) return form;
    }
    return {};
}
acnh::Image PokedexPlaque(const acnh::Image &src, int w, int h) {
    if (!src.w || !src.h || w <= 84 || h <= 0) return {};
    const int scaled_width = std::max(85, src.w * h / src.h);
    acnh::Image scaled(scaled_width, h), out(w, h);
    DockBlit(scaled, src, 0, 0, scaled_width, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x < 70 ? x : x >= w - 14 ? scaled_width - (w - x)
                : 70 + (x - 70) * (scaled_width - 84) / (w - 84);
            std::memcpy(out.At(x, y), scaled.At(sx, y), 4);
        }
    return out;
}
void PokedexHeader(acnh::Image &im, const PokedexPageArt &art, Instance &i,
                    const pla::PokedexPageSnapshot &page, const PokedexPageText &text) {
    pla::PokemonRound(im, 12, 12, 324, 112, 14, {52, 78, 103, 250});
    if (art.dex_icon.w) {
        auto icon = PokemonMask(art.dex_icon, DockCream, 3);
        acnh::Image ink = art.dex_icon;
        for (size_t p = 0; p < ink.rgba.size(); p += 4) {
            ink.rgba[p + 3] = unsigned(ink.rgba[p + 3]) * (255 - ink.rgba[p]) / 255;
            std::copy_n(DockInk.begin(), 3, ink.rgba.begin() + p);
        }
        DockBlit(icon, ink, 0, 0, icon.w, icon.h);
        DockBlit(im, icon, 22, 34, 84, 68);
    }
    const auto global = pla::ReadPokedexHeaderCounts(page, 0);
    for (unsigned line = 0; line < 2; ++line) {
        PokemonText(im, i.secondary, text.Ui(line ? pla::PokedexUiLabel::Caught : pla::PokedexUiLabel::Seen),
                    21, 120, 30 + int(line) * 46, 110, DockCream);
        const auto value = global.valid ? std::to_string(line ? global.caught : global.seen) : "—";
        const int width = std::max(48, DockTextWidth(i.font, value));
        PokemonText(im, i.font, value, 23, 318 - width, 28 + int(line) * 46, width, DockCream, 1);
    }
    std::vector<unsigned> available;
    for (unsigned area = 1; area <= 5; ++area)
        if (page.area_available[area]) available.push_back(area);
    auto selected = std::find(available.begin(), available.end(), page.area);
    size_t start = selected == available.end() ? 0 : size_t(selected - available.begin());
    if (start && start + 1 == available.size()) --start;
    for (unsigned slot = 0; slot < 2 && start + slot < available.size(); ++slot) {
        const unsigned area = available[start + slot];
        const int x = 348 + int(slot) * 378;
        const bool active = area == page.area;
        pla::PokemonRound(im, x, 12, 366, 112, 14,
                          active ? Color{78, 84, 92, 245} : Color{46, 52, 61, 245});
        char key[64];
        std::snprintf(key, sizeof key, "pokedex_icon_area_%02u^s", area + 1);
        const auto glyph = art.pokemon.Texture(art.areas, PokemonPageArt::Source(pla::PackedUiMember::PokedexAreas), key);
        DockBlit(im, PokemonMask(glyph, DockCream, 3), x + 14, 26, 44, 44);
        const auto name = pla::PokedexAreaName(area, text.Ui(), {text.area_dat, text.area_tbl});
        PokemonText(im, i.font, name.empty() ? "Unavailable" : name, 23, x + 68, 30, 290, DockCream);
        const auto counts = pla::ReadPokedexHeaderCounts(page, area);
        int next = x + 68;
        for (const auto label : {pla::PokedexUiLabel::Seen, pla::PokedexUiLabel::Caught}) {
            const auto word = text.Ui(label);
            const int width = DockTextWidth(i.secondary, word);
            PokemonText(im, i.secondary, word, 21, next, 78, width, DockCream);
            next += width + 8;
            const auto value = counts.valid
                ? std::to_string(label == pla::PokedexUiLabel::Seen ? counts.seen : counts.caught) : "—";
            const int value_width = std::max(28, DockTextWidth(i.font, value));
            PokemonText(im, i.font, value, 23, next, 76, value_width, DockCream);
            next += value_width + 26;
        }
        if (active) DockRect(im, x + 14, 114, 338, 5, DockCream);
    }
}
bool RenderPokedexPage(Instance &i, const EdenDsmodHostApi &h,
                       const pla::PokedexPageSnapshot &page, acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i, h)) return false;
    const auto paper = NativePaper(h, &i);
    if (!paper.w) return false;
    im = PokemonCrop(paper, 0, 0, 1104, 1080);
    PokedexPageArt art;
    if (!art.Load(i, h)) {
        PokemonText(im, i.font, "Unavailable", 23, 480, 522, 596, DockInk);
        return true;
    }
    PokedexPageText text;
    text.Load(i, h, true);
    PokedexHeader(im, art, i, page, text);
    const auto card = art.pokemon.Card("detail_base_like_passive_00^q");
    DockBlit(im, PokemonNine(card, 642, 946, 14), 457, 129, 642, 946);
    CardEdge(im, 457, 129, 946);
    const auto catalogue = CachedRomfs(i, h, "bin/appli/res_pokemon/list/pokemon_info_list.bin");
    const auto species = pla::PokedexVisibleSpecies(page);
    const size_t start = std::min<size_t>(page.list_start, species.size() > 9 ? species.size() - 9 : 0);
    for (unsigned slot = 0; slot < 9 && start + slot < species.size(); ++slot) {
        const unsigned id = species[start + slot];
        const auto entry = pla::ReadPokedexEntry(page.data, page.index, id);
        const bool selected = id == page.species;
        const int y = 136 + int(slot) * 103;
        const auto fg = selected ? DockInk : DockCream;
        acnh::Image row(416, 92);
        NativePlate(row, h, selected ? DockCream : Color{62, 70, 80, 255}, 8, &i);
        DockBlit(im, row, 36, y, 416, 92);
        PokemonText(im, i.secondary, "No.", 21, 40, y + 35, 44, fg);
        PokemonText(im, i.font, PokedexNumber(page.index.species[id].hisui), 23, 84, y + 33, 58, fg);
        const auto name = pla::PokedexSpeciesName(page.data, page.index, id, text.names);
        if (entry.valid && entry.revealed) {
            auto mon = PokedexPortraitVariant(PokedexListForm(page, entry), catalogue);
            // A revealed species whose forms are not yet individually revealed (e.g. a
            // research-seen evolution) still shows its base-form icon, as its name is shown.
            for (const auto &mapping : page.index.species[id].forms) {
                if (mon.has_value()) break;
                for (unsigned gender : {0u, 2u, 1u}) {
                    if (pla::PokemonIcon(catalogue, id, mapping.form, gender, false).empty()) continue;
                    pla::Pokemon base;
                    base.valid = true; base.species = id; base.form = mapping.form; base.gender = gender;
                    mon = base;
                    break;
                }
            }
            if (mon.has_value()) {
                DockBlit(im, art.pokemon.disc, 142, y + 13, 66, 66);
                DockBlit(im, PokemonPortrait(h, *mon, false, catalogue, &i), 145, y + 16, 60, 60);
            }
        }
        PokemonText(im, i.font, name.empty() ? "??????" : name, 23, 216, y + 33, 148, fg);
        if (entry.valid && entry.revealed && entry.obtained) {
            const auto rate = pla::PokedexResearchRate(page.data, page.index, page.tasks, id);
            pla::PokemonRound(im, 368, y + 26, 40, 40, 8, selected ? DockInk : Color{231, 222, 178, 255});
            PokemonText(im, i.font, rate.has_value() ? std::to_string(std::min(*rate / 10, 10u)) : "—",
                        23, 368, y + 35, 40, selected ? DockCream : DockInk, 2);
            DockBlit(im, art.ball, 408, y + 24, 44, 44);
        }
        if (selected) PokemonChevron(im, art.pokemon, 2, y + 18);
    }
    if (species.empty() && page.unreported_only && page.data.valid)
        PokemonText(im, i.font, "No unreported research", 23, 36, 520, 416, DockInk, 2);
    if (species.size() > 9) {
        const int height = int(927 * 9 / species.size());
        const int y = 136 + int(927 * start / species.size());
        pla::PokemonRound(im, 448, y, 4, height, 2, {120, 128, 136, 255});
    }
    if (!page.Valid()) {
        PokemonText(im, i.font, "Unavailable", 23, 484, 534, 588, DockInk);
        return true;
    }
    const auto entry = page.Entry();
    const auto form = page.Form();
    const auto personal = pla::ReadPokemonPersonal(CachedRomfs(i, h, "bin/pml/personal/personal_data_total.perbin"),
                                                   entry.species, form.form);
    DockBlit(im, PokemonBrush(art.pokemon.brush, 612, 68, DockInk), 472, 142, 612, 68);
    const auto number = "No. " + PokedexNumber(page.index.species[entry.species].hisui);
    const int number_width = DockTextWidth(i.font, number);
    PokemonText(im, i.font, number, 23, 508, 166, number_width, DockCream);
    const auto name = pla::PokedexSpeciesName(page.data, page.index, entry.species, text.names);
    PokemonText(im, i.hero, name.empty() ? "Unavailable" : name, 37,
                524 + number_width, 158, 552 - number_width, DockCream);
    const auto mon = PokedexPortraitVariant(form, catalogue);
    if (mon.has_value()) {
        const auto portrait = PokemonPortrait(h, *mon, true, catalogue, &i);
        if (portrait.w && portrait.h) {
            const double scale = std::min(180.0 / portrait.w, 180.0 / portrait.h);
            const int width = int(portrait.w * scale), height = int(portrait.h * scale);
            DockBlit(im, portrait, 480 + (180 - width) / 2, 216 + (180 - height) / 2, width, height);
        }
    }
    const auto category = pla::PokedexCategoryName(entry, form, {text.category_dat, text.category_tbl});
    PokemonText(im, i.secondary, category.empty() ? "Unavailable" : category, 21, 676, 218, 400, DockInk);
    if (personal.valid && personal.present) {
        int x = 676;
        for (const auto type : {personal.type1, personal.type2}) {
            if (type == personal.type2 && type == personal.type1 && x != 676) break;
            x += PokemonTypePill(im, art.pokemon, i.font, text.type_dat, type, x, 250) + 8;
        }
    }
    if (entry.obtained) DockBlit(im, art.ball, 676, 306, 36, 40);
    PokemonText(im, i.font, text.Ui(entry.obtained ? pla::PokedexUiLabel::Caught : pla::PokedexUiLabel::Seen),
                23, entry.obtained ? 720 : 676, 314, entry.obtained ? 130 : 174, DockInk);
    DockBlit(im, PokedexPlaque(art.plaque, 222, 96), 854, 300, 222, 96);
    PokemonText(im, i.secondary, text.Ui(pla::PokedexUiLabel::ResearchLevel), 21, 854, 308, 222, DockInk, 2);
    const auto rate = pla::PokedexResearchRate(page.data, page.index, page.tasks, entry.species);
    const auto level = rate.has_value() ? std::to_string(std::min(*rate / 10, 10u)) : "—";
    PokemonText(im, i.hero, level, 37, 998, 344, 64, DockInk, 1);
    if (rate.has_value() && *rate >= 100)
        PokemonText(im, i.secondary, text.Ui(entry.perfect ? pla::PokedexUiLabel::Perfect : pla::PokedexUiLabel::Complete),
                    21, 858, 356, 140, DockInk);
    pla::PokemonRound(im, 480, 412, 596, 88, 14, DockSlate);
    const char *tabs[] = {"Entry", "Research", "Weak"};
    for (unsigned tab = 0; tab < 3; ++tab) {
        const int x = 484 + int(tab) * 196;
        if (tab == page.tab) {
            pla::PokemonRound(im, x, 416, 196, 80, 10, {78, 84, 92, 255});
            const int width = DockTextWidth(i.font, tabs[tab]) + 20;
            DockRect(im, x + (196 - width) / 2, 482, width, 5, DockCream);
        }
        PokemonText(im, i.font, tabs[tab], 23, x, 442, 196, DockCream, 2);
    }
    if (page.tab != 0) return true;
    const auto description = pla::PokedexEntryComment(entry, form,
        {{text.full_dat, text.full_tbl}, {text.ongoing_dat, text.ongoing_tbl}, text.Ui()});
    const unsigned lines = PokedexTextLines(im, i.font, description.empty() ? "Unavailable" : description,
                                           23, 484, 524, 586, 7, 46, DockInk);
    for (unsigned line = 0; line < lines; ++line)
        PokemonDots(im, art.pokemon, 480, 558 + int(line) * 46, 596);
    const int y = 538 + int(std::max(lines, 4u)) * 46;
    DockBlit(im, PokemonMask(art.height, DockInk, 3), 480, y, 44, 44);
    DockBlit(im, PokemonMask(art.weight, DockInk, 3), 780, y, 44, 44);
    // Species height/weight from the game's personal table (fields 20/21).
    const auto height = personal.valid ? pla::PokemonHeightText(personal.height_cm) : std::string{};
    const auto weight = personal.valid ? pla::PokemonWeightText(personal.weight_hg) : std::string{};
    PokemonText(im, i.secondary, text.Literal("msg_ui_pokedex_12_01"), 21, 532, y + 12, 106, DockInk);
    PokemonText(im, i.font, height.empty() ? "—" : height, 23, 644, y + 10, 122, DockInk);
    PokemonText(im, i.secondary, text.Literal("msg_ui_pokedex_12_02"), 21, 832, y + 12, 104, DockInk);
    PokemonText(im, i.font, weight.empty() ? "—" : weight, 23, 942, y + 10, 130, DockInk);
    return true;
}
void PokedexThreshold(acnh::Image &im, const PokedexPageArt &art, const pla::Font &font,
                       int x, int y, int width, unsigned threshold, bool reported,
                       bool unreported, bool next) {
    DockBlit(im, PokemonStatPlate(art.pokemon, width, 40), x, y, width, 40);
    if (next)
        for (int at = 0; at < width; ++at) {
            DockRect(im, x + at, y, 1, 3, DockInk);
            DockRect(im, x + at, y + 37, 1, 3, DockInk);
        }
    if (next || unreported)
        for (int at = 0; at < 40; ++at)
            if (next || at % 10 < 7) {
                DockRect(im, x, y + at, 3, 1, DockInk);
                DockRect(im, x + width - 3, y + at, 3, 1, DockInk);
            }
    if (unreported)
        for (int at = 0; at < width; ++at)
            if (at % 10 < 7) {
                DockRect(im, x + at, y, 1, 3, DockInk);
                DockRect(im, x + at, y + 37, 1, 3, DockInk);
            }
    if (reported) DockBlit(im, PokemonMask(art.check, {227, 50, 52, 255}, 3), x - 4, y, 40, 40);
    PokemonText(im, font, std::to_string(threshold), 23, x + 4, y + 9, width - 8, DockInk, 1);
}
bool RenderPokedexResearchBody(Instance &i, const EdenDsmodHostApi &h,
                               const pla::PokedexPageSnapshot &page, acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i, h)) return false;
    im = acnh::Image(596, 1600);
    PokedexPageArt art;
    if (!page.Valid() || !page.tasks.valid || !page.Entry().ever_updated || !art.Load(i, h)) {
        PokemonText(im, i.font, "Unavailable", 23, 4, 14, 588, DockInk);
        return true;
    }
    PokedexPageText text;
    text.Load(i, h, false, true);
    const auto entry = page.Entry();
    const pla::PokedexTaskTextTables words{text.Ui(), {text.move_dat, text.move_tbl}, {text.type_dat, text.type_tbl}};
    const auto &tasks = page.tasks.species[entry.species];
    if (tasks.empty() || tasks.size() > 10) {
        PokemonText(im, i.font, "Unavailable", 23, 4, 14, 588, DockInk);
        return true;
    }
    for (unsigned slot = 0; slot < tasks.size(); ++slot) {
        const int y = 14 + int(slot) * 132;
        const auto &task = tasks[slot];
        const auto label = pla::PokedexTaskLabel(entry, task, words);
        PokemonDots(im, art.pokemon, 0, y - 8, 596);
        const bool bonus = task.points_single && task.points_bonus == task.points_single;
        DockBlit(im, PokemonMask(art.double_box, bonus ? Color{227, 50, 52, 255}
                                                       : Color{216, 212, 198, 255}, 3), 0, y + 6, 40, 40);
        if (bonus) DockBlit(im, PokedexRawMask(art.double_box, DockCream, 0), 0, y + 6, 40, 40);
        PokedexTextLines(im, i.font, label.empty() ? "Unavailable" : label,
                         23, 52, y + 8, 540, 2, 30, DockInk);
        const auto progress = pla::ReadPokedexNativeTaskProgress(page.data, page.index, page.tasks, entry.species, slot);
        if (label.empty() || !progress.valid || !progress.current_available) {
            PokemonText(im, i.font, "—", 23, 4, y + 80, 588, DockInk);
            continue;
        }
        const auto count = std::to_string(progress.current);
        const int count_width = std::max(56, DockTextWidth(i.font, count));
        PokemonText(im, i.font, count, 23, 0, y + 80, count_width, DockInk, 1);
        const int first = count_width + 18;
        const int pitch = std::min(78, (448 - first) / int(task.thresholds.size()));
        const unsigned reported = std::min<unsigned>(progress.reported_thresholds, task.thresholds.size());
        const unsigned achieved = std::min<unsigned>(progress.achieved_thresholds, task.thresholds.size());
        std::optional<unsigned> next;
        for (unsigned threshold = 0; threshold < task.thresholds.size(); ++threshold) {
            const bool is_next = !next.has_value() && threshold >= achieved;
            if (is_next) next = task.thresholds[threshold];
            PokedexThreshold(im, art, i.font, first + int(threshold) * pitch, y + 72, pitch - 6,
                             task.thresholds[threshold], threshold < reported,
                             threshold >= reported && threshold < achieved, is_next);
        }
        if (achieved > reported) {
            pla::PokemonRound(im, 448, y + 68, 148, 48, 12, DockInk);
            PokemonText(im, i.secondary, text.Literal("msg_ui_pokedex_15_01"),
                        21, 452, y + 82, 140, DockCream, 2);
        } else if (next.has_value()) {
            PokemonText(im, i.font, std::to_string(*next - std::min(*next, progress.current)) + " more",
                        23, 448, y + 80, 148, DockInk, 1);
        }
    }
    PokemonDots(im, art.pokemon, 0, 14 + int(tasks.size()) * 132 - 8, 596);
    im = PokemonCrop(im, 0, 0, 596, std::max(556, 14 + int(tasks.size()) * 132));
    return true;
}
bool RenderPokedexWeakBody(Instance &i, const EdenDsmodHostApi &h,
                           const pla::PokedexPageSnapshot &page, acnh::Image &im) {
    std::lock_guard lock(i.font_mutex);
    if (!PokemonFonts(i, h)) return false;
    im = acnh::Image(596, 1600);
    PokemonPageArt art;
    if (!page.Valid() || !art.Load(h, false, &i)) {
        PokemonText(im, i.font, "Unavailable", 23, 4, 4, 588, DockInk);
        return true;
    }
    const auto personal = pla::ReadPokemonPersonal(
        CachedRomfs(i, h, "bin/pml/personal/personal_data_total.perbin"), page.species, page.form);
    const auto match = page.chart_valid && personal.valid && personal.present
        ? pla::PokemonTypeMatchups(page.chart, personal.type1, personal.type2) : pla::PokemonMatchups{};
    if (!match.valid) {
        PokemonText(im, i.font, "Unavailable", 23, 4, 4, 588, DockInk);
        return true;
    }
    const auto type_names = CachedRomfs(i, h, "bin/message/English/common/typename.dat");
    constexpr unsigned order[] = {0, 9, 10, 12, 11, 14, 1, 3, 4, 2, 13, 6, 5, 7, 15, 16, 8, 17};
    int y = 10;
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
    im = PokemonCrop(im, 0, 0, 596, std::max(556, y));
    return true;
}
