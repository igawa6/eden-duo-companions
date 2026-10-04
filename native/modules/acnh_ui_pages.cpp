// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// module:acnh/ui/<recipe>: the chrome of the round-3 mocks, one image per element so the
// manifest places them with plain Image widgets (text stays in Label/Value widgets, except three
// short game strings baked into their art: DIY "New!", the passport header, the "x2" badge).
// Every recipe is the matching acnh_mock.py code (function named in the comment); colours are
// DATA (material / FLMC of the named pane), LIVE (sampled from the game's screen) or LAYOUT.
//
// Common parameters: w, h (size), d (diameter), s (box), c (colour rrggbb[aa]), on (0/1).
// Any ui/ key also takes sh=<rgba>&dy=<px>: the element's drop shadow (acnh_mock.paste sh/dy),
// returned in the same image (height grows by |dy|; the element keeps its top-left), and
// sc=<rgba>: the element's silhouette alone (acnh_mock.shadow_of), i.e. the exact shadow image of
// any recipe for a separate widget (applied before sh).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "acnh_catalog.h"
#include "acnh_font.h"
#include "acnh_lyt.h"
#include "acnh_romfs.h"
#include "acnh_tex.h"
#include "acnh_ui_prims.h"

namespace acnh {
namespace ui {

Image Card(Art& art, double w, double h, Rgba colour, double r) {
    return draw::MaskTint(draw::Nine(art.Tex("LMenuDeviceBtn", "DeviceBtnBase^s"), 52, w, h, r),
                          colour);
}

Image Capsule(Art& art, double w, double h, Rgba colour) {
    return draw::MaskTint(draw::Pill(art.Tex("LBookCatBtnS_00", "CmnGuideCorner^s"), w, h), colour);
}

Image Disc(Art& art, double d, Rgba colour) {
    const int n = static_cast<int>(std::nearbyint(d));
    return draw::MaskTint(draw::Resize(art.Tex("LPocketBtn", "Maru128_00^s"), n, n), colour);
}

Image Chevron(Art& art, Rgba colour, double h, bool right) {
    const Image a = draw::CropAlpha(draw::MaskTint(art.Tex("LArrowBtn", "ListArrow^s"), colour));
    if (a.Empty())
        return a;
    Image out{a.w, a.h * 2 - 8};
    draw::Composite(out, draw::FlipV(a), 0, 0);
    draw::Composite(out, a, 0, a.h - 8);
    if (!right)
        out = draw::FlipH(out);
    return draw::Fit(out, h, h);
}

Image Stripes(Art& art, const Image& shape, double period, Rgba black, Rgba white) {
    const int p = static_cast<int>(std::nearbyint(period));
    const Image tile = draw::Tile(
        shape.w, shape.h, draw::Resize(art.Tex("LPocketBtn", "DeviceAppPattern04^r"), p, p));
    return draw::Masked(draw::Tint(tile, black, white), shape);
}

Image Stripes(Art& art, const Image& shape, double period) {
    return Stripes(art, shape, period, C("ffbb00ff"), C("f38500ff"));
}

Image LcmnBg(Art& art, int w, int h) {
    Image bg{w, h};
    const Rgba cream = C(Cream);
    for (size_t p = 0; p < bg.rgba.size(); p += 4)
        std::memcpy(&bg.rgba[p], &cream, 4);
    Image wash{w, h};
    const Rgba wc = C("bfa46738");
    for (size_t p = 0; p < wash.rgba.size(); p += 4)
        std::memcpy(&wash.rgba[p], &wc, 4);
    draw::Composite(bg, wash, 0, 0);
    draw::Composite(
        bg, draw::Tile(w, h, draw::MaskTint(art.Tex("LCmnBg", "CmnBgDot^s"), C("fff4c2"))), 0, 0);
    return bg;
}

// page_map: content card = sea (LIVE colour) + MapBGPattern00^s wave rows (#12907050,
// P_SeaPattern_*) in 30 px bands every 140 px (LAYOUT, like the live screen).
Image PanelSea(Art& art, int w, int h) {
    Image sea = Card(art, w, h, C(SeaLive), 46);
    const Image waves =
        draw::Tile(sea.w, sea.h,
                   draw::MaskTint(draw::Resize(art.Tex("MenuMapFull", "MapBGPattern00^s"), 40, 28),
                                  C("12907050")));
    for (int yy = 0; yy < h; yy += 140) {
        // Only these rows carry waves; masking a full-page transparent image per band used
        // several extra canvas buffers and scanned the entire page for each 30-pixel strip.
        const int y1 = std::min(yy + 30, h);
        const Image band = draw::Crop(waves, 0, yy, sea.w, y1);
        const Image mask = draw::Crop(sea, 0, yy, sea.w, y1);
        draw::Composite(sea, draw::Masked(band, mask), 0, yy);
    }
    return sea;
}

// page_critters: W_BaseMatrix_00 = MenuBook#BookBg^w paper in a rounded card.
Image PanelBook(Art& art, int w, int h) {
    return draw::Masked(draw::Resize(art.Tex("MenuBook", "BookBg^w"), w, h),
                        Card(art, w, h, C("ffffff"), 46));
}

// page_diy: MenuRecipe P_Tex_00 = TexPaper^r (#eaeef1 -> #ffffff) tiled, rounded card.
Image PanelPaper(Art& art, int w, int h) {
    const Image paper = draw::Tint(draw::Tile(w, h, art.Tex("MenuRecipe", "TexPaper^r")),
                                   C("eaeef1ff"), C("ffffffff"));
    return draw::Masked(paper, Card(art, w, h, C("ffffff"), 46));
}

} // namespace ui

namespace {
using ui::C;
using ui::Capsule;
using ui::Card;
using ui::Disc;

int R(double v) {
    return static_cast<int>(std::nearbyint(v));
}

Image Blank(double w, double h) {
    return Image{std::max(1, R(w)), std::max(1, R(h))};
}

/// acnh_mock.txt_img for one ParkExt glyph: rendered at `px` per em, ink-cropped.
Image Glyph(Art& art, char32_t cp, double px, Rgba colour) {
    std::string s;
    AppendUtf8(s, cp);
    Image out;
    art.Font().Text("parkext", s, px, colour, out);
    return out;
}

// The game's SystemB_00_<lang> font packs for CNzh / TWzh / KRko put that language's face
// (DFP_GB_Y9_0 / DFPT_Y8 / AsiaKSDNR-B) after Seurat-B, and the page's atlas takes code points from
// U+2E80 from it first (acnh_font.h). Seurat-B has kana + JIS kanji only, so a baked CNzh text such
// as the passport header MenuProfile 0501 "护照" kept only "照" (round 7). A baked text with any
// code point from U+2E80 is drawn in that language's face (it has Latin too; a mixed Latin + CJK
// text then draws its Latin in the CJK face as well: LEAD, the fcpx per-face ranges are not
// decoded).
std::string_view CjkFace(Lang l) {
    return l == Lang::CNzh ? "ui_cn" : l == Lang::TWzh ? "ui_tw" : l == Lang::KRko ? "ui_kr" : "";
}

bool HasCjk(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (size_t k = 1; k < n && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        if (cp >= 0x2E80)
            return true;
        i += n;
    }
    return false;
}

Image TextImg(Art& art, std::string_view text, double px, Rgba colour,
              std::string_view face = "ui") {
    Image out;
    if (face == "ui") {
        const std::string_view cjk = CjkFace(art.Language());
        if (!cjk.empty() && HasCjk(text) && art.Font().Text(cjk, text, px, colour, out))
            return out;
    }
    art.Font().Text(face, text, px, colour, out);
    return out;
}

// ---- the app buttons -------------------------------------------------------------------------
// acnh_mock.app_button: LMenuDeviceBtn N_NonSelect_00 at LMenuDeviceBtn_Type frame (P_Base colour
// FLMC + icon FLTP). Mile-type frames (1, 11): the masked base (P_Base_02) is approximated with
// DeviceIconMileCircle^w (texmap 1), as in the mock. Frame <-> app pairing: UNVERIFIED (catalog).
Image AppButtonRaw(Art& art, int frame) {
    const auto l = art.Lyt("LMenuDeviceBtn");
    if (!l)
        return {};
    Layout::ComposeOptions o;
    o.anim = "LMenuDeviceBtn_Type";
    o.frame = static_cast<float>(frame);
    o.canvas_w = o.canvas_h = 200;
    if (frame == 1 || frame == 11)
        o.hide = {"P_Base_02"};
    Image im = l->Compose("N_NonSelect_00", o);
    if (frame == 1 || frame == 11) {
        Image bg{200, 200};
        draw::Composite(bg,
                        draw::Resize(art.Tex("LMenuDeviceBtn", "DeviceIconMileCircle^w"), 150, 150),
                        25, 25);
        draw::Composite(bg, im, 0, 0);
        im = std::move(bg);
    }
    return im;
}

constexpr std::array<std::pair<const char*, const char*>, 6> Tabs{{
    // label (own word), colour = NookPhone app base colour (LMenuDeviceBtn P_Base_<App> / _Type
    // FLMC)
    {"Map", "37ab7f"},
    {"Bag", "ef6372"},
    {"Critters", "ed9a21"},
    {"DIY", "c54a23"},
    {"Today", "4154df"},
    {"Phone", "8847e5"},
}};

// acnh_mock.tab_icon
Image TabIcon(Art& art, int k, double size) {
    static constexpr int frames[6] = {5, 0, 19, 2, 1, 0};
    if (frames[k]) {
        Image b;
        if (!BuildAppButton(art, frames[k], b))
            return {};
        return draw::Fit(b, size, size);
    }
    Image base = Card(art, 150, 150, C(Tabs[k].second), 46);
    Image g;
    if (k == 1) // LRecipeCategory#IconCatGoodsBag^s cream on the Custom Designs squircle
        g = draw::Fit(draw::MaskTint(art.Tex("LRecipeCategory", "IconCatGoodsBag^s"), C(ui::Cream)),
                      100, 100);
    else // MainScreenSmartPhone#IconSmartPhone^t: ^t keeps the ring in its colour channel -> b/w
         // tint
        g = draw::Fit(
            draw::CropAlpha(draw::Tint(art.Tex("MainScreenSmartPhone", "IconSmartPhone^t"),
                                       C("5b2fa000"), C("fff1c3ff"))),
            104, 104);
    draw::PasteC(base, g, 75, 75);
    return draw::Fit(base, size, size);
}

// ---- weather (acnh_mock.weather_icon) ---------------------------------------------------------
// HHP time editor LTimeEditCheckBtn: P_Sunny_00 = TimeEditorIcon04^s #ab5b00; N_Cloudy_00 =
// TimeEditorIcon05^s + 4 Maru20_00^s drops #3f3f3f. Pattern -> icon by WeatherPatternParam label
// prefix (Fine*/Commun/EventDay = sunny, Cloud/Rain = cloudy-rain): LEAD (the hourly table is
// code).
bool WeatherIcon(Art& art, int pattern, double size, Image& out) {
    const Bcsv* t = art.Cat().Table("WeatherPatternParam");
    if (!t)
        return false;
    std::string label;
    for (size_t r = 0; r < t->Rows(); ++r)
        if (static_cast<int>(t->U(r, Bcsv::Key("UniqueID", "u16"))) == pattern)
            label = t->Str(r, Bcsv::Key("Label", "string32"));
    if (label.empty())
        return false;
    const bool sunny = label.starts_with("Fine0") || label.starts_with("Commun") ||
                       label.starts_with("EventDay") ||
                       (label.starts_with("Fine") && label.find("Rain") == std::string::npos &&
                        label.find("Cloud") == std::string::npos);
    const double s = size / 64;
    out = Blank(size, size);
    if (sunny) {
        draw::PasteC(out,
                     draw::Resize(draw::MaskTint(art.Tex("LTimeEditCheckBtn", "TimeEditorIcon04^s"),
                                                 C("ab5b00")),
                                  R(size), R(size)),
                     size / 2, size / 2);
    } else {
        const Rgba c = C("3f3f3f");
        draw::PasteC(
            out,
            draw::Resize(draw::MaskTint(art.Tex("LTimeEditCheckBtn", "TimeEditorIcon05^s"), c),
                         R(64 * s), R(33 * s)),
            size / 2, size / 2 - 13 * s);
        static constexpr int drops[4][2] = {{-12, -10}, {-4, -21}, {8, -10}, {16, -21}};
        for (const auto& d : drops)
            draw::PasteC(
                out,
                draw::Resize(draw::MaskTint(art.Tex("LTimeEditCheckBtn", "Maru20_00^s"), c),
                             R(9 * s), R(9 * s)),
                size / 2 + d[0] * s, size / 2 - d[1] * s);
    }
    return true;
}

// ---- weather now by wx.type (round 7) ----------------------------------------------------------
// The game's own weather art is its TV forecast programme (romfs Model/FtrTVProgramWeather<AB>,
// AB = today/tomorrow of F fine, C cloudy, R rain, S snow; TVProgram.bcsv). Each archive's
// TVProgram_Alb.1 (128 x 64, BC1) is the forecast screen with one big symbol: FF sun, CC cloud,
// RR umbrella, SS snowman + snow crystal, at the same place (inside x 26..62, y 13..49). wx.type
// (GetWeatherType's mWeatherType enum: 0 clear, 1 fine, 2 cloudy, 3 rain clouds, 4 rain, 5 heavy
// rain, 6 snow, 7 heavy snow) picks the programme by the enum's own words: 0/1 -> F, 2/3 -> C,
// 4/5 -> R, 6/7 -> S (the forecast letter the code picks from mReport is not decoded: LEAD).
// The symbol is shown as the TV shows it (its screen square, rounded): no matting.
bool WeatherTv(Art& art, int type, double size, Image& out) {
    static constexpr const char* prog[8] = {"FF", "FF", "CC", "CC", "RR", "RR", "SS", "SS"};
    if (type < 0 || type > 7)
        return false;
    const auto arc = art.Rom().Model(std::string{"FtrTVProgramWeather"} + prog[type]);
    if (!arc)
        return false;
    Image tv;
    for (const auto& [name, bytes] : arc->members)
        if (BfresBntxDecode(bytes, "TVProgram_Alb.1", tv))
            break;
    if (tv.Empty() || tv.w < 64 || tv.h < 50)
        return false;
    const Image sq = draw::Resize(draw::Crop(tv, 26, 13, 62, 49), R(size), R(size));
    Image mask = Blank(size, size);
    draw::FillRoundRect(mask, 0, 0, size, size, size * 0.2, C("ffffff"));
    out = draw::Masked(sq, mask);
    return true;
}

// ---- zodiac / passport (acnh_mock.zodiac, page_today passport block) --------------------------
// Standard sign boundaries (LEAD: the code's date->sign table is not located).
std::string Zodiac(int m, int d) {
    static constexpr struct {
        int m, d;
        const char* sign;
    } table[] = {{1, 20, "Aquarius"}, {2, 19, "Pisces"},       {3, 21, "Aries"},
                 {4, 20, "Taurus"},   {5, 21, "Gemini"},       {6, 22, "Cancer"},
                 {7, 23, "Leo"},      {8, 23, "Virgo"},        {9, 23, "Libra"},
                 {10, 24, "Scorpio"}, {11, 23, "Sagittarius"}, {12, 22, "Capricorn"}};
    std::string sign = "Capricorn";
    for (const auto& z : table)
        if (m > z.m || (m == z.m && d >= z.d))
            sign = z.sign;
    return sign;
}

int FrameForTexture(const Layout& l, std::string_view anim, std::string_view material,
                    std::string_view tex, int frames = 24) {
    for (int f = 0; f < frames; ++f)
        if (l.AnimTexture(anim, static_cast<float>(f), material) == tex)
            return f;
    return 0;
}

bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// ---- DIY recipe card (acnh_mock.recipe_card) ----------------------------------------------------
bool RecipeCard(Art& art, const ArtKey& key, uint16_t uid, Image& out) {
    const Catalog::Recipe* rc = nullptr;
    for (const auto& r : art.Cat().Recipes())
        if (r.uid == uid)
            rc = &r;
    const auto l = art.Lyt("LRecipeBtn");
    if (!rc || !l)
        return false;
    const double w = key.Num("w", 120);
    const double k = w / 220, h = 324 * k;
    const float f = static_cast<float>(rc->board);
    const char* A = "LRecipeBtn_BoardColor";
    out = Image{static_cast<int>(w + 12 * k), static_cast<int>(h + 14 * k)};
    const double ox = static_cast<int>(6 * k), oy = static_cast<int>(2 * k);
    // W_FrameSh_00 (#131211, R12 shadow corner) + W_Frame_00 #f2e6c7 (WorkBenchCardCornerR12^s)
    draw::Paste(out,
                draw::Win(draw::Resize(art.Tex("LRecipeBtn", "WorkBenchCardCornerR12Sh^s"),
                                       R(24 * k), R(24 * k)),
                          w + 6 * k, h + 6 * k, C("13121150")),
                ox - 3 * k, oy + 3 * k);
    const int cr = std::max(2, R(14 * k));
    draw::Paste(out,
                draw::Win(draw::Resize(art.Tex("LRecipeBtn", "WorkBenchCardCornerR12^s"), cr, cr),
                          w, h, C(key.Flag("sel") ? "fff1c3" : "f2e6c7")),
                ox, oy);
    const double fw = 200 * k, fh = 304 * k, fx = ox + 10 * k, fy = oy + 10 * k;
    Image face;
    if (l->AnimVisible(A, f, "N_CardDIY_00", false)) {
        Rgba b = C("00000000"), wc = C("000000ff");
        l->AnimMaterial(A, f, "P_CardDIY_00", b, wc);
        face = draw::Tint(draw::Crop(draw::Resize(art.Tex("LRecipeBtn", "WorkBenchCardPatternA^r"),
                                                  R(fw * 2), R(fw * 2)),
                                     0, 0, static_cast<int>(fw), static_cast<int>(fh)),
                          b, wc);
    } else if (l->AnimVisible(A, f, "N_CardNook_00", false)) {
        face = draw::Resize(art.Tex("LRecipeBtn", "AnimalPatternColor^_D"), R(fw), R(fh));
    } else if (l->AnimVisible(A, f, "N_CardTapa_00", false)) {
        Rgba b = C("84848800"), wc = C("75757bff");
        l->AnimMaterial(A, f, "P_CardTapa_00", b, wc);
        face =
            draw::Tint(draw::Tile(static_cast<int>(fw), static_cast<int>(fh),
                                  draw::Resize(art.Tex("LRecipeBtn", "WorkBenchCardPatternTapa^r"),
                                               R(fw), R(fw * 84 / 276))),
                       b, wc);
    } else {
        Rgba b = C("bdbdbd00"), wc = C("bdbdbdff");
        l->AnimMaterial(A, f, "P_CardCook_00", b, wc);
        face = draw::Tint(
            draw::Resize(art.Tex("LRecipeBtn", "KitchenCardPatternRecipe^r"), R(fw), R(fh)), b, wc);
    }
    const int fr = std::max(2, R(10 * k));
    const Image rm =
        draw::Win(draw::Resize(art.Tex("LRecipeBtn", "WorkBenchCardCornerR12^s"), fr, fr), fw, fh,
                  C("ffffff"));
    draw::Paste(out, draw::Masked(face, rm), fx, fy);
    // card art (item icon resolver 0x19f6a60): "Layout_DIYRecipeIcon_<item label>" for a DIY
    // recipe, "Layout_CookingIcon_<label>" for cooking, "Layout_%s_%s_Remake_%d_%d" for the rows
    // with column 0xad0c787a set (variant 0_0 here: LEAD); P_Icon_00 = 256 x 256 at (0, +8)
    const Catalog::Item* item = art.Cat().FindItem(rc->item);
    std::vector<std::string> names;
    if (item && !item->label.empty()) {
        const bool cooking = rc->data_type == Crc32("Cooking");
        const std::string fam = cooking ? "CookingIcon" : "DIYRecipeIcon";
        if (rc->remake_art)
            names.push_back("Layout_" + fam + "_" + item->label + "_Remake_0_0");
        names.push_back("Layout_" + fam + "_" + item->label);
    }
    for (const auto& n : art.Cat().ItemIcons(rc->item, true))
        names.push_back(n);
    for (const auto& name : names) {
        const Image ic = art.ModelIcon(name);
        if (ic.Empty())
            continue;
        draw::PasteC(out, draw::Fit(ic, 256 * k, 256 * k), ox + w / 2, oy + h / 2 - 8 * k,
                     C("00000030"), std::max(2.0, 4 * k));
        break;
    }
    // customise brush (LRecipeBtn anim "Remake" frame 0 when card +0xbd: the result item has a
    // ItemRemake row, i.e. ItemParam.RemakeID != -1): P_IconRemake_00 56 x 56 at (-73, +124)
    if (item && item->remake >= 0)
        draw::PasteC(out,
                     draw::Resize(draw::Tint(art.Tex("LRecipeBtn", "WorkBenchCardRemakeMark^t"),
                                             C("bdbdbd00"), C("252525ff")),
                                  R(56 * k), R(56 * k)),
                     ox + w / 2 - 73 * k, oy + h / 2 - 124 * k);
    if (key.Flag("fav"))
        draw::PasteC(out,
                     draw::Resize(draw::Tint(art.Tex("LRecipeBtn", "PocketMenuFavoriteMark^t"),
                                             C("6d3f0000"), C("ff9400ff")),
                                  R(66 * k), R(66 * k)),
                     ox + w / 2 + 70 * k, oy + h / 2 - 122 * k);
    // N_Align_01 (56 x 56 at (-70, -124), #252525): the made item is in the pockets / storage
    if (key.Flag("pocket") || key.Flag("stor"))
        draw::PasteC(out,
                     draw::Resize(draw::Tint(art.Tex("LRecipeBtn", key.Flag("pocket")
                                                                       ? "WorkBenchPocketMark^t"
                                                                       : "WorkBenchCardStorage^t"),
                                             C("bdbdbd00"), C("252525ff")),
                                  R(56 * k), R(56 * k)),
                     ox + w / 2 - 70 * k, oy + h / 2 + 124 * k);
    if (key.Flag("made")) // N_State_01 / P_CheckState_00
        draw::PasteC(out,
                     draw::Resize(draw::Tint(art.Tex("LRecipeBtn", "WorkBenchCardCheckMark^t"),
                                             C("bdbdbd00"), C("252525ff")),
                                  R(56 * k), R(56 * k)),
                     ox + w / 2 + 72 * k, oy + h / 2 + 124 * k);
    if (key.Flag("can") || key.Flag("both")) {
        const Image g = key.Flag("can") ? art.Tex("LRecipeBtn", "IconCatCanMake_64^s")
                                        : art.Tex("LRecipeCategory", "IconCatCanMakeBoth^s");
        draw::PasteC(out,
                     draw::Resize(draw::MaskTint(art.Tex("LRecipeBtn", "WorkBenchCardNewBase^s"),
                                                 C("ffa100f0")),
                                  R(100 * k), R(100 * k)),
                     ox + w / 2 + 54 * k, oy + h / 2 + 60 * k);
        draw::PasteC(out, draw::Fit(draw::MaskTint(g, C("281f0b")), 56 * k, 56 * k),
                     ox + w / 2 + 54 * k, oy + h / 2 + 60 * k);
    }
    if (key.Flag("new")) { // W_NewBase_00 #ff1811 + MenuRecipe 0401 "New!"
        draw::Paste(out,
                    draw::MaskTint(
                        draw::Pill(art.Tex("LRecipeBtn", "WorkBenchNewBaseL^s"), 150 * k, 56 * k),
                        C("ff1811")),
                    ox - 4 * k, oy + 10 * k);
        std::string t = art.Cat().Text(art.Language(), "LayoutMsg/MenuRecipe", "0401");
        if (t.empty())
            t = "New!";
        const Image ti = TextImg(art, t, static_cast<int>(34 * k), C("ffb2b2"));
        draw::PasteC(out, ti, ox - 4 * k + 75 * k, oy + 38 * k);
    }
    return true;
}

// ---- Nook Miles+ task card (acnh_mock.quest_card, without the texts the page draws) ------------
bool QuestCard(Art& art, const ArtKey& key, uint16_t uid, Image& out) {
    const Bcsv* t = art.Cat().Table("EventFlagsLifeSupportDailyParam");
    if (!t)
        return false;
    int genre = -1;
    for (size_t r = 0; r < t->Rows(); ++r)
        if (t->U(r, Bcsv::Key("UniqueID", "u16")) == uid)
            genre = static_cast<int>(t->U(r, Bcsv::Key("GenreID", "u16")));
    if (genre < 0)
        return false;
    const double w = key.Num("w", 220), k = w / 400, h = 420 * k;
    out = Blank(w, h);
    // LPointBtn P_Base_00 QuestBtnBase00^s (left half, mirrored) #cfdbff (not done) / P_Base_01
    // #ffffff (done)
    draw::Paste(out,
                draw::MaskTint(draw::Resize(draw::MirrorH(art.Tex("LPointBtn", "QuestBtnBase00^s")),
                                            R(w), R(h)),
                               C(key.Flag("done") ? "ffffff" : "cfdbff")),
                0, 0);
    const double cx = w / 2, cy = h / 2 - 40 * k;
    // LPointGauge ring CircleGaugeBase^s (quarter, mirrored) #d3c59c
    draw::PasteC(
        out,
        draw::MaskTint(draw::Resize(draw::Mirror4(art.Tex("LPointGauge", "CircleGaugeBase^s")),
                                    R(300 * k), R(300 * k)),
                       C("d3c59c")),
        cx, cy);
    const Image disk = draw::Resize(
        draw::MaskTint(draw::Mirror4(art.Tex("LPointGauge", "PointGaugeCircle^s")), C("ffffff")),
        R(236 * k), R(236 * k));
    draw::PasteC(out, disk, cx, cy);
    char name[64];
    std::snprintf(name, sizeof name, "Layout_DailyQuestIcon_DailyQuest%02d", genre);
    const Image ic = art.ModelIcon(name);
    if (!ic.Empty())
        draw::PasteC(out, draw::Masked(draw::Fit(ic, 236 * k, 236 * k), disk), cx, cy);
    // progress capsule under the ring (the "0/20" text is the page's, centred at (cx, cy + 113k))
    if (key.Int("progress_base", 1))
        draw::PasteC(out, Capsule(art, key.Num("pw", 120 * k), 50 * k, C("ffffffe0")), cx,
                     cy + 112 * k);
    if (key.Flag("bonus")) { // BonusDaily: P_Bonus_00 PointBonusBase00^s #ff7414 + "x2" (multiplier
                             // LEAD)
        draw::PasteC(
            out,
            draw::MaskTint(draw::Resize(draw::Mirror4(art.Tex("LPointBtn", "PointBonusBase00^s")),
                                        R(110 * k), R(88 * k)),
                           C("ff7414")),
            w - 62 * k, 50 * k, C("00000030"), 3);
        std::string x = art.Cat().Text(art.Language(), "LayoutMsg/MenuPointTopQuest", "0002");
        if (x.empty())
            x = "\xC3\x97";
        draw::PasteC(out, TextImg(art, x + "2", static_cast<int>(44 * k), C("ffe6cc")), w - 62 * k,
                     52 * k);
    }
    return true;
}

// ---- Nook Miles+ x5 badge (round 7) ----------------------------------------------------------
// LPointBtn_BonusType frame 1 (IsFivePoint): P_Bonus_00 texture PointBonusBase01^s (FLTP), scale
// 1.4 (FLPA tgt 6/7), white #ff2411 (FLMC); T_BonusNumUnit_00 / T_BonusNum_00 #ffe647. The pane
// is 100 x 80 from a 50 x 40 quarter (mirrored), like the x2 badge the card draws (110 x 88 per
// 400-unit card). A separate image: at 1.4 it reaches past the card's corner. Canvas = the badge
// + its 3 px drop shadow; centred where the x2 badge is centred (card w - 62k, 50k).
bool FiveBadge(Art& art, const ArtKey& key, Image& out) {
    const double k = key.Num("w", 220) / 400, sc = 1.4;
    const double bw = 110 * k * sc, bh = 88 * k * sc;
    const auto l = art.Lyt("LPointBtn");
    Rgba b0 = C("00000000"), base = C("ff2411ff"), b1 = C("00000000"), num = C("ffe647ff");
    if (l) {
        l->AnimMaterial("LPointBtn_BonusType", 1, "P_Bonus_00", b0, base);
        l->AnimMaterial("LPointBtn_BonusType", 1, "T_BonusNum_00", b1, num);
    }
    const Image tex = art.Tex("LPointBtn", "PointBonusBase01^s");
    if (tex.Empty())
        return false;
    out = Blank(bw + 4, bh + 8);
    draw::PasteC(out, draw::MaskTint(draw::Resize(draw::Mirror4(tex), R(bw), R(bh)), base),
                 (bw + 4) / 2, bh / 2 + 2, C("00000030"), 3);
    std::string x = art.Cat().Text(art.Language(), "LayoutMsg/MenuPointTopQuest", "0002");
    if (x.empty())
        x = "\xC3\x97";
    draw::PasteC(out, TextImg(art, x + "5", 44 * k * sc, num), (bw + 4) / 2, bh / 2 + 4);
    return true;
}

// ---- passport card (page_today: LProfileBtn) --------------------------------------------------
bool PassportCard(Art& art, const ArtKey& key, Image& out) {
    const auto l = art.Lyt("LProfileBtn");
    if (!l)
        return false;
    const double pw = key.Num("w", 604), ph = key.Num("h", 250);
    const std::string sign = Zodiac(key.Int("m", 1), key.Int("d", 1));
    const char* A = "LProfileBtn_BirthdayType";
    const float fr =
        static_cast<float>(FrameForTexture(*l, A, "P_BirthdayIcon", "BirthdayIcon" + sign + "^s"));
    Rgba b0 = C("00000000"), pcol = C("717352ff");
    l->AnimMaterial(A, fr, "T_Passport_00", b0, pcol);
    Rgba b1 = C("c8d3aa00"), patc = C("e6f1c8ff");
    l->AnimMaterial(A, fr, "P_Pattern_00", b1, patc);
    out = Blank(pw, ph + 6);
    const Image corner = draw::Resize(art.Tex("LProfileBtn", "ProfileCardBase00^s"), 44, 44);
    draw::Paste(out, draw::Win(corner, pw, ph, C("2e2b1c50")), 0, 6);
    const Image base = draw::Win(corner, pw, ph, C("f8f8d1"));
    draw::Paste(out, base, 0, 0);
    const Image pat =
        draw::Tint(draw::Tile(R(pw), R(ph),
                              draw::Resize(art.Tex("LProfileBtn", "ProfilePattern01^r"), 128, 128)),
                   draw::WithAlpha(patc, 0), draw::WithAlpha(patc, 0x90));
    draw::Paste(out, draw::Masked(pat, base), 0, 0);
    if (key.Int("header", 1)) {
        std::string hd = art.Cat().Text(art.Language(), "LayoutMsg/MenuProfile", "0501");
        if (hd.empty())
            hd = "PASSPORT";
        const Rgba pc = draw::WithAlpha(pcol, 255);
        const Image ti = TextImg(art, hd, 28, pc);
        draw::PasteC(out, ti, pw / 2, 30);
        const double tw = ti.Empty() ? art.Font().TextWidth("ui", hd, 28) : ti.w;
        for (const int side : {-1, 1}) {
            const double x0 = pw / 2 + side * (tw / 2 + 16);
            draw::FillRoundRect(out, std::min(x0, x0 + side * 150), 27,
                                std::max(x0, x0 + side * 150), 33, 3, pc);
        }
    }
    return true;
}

// ---- NookPhone frame (page_phone: MenuDevice RootPane) ----------------------------------------
bool PhoneFrame(Art& art, const ArtKey& key, Image& out) {
    const auto l = art.Lyt("MenuDevice");
    if (!l)
        return false;
    Layout::ComposeOptions o;
    o.canvas_w = 900;
    o.canvas_h = 880;
    o.center_x = -360;
    o.center_y = 4;
    o.hide = {"P_CapMaskDown_00", "P_CapMaskDown_01"};
    Image phone = l->Compose("RootPane", o);
    // the body (P_Base_00 870x840 @ (-360,4)) rises from the screen edge in game; LAYOUT: closed
    // with the same squircle so nothing looks cropped
    Image clip{900, 880};
    draw::Composite(clip, Card(art, 870, 840, C("ffffff"), 120), 15, 20);
    phone = draw::Masked(phone, clip);
    const int w = key.Int("w", 900), h = key.Int("h", 880);
    if (w == 900 && h == 880) {
        out = phone;
        return true;
    }
    // Scale to the height, then widen (6-column grid, owner 2026-10-02) by stretching only the flat
    // strip 340..560 between the status icons, so the squircle corners and icons keep their shape.
    const int sw = std::max(1, static_cast<int>(std::lround(900.0 * h / 880)));
    const Image scaled = draw::Resize(phone, sw, h);
    if (w <= sw) {
        out = draw::Resize(phone, w, h);
        return true;
    }
    const int s0 = static_cast<int>(340.0 * sw / 900), s1 = static_cast<int>(560.0 * sw / 900);
    out = Image{w, h};
    draw::Composite(out, draw::Crop(scaled, 0, 0, s0, h), 0, 0);
    draw::Composite(
        out, draw::ResizeNearest(draw::Crop(scaled, s0, 0, s1, h), s1 - s0 + (w - sw), h), s0, 0);
    draw::Composite(out, draw::Crop(scaled, s1, 0, sw, h), s1 + (w - sw), 0);
    return true;
}

// ---- misc -----------------------------------------------------------------------------------
// acnh_mock.gear_button: cream LPocketBtn#Maru128_00^s disc (#fff1c3 rim, #fffbe8 face), glyph =
// ParkExt U+E130 (no gear texture exists in the 960 layouts) #746144 (MenuDevice T_Hour colour);
// pressed = the orange selected-slot stripe.
Image Gear(Art& art, double d, bool pressed) {
    Image out = Blank(d + 10, d + 10);
    draw::PasteC(out, Disc(art, d + 10, C(ui::Cream)), (d + 10) / 2, (d + 10) / 2);
    if (pressed)
        draw::PasteC(out, ui::Stripes(art, Disc(art, d, C("ffffff")), 40), (d + 10) / 2,
                     (d + 10) / 2);
    else
        draw::PasteC(out, Disc(art, d, C("fffbe8")), (d + 10) / 2, (d + 10) / 2);
    draw::PasteC(out,
                 draw::Fit(Glyph(art, 0xE130, 80, C(pressed ? "ffffff" : "746144")), d * 62 / 96,
                           d * 62 / 96),
                 (d + 10) / 2, (d + 10) / 2);
    return out;
}

// LRecipeCategory_Pattern textures; 11.. are the DIY app's other categories (Seasonal, Craftable,
// Craftable + storage, Favorites, Requests, DIY Requests), appended so earlier indices stay put
const char* const RecipeCatIcons[] = {
    "IconCatAll^s",       "IconCatTool^s",        "IconCatFurniture^s", "IconCatGoods^s",
    "IconCatWallMount^s", "IconCatCeiling^s",     "IconCatFloorWall^s", "IconCatRing^s",
    "IconCatGoodsBag^s",  "IconCatFood^s",        "IconCatDessert^s",   "IconCat_Season^s",
    "IconCatCanMake^s",   "IconCatCanMakeBoth^s", "IconCatFavorite^s",  "IconCatRequest^s",
    "IconCatTradeBox^s"};
const char* const CritCatIcons[] = {"IconCatInsect^s", "IconCatFish^s", "IconCatDiveFish^s"};
const char* const CritInv[] = {"BookBtnInvInsect^z", "BookBtnInvFish^z", "BookBtnInvDiveFish^z"};

bool Recipe(Art& art, const ArtKey& key, Image& out) {
    const auto& p = key.parts; // "ui", group, ...
    const std::string g = p.size() > 1 ? p[1] : "";
    const std::string n = p.size() > 2 ? p[2] : "";
    const auto num = [&](size_t i, int fallback) {
        try {
            return p.size() > i ? std::stoi(p[i]) : fallback;
        } catch (...) {
            return fallback;
        }
    };
    // ---------------- shared chrome
    if (g == "title" && n == "logo") {
        const auto layout = art.Lyt("LTitleUSEU");
        if (!layout)
            return false;
        Layout::ComposeOptions o;
        o.canvas_w = 1920;
        o.canvas_h = 1080;
        o.anim = "LTitleUSEU_In";
        o.frame = 120;
        const Image logo = draw::CropAlpha(layout->Compose("RootPane", o));
        if (logo.Empty())
            return false;
        out = draw::Fit(logo, key.Num("w", 900), key.Num("h", 600));
        return true;
    }
    if (g == "card") {
        out = Card(art, key.Num("w", 200), key.Num("h", 120), key.Col("c", C("fffbe8")),
                   key.Num("r", 44));
        return true;
    }
    if (g == "capsule") {
        out = Capsule(art, key.Num("w", 200), key.Num("h", 52), key.Col("c", C("f5f7e0")));
        return true;
    }
    if (g == "disc") {
        out = Disc(art, key.Num("d", 88), key.Col("c", C(ui::Cream)));
        return true;
    }
    if (g == "arrow") { // acnh_mock.arrow_btn: ListArrow chevron on a Maru128 disc
        const double d = key.Num("d", 58);
        out = Disc(art, d, key.Col("c", C("ed9a21")));
        draw::PasteC(
            out, ui::Chevron(art, key.Col("g", C("ffffff")), d * 0.46, key.Str("dir", "r") != "l"),
            d / 2, d / 2);
        return true;
    }
    if (g == "glyph") { // a ParkExt picture glyph (?cp=e130 hex), fitted into s x s
        const auto cp =
            static_cast<char32_t>(std::strtoul(key.Str("cp", "e130").c_str(), nullptr, 16));
        const Image gi = Glyph(art, cp, key.Num("px", 80), key.Col("c", C("746144")));
        if (gi.Empty())
            return false;
        out = key.params.contains("s") ? draw::Fit(gi, key.Num("s", 62), key.Num("s", 62)) : gi;
        return true;
    }
    if (g == "gear") {
        out = Gear(art, key.Num("d", 96), key.Flag("pressed"));
        return true;
    }
    if (g == "close") { // ParkExt U+E14C on a #c8bd94 disc
        const double d = key.Num("d", 70);
        out = Disc(art, d, key.Col("c", C("c8bd94")));
        draw::PasteC(out, draw::Fit(Glyph(art, 0xE14C, 60, C("ffffff")), d * 34 / 70, d * 34 / 70),
                     d / 2, d / 2);
        return true;
    }
    if (g == "check") { // selected swatch: BtnOpt_Root#IconCatCheck_64^s cream on a #f38500 disc,
                        // cream rim
        const double d = key.Num("d", 54);
        out = Disc(art, d, C(ui::Cream));
        draw::PasteC(out, Disc(art, d - 8, C("f38500")), d / 2, d / 2);
        draw::PasteC(
            out,
            draw::Fit(draw::MaskTint(art.Tex("BtnOpt_Root", "IconCatCheck_64^s"), C(ui::Cream)),
                      d * 30 / 54, d * 30 / 54),
            d / 2, d / 2);
        return true;
    }
    if (g == "glow") { // MenuDevice#Maru192blur^s
        const int d = key.Int("d", 110);
        out = draw::MaskTint(draw::Resize(art.Tex("MenuDevice", "Maru192blur^s"), d, d),
                             key.Col("c", C("ffbb00e0")));
        return true;
    }
    if (g == "cursor") { // LCursor#Cursor_00^w hand
        const int d = key.Int("d", 82);
        out = draw::Resize(art.Tex("LCursor", "Cursor_00^w"), d, d);
        return true;
    }
    if (g == "dock") {
        out = Card(art, key.Num("w", 1224), key.Num("h", 102), C("fffbe8"), 40);
        return true;
    }
    if (g == "tabicon") {
        const int k = num(2, -1);
        if (k < 0 || k > 5)
            return false;
        out = TabIcon(art, k, key.Num("s", 58));
        return !out.Empty();
    }
    if (g == "tabsel") { // active tab: W_Base_00 capsule in the app colour + white sheen (LAYOUT)
        const int k = std::clamp(key.Int("k", 0), 0, 5);
        const double w = key.Num("w", 192), h = key.Num("h", 90);
        out = Capsule(art, w, h, key.Col("c", C(Tabs[k].second)));
        draw::Paste(out,
                    draw::MaskTint(
                        draw::Pill(art.Tex("LBookCatBtnS_00", "CmnGuideCorner^s"), w - 10, h - 10),
                        C("ffffff24")),
                    5, 5);
        return true;
    }
    if (g == "panel") {
        const int w = key.Int("w", 1216), h = key.Int("h", 948);
        if (n == "map")
            out = ui::PanelSea(art, w, h);
        else if (n == "critters")
            out = ui::PanelBook(art, w, h);
        else if (n == "diy")
            out = ui::PanelPaper(art, w, h);
        else
            return false;
        return true;
    }
    if (g == "icon") {
        const double s = key.Num("s", 0);
        if (n == "mile")
            out = draw::CropAlpha(art.Tex("LMenuDeviceBtn", "DeviceIconMile^x"));
        else if (n == "bell")
            out = art.Tex("LPocketMenuBellBtn", "IconBell^w");
        else if (n == "turnip")
            out = art.Tex("SwkbdTextAreaKaburiba", "IconKabu^w");
        else if (n == "daisy")
            out = art.Tex("SwkbdTextAreaKaburiba", "SwkbdTextAreaIconKaburiba^w");
        else if (n == "milepict")
            out = draw::MaskTint(art.Tex("LPointBtn", "IconMilePict^s"), key.Col("c", C("8c735b")));
        else if (n == "island")
            out = draw::MaskTint(art.Tex("LProfileBtn", "ProfileIconIsland^s"),
                                 key.Col("c", C("0d4627")));
        else if (n == "owl")
            out =
                draw::MaskTint(art.Tex("LBookItemName", "IconMuseum^s"), key.Col("c", C("2d2a1e")));
        else if (n == "pocket" || n == "storage")
            out = draw::Tint(art.Tex("LRecipeBtn", n == "pocket" ? "WorkBenchPocketMark^t"
                                                                 : "WorkBenchCardStorage^t"),
                             C("d5c79c00"), key.Col("c", C("3e1c05ff")));
        else
            return false;
        if (out.Empty())
            return false;
        if (s > 0)
            out = draw::Fit(out, s, s);
        return true;
    }
    // ---------------- BAG (page_bag)
    if (g == "bag") {
        constexpr double k = 0.95, sc = 1.13 * k;
        if (n ==
            "blob") { // P_Base40_00 = PocketMenuBaseB^s (left half, mirrored), LIVE cream #fffbe8
            out = draw::MaskTint(
                draw::Resize(draw::MirrorH(art.Tex("PocketMenu", "PocketMenuBaseB^s")),
                             R(key.Num("w", 1133 * 1.13 * k)), R(key.Num("h", 492 * 1.13 * k))),
                key.Col("c", C("fffbe8")));
            return true;
        }
        if (n == "slotsel") { // LPocketBtn_Select P_BaseInvBlue_00 (Maru64_00^s) #007765, alpha
                              // LIVE-matched
            out = Disc(art, key.Num("d", 113 * 0.9 * sc), C("007765b8"));
            return true;
        }
        if (n == "empty") { // P_BaseInvBlue_00 #d9c095 dot
            out = Disc(art, key.Num("d", 34), C("d9c095a0"));
            return true;
        }
        if (n ==
            "fav") { // P_FavoriteBase_00 #fff1c3 + P_Favorite_00 #ff9400, 44x44 / 36x36 (x scale)
            const double s = key.Num("s", sc);
            const Image mk = art.Tex("LPocketBtn", "PocketMenuFavoriteMark^_A");
            out = draw::Resize(draw::Tint(mk, C("fff1c300"), C("fff1c3ff")), R(44 * s), R(44 * s));
            draw::PasteC(
                out,
                draw::Resize(draw::Tint(mk, C("fff1c300"), C("ff9400ff")), R(36 * s), R(36 * s)),
                out.w / 2.0, out.h / 2.0);
            return true;
        }
        if (n == "equip") { // the held-item mark: LPocketBtn PocketMenuEquipIcon^_A (alpha = disc,
                            // RGB black disc / white check) as a #00b6a9 disc with a cream check
                            // on a cream rim, LIVE-matched (pocket menu)
            const double d = key.Num("d", 40);
            const Image mk = art.Tex("LPocketBtn", "PocketMenuEquipIcon^_A");
            // Tint interpolates every channel: rgb black -> disc colour, white -> check colour,
            // alpha 0 -> the texture's alpha
            Rgba disc = key.Col("c", C("00b6a9ff"));
            disc.a = 0;
            out = draw::Resize(draw::Tint(mk, C("fff9e300"), C("fff9e3ff")), R(d), R(d));
            draw::PasteC(out,
                         draw::Resize(draw::Tint(mk, disc, C("fff9e3ff")), R(d * 0.8), R(d * 0.8)),
                         out.w / 2.0, out.h / 2.0);
            return true;
        }
        if (n ==
            "balloon") { // LPocketBalloon W_NameBase_00 (MsgWinNameBase^s caps), LIVE teal #45b6a6
            const Image body = draw::Pill(art.Tex("MsgWin", "MsgWinNameBase^s"), key.Num("w", 300),
                                          key.Num("h", 76));
            if (!key.params.contains("beak") && !key.params.contains("tip")) {
                out = draw::MaskTint(body, key.Col("c", C("45b6a6")));
                return true;
            }
            // r9 (owner): ONE shape, as the game's LPocketBalloon draws its window and P_Beak_00 in
            // one material colour: the body pill and the beak (LMsgBalloon BalloonBeak^s 60 x 24,
            // overlapping the body's visible bottom edge, its left edge `beak` px from the body's)
            // merged into one coverage mask (the larger coverage per pixel: no seam where their
            // edges meet), tinted once, over one drop shadow of the whole outline (#04554f40, 4 px
            // lower). Picture: w x (h + 24) = body, beak below it, shadow.
            const int bw = body.w, bh = body.h;
            const Image beak = draw::Resize(art.Tex("LMsgBalloon", "BalloonBeak^s"), 60, 24);
            // The native beak is asymmetric. Anchor its lowest covered pixel to the item,
            // rather than centering the texture rectangle (which shifts the visible tip right).
            int tip_x = beak.w / 2;
            for (int y = beak.h - 1; y >= 0; --y) {
                int sum = 0, count = 0;
                for (int x = 0; x < beak.w; ++x)
                    if (beak.At(x, y)[3] >= 128) {
                        sum += x;
                        ++count;
                    }
                if (count) {
                    tip_x = sum / count;
                    break;
                }
            }
            const int bx = std::clamp(key.params.contains("tip") ? key.Int("tip", bw / 2) - tip_x
                                                                 : key.Int("beak", (bw - 60) / 2),
                                      0, std::max(0, bw - 60));
            // MsgWinNameBase has transparent padding below its visible pill. Join to its
            // alpha boundary rather than the image rectangle, otherwise a gap survives even
            // though body and beak are in one bitmap. Overlap two covered rows for antialiasing.
            int bottom = bh - 1;
            while (bottom > 0 && body.At(bw / 2, bottom)[3] < 128)
                --bottom;
            const int beak_y = std::max(0, bottom - 2);
            Image mask{bw, bh + 20};
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x)
                    mask.At(x, y)[3] = body.At(x, y)[3];
            for (int y = 0; y < beak.h; ++y)
                for (int x = 0; x < beak.w && bx + x < bw; ++x) {
                    uint8_t* m = mask.At(bx + x, beak_y + y);
                    m[3] = std::max(m[3], beak.At(x, y)[3]);
                }
            for (size_t p = 0; p < mask.rgba.size(); p += 4)
                mask.rgba[p] = mask.rgba[p + 1] = mask.rgba[p + 2] = 0xFF;
            out = Image{bw, bh + 24};
            draw::Paste(out, draw::MaskTint(mask, key.Col("c", C("45b6a6"))), 0, 0, C("04554f40"),
                        4);
            return true;
        }
        if (n == "beak") { // LMsgBalloon P_Beak (crisp variant)
            out = draw::MaskTint(draw::Resize(art.Tex("LMsgBalloon", "BalloonBeak^s"),
                                              key.Int("w", 60), key.Int("h", 24)),
                                 key.Col("c", C("45b6a6")));
            return true;
        }
        if (n == "chip") { // BellUnitBase^s capsule (companion strip, LAYOUT)
            out = draw::MaskTint(draw::Pill(art.Tex("LPocketMenuBellBtn", "BellUnitBase^s"),
                                            key.Num("w", 438), key.Num("h", 80)),
                                 key.Col("c", C(ui::Cream)));
            return true;
        }
        return false;
    }
    // ---------------- CRITTERS (page_critters)
    if (g == "crit") {
        if (n ==
            "cloud") { // W_CategoryBase_00 CmnCategoryBase_L^s, LIVE #f5f7e0 (material #e9edbf)
            out = draw::MaskTint(draw::Pill(art.Tex("MenuBook", "CmnCategoryBase_L^s"),
                                            key.Num("w", 330), key.Num("h", 112)),
                                 key.Col("c", C("f5f7e0")));
            return true;
        }
        if (n == "cat") { // LBookCatBtnS_00 P_Icon_00: idle LIVE #8a7b67, selected #ed9a21 (_Select
                          // FLMC)
            const int k = num(3, -1);
            if (k < 0 || k > 2)
                return false;
            out = draw::Fit(draw::MaskTint(art.Tex("LBookCatBtnS_00", CritCatIcons[k]),
                                           C(key.Flag("on") ? "ed9a21" : "8a7b67")),
                            key.Num("s", 72), key.Num("s", 72));
            return true;
        }
        if (n == "cell" ||
            n ==
                "focus") { // BookBtnMatrixFrame^z / Focus^z (quarter, mirrored) b #19180d w #9e8c46
            const Image t = draw::Tint(
                draw::Mirror4(art.Tex("LBookBtnMatrix", n == "cell" ? "BookBtnMatrixFrame^z"
                                                                    : "BookBtnMatrixFocus^z")),
                C("19180d00"), C("9e8c46ff"));
            out = draw::Resize(t, key.Int("w", n == "cell" ? 141 : 149),
                               key.Int("h", n == "cell" ? 120 : 128));
            if (n == "cell")
                out = draw::Fade(out, 0.55);
            return true;
        }
        if (n == "inv") { // BookBtnInv* silhouette (frame texmap 1), colour LAYOUT #b3a57a
            const int k = num(3, -1);
            if (k < 0 || k > 2)
                return false;
            out = draw::Fit(draw::MaskTint(draw::AlphaMask(art.Tex("LBookBtnMatrix", CritInv[k])),
                                           key.Col("c", C("b3a57a"))),
                            key.Num("s", 65), key.Num("s", 65));
            return true;
        }
        if (n ==
            "caption") { // LBookItemName P_InfoBase_00 BookCaptionBaseMatrix^t b #6d6253 w #ffffff
            out = draw::Pill(draw::Tint(art.Tex("LBookItemName", "BookCaptionBaseMatrix^t"),
                                        C("6d625300"), C("ffffffff")),
                             key.Num("w", 300), key.Num("h", 80));
            return true;
        }
        return false;
    }
    // ---------------- DIY (page_diy)
    if (g == "diy") {
        if (n == "sizegrid") {
            const auto l = art.Lyt("MenuRecipe");
            if (!l)
                return true;
            // Native P_Size_00 is bottom-left at (-21.5,-21.5) under a 60px frame.
            // SizeW/SizeH animate the fill dimensions in half-grid units (0..6).
            const auto extent = [&](std::string_view anim, int target, double units) {
                const auto* a = l->Anim(anim);
                if (!a)
                    return 0.0;
                const auto v = a->At("P_Size_00", "FLPA", 0, target,
                                     static_cast<float>(std::clamp(units, 0.0, 6.0)));
                return v ? static_cast<double>(*v) : 0.0;
            };
            Image grid(60, 60);
            draw::FillRect(grid, 8.5, 51.5 - extent("MenuRecipe_SizeH", 9, key.Num("z", 0)),
                           8.5 + extent("MenuRecipe_SizeW", 8, key.Num("x", 0)), 51.5, C("ff6600"));
            draw::Composite(
                grid,
                draw::Resize(draw::Tint(art.Tex("MenuRecipe", "WorkBenchUnitSizeMark^s"),
                                        C("d5c79c00"), C("3e1c05")),
                             60, 60),
                0, 0);
            out = draw::Fit(grid, key.Num("s", 44), key.Num("s", 44));
            return true;
        }
        if (n == "cloud") { // MenuRecipe W_CategoryBase_00 CmnCategoryBase_L^s #e9edbf
            out = draw::MaskTint(draw::Pill(art.Tex("MenuRecipe", "CmnCategoryBase_L^s"),
                                            key.Num("w", 1138), key.Num("h", 104)),
                                 key.Col("c", C("e9edbf")));
            return true;
        }
        if (n ==
            "cat") { // LRecipeCategory#IconCat*^s #413223, selected #e55000 (pairing to names LEAD)
            const int k = num(3, -1);
            if (k < 0 || k >= static_cast<int>(std::size(RecipeCatIcons)))
                return false;
            out = draw::Fit(draw::MaskTint(art.Tex("LRecipeCategory", RecipeCatIcons[k]),
                                           C(key.Flag("on") ? "e55000" : "413223")),
                            key.Num("s", 62), key.Num("s", 62));
            return !out.Empty();
        }
        if (n == "card") {
            const int uid = num(3, -1);
            return uid >= 0 && uid < 0x10000 &&
                   RecipeCard(art, key, static_cast<uint16_t>(uid), out);
        }
        if (n == "canmake") { // N_CanMake_00: WorkBenchCardNewBase^s #ffa100 + IconCatCanMake_64^s
                              // / ...Both^s #281f0b
            const double d = key.Num("d", 96);
            out = draw::Resize(
                draw::MaskTint(art.Tex("LRecipeBtn", "WorkBenchCardNewBase^s"), C("ffa100f0")),
                R(d), R(d));
            const Image gi = key.Flag("both") ? art.Tex("LRecipeCategory", "IconCatCanMakeBoth^s")
                                              : art.Tex("LRecipeBtn", "IconCatCanMake_64^s");
            draw::PasteC(out, draw::Fit(draw::MaskTint(gi, C("281f0b")), d * 54 / 96, d * 54 / 96),
                         d / 2, d / 2);
            return true;
        }
        if (n == "star") { // r9: the detail pane's Favorite button: a cream disc with the DIY
                           // card's own favourite mark (LRecipeBtn PocketMenuFavoriteMark^t, the
                           // card's #ff9400) when starred, the same mark in the card frame's
                           // muted #c9b98c when not
            const double d = key.Num("d", 72);
            const Image mk = art.Tex("LRecipeBtn", "PocketMenuFavoriteMark^t");
            out = Disc(art, d, C(ui::Cream));
            draw::PasteC(out, Disc(art, d - 8, C("fffbe8")), out.w / 2.0, out.h / 2.0);
            draw::PasteC(out,
                         draw::Resize(draw::Tint(mk, C(key.Flag("on") ? "6d3f0000" : "9c8c6000"),
                                                 C(key.Flag("on") ? "ff9400ff" : "c9b98cff")),
                                      R(d * 0.66), R(d * 0.66)),
                         out.w / 2.0, out.h / 2.0);
            return true;
        }
        if (n == "catsq") { // detail panel: category icon #d5c48f on a #3e1c05 square
            const int k =
                std::clamp(key.Int("k", 0), 0, static_cast<int>(std::size(RecipeCatIcons)) - 1);
            const double s = key.Num("s", 44);
            out = Card(art, s, s, C("3e1c05"), 10);
            draw::PasteC(out,
                         draw::Fit(draw::MaskTint(art.Tex("LRecipeCategory", RecipeCatIcons[k]),
                                                  C("d5c48f")),
                                   s * 36 / 44, s * 36 / 44),
                         s / 2, s / 2);
            return true;
        }
        if (n == "dotline") { // MenuRecipe WorkBenchDotLineBold^s #3e1c05
            out = draw::Tile(key.Int("w", 400), 8,
                             draw::MaskTint(art.Tex("MenuRecipe", "WorkBenchDotLineBold^s"),
                                            key.Col("c", C("3e1c05"))));
            return true;
        }
        return false;
    }
    // ---------------- TODAY (page_today)
    if (g == "today") {
        if (n == "clockcard") { // LAYOUT card #37ab7f + map wave pattern #12907040
            const double w = key.Num("w", 600), h = key.Num("h", 250);
            out = Card(art, w, h, C("37ab7f"), 40);
            const Image waves = draw::Tile(
                R(w), R(h),
                draw::MaskTint(draw::Resize(art.Tex("MenuMapFull", "MapBGPattern00^s"), 40, 28),
                               C("12907040")));
            draw::Composite(out, draw::Masked(waves, Card(art, w, h, C("ffffff"), 40)), 0, 0);
            return true;
        }
        if (n == "colon") { // LClock ClockColon^s
            const int s = key.Int("s", 18);
            out = draw::Resize(
                draw::MaskTint(art.Tex("LClock", "ClockColon^s"), key.Col("c", C("faffe6"))), s, s);
            return true;
        }
        if (n == "weekpill") { // W_WeekBase_00 ClockWeekBase^s (white)
            out = draw::Pill(art.Tex("LClock", "ClockWeekBase^s"), key.Num("w", 120),
                             key.Num("h", 58));
            return true;
        }
        if (n == "weather") {
            const int pat = num(3, -1);
            return pat >= 0 && WeatherIcon(art, pat, key.Num("s", 110), out);
        }
        if (n == "wx") // ui/today/wx/<wx.type>?s=110: the TV forecast symbol
            return WeatherTv(art, num(3, -1), key.Num("s", 110), out);
        if (n == "five") // ui/today/five?w=<card w>: the x5 burst badge
            return FiveBadge(art, key, out);
        if (n == "passport")
            return PassportCard(art, key, out);
        if (n == "sign") { // zodiac BirthdayIcon<Sign>^s, colour from LProfileBtn_BirthdayType
                           // (P_BirthdayIcon)
            const auto l = art.Lyt("LProfileBtn");
            if (!l)
                return false;
            const std::string sign = Zodiac(key.Int("m", 1), key.Int("d", 1));
            const std::string tex = "BirthdayIcon" + sign + "^s";
            Rgba b = C("76725f00"), icol = C("76725fff");
            l->AnimMaterial("LProfileBtn_BirthdayType",
                            static_cast<float>(FrameForTexture(*l, "LProfileBtn_BirthdayType",
                                                               "P_BirthdayIcon", tex)),
                            "P_BirthdayIcon", b, icol);
            const Rgba c = key.Col("c", draw::WithAlpha(icol, 255));
            out = draw::Fit(draw::MaskTint(art.Tex("LProfileBtn", tex), c), key.Num("s", 38),
                            key.Num("s", 38));
            return !out.Empty();
        }
        if (n == "photoframe") { // ProfilePhotoBtnBase^s (quarter, mirrored) #f8f8d1; ?c= recolours
                                 // (its drop shadow: c=2e2b1c40)
            const int d = key.Int("d", 166);
            out = draw::MaskTint(
                draw::Resize(draw::Mirror4(art.Tex("LProfileBtn", "ProfilePhotoBtnBase^s")), d, d),
                key.Col("c", C("f8f8d1")));
            return true;
        }
        if (n == "fruit") { // LProfileBtn_FruitType frame whose P_Fruit_00 texture is the fruit,
                            // its colour
            const int item = num(3, -1);
            const auto l = art.Lyt("LProfileBtn");
            const auto* it = item >= 0 ? art.Cat().FindItem(static_cast<uint16_t>(item)) : nullptr;
            if (!l || !it)
                return false;
            for (int f = 0; f < 5; ++f) {
                const std::string tex =
                    l->AnimTexture("LProfileBtn_FruitType", static_cast<float>(f), "P_Fruit_00");
                if (!IEquals(tex, "ProfileFruitIcon" + it->label + "^s"))
                    continue;
                Rgba b = C("00000000"), fcol = C("ff4141ff");
                l->AnimMaterial("LProfileBtn_FruitType", static_cast<float>(f), "P_Fruit_00", b,
                                fcol);
                out = draw::Fit(
                    draw::MaskTint(art.Tex("LProfileBtn", tex), draw::WithAlpha(fcol, 255)),
                    key.Num("s", 38), key.Num("s", 38));
                return !out.Empty();
            }
            return false;
        }
        if (n == "quest") {
            const int uid = num(3, -1);
            return uid >= 0 && QuestCard(art, key, static_cast<uint16_t>(uid), out);
        }
        return false;
    }
    // ---------------- PHONE (page_phone)
    if (g == "phone") {
        if (n == "frame")
            return PhoneFrame(art, key, out);
        if (n == "dot") { // LMenuDeviceIndicator: Maru64_00^s #007b68 current / #a09e8a
            const int s = key.Int("s", 22);
            out = draw::MaskTint(draw::Resize(art.Tex("LMenuDeviceIndicator", "Maru64_00^s"), s, s),
                                 C(key.Flag("on") ? "007b68" : "a09e8a"));
            return true;
        }
        if (n == "sel") { // LAYOUT selection: cream ring + orange stripes in a squircle
            const double s = key.Num("s", 176);
            out = Card(art, s, s, C(ui::Cream), 50 * s / 176);
            draw::PasteC(out,
                         ui::Stripes(art, Card(art, s - 8, s - 8, C("ffffff"), 48 * s / 176), 48),
                         s / 2, s / 2);
            return true;
        }
        return false;
    }
    // ---------------- MAP chrome (page_map; the island itself is module:acnh/map/...)
    if (g == "mapui") {
        if (n == "panel") { // residents panel: MapBtnBase^s (top half, mirrored) #1e857740 x
                            // MapBasePattern^r
            const double w = key.Num("w", 318), h = key.Num("h", 912);
            const Image shape =
                draw::Resize(draw::MirrorV(art.Tex("MenuMapFull", "MapBtnBase^s")), R(w), R(h));
            out = draw::MaskTint(shape, C("1e857740"));
            const Image pat = draw::Tint(
                draw::Tile(R(w), R(h),
                           draw::Resize(art.Tex("MenuMapFull", "MapBasePattern^r"), 260, 183)),
                C("1e857700"), C("0f6f6230"));
            draw::Composite(out, draw::Masked(pat, shape), 0, 0);
            return true;
        }
        if (n == "plate") { // "Residents" plate MapSubTitleBase^s (left half, mirrored) #1c7b74
            out = draw::MaskTint(
                draw::Resize(draw::MirrorH(art.Tex("MenuMapFull", "MapSubTitleBase^s")),
                             key.Int("w", 278), key.Int("h", 70)),
                C("1c7b7488"));
            return true;
        }
        if (n == "resbtn") { // LMapBtn MapNPCBtnBase^s: idle #04554f64; selected stripes
                             // #e55f00->#e96f00 + line #fff1c3
            // ?w=&h= (r7 mapfix): drawn at that size (the 3-column residents layouts show the
            // 130 x 108 button smaller); every part is laid out at the size, no picture scaling
            const double bw = std::clamp(key.Num("w", 130), 8.0, 520.0),
                         bh = std::clamp(key.Num("h", 108), 8.0, 432.0);
            const double sx = bw / 130.0, sy = bh / 108.0;
            const Image hb =
                draw::Resize(art.Tex("LMapBtn", "MapNPCBtnBase^s"), R(118 * sx), R(94 * sy));
            out = Blank(R(bw), R(bh));
            if (key.Flag("on")) {
                draw::PasteC(out,
                             draw::MaskTint(draw::Resize(art.Tex("LMapBtn", "MapNPCBtnLine^s"),
                                                         R(130 * sx), R(104 * sy)),
                                            C(ui::Cream)),
                             65 * sx, (52 - 2) * sy);
                draw::PasteC(out, ui::Stripes(art, hb, 36 * sx, C("e55f00ff"), C("e96f00ff")),
                             65 * sx, 52 * sy, C("04554f50"), 4 * sy);
            } else {
                draw::PasteC(out, draw::MaskTint(hb, C("04554f64")), 65 * sx, 52 * sy);
            }
            return true;
        }
        if (n == "bannerend") { // W_TopFrame_01 MapFullBase^t ribbon end b #6d4e11 w #ffcf11
            const Image e = draw::CropAlpha(
                draw::Tint(art.Tex("MenuMapFull", "MapFullBase^t"), C("6d4e1100"), C("ffcf11ff")));
            const double h = key.Num("h", 92);
            out = draw::Resize(e, R(e.w * h / e.h), R(h));
            if (key.Flag("flip"))
                out = draw::FlipH(out);
            return true;
        }
        if (n == "banner") { // body LIVE #ffec4c
            out = Card(art, key.Num("w", 400), key.Num("h", 76), key.Col("c", C("ffec4c")), 12);
            return true;
        }
        return false;
    }
    // ---------------- TOOL RING picker + blocking card (star lane, page_bag)
    if (g == "ring") {
        const double d = key.Num("d", 120);
        if (n ==
            "slot") { // LRingMenuToolsBtn P_Base_01 (ClosetBalloon^s + KumoPat^s): LIVE #fff9e3;
                      // on=1 = N_ActiveReg_01 P_Base_00 #fbd558 (the Favorite picker's balloon)
            const bool on = key.Flag("on");
            out = Disc(art, d, C(on ? "fbd558" : "fff9e3"));
            const Image kumo =
                draw::Tile(out.w, out.h,
                           draw::MaskTint(draw::Resize(art.Tex("LRingMenuToolsBtn", "KumoPat^s"),
                                                       R(d * 0.5), R(d * 0.5)),
                                          C(on ? "e8b83030" : "d9cfa830")));
            draw::Composite(out, draw::Masked(kumo, out), 0, 0);
            return true;
        }
        if (n == "back") { // the picker's circle backdrop (owner 2026-10-02, LAYOUT):
                           // LPocketBtn#Maru128_00^s cropped to its ink (the texture has a wide
                           // transparent margin), a cream rim #fff1c3 of `rim` px around a warm
                           // dark disc (c), one static image
            const Image m = draw::CropAlpha(art.Tex("LPocketBtn", "Maru128_00^s"), 128);
            const int rim = key.Int("rim", 8);
            if (m.Empty())
                return false;
            out = draw::MaskTint(draw::Resize(m, R(d), R(d)), C(ui::Cream));
            draw::PasteC(out,
                         draw::MaskTint(draw::Resize(m, R(d) - 2 * rim, R(d) - 2 * rim),
                                        key.Col("c", C("5a4630"))),
                         out.w / 2.0, out.h / 2.0);
            return true;
        }
        if (n == "empty") { // N_Invalid_00: P_CaptureInv_00 #010203 (translucent) + P_IconInv_00
                            // RingMenuToolsIconInv^s 106/192 of the button; LIVE #1c1d1f / #969696
            out = Disc(art, d, C("1c1d1fd0"));
            draw::PasteC(
                out,
                draw::Fit(draw::MaskTint(art.Tex("LRingMenuToolsBtn", "RingMenuToolsIconInv^s"),
                                         C("969696")),
                          d * 106 / 192, d * 106 / 192),
                out.w / 2.0, out.h / 2.0);
            return true;
        }
        if (n == "star") { // the star button: cream disc + the pocket menu's favourite mark
                           // (PocketMenuFavoriteMark^_A, P_FavoriteBase_00 #fff1c3 / P_Favorite_00
                           // #ff9400)
            const Image mk = art.Tex("LPocketBtn", "PocketMenuFavoriteMark^_A");
            out = Disc(art, d, C(ui::Cream));
            draw::PasteC(out, Disc(art, d - 10, C("fffbe8")), out.w / 2.0, out.h / 2.0);
            draw::PasteC(out,
                         draw::Resize(draw::Tint(mk, C("ff940000"), C("ff9400ff")), R(d * 0.62),
                                      R(d * 0.62)),
                         out.w / 2.0, out.h / 2.0);
            return true;
        }
        if (n == "label") { // LRingMenuItemName-style balloon (MsgWin#MsgWinNameBase^s, LIVE teal
                            // #45b6a6, text #fff5ab) with a game text baked in the game language:
                            // t=title STR_Common 974 "Tool Ring", fav CmnSubWin 0146 "Favorite",
                            // clear CmnSubWin 0147 "Clear Favorite"
            const std::string t = key.Str("t", "title");
            std::string txt;
            if (t == "title")
                txt = art.Cat().Text(art.Language(), "String/STR_Common", "974");
            else if (t == "fav")
                txt = art.Cat().Text(art.Language(), "LayoutMsg/CmnSubWin", "0146");
            else if (t == "clear")
                txt = art.Cat().Text(art.Language(), "LayoutMsg/CmnSubWin", "0147");
            if (txt.empty())
                return false;
            const double w = key.Num("w", 360), h = key.Num("h", 76);
            out = draw::MaskTint(draw::Pill(art.Tex("MsgWin", "MsgWinNameBase^s"), w, h),
                                 key.Col("c", C("45b6a6")));
            Image ti = TextImg(art, txt, key.Num("px", h * 0.5), key.Col("tc", C("fff5ab")));
            if (ti.w > w - h * 0.7)
                ti = draw::Fit(ti, w - h * 0.7, ti.h);
            draw::PasteC(out, ti, out.w / 2.0, out.h / 2.0);
            return true;
        }
        return false;
    }
    if (g == "block") {
        if (n ==
            "win") { // CmnDialog P_Base_00 CmnDialogWinBase^s (left half, mirrored), P_Capture_00
                     // #fff1c3; msg=1 bakes T_Text_00 (#3a2b18) = DIALOG_WherearenMsg 4009
                     // ("You can't do that right now.") in the game language at y = ty x h
            const double w = key.Num("w", 760), h = key.Num("h", 420);
            out = draw::MaskTint(
                draw::Resize(draw::MirrorH(art.Tex("CmnDialog", "CmnDialogWinBase^s")), R(w), R(h)),
                key.Col("c", C(ui::Cream)));
            if (key.Flag("msg")) {
                const std::string txt =
                    art.Cat().Text(art.Language(), "Dialog/DIALOG_WherearenMsg", "4009");
                std::vector<Image> lines;
                size_t a = 0;
                const double px = key.Num("px", 44);
                while (a <= txt.size()) {
                    const size_t b = std::min(txt.find('\n', a), txt.size());
                    Image li = TextImg(art, txt.substr(a, b - a), px, C("3a2b18"));
                    if (li.w > w * 0.8)
                        li = draw::Fit(li, w * 0.8, li.h);
                    if (!li.Empty())
                        lines.push_back(std::move(li));
                    a = b + 1;
                }
                const double pitch = px * 1.35, cy = h * key.Num("ty", 0.7);
                for (size_t i = 0; i < lines.size(); ++i)
                    draw::PasteC(out, lines[i], w / 2,
                                 cy + (static_cast<double>(i) - (lines.size() - 1) / 2.0) * pitch);
            }
            return true;
        }
        if (n == "icon") { // the reason, wordless: game art only
            const std::string r = p.size() > 3 ? p[3] : "";
            const double s = key.Num("s", 150);
            Image ic;
            bool slash = false;
            if (r == "loading") {
                ArtKey lk;
                if (ArtKey::Parse("ui/wait/loadicon?w=" + std::to_string(R(s)), lk))
                    Recipe(art, lk, ic);
            } else if (r == "menu" || r == "phone") {
                ic = TabIcon(art, r == "menu" ? 1 : 5, s * 0.82);
                slash = true;
            } else if (r == "ring") { // a ring menu open (round 7): the tool ring's own empty-slot
                                      // icon, as the ring draws it (N_Invalid_00: dark disc #1c1d1f
                                      // + RingMenuToolsIconInv^s #969696, 106/192 of the button)
                const double d = s * 0.82;
                ic = Disc(art, d, C("1c1d1fe8"));
                draw::PasteC(
                    ic,
                    draw::Fit(draw::MaskTint(art.Tex("LRingMenuToolsBtn", "RingMenuToolsIconInv^s"),
                                             C("969696")),
                              d * 106 / 192, d * 106 / 192),
                    ic.w / 2.0, ic.h / 2.0);
                slash = true;
            } else if (r == "online") {
                BuildAppButton(art, 13, ic); // Best Friends list (LMenuDeviceBtn_Type frame 13)
                ic = draw::Fit(ic, s * 0.82, s * 0.82);
                slash = true;
            } else if (r == "busy") {
                ic = draw::Fit(art.ModelIcon("Layout_ManpuIcon_Hesitate"), s, s);
            } else if (r == "refused") {
                ic = draw::Fit(art.ModelIcon("Layout_ManpuIcon_Aha"), s, s); // the game's "!"
            } else if (r == "unusable") {
                ic = draw::Fit(art.ModelIcon("Layout_ManpuIcon_QuestionMark"), s, s);
            }
            if (ic.Empty())
                return false;
            out = Blank(s, s);
            draw::PasteC(out, ic, s / 2, s / 2);
            if (slash) { // LRingMenuToolsBtn P_NoEquip_00: PocketMenuNoEquipIcon^s #ff0909 on a
                         // cream disc
                const double sd = s * 0.46;
                const double cx = s - sd / 2 - 1, cy = s - sd / 2 - 1;
                draw::PasteC(out, Disc(art, sd, C(ui::Cream)), cx, cy);
                draw::PasteC(out,
                             draw::Resize(draw::MaskTint(art.Tex("LRingMenuToolsBtn",
                                                                 "PocketMenuNoEquipIcon^s"),
                                                         C("ff0909")),
                                          R(sd * 0.86), R(sd * 0.86)),
                             cx, cy);
            }
            return true;
        }
        return false;
    }
    // ---------------- WAITING / WRONG VERSION
    if (g == "wait" && n == "loadicon") { // MenuMapFull P_LoadIcon_01 (b #130f0a w #e7dfbf) +
                                          // P_LoadIcon_00 (#ff3708)
        // ?w= scales the whole 300 x 200 composition (default 300; h follows)
        const double k = std::clamp(key.Num("w", 300), 30.0, 1200.0) / 300.0;
        const auto px = [k](double v) { return static_cast<int>(std::lround(v * k)); };
        out = Blank(px(300), px(200));
        draw::PasteC(out,
                     draw::Resize(draw::Tint(art.Tex("MenuMapFull", "MapLoadIcon01^t"),
                                             C("130f0a00"), C("e7dfbfff")),
                                  px(300), px(105)),
                     px(150), px(140));
        draw::PasteC(out,
                     draw::Resize(draw::Tint(art.Tex("MenuMapFull", "MapLoadIcon00^t"),
                                             C("00000000"), C("ff3708ff")),
                                  px(150), px(107)),
                     px(150), px(60), C("13110a30"), px(10));
        return true;
    }
    // ---------------- THEME sheet (page_theme)
    if (g ==
        "swatch") { // a crop of the page background (same scale), most varied position, squircle
        const int theme = num(2, -1);
        const double w = key.Num("w", 250), h = key.Num("h", 120);
        Image full;
        ArtKey bg;
        if (theme < 0 || !ArtKey::Parse("bg/" + std::to_string(theme) + "/theme", bg) ||
            !BuildBackground(art, bg, full))
            return false;
        int bx = 0, by = 0;
        double best = -1;
        for (int ox = 0; ox < full.w - static_cast<int>(w); ox += 160)
            for (int oy = 0; oy < full.h - static_cast<int>(h); oy += 160) {
                double s = 0, s2 = 0;
                int nn = 0;
                for (int y = oy; y < oy + static_cast<int>(h); y += 2)
                    for (int x = ox; x < ox + static_cast<int>(w); x += 2) {
                        const uint8_t* q = full.At(x, y);
                        const double lum = (q[0] * 299 + q[1] * 587 + q[2] * 114) / 1000.0;
                        s += lum;
                        s2 += lum * lum;
                        ++nn;
                    }
                const double var = nn ? s2 / nn - (s / nn) * (s / nn) : 0;
                if (var > best) {
                    best = var;
                    bx = ox;
                    by = oy;
                }
            }
        out = draw::Masked(
            draw::Crop(full, bx, by, bx + static_cast<int>(w), by + static_cast<int>(h)),
            Card(art, w, h, C("ffffff"), key.Num("r", 26)));
        return true;
    }
    if (g == "swatchframe") { // selected: #210 stripe frame; idle: #e2d6b0 squircle (LAYOUT)
        const double w = key.Num("w", 250), h = key.Num("h", 120);
        if (key.Flag("on"))
            out = ui::Stripes(art, Card(art, w + 20, h + 20, C("ffffff"), 34), 40);
        else
            out = Card(art, w + 8, h + 8, C("e2d6b0"), 30);
        return true;
    }
    return false;
}
} // namespace

bool BuildAppButton(Art& art, int frame, Image& out) {
    out = draw::CropAlpha(AppButtonRaw(art, frame));
    return !out.Empty() && out.w > 8;
}

bool BuildUi(Art& art, const ArtKey& key, Image& out) {
    if (key.parts.size() < 2 || !Recipe(art, key, out) || out.Empty())
        return false;
    // sc=<rgba>: the element's silhouette in one colour (acnh_mock.shadow_of: rgb = sc, alpha =
    // element alpha x sc alpha), for a shadow drawn as its own widget under any recipe.
    if (key.params.contains("sc"))
        out = draw::ShadowOf(out, key.Col("sc", C("00000040")));
    if (key.params.contains("sh")) {
        const int dy = std::clamp(key.Int("dy", 6), -64, 64);
        Image framed{out.w, out.h + std::abs(dy)};
        const int y0 = dy < 0 ? -dy : 0;
        draw::Composite(framed, draw::ShadowOf(out, key.Col("sh", C("00000040"))), 0, y0 + dy);
        draw::Composite(framed, out, 0, y0);
        out = std::move(framed);
    }
    return true;
}

} // namespace acnh
