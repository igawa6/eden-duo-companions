// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Image key dispatcher + the families that are plain lookups (icons, layout textures, digits,
// font atlas, passport photo). The UI chrome recipes are in acnh_ui_pages.cpp, the backgrounds
// in acnh_ui_bg.cpp.

#include "acnh_ui_art.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "acnh_catalog.h"
#include "acnh_draw.h"
#include "acnh_font.h"
#include "acnh_lyt.h"
#include "acnh_photo.h"
#include "acnh_romfs.h"
#include "acnh_tex.h"

namespace acnh {
namespace {
// The host keeps its own copy of every delivered image, so this cache only saves rebuilding a
// key asked for again (a page re-opened after the host dropped it): keep it small.
constexpr size_t CacheBudget = 24u << 20;
// The host takes module images up to 4096 x 4096 and 16 MiB of RGBA.
constexpr int MaxSide = 4096;
constexpr size_t MaxBytes = 16u << 20;
constexpr double MaxParam = 1u << 20;

bool SafeName(std::string_view s) {
    return !s.empty() && s.size() <= 96 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '^' || c == '-' || c == '+';
    });
}

std::optional<int> Number(std::string_view s) {
    int v{};
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size())
        return std::nullopt;
    return v;
}
} // namespace

bool ArtKey::Parse(std::string_view key, ArtKey& out) {
    out = {};
    if (key.empty() || key.size() > 512)
        return false;
    const auto q = key.find('?');
    out.path = std::string{key.substr(0, q)};
    std::string_view rest = out.path;
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        out.parts.emplace_back(rest.substr(0, slash));
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    if (q != std::string_view::npos) {
        std::string_view params = key.substr(q + 1);
        while (!params.empty()) {
            const auto amp = params.find('&');
            const std::string_view kv = params.substr(0, amp);
            const auto eq = kv.find('=');
            if (eq != std::string_view::npos)
                out.params[std::string{kv.substr(0, eq)}] = std::string{kv.substr(eq + 1)};
            else if (!kv.empty())
                out.params[std::string{kv}] = "1";
            if (amp == std::string_view::npos)
                break;
            params.remove_prefix(amp + 1);
        }
    }
    return !out.parts.empty();
}

double ArtKey::Num(std::string_view k, double fallback) const {
    const auto it = params.find(k);
    if (it == params.end())
        return fallback;
    // strtod, not from_chars: the NDK's libc++ has no floating-point from_chars.
    // strtod also takes "nan", "inf" and "1e300": anything that is not a finite number of a sane
    // size is the fallback (every size / offset / count of the recipes is far below MaxParam).
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end && end != it->second.c_str() && *end == '\0' && std::isfinite(v) &&
                   std::abs(v) <= MaxParam
               ? v
               : fallback;
}

int ArtKey::Int(std::string_view k, int fallback) const {
    return static_cast<int>(std::clamp(Num(k, fallback), -MaxParam, MaxParam));
}

Rgba ArtKey::Col(std::string_view k, Rgba fallback) const {
    const auto it = params.find(k);
    if (it == params.end())
        return fallback;
    const Rgba c = draw::Color(it->second);
    return (c.a == 0 && it->second.size() != 8) ? fallback : c;
}

std::string ArtKey::Str(std::string_view k, std::string fallback) const {
    const auto it = params.find(k);
    return it == params.end() ? fallback : it->second;
}

Art::Art(Romfs& r, Catalog& c, Fonts& f, Sources s)
    : romfs{r}, catalog{c}, fonts{f}, sources{std::move(s)} {}
Art::~Art() = default;

Lang Art::Language() {
    return sources.language ? sources.language() : Lang::USen;
}

std::shared_ptr<Layout> Art::Lyt(std::string_view layout) {
    return SafeName(layout) ? LoadLayout(romfs, layout) : nullptr;
}

Image Art::Tex(std::string_view layout, std::string_view tex) {
    const auto l = Lyt(layout);
    if (!l)
        return {};
    const auto t = l->Texture(tex);
    return t ? *t : Image{};
}

Image Art::ModelIcon(std::string_view archive) {
    if (!SafeName(archive))
        return {};
    const auto arc = romfs.Model(archive);
    if (!arc)
        return {};
    for (const auto& [name, bytes] : arc->members) {
        Image img;
        if (BfresBntxDecode(bytes, {}, img))
            return img;
    }
    return {};
}

bool Art::Passport(Image& out) {
    std::vector<uint8_t> jpeg;
    uint64_t rev = 0;
    if (!sources.passport_jpeg || !sources.passport_jpeg(jpeg, rev) || jpeg.empty())
        return false;
    {
        std::scoped_lock lock{mutex};
        if (photo && rev == photo_rev) {
            out = *photo;
            return true;
        }
    }
    Image img;
    if (!DecodeJpeg(jpeg, img))
        return false;
    std::scoped_lock lock{mutex};
    photo = std::make_shared<const Image>(img);
    photo_rev = rev;
    out = std::move(img);
    return true;
}

std::vector<std::string> Art::Keys() {
    std::vector<std::string> keys{"icon/item/2286",
                                  "icon/itemart/2596",
                                  "icon/crit/1/24",
                                  "icon/critbook/1/24",
                                  "icon/npc/ant00",
                                  "icon/quest/3",
                                  "num/outline/33",
                                  "font/ui:30",
                                  "ui/card?w=600&h=250&c=fffbe8&r=40",
                                  "ui/capsule?w=240&h=52&c=f5f7e0",
                                  "ui/disc?d=88&c=8847e5",
                                  "ui/arrow?d=58&c=ed9a21&dir=r",
                                  "ui/glyph?cp=e0e0&px=60&s=48&c=fff1c3",
                                  "ui/gear?d=96",
                                  "ui/gear?d=96&pressed=1",
                                  "ui/close?d=70",
                                  "ui/check?d=54",
                                  "ui/glow?d=110&c=ffbb00e0",
                                  "ui/cursor?d=82",
                                  "ui/dock?w=1224&h=102",
                                  "ui/tabsel?k=2&w=192&h=90",
                                  "ui/panel/map?w=1216&h=948",
                                  "ui/panel/critters?w=1216&h=948",
                                  "ui/panel/diy?w=1216&h=948",
                                  "ui/icon/mile?s=74",
                                  "ui/icon/bell?s=80",
                                  "ui/icon/turnip?s=60",
                                  "ui/icon/daisy?s=92",
                                  "ui/icon/milepict?s=34",
                                  "ui/icon/island?s=36",
                                  "ui/icon/owl?s=46",
                                  "ui/icon/pocket?s=44",
                                  "ui/icon/storage?s=44",
                                  "ui/bag/blob",
                                  "ui/bag/slotsel",
                                  "ui/bag/empty",
                                  "ui/bag/fav",
                                  "ui/bag/balloon?w=320&h=76",
                                  "ui/bag/beak",
                                  "ui/bag/chip?w=438&h=80",
                                  "ui/crit/cloud?w=330&h=112",
                                  "ui/crit/cell?w=141&h=120",
                                  "ui/crit/focus?w=149&h=128",
                                  "ui/crit/caption?w=300&h=80",
                                  "ui/diy/cloud?w=1138&h=104",
                                  "ui/diy/card/6?w=120&made=1&fav=1&new=1&can=1",
                                  "ui/diy/canmake?d=96&both=1",
                                  "ui/diy/catsq?k=1&s=44",
                                  "ui/diy/star?d=72&on=1",
                                  "ui/diy/star?d=72",
                                  "ui/diy/dotline?w=400",
                                  "ui/today/clockcard?w=600&h=250",
                                  "ui/today/colon?s=18",
                                  "ui/today/weekpill?w=120&h=58",
                                  "ui/today/weather/6?s=110",
                                  "ui/today/weather/12?s=110",
                                  "ui/today/wx/0?s=110",
                                  "ui/today/wx/2?s=110",
                                  "ui/today/wx/4?s=110",
                                  "ui/today/wx/6?s=110",
                                  "ui/today/five?w=220",
                                  "ui/today/passport?w=604&h=250&m=9&d=25",
                                  "ui/today/sign?m=9&d=25&s=38",
                                  "ui/today/photoframe?d=166",
                                  "ui/today/fruit/2286?s=38",
                                  "ui/today/quest/3?w=220&bonus=1",
                                  "ui/phone/frame",
                                  "ui/phone/dot?on=1",
                                  "ui/phone/sel?s=176",
                                  "ui/mapui/panel?w=318&h=912",
                                  "ui/mapui/plate?w=278&h=70",
                                  "ui/mapui/resbtn?on=1",
                                  "ui/mapui/resbtn",
                                  "ui/mapui/resbtn?w=99&h=82",
                                  "ui/mapui/resbtn?on=1&w=99&h=82",
                                  "ui/mapui/bannerend?h=92",
                                  "ui/mapui/banner?w=400&h=76",
                                  "ui/ring/slot?d=120",
                                  "ui/ring/slot?d=120&on=1",
                                  "ui/ring/empty?d=120",
                                  "ui/ring/back?d=600",
                                  "ui/ring/star?d=96",
                                  "ui/ring/label?t=title&w=360&h=76",
                                  "ui/ring/label?t=clear&w=420&h=80&c=fff1c3&tc=3a2b18",
                                  "ui/block/win?w=760&h=420&msg=1",
                                  "ui/block/win?w=760&h=420",
                                  "ui/block/icon/loading?s=150",
                                  "ui/block/icon/menu?s=150",
                                  "ui/block/icon/phone?s=150",
                                  "ui/block/icon/online?s=150",
                                  "ui/block/icon/busy?s=150",
                                  "ui/block/icon/refused?s=150",
                                  "ui/block/icon/unusable?s=150",
                                  "ui/block/icon/ring?s=150",
                                  "ui/title/logo?w=900&h=600",
                                  "ui/diy/sizegrid?s=44",
                                  "ui/wait/loadicon",
                                  "ui/wait/loadicon?w=480",
                                  "ui/swatch/1?w=250&h=120",
                                  "ui/swatchframe?w=250&h=120&on=1",
                                  "ui/card?w=300&h=120&c=fffbe8&sh=82611040&dy=5",
                                  "icon/item/2286?s=101",
                                  "icon/npc/ant00?s=86",
                                  "icon/itemart/2596?s=190",
                                  "icon/app/3?s=132",
                                  "num/outline/33?h=25&w=71",
                                  "ui/today/photoframe?d=166&c=2e2b1c40",
                                  "ui/gear?d=96&sc=74614450"};
    for (int k = 0; k < 6; ++k)
        keys.push_back("ui/tabicon/" + std::to_string(k) + "?s=58");
    for (int k = 0; k < 3; ++k) {
        keys.push_back("ui/crit/cat/" + std::to_string(k) + "?on=" + (k == 1 ? "1" : "0"));
        keys.push_back("ui/crit/inv/" + std::to_string(k) + "?s=65");
    }
    for (int k = 0; k < 11; ++k)
        keys.push_back("ui/diy/cat/" + std::to_string(k) + "?on=" + (k == 0 ? "1" : "0"));
    for (int f = 1; f <= 20; ++f)
        keys.push_back("icon/app/" + std::to_string(f));
    static constexpr const char* pages[] = {"map",   "bag",   "critters", "diy",
                                            "today", "phone", "theme",    "waiting"};
    for (const char* p : pages)
        keys.push_back(std::string{"bg/0/"} + p);
    for (int t = 1; t <= 20; ++t)
        keys.push_back("bg/" + std::to_string(t) + "/map");
    return keys;
}

size_t Art::CachedBytes() {
    std::scoped_lock lock{mutex};
    return cached;
}

bool Art::Load(std::string_view key_text, Image& out) {
    // A few recipes bake short game texts (DIY "New!", Nook Miles+ "x2", "PASSPORT") in the game
    // language, which is not part of their keys: the language is part of the cache key. The photo
    // is never cached (it follows the passport revision).
    const std::string k =
        std::string{key_text} + '#' + std::to_string(static_cast<int>(Language()));
    const bool volatile_key = key_text.starts_with("photo/");
    if (!volatile_key) {
        std::scoped_lock lock{mutex};
        if (const auto it = cache.find(k); it != cache.end()) {
            lru.erase(std::find(lru.begin(), lru.end(), k));
            lru.push_back(k);
            out = *it->second;
            return true;
        }
    }
    ArtKey key;
    if (!ArtKey::Parse(key_text, key))
        return false;
    // The size parameters are bounded before anything is allocated (a key is just text).
    for (const char* dim : {"w", "h", "d", "s", "l", "pw", "px"})
        if (std::abs(key.Num(dim, 0)) > MaxSide)
            return false;
    if (key.Num("w", 0) * key.Num("h", 0) * 4 > static_cast<double>(MaxBytes))
        return false;
    // Square canvas parameters have no separate height. Reject them before the recipe's
    // intermediate masks/resamples, rather than allocating 64 MiB and checking the result.
    for (const char* dim : {"d", "s"}) {
        const double side = key.Num(dim, 0);
        if (side * side * 4 > static_cast<double>(MaxBytes))
            return false;
    }
    // A negative nine-slice radius expands the centre resample beyond the requested canvas.
    if (key.Num("r", 0) < 0)
        return false;
    Image img;
    if (!Build(key, img) || img.Empty() || img.w > MaxSide || img.h > MaxSide ||
        img.rgba.size() > MaxBytes)
        return false;
    // an image larger than a quarter of the budget would evict everything else: not kept
    if (!volatile_key && img.rgba.size() <= CacheBudget / 4) {
        std::scoped_lock lock{mutex};
        if (!cache.contains(k)) {
            auto shared = std::make_shared<const Image>(img);
            cached += shared->rgba.size();
            cache.emplace(k, std::move(shared));
            lru.push_back(k);
            while (cached > CacheBudget && lru.size() > 1) {
                const auto it = cache.find(lru.front());
                if (it != cache.end()) {
                    cached -= it->second->rgba.size();
                    cache.erase(it);
                }
                lru.erase(lru.begin());
            }
        }
    }
    out = std::move(img);
    return true;
}

bool Art::Build(const ArtKey& key, Image& out) {
    const auto& p = key.parts;
    const std::string_view family = p[0];
    if (family == "icon" &&
        (key.params.contains("s") || key.params.contains("w") || key.params.contains("h"))) {
        // Display size: the runtime draws images nearest-neighbour, so the module delivers the
        // exact widget size (linear-light area average, acnh_draw.h ResizeArea).
        ArtKey base = key;
        base.params.clear();
        Image raw;
        if (!Build(base, raw) || raw.Empty())
            return false;
        out = draw::SizeTo(raw, key.Num("s", 0), key.Num("w", 0), key.Num("h", 0));
        return !out.Empty();
    }
    if (family == "icon" && p.size() >= 3) {
        const std::string_view kind = p[1];
        if (kind == "item" || kind == "itemart") {
            const auto id = Number(p[2]);
            if (!id || *id < 0 || *id > 0xFFFF)
                return false;
            for (const auto& name :
                 catalog.ItemIcons(static_cast<uint16_t>(*id), kind == "itemart")) {
                out = ModelIcon(name);
                if (!out.Empty())
                    return true;
            }
            return false;
        }
        if ((kind == "crit" || kind == "critbook") && p.size() >= 4) {
            const auto k = Number(p[2]);
            const auto uid = Number(p[3]);
            if (!k || !uid)
                return false;
            const auto* c = catalog.FindCritter(*k, static_cast<uint16_t>(*uid));
            if (!c)
                return false;
            if (kind == "critbook") {
                out = ModelIcon(c->book_icon);
                return !out.Empty();
            }
            const auto* it = catalog.FindItem(c->item);
            if (!it || it->menu_icon.empty())
                return false;
            out = ModelIcon(it->menu_icon);
            return !out.Empty();
        }
        if (kind == "npc") {
            out = ModelIcon("Layout_NpcIcon_" + std::string{p[2]});
            return !out.Empty();
        }
        if (kind == "app") {
            const auto f = Number(p[2]);
            return f && *f >= 0 && *f <= 64 && BuildAppButton(*this, *f, out);
        }
        if (kind == "quest") {
            const auto g = Number(p[2]);
            if (!g || *g < 0 || *g > 999)
                return false;
            char name[64];
            std::snprintf(name, sizeof name, "Layout_DailyQuestIcon_DailyQuest%02d", *g);
            out = ModelIcon(name);
            return !out.Empty();
        }
        return false;
    }
    if (family == "num" && p.size() == 3) {
        const std::string_view digits = p[2];
        if (digits.empty() || digits.size() > 12)
            return false;
        bool ok = false;
        if (p[1] == "outline")
            ok = fonts.Number("SystemBOutline_00", digits, key.Num("h", 25),
                              key.Col("fill", draw::Color("312a1fc0")),
                              key.Col("line", draw::Color("fff1c3ff")), out);
        if (!ok || out.Empty())
            return false;
        // ?w=<px>: a fixed-width box with the digits right-aligned (one widget for any count);
        // digits wider than w are scaled down to width w (aspect kept, centred in the box height).
        if (const int w = std::min(key.Int("w", 0), MaxSide); w > 0 && w != out.w) {
            Image digits_img = std::move(out);
            const int box_h = digits_img.h;
            if (digits_img.w > w)
                digits_img = draw::SizeTo(digits_img, 0, w, 0);
            out = Image{w, box_h};
            const int x0 = w - digits_img.w, y0 = (box_h - digits_img.h) / 2;
            for (int y = 0; y < digits_img.h; ++y)
                std::memcpy(out.At(x0, y0 + y), digits_img.At(0, y), size_t(digits_img.w) * 4);
        }
        return true;
    }
    if (family == "font" && p.size() >= 2) {
        std::string spec = key.path.substr(5);
        const auto atlas = fonts.Atlas(spec);
        if (!atlas || atlas->atlas.Empty())
            return false;
        out = atlas->atlas;
        return true;
    }
    if (family == "photo" && p.size() == 2 && p[1] == "passport") {
        Image ph;
        if (!Passport(ph))
            return false;
        const int d = key.Int("d", 0);
        if (d > 0)
            ph = draw::Resize(ph, d, d);
        if (key.Flag("mask")) {
            const Image m = Tex("LProfileBtn", "ProfilePhotoMaskView^s");
            if (!m.Empty())
                ph = draw::Masked(ph, draw::Resize(draw::Mirror4(m), ph.w, ph.h));
        } else if (key.Flag("disc")) {
            const Image m = Tex("LPocketBtn", "Maru128_00^s");
            if (!m.Empty())
                ph = draw::Masked(ph, draw::Resize(m, ph.w, ph.h));
        }
        out = std::move(ph);
        return true;
    }
    if (family == "bg")
        return BuildBackground(*this, key, out);
    if (family == "ui")
        return BuildUi(*this, key, out);
    return false;
}

} // namespace acnh
