// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The Binding of Isaac: Repentance: asset-free art (see isaac_assets.h). Owner: ASSETS lane.
// Every key -> image rule cites its source; research/isaac/assets/REPORT.md has the full table.

#include "isaac_assets.h"

#include "isaac_anm2.h"
#include "isaac_assets_xml.h"
#include "isaac_font.h"
#include "isaac_map.h"
#include "isaac_pcx.h"
#include "isaac_romfs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace isaac_assets {

using isaac_pcx::Image;
using ImagePtr = std::shared_ptr<const Image>;

namespace {

constexpr isaac_romfs::Lang ArtLang = isaac_romfs::Lang::En;

bool ParseInt(std::string_view s, long long& out, long long lo, long long hi) {
    if (s.empty() || s.size() > 12)
        return false;
    bool neg = false;
    std::size_t i = 0;
    if (s[0] == '-') {
        neg = true;
        i = 1;
    }
    if (i >= s.size())
        return false;
    long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (s[i] - '0');
    }
    v = neg ? -v : v;
    if (v < lo || v > hi)
        return false;
    out = v;
    return true;
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t e = s.find(sep, at);
        out.push_back(s.substr(at, e == std::string_view::npos ? s.npos : e - at));
        if (e == std::string_view::npos)
            break;
        at = e + 1;
    }
    return out;
}

std::string Lower(std::string_view s) {
    std::string o{s};
    for (char& c : o)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return o;
}

std::string PngToPcx(std::string p) {
    p = Lower(p);
    if (p.size() >= 4 && p.compare(p.size() - 4, 4, ".png") == 0)
        p.replace(p.size() - 4, 4, ".pcx");
    return p;
}

/// 1-px hard outline of the silhouette (alpha > 96) in `rgb`, drawn inside the image's own box
/// (the icon's transparent border); the image is composited on top (Pillow maths).
Image OutlineInside(const Image& src, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Image sil = isaac_pcx::Blank(src.w, src.h);
    for (std::uint32_t y = 0; y < src.h; ++y)
        for (std::uint32_t x = 0; x < src.w; ++x)
            if (src.At(x, y)[3] > 96) {
                std::uint8_t* q = sil.At(x, y);
                q[0] = r;
                q[1] = g;
                q[2] = b;
                q[3] = 255;
            }
    Image out = isaac_pcx::Blank(src.w, src.h);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx || dy)
                isaac_pcx::Over(out, sil, dx, dy);
    isaac_pcx::Over(out, src, 0, 0);
    return out;
}

/// Same outline but growing the image by 1 px on each side (text labels).
// Round 8: 90 degrees clockwise, pixel for pixel (a right-pointing arrow points down).
Image Rot90(const Image& src) {
    Image out;
    out.w = src.h;
    out.h = src.w;
    out.px.assign(src.px.size(), 0);
    for (std::uint32_t y = 0; y < out.h; ++y)
        for (std::uint32_t x = 0; x < out.w; ++x)
            std::memcpy(out.At(x, y), src.At(y, src.h - 1 - x), 4);
    return out;
}

Image OutlineGrow(const Image& src) {
    Image padded = isaac_pcx::Blank(src.w + 2, src.h + 2);
    isaac_pcx::Over(padded, src, 1, 1);
    return OutlineInside(padded, 0, 0, 0);
}

} // namespace

class LibraryImpl {
public:
    explicit LibraryImpl(std::size_t budget) : budget{budget} {}

    std::mutex mu;
    std::size_t budget;
    const EdenDsmodHostApi* host{};
    isaac_romfs::FileCache files{std::size_t(16) << 20};

    // decoded sheets (by romfs path) and parsed anm2 (by path)
    struct SheetEntry {
        ImagePtr img;
        std::list<std::string>::iterator it;
    };
    std::list<std::string> sheet_lru;
    std::unordered_map<std::string, SheetEntry> sheets;
    std::size_t sheet_bytes{};
    std::unordered_map<std::string, std::shared_ptr<const isaac_anm2::File>> anms;
    std::unordered_map<std::string, std::shared_ptr<const isaac_font::BmFont>> fonts;
    bool bundle_tried{};
    std::unordered_map<std::uint32_t,std::shared_ptr<const isaac_anm2::File>> bundle;

    // finished images (LRU)
    std::list<std::string> lru;
    struct Entry {
        ImagePtr img;
        std::list<std::string>::iterator it;
    };
    std::unordered_map<std::string, Entry> images;
    std::size_t image_bytes{};

    // game tables (lazy)
    bool tables_loaded{}, tables_ok{};
    std::string gfxroot;
    std::map<int, std::string> coll_gfx, trink_gfx; // id -> items.xml gfx
    std::map<int, std::string> card_hud;
    std::map<int, int> card_pickup;                 // card id -> entity 5.300 subtype
    std::string anm2root;
    std::map<std::pair<int, int>, std::string> pickup_anm2; // (variant, subtype) of type 5

    bool map_sprites_ok{};
    bool map_sprites_tried{};
    isaac_map::Sprites map_sprites;

    bool Afterbirth() const { return host && host->get_i64 && host->get_i64(host->userdata, "__source:aoc", -1) == 0; }

    // ---- helpers (mu held) ----
    ImagePtr DecodedSheet(const std::string& key, bool raw) {
        const auto it = sheets.find(key);
        if (it != sheets.end()) {
            sheet_lru.splice(sheet_lru.begin(), sheet_lru, it->second.it);
            return it->second.img;
        }
        const auto bytes = raw ? files.Raw(host, std::string_view{key}.substr(1))
                               : files.Resource(host, key, ArtLang);
        if (!bytes)
            return nullptr;
        auto img = std::make_shared<Image>();
        if (!isaac_pcx::Decode(bytes->data(), bytes->size(), *img))
            return nullptr;
        return KeepSheet(key, img);
    }
    ImagePtr KeepSheet(const std::string& key, ImagePtr img) {
        constexpr std::size_t SheetBudget = std::size_t(48) << 20;
        if (img->px.size() > SheetBudget)
            return img;
        sheet_lru.push_front(key);
        sheets.emplace(key, SheetEntry{img, sheet_lru.begin()});
        sheet_bytes += img->px.size();
        while (sheet_bytes > SheetBudget || sheets.size() > 256) {
            const auto victim = sheets.find(sheet_lru.back());
            sheet_bytes -= victim->second.img->px.size();
            sheets.erase(victim);
            sheet_lru.pop_back();
        }
        return img;
    }
    ImagePtr Sheet(const std::string& rel) {
        return DecodedSheet(rel, false);
    }
    ImagePtr RawSheet(const std::string& source_path) {
        return DecodedSheet("@" + source_path, true);
    }
    std::shared_ptr<const isaac_anm2::File> Anm(const std::string& rel) {
        const auto it = anms.find(rel);
        if (it != anms.end())
            return it->second;
        const auto bytes = files.Resource(host, rel, ArtLang);
        if (!bytes) {
            if (!bundle_tried) {
                bundle_tried=true;
                const auto packed=files.Resource(host,"animations.b",ArtLang);
                if(packed)isaac_anm2::ParseBundle(*packed,bundle);
            }
            const auto entry=bundle.find(isaac_anm2::BundleHash(rel));
            return entry==bundle.end()?nullptr:entry->second;
        }
        auto f = std::make_shared<isaac_anm2::File>();
        if (!isaac_anm2::Parse({reinterpret_cast<const char*>(bytes->data()), bytes->size()}, *f))
            return nullptr;
        if (anms.size() > 256)
            anms.clear();
        anms.emplace(rel, f);
        return f;
    }
    std::shared_ptr<const isaac_font::BmFont> Font(const std::string& name) {
        if (!isaac_font::ValidFontName(name))
            return nullptr;
        const auto it = fonts.find(name);
        if (it != fonts.end())
            return it->second;
        const auto bytes = files.Resource(host, "font/" + name + ".fnt", ArtLang);
        if (!bytes)
            return nullptr;
        auto f = std::make_shared<isaac_font::BmFont>();
        if (!isaac_font::ParseBmFont(*bytes, *f))
            return nullptr;
        if (fonts.size() >= 32)
            fonts.clear();
        fonts.emplace(name, f);
        return f;
    }

    /// One layer's frame at time t as a sprite (crop + pivot). layer: -1 = the first layer
    /// animation with a non-empty visible frame (file order), else the layer id.
    isaac_map::Sprite Frame(const std::string& anm2_rel, std::string_view anim, int t,
                            int layer = -1) {
        isaac_map::Sprite s;
        const auto f = Anm(anm2_rel);
        if (!f)
            return s;
        const isaac_anm2::Animation* a = f->Find(anim);
        if (!a)
            return s;
        for (const auto& la : a->layers) {
            if (layer >= 0 && la.layer_id != layer)
                continue;
            const isaac_anm2::Frame* fr = isaac_anm2::FrameAt(la, t);
            if (!fr || fr->w <= 0 || fr->h <= 0 || (layer < 0 && !fr->visible))
                continue;
            const isaac_anm2::Layer* L = f->FindLayer(la.layer_id);
            const isaac_anm2::Sheet* S = L ? f->FindSheet(L->sheet) : nullptr;
            if (!S)
                return s;
            const ImagePtr sheet = Sheet(isaac_anm2::SheetPath(f->sheet_root.empty()?anm2_rel:f->sheet_root+"/animation.anm2", S->path));
            if (!sheet)
                return s;
            s.img = isaac_pcx::Crop(*sheet, fr->x, fr->y, static_cast<std::uint32_t>(fr->w),
                                    static_cast<std::uint32_t>(fr->h));
            s.pivot_x = fr->pivot_x;
            s.pivot_y = fr->pivot_y;
            s.ok = true;
            return s;
        }
        return s;
    }

    bool LoadTables() {
        if (tables_loaded)
            return tables_ok;
        tables_loaded = true;
        isaac_xml::Node root;
        auto b = files.Resource(host, "items.xml", ArtLang);
        if (!b || !isaac_xml::Parse(*b, root) || root.name != "items")
            return false;
        gfxroot = Lower(root.AttrOr("gfxroot"));
        for (const auto& e : root.children) {
            long long id;
            if (!isaac_xml::AttrInt(*e, "id", id) || !e->Attr("gfx"))
                continue;
            if (e->name == "passive" || e->name == "active" || e->name == "familiar")
                coll_gfx[static_cast<int>(id)] = *e->Attr("gfx");
            else if (e->name == "trinket")
                trink_gfx[static_cast<int>(id)] = *e->Attr("gfx");
        }
        b = files.Resource(host, "pocketitems.xml", ArtLang);
        if (b && isaac_xml::Parse(*b, root))
            for (const auto& e : root.children) {
                long long id, pickup;
                if ((e->name == "card" || e->name == "rune") && isaac_xml::AttrInt(*e, "id", id) && e->Attr("hud"))
                    card_hud[static_cast<int>(id)] = *e->Attr("hud");
                if ((e->name == "card" || e->name == "rune") && isaac_xml::AttrInt(*e, "id", id) &&
                    isaac_xml::AttrInt(*e, "pickup", pickup))
                    card_pickup[static_cast<int>(id)] = static_cast<int>(pickup);
            }
        b = files.Resource(host, "entities2.xml", ArtLang);
        if (b && isaac_xml::Parse(*b, root)) {
            anm2root = Lower(root.AttrOr("anm2root"));
            for (const auto& e : root.children) {
                long long id, variant, subtype;
                if (e->name != "entity" || !isaac_xml::AttrInt(*e, "id", id) || id != 5 ||
                    !isaac_xml::AttrInt(*e, "variant", variant) || !e->Attr("anm2path"))
                    continue;
                subtype = isaac_xml::AttrIntOr(*e, "subtype", 0);
                pickup_anm2[{static_cast<int>(variant), static_cast<int>(subtype)}] =
                    *e->Attr("anm2path");
            }
        }
        tables_ok = true;
        return true;
    }

    bool MapSprites() {
        if (map_sprites_tried)
            return map_sprites_ok;
        map_sprites_tried = true;
        const std::string mm2 = "gfx/ui/minimap2.anm2", icons = Afterbirth() ? mm2 : "gfx/ui/minimap_icons.anm2";
        map_sprites.outline = Frame(mm2, "RoomOutline", 0);
        static constexpr const char* States[3] = {"RoomVisited", "RoomUnvisited", "RoomCurrent"};
        for (int s = 0; s < 3; ++s)
            for (int i = 0; i < 12; ++i) {
                map_sprites.room[s][i] = Frame(mm2, States[s], i);
                if (!map_sprites.room[s][i].ok)
                    return false;
            }
        const auto f = Anm(icons);
        if (!f || !map_sprites.outline.ok)
            return false;
        for (const auto& a : f->anims)
            map_sprites.icons[a.name] = Frame(icons, a.name, 0);
        map_sprites_ok = true;
        return true;
    }

    // ---- keys ----
    bool Key(std::string_view key, Image& out);
    bool Background(std::string_view part, Image& out);
    bool Collectible(int id, Image& out);
    bool Trinket(int id, bool gold, Image& out);
    bool Pocket(int kind, int id, bool small, Image& out);
    bool Hud(std::string_view name, Image& out);
    bool Stat(int n, bool white, Image& out);
    bool Tally(int n, int marks, bool white, Image& out);
    bool Charge(int cur, int max, int battery, Image& out);
    ImagePtr FontAtlasImage(const std::string& name);
    bool FontAtlas(const std::string& name, Image& out);
    bool TextImage(const std::string& font, std::string_view utf8, bool outline, Image& out);
    bool Ui(std::string_view spec, Image& out);
    bool Compose(const std::string& anm2_rel, std::string_view anim, int t, std::string_view over,
                 int t2, const std::vector<int>& only, Image& out);
    bool Anim(std::string_view spec, Image& out);
};

Library::Library(std::size_t image_budget) : p{std::make_unique<LibraryImpl>(image_budget)} {}
Library::~Library() = default;

std::unique_ptr<Library> CreateLibrary(std::size_t image_budget) {
    return std::make_unique<Library>(image_budget);
}

// ---------------------------------------------------------------------------------------------
// Wii U background: base:resources/wiiubottomscreen.pcx (854x480, base romfs only).
// The paper pieces are cut along their own black outline (flood fill of the surrounding wood,
// stopped by the outline (mean < 22) or paper (mean > 130)); `page` recomposes the mockup's
// background at 1x (620x540, the UI stretches it x2 to the 1240x1080 aux canvas).
namespace {
constexpr int BigRect[4] = {166, 11, 682, 479};
constexpr int ScrapRect[4] = {13, 95, 171, 245};
constexpr int WoodRect[4] = {700, 0, 854, 480};
constexpr int PageW = 620, PageH = 540;
constexpr int BigAt[2] = {106, 74}, ScrapAt[2] = {3, 75};
constexpr int ShadowOff[2] = {2, 3};
constexpr std::uint8_t ShadowAlpha = 110;

Image Cutout(const Image& src, const int r[4]) {
    Image im = isaac_pcx::Crop(src, r[0], r[1], static_cast<std::uint32_t>(r[2] - r[0]),
                               static_cast<std::uint32_t>(r[3] - r[1]));
    const int W = static_cast<int>(im.w), H = static_cast<int>(im.h);
    std::vector<std::uint8_t> wood(std::size_t(W) * H, 0);
    std::vector<std::pair<int, int>> st;
    for (int x = 0; x < W; ++x) {
        st.push_back({x, 0});
        st.push_back({x, H - 1});
    }
    for (int y = 0; y < H; ++y) {
        st.push_back({0, y});
        st.push_back({W - 1, y});
    }
    while (!st.empty()) {
        const auto [x, y] = st.back();
        st.pop_back();
        if (x < 0 || y < 0 || x >= W || y >= H || wood[std::size_t(y) * W + x])
            continue;
        const std::uint8_t* p = im.At(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        const int sum = p[0] + p[1] + p[2];
        if (sum < 66 || sum > 390) // mean < 22 (outline) or > 130 (paper)
            continue;
        wood[std::size_t(y) * W + x] = 1;
        st.push_back({x + 1, y});
        st.push_back({x - 1, y});
        st.push_back({x, y + 1});
        st.push_back({x, y - 1});
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (wood[std::size_t(y) * W + x])
                std::memset(im.At(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)), 0, 4);
    return im;
}

Image WoodCanvas(const Image& src) {
    const Image t = isaac_pcx::Crop(src, WoodRect[0], WoodRect[1],
                                    static_cast<std::uint32_t>(WoodRect[2] - WoodRect[0]),
                                    static_cast<std::uint32_t>(WoodRect[3] - WoodRect[1]));
    const Image tf = isaac_pcx::FlipX(t), tv = isaac_pcx::FlipY(t), tvf = isaac_pcx::FlipY(tf);
    Image c = isaac_pcx::Blank(PageW, PageH);
    const int sw = static_cast<int>(t.w), sh = static_cast<int>(t.h);
    const int y0 = PageH - sh;
    for (int i = 0, x = 0; x < PageW; ++i, x += sw) {
        isaac_pcx::Over(c, i % 2 == 0 ? t : tf, x, y0);
        isaac_pcx::Over(c, i % 2 == 0 ? tv : tvf, x, y0 - sh);
    }
    return c;
}

void Shadow(Image& dst, const Image& piece, int x, int y) {
    Image s = isaac_pcx::Blank(piece.w, piece.h);
    for (std::uint32_t yy = 0; yy < piece.h; ++yy)
        for (std::uint32_t xx = 0; xx < piece.w; ++xx)
            s.At(xx, yy)[3] = piece.At(xx, yy)[3] ? ShadowAlpha : 0;
    isaac_pcx::Over(dst, s, x + ShadowOff[0], y + ShadowOff[1]);
}

// Round 3 `sheetframe`: the MAP page's map box (the big sheet clipped to the canvas: canvas
// (250,148) 990x932 = 1x (125,74) 495x466, the sheet's own top-left) as the page shows it -- wood,
// the sheet's drop shadow, the sheet's torn black outline -- with the sheet's paper interior
// transparent. Drawn above the map image, it clips the map to the paper. Outline = black pixels
// 4-connected (through black) to the transparent surroundings of the cut sheet; interior = every
// other opaque sheet pixel (research/isaac/r3/tools/sheet_interior.py does the same offline).
constexpr int FrameAt[2] = {125, 74};
constexpr int FrameW = 495, FrameH = 466;

Image SheetFrame(const Image& src) {
    const Image big = Cutout(src, BigRect);
    const int W = static_cast<int>(big.w), H = static_cast<int>(big.h);
    const auto idx = [W](int x, int y) { return std::size_t(y) * W + x; };
    const auto alpha = [&](int x, int y) {
        return big.At(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))[3];
    };
    const auto black = [&](int x, int y) {
        const std::uint8_t* p = big.At(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        return p[3] != 0 && p[0] == 0 && p[1] == 0 && p[2] == 0;
    };
    // 0 = not yet, 1 = outside (transparent, border-connected), 2 = outline
    std::vector<std::uint8_t> cls(std::size_t(W) * H, 0);
    std::vector<std::pair<int, int>> st;
    for (int x = 0; x < W; ++x) {
        st.push_back({x, 0});
        st.push_back({x, H - 1});
    }
    for (int y = 0; y < H; ++y) {
        st.push_back({0, y});
        st.push_back({W - 1, y});
    }
    while (!st.empty()) {
        const auto [x, y] = st.back();
        st.pop_back();
        if (x < 0 || y < 0 || x >= W || y >= H || cls[idx(x, y)] || alpha(x, y) != 0)
            continue;
        cls[idx(x, y)] = 1;
        st.insert(st.end(), {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}});
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!black(x, y) || cls[idx(x, y)])
                continue;
            const bool touches = (x > 0 && cls[idx(x - 1, y)] == 1) ||
                                 (x + 1 < W && cls[idx(x + 1, y)] == 1) ||
                                 (y > 0 && cls[idx(x, y - 1)] == 1) ||
                                 (y + 1 < H && cls[idx(x, y + 1)] == 1) || x == 0 || y == 0 ||
                                 x + 1 == W || y + 1 == H;
            if (touches)
                st.push_back({x, y});
        }
    while (!st.empty()) {
        const auto [x, y] = st.back();
        st.pop_back();
        if (x < 0 || y < 0 || x >= W || y >= H || cls[idx(x, y)] || !black(x, y))
            continue;
        cls[idx(x, y)] = 2;
        st.insert(st.end(), {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}});
    }
    Image out = isaac_pcx::Crop(WoodCanvas(src), FrameAt[0], FrameAt[1], FrameW, FrameH);
    Shadow(out, big, 0, 0);
    isaac_pcx::Over(out, big, 0, 0);
    for (int y = 0; y < std::min(H, FrameH); ++y)
        for (int x = 0; x < std::min(W, FrameW); ++x)
            if (alpha(x, y) != 0 && cls[idx(x, y)] != 2)
                std::memset(out.At(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)), 0, 4);
    return out;
}
} // namespace

bool LibraryImpl::Background(std::string_view part, Image& out) {
    const ImagePtr src = RawSheet("base:resources/wiiubottomscreen.pcx");
    if (!src)
        return false;
    if (part.empty()) {
        out = *src;
        return true;
    }
    if (src->w != 854 || src->h != 480)
        return false; // the cut rectangles belong to the 854x480 art
    if (part == "sheet") {
        out = Cutout(*src, BigRect);
        return true;
    }
    if (part == "scrap") {
        out = Cutout(*src, ScrapRect);
        return true;
    }
    if (part == "wood") {
        out = WoodCanvas(*src);
        return true;
    }
    if (part == "sheetframe") {
        out = SheetFrame(*src);
        return true;
    }
    if (part == "page") {
        out = WoodCanvas(*src);
        const Image big = Cutout(*src, BigRect), scrap = Cutout(*src, ScrapRect);
        Shadow(out, big, BigAt[0], BigAt[1]);
        isaac_pcx::Over(out, big, BigAt[0], BigAt[1]);
        Shadow(out, scrap, ScrapAt[0], ScrapAt[1]);
        isaac_pcx::Over(out, scrap, ScrapAt[0], ScrapAt[1]);
        return true;
    }
    return false;
}

// Collectible / trinket icons: items.xml gfxroot + "collectibles/" | "trinkets/" + gfx
// (lower-case, .png -> .pcx), 32x32 as stored.
bool LibraryImpl::Collectible(int id, Image& out) {
    if (!LoadTables())
        return false;
    const auto it = coll_gfx.find(id);
    if (it == coll_gfx.end())
        return false;
    const ImagePtr img = Sheet(PngToPcx(gfxroot + "collectibles/" + it->second));
    if (!img)
        return false;
    out = *img;
    return true;
}

// Golden trinket (owner-approved, CONTRACT): the game uses a shader; we draw a 1-px gold
// (232,184,40) outline around the icon's silhouette, inside its own 32x32 box.
bool LibraryImpl::Trinket(int id, bool gold, Image& out) {
    if (!LoadTables())
        return false;
    const auto it = trink_gfx.find(id);
    if (it == trink_gfx.end())
        return false;
    const ImagePtr img = Sheet(PngToPcx(gfxroot + "trinkets/" + it->second));
    if (!img)
        return false;
    out = gold ? OutlineInside(*img, 232, 184, 40) : *img;
    return true;
}

// Pocket items as the HUD draws them (HUD::PlayerHUD::RenderPocketItems @+0x3A6094): the pickup
// entity's anm2 (entities2.xml type 5, variant 300 subtype = pocketitems.xml pickup for cards and
// runes, variant 70 subtype = pill colour) played at "HUD" (slot 0, 32x32) or "HUDSmall" (slots
// 1..3, 16x16, SetFrame @0x3A6250). A pocket active (kind 2) is drawn by RenderActiveItem: the
// collectible icon.
bool LibraryImpl::Pocket(int kind, int id, bool small, Image& out) {
    if (!LoadTables())
        return false;
    if (kind == 2)
        return Collectible(id, out);
    if (Afterbirth()) {
        std::string anim;
        int frame = 0;
        if (kind == 0 && id >= 1 && id <= 13) {
            anim = "Pills"; frame = id - 1;
        } else if (kind == 1 && id >= 1 && id <= 54) {
            // ItemConfig::LoadPocketItems +0x1FCAAC; HUD table +0x7A6828.
            static constexpr int Types[] = {7,8,10,2,9,2,12,2,6,5,11,8,8,8};
            static constexpr const char* Names[] = {"Cards","Cards","Cards","Runes","Runes","Pickups","Pickups","Runes","Cards","Cards","Cards","Cards","Cards"};
            static constexpr int Frames[] = {1,0,1,0,1,1,0,2,2,3,4,5,6};
            const int type = id <= 22 ? 1 : id <= 31 ? 2 : id <= 35 ? 3 : id <= 40 ? 4 : Types[id-41];
            anim = Names[type]; frame = Frames[type];
        } else return false;
        if (small) anim += "Small";
        const auto sprite = Frame("gfx/ui/ui_cardspills.anm2", anim, frame);
        if (!sprite.ok) return false;
        out = sprite.img;
        return true;
    }
    int variant, subtype;
    if (kind == 1) {
        const auto it = card_pickup.find(id);
        if (it == card_pickup.end())
            return false;
        variant = 300;
        subtype = it->second;
    } else if (kind == 0) {
        variant = 70;
        subtype = id;
    } else {
        return false;
    }
    const auto it = pickup_anm2.find({variant, subtype});
    if (it == pickup_anm2.end())
        return false;
    const std::string rel = isaac_anm2::NormalizePath(anm2root + it->second);
    const isaac_map::Sprite s = Frame(rel, small ? "HUDSmall" : "HUD", 0);
    if (!s.ok)
        return false;
    out = s.img;
    return true;
}

// HUD pickups: HUD::Render @+0x3AC71C, hudpickups.anm2 "Idle": coin frame 0 (0x3ACAD0), bomb 2 /
// golden bomb 6 (P+0x16D9, 0x3ACC44), key 1 / golden key 3 (P+0x16D8, 0x3ACE10). Any other name
// is a ui_hearts.anm2 animation (frame 0).
bool LibraryImpl::Hud(std::string_view name, Image& out) {
    int frame = -1;
    if (name == "coin")
        frame = 0;
    else if (name == "key")
        frame = 1;
    else if (name == "bomb")
        frame = 2;
    else if (name == "gkey")
        frame = 3;
    else if (name == "gbomb")
        frame = 6;
    isaac_map::Sprite s = frame >= 0 ? Frame("gfx/ui/hudpickups.anm2", "Idle", frame)
                                     : Frame("gfx/ui/ui_hearts.anm2", name, 0);
    if (!s.ok)
        return false;
    out = std::move(s.img);
    return true;
}

// Pause-screen stat icons: the ink of pausescreen.pcx inside the pausescreenstats.anm2 "Paper"
// frame (Idle, layer 0: 52,50 128x56); six boxes (speed, tears, damage | range, shot speed,
// luck), ink = alpha && mean < 110, drawn in (58,44,36), trimmed and centred in 24x16.
bool LibraryImpl::Stat(int n, bool white, Image& out) {
    static constexpr int Box[6][4] = {{0, 0, 30, 18},  {0, 18, 30, 37},  {0, 37, 30, 56},
                                      {70, 0, 90, 18}, {70, 18, 90, 37}, {70, 37, 90, 56}};
    if (n < 0 || n > 5)
        return false;
    const isaac_map::Sprite paper = Frame("gfx/ui/pausescreenstats.anm2", "Idle", 0, 0);
    if (!paper.ok)
        return false;
    const int* b = Box[n];
    Image ink = isaac_pcx::Crop(paper.img, b[0], b[1], static_cast<std::uint32_t>(b[2] - b[0]),
                                static_cast<std::uint32_t>(b[3] - b[1]));
    for (std::uint32_t y = 0; y < ink.h; ++y)
        for (std::uint32_t x = 0; x < ink.w; ++x) {
            std::uint8_t* q = ink.At(x, y);
            if (q[3] && (q[0] + q[1] + q[2]) < 330) {
                // round 3 "/w": the same ink white, for the left card on the wood (the UI
                // outlines it black like its text)
                q[0] = white ? 255 : 58;
                q[1] = white ? 255 : 44;
                q[2] = white ? 255 : 36;
                q[3] = 255;
            } else {
                std::memset(q, 0, 4);
            }
        }
    ink = isaac_pcx::Trim(ink);
    out = isaac_pcx::Blank(24, 16);
    isaac_pcx::Over(out, ink, (24 - static_cast<int>(ink.w)) / 2, (16 - static_cast<int>(ink.h)) / 2);
    return true;
}

// Pause-screen stat tally (32x16): PauseScreen::Update (nro+0x432AD8..0x432B34) draws
// pausescreenstats.anm2 "Idle" frame 0 with layer stat+1 ("Meter1".."Meter6") set to frame =
// rating 0..7 (PauseScreen::Show 0x43274C clamps the rating to 0..7). Frame k = k solid marks;
// behind them the paper (layer 0) has 7 faded marks. The image = the paper's 32x16 box under the
// meter (meter top-left = paper pivot - meter pivot) without the paper colour, the meter frame on
// top. Game colours; "/w" (round 5, for the left card on the wood): ink coverage relative to the
// paper as white alpha (paper 0, ink 1: alpha = (paper.r - r) / (paper.r - ink.r)), so the solid
// marks are white and the faded ones ~32 % white, like the paper's own contrast.
bool LibraryImpl::Tally(int n, int marks, bool white, Image& out) {
    if (n < 0 || n > 5 || marks < 0 || marks > 7)
        return false;
    const std::string a = "gfx/ui/pausescreenstats.anm2";
    const isaac_map::Sprite paper = Frame(a, "Idle", 0, 0);
    const isaac_map::Sprite meter = Frame(a, "Idle", marks, n + 1);
    if (!paper.ok || !meter.ok || paper.img.w == 0 || paper.img.h == 0)
        return false;
    const int bx = paper.pivot_x - meter.pivot_x, by = paper.pivot_y - meter.pivot_y;
    out = isaac_pcx::Crop(paper.img, bx, by, meter.img.w, meter.img.h);
    const std::uint8_t* bg = paper.img.At(0, 0); // the paper colour (its corner)
    const std::uint8_t pr = bg[0], pg = bg[1], pb = bg[2];
    for (std::uint32_t y = 0; y < out.h; ++y)
        for (std::uint32_t x = 0; x < out.w; ++x) {
            std::uint8_t* q = out.At(x, y);
            if (q[0] == pr && q[1] == pg && q[2] == pb)
                std::memset(q, 0, 4);
        }
    isaac_pcx::Over(out, meter.img, 0, 0);
    if (!white)
        return true;
    int ink = 255; // the meter's darkest ink (red channel)
    for (std::uint32_t y = 0; y < meter.img.h; ++y)
        for (std::uint32_t x = 0; x < meter.img.w; ++x) {
            const std::uint8_t* q = meter.img.At(x, y);
            if (q[3])
                ink = std::min<int>(ink, q[0]);
        }
    if (marks == 0)
        ink = 54; // frame 0 is empty: the ink of frames 1..7 (pausescreen.pcx (54,47,45))
    const int span = std::max(1, pr - ink);
    for (std::uint32_t y = 0; y < out.h; ++y)
        for (std::uint32_t x = 0; x < out.w; ++x) {
            std::uint8_t* q = out.At(x, y);
            if (!q[3])
                continue;
            const int al = std::clamp((pr - q[0]) * 255 / span, 0, 255) * q[3] / 255;
            q[0] = q[1] = q[2] = 255;
            q[3] = static_cast<std::uint8_t>(al);
        }
    return true;
}

// Charge bar: HUD::PlayerHUD::RenderActiveItem @+0x3A3ED4, ui_chargebar.anm2:
//   "BarEmpty" (0x3A4DE0); "BarFull" with the top cropped by 3 + (1 - charge/max) * 23 px
//   (0x3A523C); battery charge: "BarFull" again with ColorMod offset (255,0,0), top cropped by
//   3 + (1 - min(battery/max, 1)) * 23 (0x3A539C); overlay "BarOverlay<max>" from the table at
//   nro+0xA382A8 (max 2..12: 2,3,4,5,6,1,8,1,1,1,12; else BarOverlay1) (0x3A4D8C).
// A cropped quad shows the rows whose centre lies below the crop line (nearest sampling).
bool LibraryImpl::Charge(int cur, int max, int battery, Image& out) {
    static constexpr int Overlay[11] = {2, 3, 4, 5, 6, 1, 8, 1, 1, 1, 12};
    if (max < 1 || max > 12 || cur < 0 || cur > 99 || battery < 0 || battery > 99)
        return false;
    const std::string a = "gfx/ui/ui_chargebar.anm2";
    const isaac_map::Sprite empty = Frame(a, "BarEmpty", 0), full = Frame(a, "BarFull", 0);
    const int ov = (max >= 2 && max <= 12) ? Overlay[max - 2] : 1;
    const isaac_map::Sprite over = Frame(a, "BarOverlay" + std::to_string(ov), 0);
    if (!empty.ok || !full.ok || !over.ok)
        return false;
    out = isaac_pcx::Blank(empty.img.w, empty.img.h);
    const auto place = [&](const isaac_map::Sprite& s) {
        return std::pair{empty.pivot_x - s.pivot_x, empty.pivot_y - s.pivot_y};
    };
    isaac_pcx::Over(out, empty.img, 0, 0);
    const auto clipped = [&](double frac, bool red) {
        frac = std::clamp(frac, 0.0, 1.0);
        const float crop = static_cast<float>(3.0 + (1.0 - frac) * 23.0);
        Image part = full.img;
        for (std::uint32_t y = 0; y < part.h; ++y) {
            const bool shown = static_cast<float>(y) + 0.5f >= crop;
            for (std::uint32_t x = 0; x < part.w; ++x) {
                std::uint8_t* q = part.At(x, y);
                if (!shown)
                    std::memset(q, 0, 4);
                else if (red && q[3])
                    q[0] = 255;
            }
        }
        const auto [dx, dy] = place(full);
        isaac_pcx::Over(out, part, dx, dy);
    };
    if (cur > 0)
        clipped(static_cast<double>(cur) / max, false);
    if (battery > 0)
        clipped(static_cast<double>(battery) / max, true);
    const auto [ox, oy] = place(over);
    isaac_pcx::Over(out, over.img, ox, oy);
    return true;
}

// Font atlas: font/<name>.fnt (binary BMFont) page 0 (<page>.png -> .pcx next to the .fnt). The
// TeamMeat pages store white glyphs premultiplied (r = g = b = a); they are returned as white +
// alpha. Pages with real colour (baked shadows, e.g. pftempestasevencondensed) are returned as is.
ImagePtr LibraryImpl::FontAtlasImage(const std::string& name) {
    const std::string key = "!font/" + name;
    const auto cached = sheets.find(key);
    if (cached != sheets.end()) {
        sheet_lru.splice(sheet_lru.begin(), sheet_lru, cached->second.it);
        return cached->second.img;
    }
    const auto f = Font(name);
    if (!f || f->pages.empty())
        return nullptr;
    const ImagePtr page = Sheet(isaac_anm2::SheetPath("font/" + name + ".fnt", PngToPcx(f->pages[0])));
    if (!page)
        return nullptr;
    Image out = *page;
    bool premult_white = true;
    for (std::size_t i = 0; i + 3 < out.px.size() && premult_white; i += 4)
        if (out.px[i + 3] && !(out.px[i] == out.px[i + 3] && out.px[i + 1] == out.px[i + 3] &&
                               out.px[i + 2] == out.px[i + 3]))
            premult_white = false;
    if (premult_white) {
        for (std::size_t i = 0; i + 3 < out.px.size(); i += 4) {
            const std::uint8_t v = out.px[i + 3] ? 255 : 0;
            out.px[i] = out.px[i + 1] = out.px[i + 2] = v;
        }
    }
    return KeepSheet(key, std::make_shared<const Image>(std::move(out)));
}

bool LibraryImpl::FontAtlas(const std::string& name, Image& out) {
    const auto atlas = FontAtlasImage(name);
    if (!atlas)
        return false;
    out = *atlas;
    return true;
}

// A string in a game font (white glyphs from the atlas, BMFont layout incl. kerning; a missing
// glyph falls back to its upper-case form, then '?'), trimmed, then a 1-px black outline.
bool LibraryImpl::TextImage(const std::string& name, std::string_view utf8, bool outline, Image& out) {
    const auto f = Font(name);
    const auto atlas = FontAtlasImage(name);
    if (!f || !atlas)
        return false;
    const std::vector<std::uint32_t> cps = isaac_font::DecodeUtf8(utf8);
    if (cps.empty() || cps.size() > 256)
        return false;
    const auto glyph = [&](std::uint32_t cp) -> std::pair<std::uint32_t, const isaac_font::Char*> {
        if (const auto* c = f->Find(cp))
            return {cp, c};
        if (cp >= 'a' && cp <= 'z')
            if (const auto* c = f->Find(cp - 32))
                return {cp - 32, c};
        return {'?', f->Find('?')};
    };
    int width = 0, pen = 0;
    std::uint32_t prev = 0;
    bool first = true;
    for (const std::uint32_t cp0 : cps) {
        const auto [cp, c] = glyph(cp0);
        if (!c)
            return false;
        if (!first) {
            const auto k = f->kerning.find({prev, cp});
            if (k != f->kerning.end())
                pen += k->second;
        }
        width = std::max(width, pen + c->xoffset + c->w);
        pen += c->xadvance;
        width = std::max(width, pen);
        prev = cp;
        first = false;
    }
    if (width <= 0 || width > 4000)
        return false;
    Image canvas = isaac_pcx::Blank(static_cast<std::uint32_t>(width + 8), f->line_height + 8u);
    pen = 2;
    first = true;
    for (const std::uint32_t cp0 : cps) {
        const auto [cp, c] = glyph(cp0);
        if (!first) {
            const auto k = f->kerning.find({prev, cp});
            if (k != f->kerning.end())
                pen += k->second;
        }
        if (c->w && c->h && c->page == 0) {
            const Image g = isaac_pcx::Crop(*atlas, c->x, c->y, c->w, c->h);
            isaac_pcx::Over(canvas, g, pen + c->xoffset, 2 + c->yoffset);
        }
        pen += c->xadvance;
        prev = cp;
        first = false;
    }
    Image t = isaac_pcx::Trim(canvas);
    std::uint32_t x0, y0, x1, y1;
    if (!isaac_pcx::AlphaBox(t, x0, y0, x1, y1))
        t = isaac_pcx::Blank(1, 1); // only spaces
    out = outline ? OutlineGrow(t) : t;
    return true;
}

// Generic anm2 frame: "<anm2 path>#<anim>#<frame>[#<layer id>]", the frame's own Width x Height.
bool LibraryImpl::Ui(std::string_view spec, Image& out) {
    const auto parts = Split(spec, '#');
    if (parts.size() < 3 || parts.size() > 4)
        return false;
    const std::string rel = isaac_anm2::NormalizePath(parts[0]);
    if (rel.size() < 6 || rel.compare(rel.size() - 5, 5, ".anm2") != 0)
        return false;
    long long frame, layer = -1;
    if (!ParseInt(parts[2], frame, 0, 100000) ||
        (parts.size() == 4 && !ParseInt(parts[3], layer, 0, 1000)))
        return false;
    const isaac_map::Sprite s = Frame(rel, parts[1], static_cast<int>(frame), static_cast<int>(layer));
    if (!s.ok)
        return false;
    out = s.img;
    return true;
}

// Round 6 (loading page): an anm2 animation drawn the way ANM2 rendering places it at the
// entity origin: every visible layer animation (file order; `only` = these layer ids) shows its
// keyframe at time t (FrameAt, t wrapped to the animation's FrameNum), its crop scaled by
// XScale/YScale (nearest; a negative scale mirrors) around its pivot, top-left at
// Position - Pivot*scale. An optional overlay animation of the same file (the player's head:
// Entity_Player draws the body animation with the head animation as its overlay, both on the one
// 001.000_player.anm2) is drawn after it at t2. The canvas is the union of EVERY keyframe of the
// animation(s), so all frames of one animation have the same size (a fixed widget rect, no
// jitter) and the origin sits at the same pixel. Not reproduced (none of the keys the UI uses
// needs them): keyframe interpolation, rotation, tint / colour offsets, the root animation.
bool LibraryImpl::Compose(const std::string& rel, std::string_view anim, int t, std::string_view over,
                          int t2, const std::vector<int>& only, Image& out) {
    const auto f = Anm(rel);
    if (!f)
        return false;
    struct Pass {
        const isaac_anm2::Animation* a;
        int t;
    };
    std::vector<Pass> passes;
    if (const auto* a = f->Find(anim))
        passes.push_back({a, t});
    else
        return false;
    if (!over.empty()) {
        const auto* b = f->Find(over);
        if (!b)
            return false;
        passes.push_back({b, t2});
    }
    const auto wanted = [&](const isaac_anm2::LayerAnim& la) {
        if (!la.visible)
            return false;
        return only.empty() || std::find(only.begin(), only.end(), la.layer_id) != only.end();
    };
    struct Box {
        long long x0, y0, x1, y1;
    };
    const auto place = [](const isaac_anm2::Frame& fr) {
        const double sx = fr.scale_x / 100.0, sy = fr.scale_y / 100.0;
        const double ax = fr.pos_x - fr.pivot_x * sx, bx = fr.pos_x + (fr.w - fr.pivot_x) * sx;
        const double ay = fr.pos_y - fr.pivot_y * sy, by = fr.pos_y + (fr.h - fr.pivot_y) * sy;
        return Box{static_cast<long long>(std::floor(std::min(ax, bx) + 0.5)),
                   static_cast<long long>(std::floor(std::min(ay, by) + 0.5)),
                   static_cast<long long>(std::floor(std::max(ax, bx) + 0.5)),
                   static_cast<long long>(std::floor(std::max(ay, by) + 0.5))};
    };
    bool any = false;
    Box box{0, 0, 0, 0};
    for (const auto& p : passes)
        for (const auto& la : p.a->layers) {
            if (!wanted(la))
                continue;
            for (const auto& fr : la.frames) {
                if (!fr.visible || fr.w <= 0 || fr.h <= 0 || fr.scale_x == 0 || fr.scale_y == 0)
                    continue;
                const Box b = place(fr);
                if (b.x1 <= b.x0 || b.y1 <= b.y0)
                    continue;
                box = any ? Box{std::min(box.x0, b.x0), std::min(box.y0, b.y0), std::max(box.x1, b.x1),
                                std::max(box.y1, b.y1)}
                          : b;
                any = true;
            }
        }
    if (!any || box.x1 - box.x0 > 2048 || box.y1 - box.y0 > 2048)
        return false;
    Image canvas = isaac_pcx::Blank(static_cast<std::uint32_t>(box.x1 - box.x0),
                                    static_cast<std::uint32_t>(box.y1 - box.y0));
    for (const auto& p : passes) {
        const int n = std::max(1, p.a->frame_num);
        const int tt = ((p.t % n) + n) % n;
        for (const auto& la : p.a->layers) {
            if (!wanted(la))
                continue;
            const isaac_anm2::Frame* fr = isaac_anm2::FrameAt(la, tt);
            if (!fr || !fr->visible || fr->w <= 0 || fr->h <= 0 || fr->scale_x == 0 ||
                fr->scale_y == 0)
                continue;
            const isaac_anm2::Layer* L = f->FindLayer(la.layer_id);
            const isaac_anm2::Sheet* S = L ? f->FindSheet(L->sheet) : nullptr;
            const ImagePtr sheet = S ? Sheet(isaac_anm2::SheetPath(f->sheet_root.empty()?rel:f->sheet_root+"/animation.anm2", S->path)) : nullptr;
            if (!sheet)
                return false;
            const Image crop = isaac_pcx::Crop(*sheet, fr->x, fr->y, static_cast<std::uint32_t>(fr->w),
                                               static_cast<std::uint32_t>(fr->h));
            const Box b = place(*fr);
            const auto dw = static_cast<std::uint32_t>(b.x1 - b.x0);
            const auto dh = static_cast<std::uint32_t>(b.y1 - b.y0);
            if (!dw || !dh)
                continue;
            Image scaled = isaac_pcx::Blank(dw, dh);
            for (std::uint32_t y = 0; y < dh; ++y)
                for (std::uint32_t x = 0; x < dw; ++x) {
                    std::uint32_t u = std::min<std::uint32_t>(crop.w - 1, x * crop.w / dw);
                    std::uint32_t v = std::min<std::uint32_t>(crop.h - 1, y * crop.h / dh);
                    if (fr->scale_x < 0)
                        u = crop.w - 1 - u;
                    if (fr->scale_y < 0)
                        v = crop.h - 1 - v;
                    std::memcpy(scaled.At(x, y), crop.At(u, v), 4);
                }
            isaac_pcx::Over(canvas, scaled, static_cast<int>(b.x0 - box.x0),
                            static_cast<int>(b.y0 - box.y0));
        }
    }
    out = std::move(canvas);
    return true;
}

// "<anm2 path>#<anim>#<t>[#<overlay anim>#<t2>]" (round 6, loading page sprites).
bool LibraryImpl::Anim(std::string_view spec, Image& out) {
    const auto parts = Split(spec, '#');
    if (parts.size() != 3 && parts.size() != 5)
        return false;
    const std::string rel = isaac_anm2::NormalizePath(parts[0]);
    if (rel.size() < 6 || rel.compare(rel.size() - 5, 5, ".anm2") != 0 || parts[1].empty())
        return false;
    long long t = 0, t2 = 0;
    if (!ParseInt(parts[2], t, 0, 100000) || (parts.size() == 5 && !ParseInt(parts[4], t2, 0, 100000)))
        return false;
    return Compose(rel, parts[1], static_cast<int>(t), parts.size() == 5 ? parts[3] : std::string_view{},
                   static_cast<int>(t2), {}, out);
}

bool LibraryImpl::Key(std::string_view key, Image& out) {
    if (!key.starts_with("isaac:"))
        return false;
    key.remove_prefix(6);
    const std::size_t slash = key.find('/');
    const std::string_view kind = key.substr(0, slash);
    const std::string_view rest = slash == std::string_view::npos ? std::string_view{} : key.substr(slash + 1);
    const auto args = Split(rest, '/');
    long long a = 0, b = 0, c = 0;
    if (kind == "bg") {
        if (rest == "wiiu")
            return Background({}, out);
        if (rest.starts_with("wiiu/"))
            return Background(rest.substr(5), out);
        return false;
    }
    if (kind == "coll") {
        if (args.size() == 1 && args[0] == "q") {
            // Blind pedestal: Entity_Pickup::ReloadGraphics @+0x26AAF8 -> SetupCollectibleGraphics
            // @+0x26C3EC with blind set (tbz w4 @+0x26C414) replaces the item layer's sheet with
            // "gfx/Items/Collectibles/questionmark.png" (@+0x88DB1B; .pcx on the Switch), 32x32.
            const ImagePtr img = Sheet(PngToPcx("gfx/Items/Collectibles/questionmark.png"));
            if (!img)
                return false;
            out = *img;
            return true;
        }
        return args.size() == 1 && ParseInt(args[0], a, 1, 99999) && Collectible(static_cast<int>(a), out);
    }
    if (kind == "trink") {
        if (args.empty() || args.size() > 2 || !ParseInt(args[0], a, 1, 0x7FFF))
            return false;
        if (args.size() == 2 && args[1] != "g")
            return false;
        return Trinket(static_cast<int>(a), args.size() == 2, out);
    }
    if (kind == "card") {
        // ui_cardspills.anm2 "CardFronts" layer 0, frame = card id (data/REPORT.md §2.2)
        if (args.size() != 1 || !ParseInt(args[0], a, 1, 999))
            return false;
        if (Afterbirth()) {
            if (!LoadTables()) return false;
            if (a >= 32 && a <= 41) return Pocket(1,static_cast<int>(a),false,out);
            if (!card_hud.contains(static_cast<int>(a))) return false;
            const auto sprite = Frame("gfx/ui/ui_cardfronts.anm2", card_hud.at(static_cast<int>(a)), 0, 0);
            if (!sprite.ok) return false;
            out = sprite.img; return true;
        }
        const isaac_map::Sprite s = Frame("gfx/ui/ui_cardspills.anm2", "CardFronts", static_cast<int>(a), 0);
        if (!s.ok)
            return false;
        out = s.img;
        return true;
    }
    if (kind == "pocket") {
        if (args.size() < 2 || args.size() > 3 || !ParseInt(args[0], a, 0, 2) ||
            !ParseInt(args[1], b, 0, 0xFFFF) || (args.size() == 3 && args[2] != "s"))
            return false;
        return Pocket(static_cast<int>(a), static_cast<int>(b), args.size() == 3, out);
    }
    if (kind == "pill") {
        if (args.size() < 1 || args.size() > 2 || !ParseInt(args[0], a, 1, 0xFFFF) ||
            (args.size() == 2 && args[1] != "s"))
            return false;
        return Pocket(0, static_cast<int>(a), args.size() == 2, out);
    }
    if (kind == "hud")
        return args.size() == 1 && !args[0].empty() && args[0].size() < 64 && Hud(args[0], out);
    if (kind == "stat")
        return (args.size() == 1 || (args.size() == 2 && args[1] == "w")) &&
               ParseInt(args[0], a, 0, 5) && Stat(static_cast<int>(a), args.size() == 2, out);
    if (kind == "tally")
        return (args.size() == 2 || (args.size() == 3 && args[2] == "w")) &&
               ParseInt(args[0], a, 0, 5) && ParseInt(args[1], b, 0, 7) &&
               Tally(static_cast<int>(a), static_cast<int>(b), args.size() == 3, out);
    if (kind == "charge") {
        if (args.size() < 2 || args.size() > 3 || !ParseInt(args[0], a, 0, 99) ||
            !ParseInt(args[1], b, 1, 12) || (args.size() == 3 && !ParseInt(args[2], c, 0, 99)))
            return false;
        return Charge(static_cast<int>(a), static_cast<int>(b), static_cast<int>(c), out);
    }
    if (kind == "room") {
        std::string name;
        if (args.size() != 1)
            return false;
        if (ParseInt(args[0], a, 0, 255))
            name = std::string{isaac_map::RoomIcon(static_cast<int>(a))};
        else if (args[0].starts_with("Icon") && args[0].size() < 48)
            name = std::string{args[0]};
        if (name.empty())
            return false;
        const isaac_map::Sprite s = Frame("gfx/ui/minimap_icons.anm2", name, 0);
        if (!s.ok)
            return false;
        out = s.img;
        return true;
    }
    if (kind == "map") {
        isaac_map::Floor floor;
        if (!isaac_map::DecodeHex(rest, floor))
            return false;
        if (!floor.lost && !MapSprites())
            return false;
        out = isaac_map::RenderCanvas(floor, map_sprites);
        return true;
    }
    if (kind == "font")
        return args.size() == 1 && FontAtlas(std::string{args[0]}, out);
    if (kind == "text") {
        if (args.size() < 2 || args.size() > 3 || args[1].size() % 2 || args[1].empty() ||
            args[1].size() > 1024 || (args.size() == 3 && args[2] != "plain"))
            return false;
        std::string s;
        for (std::size_t i = 0; i < args[1].size(); i += 2) {
            const auto h = [](char ch) {
                return ch >= '0' && ch <= '9' ? ch - '0'
                       : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                       : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                : -1;
            };
            const int hi = h(args[1][i]), lo = h(args[1][i + 1]);
            if (hi < 0 || lo < 0)
                return false;
            s.push_back(static_cast<char>(hi * 16 + lo));
        }
        return TextImage(std::string{args[0]}, s, args.size() == 2, out);
    }
    if (kind == "ui") {
        // Round 8: "<spec>/rot90" = the frame turned 90 degrees clockwise by a lossless pixel
        // transpose (no resampling): out(x', y') = in(y', h-1-x'), size h x w. The left card's BAG
        // page uses it to point ui_crafting.anm2 "Result" frame 0 (arrow + bracket, arrow pointing
        // right; the anm2 has no down-pointing variant) down: arrow on top, bracket below.
        constexpr std::string_view rot = "/rot90";
        if (rest.size() > rot.size() && rest.ends_with(rot)) {
            Image src;
            if (rest.size() >= 512 || !Ui(rest.substr(0, rest.size() - rot.size()), src))
                return false;
            out = Rot90(src);
            return true;
        }
        return !rest.empty() && rest.size() < 512 && Ui(rest, out);
    }
    if (kind == "anim")
        return !rest.empty() && rest.size() < 512 && Anim(rest, out);
    if (kind == "logo") {
        // The title screen's logo: gfx/ui/main menu/titlemenu.anm2 "Idle" frame 0, layers
        // LogoShadow (3) and Logo (2) in the file's order (sheet logo.png = aoc: logo.pcx 480x320:
        // logo crop (0,0) 480x160, its blurred shadow (0,160) 480x160, 2 px higher); the canvas spans
        // every Idle keyframe (the logo bobs down 1.5 px), so 480x164.
        const bool abp=host && host->get_i64 && host->get_i64(host->userdata,"__source:aoc",-1)==0;
        if (!rest.empty() || !Compose("gfx/ui/main menu/titlemenu.anm2", "Idle", 0, {}, 0,
                                      abp?std::vector<int>{2}:std::vector<int>{2,3},out)) return false;
        if(abp && out.h<164) { auto padded=isaac_pcx::Blank(480,164);isaac_pcx::Over(padded,out,0,(164-out.h)/2);out=std::move(padded); }
        return true;
    }
    return false;
}

bool LoadImage(Library& lib, const EdenDsmodHostApi* host, const char* key,
               std::vector<std::uint8_t>& rgba, std::uint32_t& w, std::uint32_t& h) {
    rgba.clear();
    w = h = 0;
    if (!host || !key)
        return false;
    const std::string k{key};
    if (k.size() > 4096)
        return false;
    LibraryImpl& L = lib.impl();
    std::lock_guard lk{L.mu};
    L.host = host;
    ImagePtr img;
    const auto it = L.images.find(k);
    if (it != L.images.end()) {
        L.lru.splice(L.lru.begin(), L.lru, it->second.it);
        img = it->second.img;
    } else {
        Image out;
        const bool ok = L.Key(k, out);
        L.host = nullptr;
        if (!ok || !out.Valid() || out.w > isaac_pcx::MaxSide || out.h > isaac_pcx::MaxSide)
            return false;
        img = std::make_shared<const Image>(std::move(out));
        if (img->px.size() <= L.budget) {
            L.lru.push_front(k);
            L.images[k] = LibraryImpl::Entry{img, L.lru.begin()};
            L.image_bytes += img->px.size();
            while ((L.image_bytes > L.budget || L.images.size() > 512) && !L.lru.empty()) {
                const auto v = L.images.find(L.lru.back());
                L.image_bytes -= v->second.img->px.size();
                L.images.erase(v);
                L.lru.pop_back();
            }
        }
    }
    L.host = nullptr;
    rgba = img->px;
    w = img->w;
    h = img->h;
    return true;
}

bool DecodeFont(Library& lib, const EdenDsmodHostApi* host, const std::uint8_t* bytes,
                std::size_t size, FontOut& out) {
    out = FontOut{};
    if (!host || !bytes || !size)
        return false;
    std::string name;
    std::uint32_t lh = 0;
    if (!isaac_font::ParseRequest({bytes, size}, name, lh))
        return false;
    LibraryImpl& L = lib.impl();
    std::lock_guard lk{L.mu};
    L.host = host;
    const auto f = L.Font(name);
    L.host = nullptr;
    if (!f)
        return false;
    std::vector<isaac_font::HostGlyph> g;
    std::uint32_t first = 0;
    if (!isaac_font::BuildHostGlyphs(*f, first, g))
        return false;
    out.line_height = lh ? lh : f->line_height;
    out.first_codepoint = first;
    out.glyphs.reserve(g.size());
    for (const auto& x : g)
        out.glyphs.push_back({x.x, x.y, x.w, x.h, x.bearing_x, x.bearing_y, x.advance});
    return true;
}

} // namespace isaac_assets
