// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lp_assets.h"
#include "lp_map.h"
#include "lp_poketch.h"
#include "lp_raster.h"
#include "lp_vanilla.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>

namespace lp_assets {

using lp_lang::Reflow;

namespace {

// Short bundle names used in module:lp:ui/ keys -> bundle paths below the asset root.
const std::map<std::string_view, std::string_view>& Bundles() {
    static const std::map<std::string_view, std::string_view> b{
        {"sharedui", lp_unity::bundles::SharedUi},
        {"sharedui_lang.en", lp_unity::bundles::SharedUiLangEn},
        {"battle_lang.en", lp_unity::bundles::BattleLangEn},
        {"common", lp_unity::bundles::Common},
        {"common_lang.en", lp_unity::bundles::CommonLangEn},
        {"pokemon", lp_unity::bundles::Pokemon},
        {"menutop", lp_unity::bundles::MenuTop},
        {"texturemass", lp_unity::bundles::TextureMass},
        {"box", "UIs/textures/box"},
        {"field", "UIs/textures/field"},
        {"map", "UIs/textures/map"},
        {"resident", "UIs/ui/uiresidentwindow"},
        {"zukan", "UIs/textures/zukan"},
    };
    return b;
}

// A short bundle name -> its path, including the per-language UI bundles
// (common_lang.<sfx>, sharedui_lang.<sfx>, battle_lang.<sfx>).
std::optional<std::string> BundlePath(std::string_view name) {
    if (const auto it = Bundles().find(name); it != Bundles().end())
        return std::string{it->second};
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || name.size() - dot != 3)
        return std::nullopt;
    const auto sfx = name.substr(dot + 1);
    if (!std::all_of(sfx.begin(), sfx.end(), [](char c) { return c >= 'a' && c <= 'z'; }))
        return std::nullopt;
    const auto base = name.substr(0, dot);
    if (base == "common_lang")
        return "UIs/textures/" + std::string{name};
    if (base == "sharedui_lang" || base == "battle_lang")
        return "UIs/shareduiassets/" + std::string{name};
    return std::nullopt;
}

bool ValidSfx(std::string_view sfx) {
    return sfx.size() == 2 && std::all_of(sfx.begin(), sfx.end(), [](char c) { return c >= 'a' && c <= 'z'; });
}

// A message table by label name (the words' str concatenated).
lp_lang::LabelMap ReadLabels(lp_unity::Reader& unity, std::string_view bundle, std::string_view object) {
    lp_lang::LabelMap out;
    const auto v = unity.ReadObjectByName(bundle, object, 114);
    const auto* labels = v ? v->Get("labelDataArray") : nullptr;
    if (!labels)
        return out;
    for (const auto& label : labels->items) {
        const auto* name = label.Get("labelName");
        if (!name)
            continue;
        std::string text;
        if (const auto* words = label.Get("wordDataArray"))
            for (const auto& word : words->items) {
                if (const auto* str = word.Get("str"))
                    text += str->AsString();
                // SetupCurrentTownmapGuideMessage registers native name slots 1 (rival)
                // and 2 (supporter). Word tagIndex indexes tagDataArray, not the name slot.
                if (object.ends_with("_ss_xmenu_timeline")) {
                    const auto* pattern = word.Get("patternID");
                    const auto* index = word.Get("tagIndex");
                    const auto* tags = label.Get("tagDataArray");
                    const auto at = index ? index->AsInt(-1) : -1;
                    if (pattern && pattern->AsInt() == 5 && tags && at >= 0 &&
                        static_cast<std::size_t>(at) < tags->items.size()) {
                        const auto& tag = tags->items[static_cast<std::size_t>(at)];
                        const auto* group = tag.Get("groupID");
                        const auto* type = tag.Get("tagID");
                        const auto* slot = tag.Get("tagIndex");
                        if (group && group->AsInt() == 1 && type && type->AsInt() == 0 && slot) {
                            if (slot->AsInt() == 1) text += "{rival}";
                            else if (slot->AsInt() == 2) text += "{supporter}";
                        }
                    }
                }
                // Native timeline line breaks separate words. Rewrap objectives to our banner
                // width, preserving that boundary without importing the native screen's wrap.
                if (object.ends_with("_ss_xmenu_timeline"))
                    if (const auto* event = word.Get("eventID"); event && event->AsInt() == 1 &&
                        !text.empty() && text.back() != ' ' && text.back() != '\n')
                        text += ' ';
            }
        out[std::string{name->AsString()}] = std::move(text);
    }
    return out;
}

std::vector<int> SplitInts(std::string_view s) {
    std::vector<int> out;
    while (!s.empty()) {
        const auto slash = s.find('/');
        const auto part = s.substr(0, slash);
        int v = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), v);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size())
            return {};
        out.push_back(v);
        if (slash == std::string_view::npos)
            break;
        s.remove_prefix(slash + 1);
        if (s.empty()) return {}; // a trailing separator is not an integer
    }
    return out;
}

// ---- Classic Platinum DS theme ---------------------------------------------------------------
// The theme keeps every native sprite's outline (so slicing, sizes and positions are unchanged)
// and repaints it in the DS Platinum button language: dark outline, white rim, flat colour body
// with a lighter upper half. The Pokétch casing turns Platinum silver.
struct Rgb { int r, g, b; };
std::optional<Rgb> ClassicColour(std::string_view k) {
    static constexpr std::array<Rgb, 18> TypeRgb{{
        {168, 168, 120}, {192, 48, 40}, {168, 144, 240}, {160, 64, 160}, {224, 192, 104}, {184, 160, 56},
        {168, 184, 32}, {112, 88, 152}, {184, 184, 208}, {240, 128, 48}, {104, 144, 240}, {120, 200, 80},
        {248, 208, 48}, {248, 88, 136}, {152, 216, 216}, {112, 56, 248}, {112, 88, 72}, {238, 153, 172}}};
    const auto tail = [&](std::string_view prefix) -> int {
        if (!k.starts_with(prefix)) return -1;
        int v = -1;
        std::from_chars(k.data() + prefix.size(), k.data() + k.size(), v);
        return v;
    };
    if (const int t = tail("ui/sharedui/btl_bt_waza_01_body_"); t >= 0 && t < 18) return TypeRgb[t];
    switch (tail("ui/sharedui/btl_bt_command_01_body_")) {
    case 1: return Rgb{216, 64, 56};   // Fight
    case 2: return Rgb{72, 168, 72};   // Pokémon
    case 3: return Rgb{224, 168, 40};  // Bag
    case 4: case 5: return Rgb{64, 112, 200}; // Run / Back
    default: break;
    }
    switch (tail("ui/menutop/menu_bt_01_body_")) {
    case 1: return Rgb{216, 72, 64};   // Pokédex
    case 2: return Rgb{72, 168, 72};   // Party
    case 3: return Rgb{224, 168, 40};  // Bag
    case 6: return Rgb{64, 136, 216};  // Map
    case 9: return Rgb{48, 160, 168};  // Pokétch
    default: break;
    }
    if (k == "ui/pokemon/cmn_bt_pokemon_01_body_01") return Rgb{80, 168, 208};
    if (k == "ui/pokemon/cmn_bt_pokemon_01_body_02") return Rgb{240, 136, 56};
    if (k == "ui/sharedui/btl_bt_quickitem_01_body") return Rgb{64, 112, 200}; // a DS button: white label
    return std::nullopt;
}

void ClassicRestyle(std::string_view k, Img& im) {
    const int w = static_cast<int>(im.w), h = static_cast<int>(im.h);
    auto px = [&](int x, int y) { return im.rgba.data() + (static_cast<size_t>(y) * w + x) * 4; };
    if (k == "ui/resident/pkc_img_poketch_01_01") {
        // Platinum's silver casing: blue becomes light grey, reds and blacks stay.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                auto* p = px(x, y);
                if (p[2] > p[0] + 24 && p[2] > p[1]) {
                    const int l = std::min(255, (p[0] * 54 + p[1] * 183 + p[2] * 19) / 256 + 90);
                    p[0] = p[1] = static_cast<std::uint8_t>(l);
                    p[2] = static_cast<std::uint8_t>(std::min(255, l + 6));
                }
            }
        return;
    }
    const auto base = ClassicColour(k);
    if (!base) return;
    // Native command icons are pale ink on an opaque body, not alpha cutouts.
    // Keep that ink when repainting the frame; otherwise classic silently erases
    // every command icon. Sample the body before modifying it, outside the icon cap.
    const bool command = k.starts_with("ui/sharedui/btl_bt_command_01_body_") && w == 252 && h == 78;
    std::array<std::array<int, 3>, 78> command_body{};
    if (command) for (int y = 0; y < h; ++y)
        for (int channel = 0; channel < 3; ++channel)
            command_body[y][channel] = px(126, y)[channel];
    // Distance (px) to the sprite's transparent edge, two-pass chamfer on the alpha mask.
    std::vector<int> d(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            d[y * w + x] = px(x, y)[3] < 128 ? 0 : 1 << 20;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int& v = d[y * w + x];
            if (x == 0 || y == 0) v = std::min(v, 1);
            if (x > 0) v = std::min(v, d[y * w + x - 1] + 1);
            if (y > 0) v = std::min(v, d[(y - 1) * w + x] + 1);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            int& v = d[y * w + x];
            if (x == w - 1 || y == h - 1) v = std::min(v, 1);
            if (x < w - 1) v = std::min(v, d[y * w + x + 1] + 1);
            if (y < h - 1) v = std::min(v, d[(y + 1) * w + x] + 1);
        }
    const int outline = std::max(2, std::min(w, h) / 28), rim = outline + std::max(2, std::min(w, h) / 22);
    // the inner top/bottom edge of the opaque area per column decides the two-tone split
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto* p = px(x, y);
            if (p[3] < 8) continue;
            const int dist = d[y * w + x];
            Rgb c;
            if (dist <= outline) c = {base->r * 2 / 5, base->g * 2 / 5, base->b * 2 / 5};
            else if (dist <= rim) c = {248, 248, 248};
            else if (y < h * 45 / 100) c = {(base->r * 3 + 255 * 2) / 5, (base->g * 3 + 255 * 2) / 5, (base->b * 3 + 255 * 2) / 5};
            else c = *base;
            if (k.find("btl_bt_waza_01_body_") != std::string_view::npos && dist > rim) {
                // DS move button: pale type-tinted body, the type colour as a band on the left cap
                c = x < h ? *base : Rgb{(base->r + 255 * 3) / 4, (base->g + 255 * 3) / 4, (base->b + 255 * 3) / 4};
            }
            if (command && x >= 152 && dist > rim) {
                // White line art raises all three channels above the same-row
                // body colour. The minimum removes ordinary hue/gradient changes.
                int ink = 255;
                for (int channel = 0; channel < 3; ++channel) {
                    const int body = command_body[y][channel];
                    ink = std::min(ink, std::max(0, (static_cast<int>(p[channel]) - body) * 255 / std::max(1, 255 - body)));
                }
                c = {c.r + (255 - c.r) * ink / 255,
                     c.g + (255 - c.g) * ink / 255,
                     c.b + (255 - c.b) * ink / 255};
            }
            p[0] = static_cast<std::uint8_t>(c.r);
            p[1] = static_cast<std::uint8_t>(c.g);
            p[2] = static_cast<std::uint8_t>(c.b);
        }
}

Img ToImg(lp_unity::Image&& i) {
    return Img{i.width, i.height, std::move(i.rgba)};
}

// Fit a native title logo into the loading page's 760:432 slot without stretching.
Img StartLogo(Img image) {
    std::uint32_t x0 = image.w, y0 = image.h, x1 = 0, y1 = 0;
    if (image.w == 1280 && image.h == 720) {
        // LP's title texture also contains unrelated pixels at the lower right.
        x0 = 173; y0 = 33; x1 = 1107; y1 = 564;
    } else {
        for (std::uint32_t y = 0; y < image.h; ++y)
            for (std::uint32_t x = 0; x < image.w; ++x)
                if (image.rgba[(static_cast<size_t>(y) * image.w + x) * 4 + 3]) {
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1);
                }
    }
    if (x1 <= x0 || y1 <= y0) return image;
    const auto width = x1 - x0, height = y1 - y0;
    const auto unit = std::max((width + 94) / 95, (height + 53) / 54);
    Img out{unit * 95, unit * 54, std::vector<std::uint8_t>(static_cast<size_t>(unit * 95) * unit * 54 * 4, 0)};
    const auto left = (out.w - width) / 2, top = (out.h - height) / 2;
    for (std::uint32_t y = 0; y < height; ++y)
        std::copy_n(image.rgba.data() + (static_cast<size_t>(y + y0) * image.w + x0) * 4, width * 4,
                    out.rgba.data() + (static_cast<size_t>(y + top) * out.w + left) * 4);
    return out;
}

Img SquareIcon(Img&& image) {
    // Transparent padding preserves the native sprite ratio in the UI's square icon slots.
    if (!lp_raster::Fits(image.w, image.h, image.rgba.size()))
        return {}; // empty: the image callback refuses it
    const auto side = std::max(image.w, image.h);
    Img out{side, side, std::vector<std::uint8_t>(static_cast<size_t>(side) * side * 4, 0)};
    const auto left = (side - image.w) / 2;
    const auto top = (side - image.h) / 2;
    for (std::uint32_t row = 0; row < image.h; ++row)
        std::copy_n(image.rgba.data() + static_cast<size_t>(row) * image.w * 4,
                    static_cast<size_t>(image.w) * 4,
                    out.rgba.data() + (static_cast<size_t>(row + top) * side + left) * 4);
    return out;
}

std::optional<std::pair<std::string, std::uint32_t>> ParseFontSpec(std::string_view text) {
    // The package font file: comment lines starting with '#', then "<name>:<px>".
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        const auto colon = line.rfind(':');
        if (colon == std::string_view::npos)
            return std::nullopt;
        std::uint32_t px = 0;
        const auto num = line.substr(colon + 1);
        const auto parsed = std::from_chars(num.data(), num.data() + num.size(), px);
        if (parsed.ec != std::errc{} || parsed.ptr != num.data() + num.size() || px < 8 || px > 256)
            return std::nullopt;
        return std::make_pair(std::string{line.substr(0, colon)}, px);
    }
    return std::nullopt;
}

// An integer field of a table row: `missing` when the field is absent, else AsInt(as_default).
int IntOr(const lp_unity::Value& v, std::string_view k, int missing, std::int64_t as_default = 0) {
    const auto* x = v.Get(k);
    return x ? static_cast<int>(x->AsInt(as_default)) : missing;
}

std::string Two(int v) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d", v);
    return b;
}

} // namespace

lp_unity::RangeReader MakeRomfsReader(const EdenDsmodHostApi& api) {
    // Paths come in as "/Data/..." (romfs) or with an explicit "base:" prefix. read_romfs and
    // userdata stay valid for the module's lifetime, so a copy is safe on any thread.
    const auto fn = api.read_romfs;
    void* ud = api.userdata;
    auto full = [](std::string_view p) {
        if (p.starts_with("base:") || p.starts_with("romfs:"))
            return std::string{p};
        return "romfs:" + std::string{p};
    };
    lp_unity::RangeReader r;
    r.size = [fn, ud, full](std::string_view p) -> std::optional<std::uint64_t> {
        if (!fn)
            return std::nullopt;
        const auto path = full(p);
        const size_t n = fn(ud, path.c_str(), 0, nullptr, 0);
        if (n == 0)
            return std::nullopt;
        return n;
    };
    r.read = [fn, ud, full](std::string_view p, std::uint64_t off, std::uint8_t* out, std::size_t n) {
        if (!fn)
            return false;
        const auto path = full(p);
        // read_romfs is capped at 64 MiB per call
        std::size_t done = 0;
        while (done < n) {
            const std::size_t chunk = std::min<std::size_t>(n - done, 32u << 20);
            if (fn(ud, path.c_str(), off + done, out + done, chunk) != chunk)
                return false;
            done += chunk;
        }
        return true;
    };
    return r;
}

int Effectiveness(int attack, int def1, int def2) {
    // Rows: attacking type; columns: defending type. 0 immune, 1 half, 2 neutral, 4 double (halves).
    // NOR FIG FLY POI GRO ROC BUG GHO STE FIR WAT GRA ELE PSY ICE DRA DAR FAI
    static constexpr unsigned char Chart[18][18] = {
        {2, 2, 2, 2, 2, 1, 2, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2}, // Normal
        {4, 2, 1, 1, 2, 4, 1, 0, 4, 2, 2, 2, 2, 1, 4, 2, 4, 1}, // Fighting
        {2, 4, 2, 2, 2, 1, 4, 2, 1, 2, 2, 4, 1, 2, 2, 2, 2, 2}, // Flying
        {2, 2, 2, 1, 1, 1, 2, 1, 0, 2, 2, 4, 2, 2, 2, 2, 2, 4}, // Poison
        {2, 2, 0, 4, 2, 4, 1, 2, 4, 4, 2, 1, 4, 2, 2, 2, 2, 2}, // Ground
        {2, 1, 4, 2, 1, 2, 4, 2, 1, 4, 2, 2, 2, 2, 4, 2, 2, 2}, // Rock
        {2, 1, 1, 1, 2, 2, 2, 1, 1, 1, 2, 4, 2, 4, 2, 2, 4, 1}, // Bug
        {0, 2, 2, 2, 2, 2, 2, 4, 2, 2, 2, 2, 2, 4, 2, 2, 1, 2}, // Ghost
        {2, 2, 2, 2, 2, 4, 2, 2, 1, 1, 1, 2, 1, 2, 4, 2, 2, 4}, // Steel
        {2, 2, 2, 2, 2, 1, 4, 2, 4, 1, 1, 4, 2, 2, 4, 1, 2, 2}, // Fire
        {2, 2, 2, 2, 4, 4, 2, 2, 2, 4, 1, 1, 2, 2, 2, 1, 2, 2}, // Water
        {2, 2, 1, 1, 4, 4, 1, 2, 1, 1, 4, 1, 2, 2, 2, 1, 2, 2}, // Grass
        {2, 2, 4, 2, 0, 2, 2, 2, 2, 2, 4, 1, 1, 2, 2, 1, 2, 2}, // Electric
        {2, 4, 2, 4, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 0, 2}, // Psychic
        {2, 2, 4, 2, 4, 2, 2, 2, 1, 1, 1, 4, 2, 2, 1, 4, 2, 2}, // Ice
        {2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 4, 2, 0}, // Dragon
        {2, 1, 2, 2, 2, 2, 2, 4, 2, 2, 2, 2, 2, 4, 2, 2, 1, 1}, // Dark
        {2, 4, 2, 1, 2, 2, 2, 2, 1, 1, 2, 2, 2, 2, 2, 4, 4, 2}, // Fairy
    };
    auto ok = [](int t) { return t >= 0 && t < 18; };
    if (!ok(attack) || !ok(def1))
        return 4;
    int m = Chart[attack][def1];
    m *= ok(def2) && def2 != def1 ? Chart[attack][def2] : 2;
    return m; // product of halves: 4 = neutral
}

std::string UiKey(std::string_view bundle, std::string_view sprite) {
    return "module:lp:ui/" + std::string{bundle} + "/" + std::string{sprite};
}
std::string IconKey(int species, int form, int gender, bool shiny) {
    if (species <= 0)
        return {};
    return "module:lp:icon/" + std::to_string(species) + "/" + std::to_string(form) + "/" +
           std::to_string(gender) + "/" + (shiny ? "1" : "0");
}
std::string ItemKey(int item) {
    return item > 0 ? "module:lp:item/" + std::to_string(item) : std::string{};
}
std::string TypeTagKey(int type, std::string_view sfx) {
    const std::string bundle = "common_lang." + std::string{ValidSfx(sfx) ? sfx : "en"};
    return type >= 0 && type < 18 ? UiKey(bundle, "cmn_ico_type_01_" + Two(type)) : std::string{};
}
std::string WazaBodyKey(int type) {
    return type >= 0 && type < 18 ? UiKey("sharedui", "btl_bt_waza_01_body_" + Two(type)) : std::string{};
}

Assets::Assets(lp_unity::RangeReader r, bool pearl) : pearl_title{pearl}, wanted_pearl{pearl}, read{r}, unity{r} {
    for (const auto cp : lp_unity::DefaultCodepoints())
        font_known.Add(cp);
}

Assets::~Assets() {
    Stop();
}

void Assets::Stop() {
    stopping = true;
    if (worker.joinable())
        worker.join();
    if (font_worker.joinable())
        font_worker.join();
}

void Assets::SetAstcDecoder(void* userdata, EdenDsmodAstcDecoder decode) {
    if (decode)
        unity.SetAstcDecoder(lp_unity::MakeHostAstcDecoder(userdata, decode));
}

bool Assets::CatalogReady() const {
    std::scoped_lock lock{mutex};
    return catalog != nullptr;
}

void Assets::SetLanguage(int msg_lang_id, bool kanji) {
    if (!lp_lang::Valid(msg_lang_id))
        return;
    const int want = msg_lang_id * 2 + (msg_lang_id == lp_lang::Jpn && kanji ? 1 : 0);
    if (wanted_lang.exchange(want) != want) {
        attempts = 0; // a new language gets its own retries
        retry_tick = 0;
    }
}

bool Assets::IsLumi() const {
    return read.size && read.size(lp_vanilla::LumiMarker).value_or(0) > 0;
}

void Assets::SetDataVersion(bool pearl) {
    if (wanted_pearl.exchange(pearl) != pearl) {
        attempts = 0;
        retry_tick = 0;
    }
}

void Assets::StartCatalog(std::uint64_t tick) {
    if (running || stopping)
        return;
    const int want = wanted_lang;
    const bool pearl = wanted_pearl;
    std::shared_ptr<const Catalog> prev;
    {
        std::scoped_lock lock{mutex};
        prev = catalog;
    }
    if (prev && prev->pearl == pearl && prev->lang.id * 2 + (prev->lang.kanji ? 1 : 0) == want)
        return;
    if (attempts >= 5 || (attempts > 0 && tick < retry_tick))
        return;
    running = true; // before ++attempts, so CatalogFailed never sees the last attempt as over
    ++attempts;
    retry_tick = tick + 5 * 60;
    if (worker.joinable())
        worker.join();
    worker = std::thread([this, want, pearl, prev] {
        // A language change keeps the language-independent tables and rereads only the text.
        const bool reuse = prev && prev->pearl == pearl;
        auto c = reuse ? std::make_shared<Catalog>(*prev) : std::make_shared<Catalog>();
        c->pearl = pearl;
        if (!reuse)
            BuildData(*c);
        if (!stopping)
            BuildText(*c, lp_lang::Lookup(want / 2, (want & 1) != 0));
        const bool ok = !stopping && !c->moves.empty() && !c->species.empty() && !c->waza.empty();
        {
            std::scoped_lock lock{mutex};
            if (ok) {
                c->generation = ++generations;
                catalog = std::move(c);
            }
        }
        running = false;
    });
}

void Assets::BuildText(Catalog& c, const lp_lang::Info& L) {
    const std::string msg = "Message/" + std::string{L.bundle};
    const std::string prefix = std::string{L.prefix} + "_";
    constexpr std::string_view Common = "Message/common_msbt";
    constexpr std::string_view English = lp_unity::bundles::MessageEnglish;
    const bool is_english = L.id == lp_lang::English;
    const auto bundle_of = [&](bool common) { return common ? Common : std::string_view{msg}; };
    // Tables use labelIndex except numeric item lookups, which use arrayIndex. Labels left empty (LP-added items in every mod,
    // es item descriptions) are filled from English.
    auto table = [&](bool common, std::string_view name, bool events, bool array_index = false) {
        if (stopping)
            return std::vector<std::string>{};
        auto v = lp_unity::ReadMessageTable(unity, bundle_of(common), prefix + std::string{name}, events, array_index);
        if (is_english)
            return v;
        const auto en = lp_unity::ReadMessageTable(unity, common ? Common : English, "english_" + std::string{name}, events, array_index);
        if (v.size() < en.size())
            v.resize(en.size());
        for (std::size_t i = 0; i < en.size(); ++i)
            if (v[i].empty())
                v[i] = en[i];
        return v;
    };
    // Species names and dex entries sit in the shared message bundle, the rest in the language's.
    c.species = table(true, "ss_monsname", false);
    c.moves = table(false, "ss_wazaname", false);
    c.items = table(false, "ss_itemname", false, true);
    c.item_plurals = table(false, "ss_itemname_plural", false, true);
    // eventID 1 terminates a native description line; Reflow keeps only sentence ends.
    c.dex_descriptions = Reflow(table(true, lp_vanilla::DexTable(c.pearl), true));
    c.item_descriptions = Reflow(table(false, "ss_iteminfo", true, true));
    c.move_descriptions = Reflow(table(false, "ss_wazainfo", true));
    c.ability_names = table(false, "ss_tokusei", false);
    c.ability_descriptions = Reflow(table(false, "ss_tokuseiinfo", true));
    c.type_names = table(false, "ss_typename", false);

    // Labels by name (labelIndex differs from the suffix numbers).
    c.labels = {};
    for (const auto& def : lp_lang::LabelTableList) {
        if (stopping)
            return;
        auto own = ReadLabels(unity, bundle_of(def.common), prefix + std::string{def.table});
        c.labels.english[std::string{def.table}] =
            is_english ? own : ReadLabels(unity, def.common ? Common : English, "english_" + std::string{def.table});
        c.labels.lang[std::string{def.table}] = std::move(own);
    }
    auto areas = ReadLabels(unity, msg, prefix + "dp_fld_areaname_display");
    if (!is_english)
        for (auto& [label, text] : ReadLabels(unity, English, "english_dp_fld_areaname_display"))
            if (auto& own = areas[label]; own.empty())
                own = std::move(text);
    c.area_names.clear();
    for (const auto& [zone, label] : c.zone_labels)
        if (const auto found = areas.find(label); found != areas.end())
            c.area_names[zone] = found->second;

    // The Pokédex's name order in this language (the game's search index; LP ships none, so it is
    // Brilliant Diamond's, which the translation mods leave alone).
    c.name_rank.clear();
    if (!stopping)
        if (auto index = unity.ReadObjectByName("UIs/searchdatas/indexdata." + std::string{L.sfx}, "SearchIndexData", 114))
            if (const auto* names = index->Get("MonsterName")) {
                int rank = 0;
                for (const auto& n : names->items) {
                    const auto* id = n.Get("MessageID");
                    const auto label = id ? id->AsString() : std::string_view{};
                    if (!label.starts_with("MONSNAME_"))
                        continue;
                    const int species = std::atoi(std::string{label.substr(9)}.c_str());
                    if (species <= 0 || species > 2000)
                        continue;
                    if (c.name_rank.size() <= static_cast<std::size_t>(species))
                        c.name_rank.resize(static_cast<std::size_t>(species) + 1, -1);
                    if (c.name_rank[static_cast<std::size_t>(species)] < 0)
                        c.name_rank[static_cast<std::size_t>(species)] = rank++;
                }
            }

    // Which text fills the slot (lp_lang::Detect), and whether the slot has its own title logo:
    // LP ships a 61.9 KB "Non English save file detected" placeholder for every other language.
    std::string select0;
    if (const auto t = c.labels.lang.find("ss_language_select"); t != c.labels.lang.end())
        if (const auto it = t->second.find("SS_language_select_000"); it != t->second.end())
            select0 = it->second;
    c.lang = L;
    c.detected = lp_lang::Detect(L.id, select0, c.moves.size() > 33 ? c.moves[33] : std::string{},
                                 c.type_names.size() > 1 ? c.type_names[1] : std::string{});
    c.logo_sfx = "en";
    if (L.sfx != "en" && read.size) {
        const bool lumi = IsLumi();
        const std::string family{lp_vanilla::LogoFamily(pearl_title, lumi)};
        const auto logo = read.size(std::string{lp_unity::DefaultAssetRoot} + "Dpr/movie/" + family +
                                    "/logo/logo_" + family + "_" + std::string{L.sfx});
        if (logo && *logo > 0 && (!lumi || lp_vanilla::OwnLogo(*logo)))
            c.logo_sfx = std::string{L.sfx};
    }

    // The font's glyph set: every code point of the text above.
    lp_lang::CodepointSet cps;
    for (const auto* list : {&c.species, &c.moves, &c.items, &c.item_plurals, &c.dex_descriptions,
                             &c.item_descriptions, &c.move_descriptions, &c.ability_names,
                             &c.ability_descriptions, &c.type_names})
        for (const auto& t : *list)
            cps.AddText(t);
    for (const auto& [zone, name] : c.area_names)
        cps.AddText(name);
    for (const auto& [table, labels] : c.labels.lang)
        for (const auto& [label, text] : labels)
            cps.AddText(text);
    c.codepoints = cps.Sorted();
}

void Assets::BuildData(Catalog& c) {
    constexpr std::string_view Pml = lp_unity::bundles::PersonalMasterdatas;
    // Use the installed game's regional numbering, including a mod's expanded Sinnoh dex.
    if (!stopping)
        if (auto info = unity.ReadObjectByName("Dpr/masterdatas", "PokemonInfo", 114))
            if (const auto* rows = info->Get("Catalog"))
                for (const auto& row : rows->items) {
                    const int species = IntOr(row, "MonsNo", 0), regional = IntOr(row, "SinnohNo", 0);
                    if (species > 0 && regional > 0)
                        c.sinnoh_numbers.emplace(species, regional);
                }
    if (!stopping)
        if (auto waza = unity.ReadObjectByName(Pml, "WazaTable", 114))
            if (const auto* arr = waza->Get("Waza"))
                for (const auto& w : arr->items) {
                    WazaInfo i;
                    const int no = IntOr(w, "wazaNo", -1);
                    if (no < 0)
                        continue;
                    i.type = IntOr(w, "type", -1, -1);
                    // damageType: 0 status, 1 physical, 2 special ("category" is the AI category)
                    i.category = IntOr(w, "damageType", 0);
                    i.power = IntOr(w, "power", 0);
                    i.accuracy = IntOr(w, "hitPer", 0);
                    i.base_pp = IntOr(w, "basePP", 0);
                    c.waza[no] = i;
                }
    if (!stopping)
        if (auto it = unity.ReadObjectByName(Pml, "ItemTable", 114))
            if (const auto* arr = it->Get("Item"))
                for (const auto& e : arr->items) {
                    const int no = IntOr(e, "no", -1, -1);
                    if (no <= 0)
                        continue;
                    ItemData d;
                    d.type = IntOr(e, "type", 0);
                    d.pocket = IntOr(e, "fld_pocket", -1, -1);
                    d.sort = IntOr(e, "sort", 0);
                    d.field_func = IntOr(e, "field_func", 0);
                    d.hp_rcv = IntOr(e, "wk_prm_hp_rcv", 0);
                    d.icon_id = IntOr(e, "iconid", -1, -1);
                    d.pp_rcv = IntOr(e, "wk_prm_pp_rcv", 0);
                    d.flags = static_cast<std::uint32_t>(IntOr(e, "flags0", 0));
                    for (int k = 0; k < 3; ++k)
                        d.friend_change[k] = IntOr(e, "wk_friend" + std::to_string(k + 1), 0);
                    c.item_data[no] = d;
                }
    if (!stopping)
        if (auto pt = unity.ReadObjectByName(Pml, "PersonalTable", 114))
            if (const auto* arr = pt->Get("Personal")) {
                c.personal.reserve(arr->items.size());
                for (const auto& p : arr->items) {
                    Catalog::Personal e;
                    e.type1 = IntOr(p, "type1", -1, -1);
                    e.type2 = IntOr(p, "type2", -1, -1);
                    e.form_index = IntOr(p, "form_index", 0);
                    e.form_max = IntOr(p, "form_max", 1, 1);
                    static constexpr const char* Base[6] = {"basic_hp", "basic_atk", "basic_def",
                                                            "basic_spatk", "basic_spdef", "basic_agi"};
                    for (int k = 0; k < 6; ++k)
                        e.base[k] = IntOr(p, Base[k], 0);
                    for (int k = 0; k < 3; ++k)
                        e.abilities[k] = IntOr(p, "tokusei" + std::to_string(k + 1), 0);
                    auto& d = e.dex;
                    d.valid = IntOr(p, "valid_flag", 1) != 0;
                    d.height = IntOr(p, "height", 0);
                    d.weight = IntOr(p, "weight", 0);
                    d.sex = IntOr(p, "sex", 255);
                    d.egg_cycles = IntOr(p, "egg_birth", 0);
                    d.friendship = IntOr(p, "initial_friendship", 0);
                    d.egg1 = IntOr(p, "egg_group1", 0);
                    d.egg2 = IntOr(p, "egg_group2", 0);
                    d.growth = IntOr(p, "grow", 0);
                    d.catch_rate = IntOr(p, "get_rate", 0);
                    d.base_exp = IntOr(p, "give_exp", 0);
                    d.ev_packed = IntOr(p, "exp_value", 0);
                    c.personal.push_back(e);
                    c.dex_table.push_back({IntOr(p, "monsno", 0), e.form_index, e.form_max, d.valid, {}});
                }
            }
    // EvolveTable: one row per PersonalTable id, "ar" = method, param, species, form, level per entry
    if (!stopping && !c.dex_table.empty())
        if (auto evo = unity.ReadObjectByName(Pml, "EvolveTable", 114))
            if (const auto* arr = evo->Get("Evolve")) {
                for (const auto& row : arr->items) {
                    const int id = IntOr(row, "id", -1);
                    const auto* ar = row.Get("ar");
                    if (id <= 0 || id >= static_cast<int>(c.dex_table.size()) || !ar)
                        continue;
                    std::vector<int> v;
                    for (const auto& x : ar->items)
                        v.push_back(static_cast<int>(x.AsInt()));
                    auto& out = c.dex_table[static_cast<std::size_t>(id)].evolve;
                    for (std::size_t k = 0; k + 4 < v.size(); k += 5)
                        if (v[k] > 0 && v[k + 2] > 0)
                            out.push_back({v[k], v[k + 1], v[k + 2], v[k + 3], v[k + 4]});
                }
                lp_dex::AddImplicit(c.dex_table);
            }
    constexpr std::string_view Settings = "Dpr/scriptableobjects/gamesettings";
    std::map<int,bool> map_towns;
    if (!stopping)
        if (auto map = unity.ReadObjectByName(Settings, "MapInfo", 114))
            if (const auto* zones = map->Get("ZoneData"))
                for (const auto& row : zones->items) {
                    const auto* id = row.Get("ZoneID");
                    const auto* label = row.Get("MSLabel");
                    if (!id || !label) continue;
                    if (auto type=row.Get("MapType")) {
                        c.map_types[static_cast<int>(id->AsInt())] = static_cast<int>(type->AsInt());
                        map_towns[static_cast<int>(id->AsInt())]=type->AsInt()==0;
                    }
                    c.zone_labels[static_cast<int>(id->AsInt())] = std::string{label->AsString()};
                }
    if (!stopping)
        if (auto map = unity.ReadObjectByName("UIs/masterdatas/uimasterdatas", "TownMapTable", 114))
            if (const auto* rows = map->Get("Data"))
                for (const auto& row : rows->items) {
                    if (!row.Get("zoneID") || !row.Get("Width") || !row.Get("NowPosXZ")) continue;
                    const int zone = static_cast<int>(row.Get("zoneID")->AsInt());
                    const Catalog::MapGate gate{IntOr(row, "ViewFlag", 500), IntOr(row, "ColorOnFlag", 1000),
                                                map_towns[zone]};
                    if (const auto* symbol=row.Get("SymbolName");symbol && !symbol->AsString().empty())
                        c.map_gates[std::string{symbol->AsString()}]=gate;
                    if (const auto* fly = row.Get("FlyingAvailablePlace"); fly && fly->AsInt() != 0)
                        if (const auto* pos = row.Get("NowPosXZ"); pos->Get("x") && pos->Get("y")) {
                            Catalog::FlySpot f;
                            f.zone = zone;
                            f.locator = IntOr(row, "MapInfoLocatorIndex", 0);
                            f.x = static_cast<int>(pos->Get("x")->AsInt());
                            f.y = static_cast<int>(pos->Get("y")->AsInt());
                            f.gate = gate;
                            c.fly_spots.push_back(f);
                        }
                    Catalog::MapRow r;
                    r.width = static_cast<int>(row.Get("Width")->AsInt());
                    for (int i = 0; i < 3; ++i)
                        r.indices[i] = IntOr(row, "Index" + std::to_string(i + 1), -1, -1);
                    if (const auto* mp = row.Get("MarkingMapPosXZ"); mp && mp->Get("x") && mp->Get("y") &&
                        mp->Get("x")->AsInt() >= 0 && mp->Get("y")->AsInt() >= 0)
                        c.marking_pos[zone] = {
                            static_cast<int>(mp->Get("x")->AsInt()), static_cast<int>(mp->Get("y")->AsInt())};
                    const auto* pos = row.Get("NowPosXZ");
                    if (!pos->Get("x") || !pos->Get("y")) continue;
                    r.x = static_cast<int>(pos->Get("x")->AsInt());
                    r.y = static_cast<int>(pos->Get("y")->AsInt());
                    if (r.width > 0 && r.x >= 0 && r.x < 34 && r.y >= 0 && r.y < 27)
                        c.town_map[zone].push_back(r);
                }
    // Native UIManager.GetCurrentTownmapGuideData reads EvWork 249 and exact-matches Id.
    if (!stopping)
        if (auto guide = unity.ReadObjectByName("UIs/masterdatas/uimasterdatas", "TownMapGuideTable", 114))
            if (const auto* rows = guide->Get("Guide"))
                for (const auto& row : rows->items) {
                    const auto* file = row.Get("MSFile");
                    const auto* label = row.Get("MSLabel");
                    if (!file || !label) continue;
                    c.map_guides.push_back({IntOr(row, "Id", 0), IntOr(row, "TownMapX", -1, -1),
                                            IntOr(row, "TownMapY", -1, -1),
                                            std::string{file->AsString()}, std::string{label->AsString()}});
                }
    // Brilliant Diamond title: the host reads the effective LP overlay before base romfs.
    if (!stopping)
        if (auto table = unity.ReadObjectByName(Settings, lp_vanilla::EncounterTable(c.pearl), 114))
            if (const auto* rows = table->Get("table")) {
                if (const auto* mv = table->Get("mvpoke"))
                    for (const auto& r : mv->items)
                        c.roamer_zones.push_back(IntOr(r, "zoneID", -1));
                c.encounters_ready = true;
                for (const auto& row : rows->items) {
                    const auto* zone = row.Get("zoneID");
                    if (!zone || zone->AsInt() < 0) continue;
                    auto& out = c.encounters[static_cast<int>(zone->AsInt())];
                    auto add = [&](const char* key, Method method, const char* rate) {
                        if (rate && (!row.Get(rate) || row.Get(rate)->AsInt() <= 0)) return;
                        const auto* mons = row.Get(key);
                        if (!mons) return;
                        for (const auto& mon : mons->items) {
                            const auto* species = mon.Get("monsNo");
                            const auto* low = mon.Get("minlv");
                            const auto* high = mon.Get("maxlv");
                            if (!species || !low || !high || species->AsInt() <= 0 || low->AsInt() <= 0) continue;
                            Encounter e{static_cast<int>(species->AsInt()), static_cast<int>(low->AsInt()),
                                        static_cast<int>(high->AsInt()), method};
                            bool merged = false;
                            for (auto& existing : out)
                                if (existing.species == e.species && existing.method == e.method) {
                                    existing.min_level = std::min(existing.min_level, e.min_level);
                                    existing.max_level = std::max(existing.max_level, e.max_level);
                                    merged = true;
                                    break;
                                }
                            if (!merged) out.push_back(std::move(e));
                        }
                    };
                    add("ground_mons", Method::Grass, "encRate_gr");
                    add("day", Method::GrassDay, "encRate_gr");
                    add("night", Method::GrassNight, "encRate_gr");
                    add("water_mons", Method::Surf, "encRate_wat");
                    add("boro_mons", Method::OldRod, "encRate_turi_boro");
                    add("ii_mons", Method::GoodRod, "encRate_turi_ii");
                    add("sugoi_mons", Method::SuperRod, "encRate_sugoi");
                    add("tairyo", Method::Swarm, "encRate_gr");
                    add("swayGrass", Method::Radar, "encRate_gr");
                }
            }
}

LangState Assets::Language() const {
    std::scoped_lock lock{mutex};
    LangState l;
    if (!catalog)
        return l;
    l.id = catalog->lang.id;
    l.kanji = catalog->lang.kanji;
    l.variant = catalog->detected.variant;
    l.modded = catalog->detected.modded;
    l.sfx = std::string{catalog->lang.sfx};
    l.logo_sfx = catalog->logo_sfx;
    l.generation = catalog->generation;
    return l;
}
std::string Assets::Label(std::string_view table, std::string_view label) const {
    std::scoped_lock lock{mutex};
    return catalog ? catalog->labels.Get(table, label) : std::string{};
}
std::vector<int> Assets::NameRank() const {
    std::scoped_lock lock{mutex};
    return catalog ? catalog->name_rank : std::vector<int>{};
}

std::string Assets::TypeName(int type) const {
    std::scoped_lock lock{mutex};
    return catalog && type >= 0 && type < static_cast<int>(catalog->type_names.size()) ? catalog->type_names[type]
                                                                                       : std::string{};
}
std::vector<std::uint32_t> Assets::Codepoints() const {
    std::scoped_lock lock{mutex};
    return catalog ? catalog->codepoints : std::vector<std::uint32_t>{};
}
std::vector<std::uint32_t> Assets::FontMissing() {
    std::scoped_lock lock{font_mutex};
    return served ? served->atlas.missing : std::vector<std::uint32_t>{};
}
std::string Assets::CatalogText(std::vector<std::string> Catalog::*list, int i) const {
    std::scoped_lock lock{mutex};
    if (!catalog)
        return {};
    const auto& texts = (*catalog).*list;
    return i > 0 && i < static_cast<int>(texts.size()) ? texts[static_cast<std::size_t>(i)] : std::string{};
}
std::string Assets::SpeciesName(int species) const {
    return CatalogText(&Catalog::species, species);
}
std::string Assets::DexDescription(int species) const {
    return CatalogText(&Catalog::dex_descriptions, species);
}
std::string Assets::AreaName(int zone) const {
    std::scoped_lock lock{mutex};
    if (!catalog) return {};
    const auto it = catalog->area_names.find(zone);
    return it == catalog->area_names.end() ? std::string{} : it->second;
}
bool Assets::EncounterDataReady() const {
    std::scoped_lock lock{mutex};
    return catalog && catalog->encounters_ready;
}
std::vector<Encounter> Assets::Encounters(int zone) const {
    std::scoped_lock lock{mutex};
    if (!catalog) return {};
    const auto it = catalog->encounters.find(zone);
    return it == catalog->encounters.end() ? std::vector<Encounter>{} : it->second;
}
std::string Assets::WazaName(int waza) const {
    return CatalogText(&Catalog::moves, waza);
}
std::string Assets::ItemName(int item) const {
    return CatalogText(&Catalog::items, item);
}
std::string Assets::ItemPlural(int item) const {
    return CatalogText(&Catalog::item_plurals, item);
}
std::string Assets::AbilityName(int ability) const {
    return CatalogText(&Catalog::ability_names, ability);
}
std::string Assets::AbilityDescription(int ability) const {
    return CatalogText(&Catalog::ability_descriptions, ability);
}
std::string Assets::ItemDescription(int item) const {
    return CatalogText(&Catalog::item_descriptions, item);
}
std::string Assets::WazaDescription(int waza) const {
    return CatalogText(&Catalog::move_descriptions, waza);
}

std::optional<WazaInfo> Assets::Waza(int waza) const {
    std::scoped_lock lock{mutex};
    if (!catalog || waza <= 0)
        return std::nullopt;
    const auto it = catalog->waza.find(waza);
    if (it == catalog->waza.end())
        return std::nullopt;
    return it->second;
}
std::pair<int,int> Assets::MapMasks(const std::array<int32_t,500>& work,const std::array<std::uint8_t,1000>& sys) const {
    std::lock_guard lock{mutex};
    if (!catalog) return {0,0};
    int visible=0,arrived=0;
    for (size_t i=0;i<lp_map::overlays.size();++i) {
        const auto it=catalog->map_gates.find(lp_map::overlays[i].name);
        const auto gate=it==catalog->map_gates.end() ? Catalog::MapGate{} : it->second;
        if (lp_map::Visible(gate.view,work)) visible|=1<<i;
        if (lp_map::Arrived(gate.view,gate.color,gate.town,work,sys)) arrived|=1<<i;
    }
    return {visible,arrived};
}

bool Assets::MapRoom(int zone) const {
    std::lock_guard lock{mutex};
    if (!catalog) return false;
    const auto it = catalog->map_types.find(zone);
    return it != catalog->map_types.end() && it->second == 3;
}

std::optional<std::pair<int, int>> Assets::MapCell(int zone, int grid_x, int grid_y) const {
    std::lock_guard lock{mutex};
    if (!catalog) return std::nullopt;
    const auto rows = catalog->town_map.find(zone);
    if (rows == catalog->town_map.end()) return std::nullopt;
    // The game's (g + 31) >> 5 for negative g (an arithmetic shift) is C# / C++ truncating division
    // (contracts/poketch-apps.md, player cursor step 2): -1 -> 0, -32 -> -1, -33 -> -1. Not unit
    // tested: the catalog only comes from the game's files.
    const int x = grid_x / 32;
    const int y = grid_y / 32;
    // Native lookup gives Index1 priority across all rows, then Index2, then Index3.
    for (int k = 0; k < 3; ++k)
        for (const auto& row : rows->second)
            if (row.indices[k] >= 0 && row.indices[k] == row.width * y + x)
                return std::pair{row.x, row.y};
    return std::nullopt;
}

std::optional<Catalog::MapGuide> Assets::MapGuide(int sequence) const {
    std::lock_guard lock{mutex};
    if (sequence < 0 || !catalog || catalog->map_guides.empty()) return std::nullopt;
    for (const auto& row : catalog->map_guides)
        if (row.id == sequence) return row;
    // Native Array.BinarySearch failure is clamped to index zero, not the preceding Id.
    return catalog->map_guides.front();
}

std::optional<Catalog::FlySpot> Assets::FlySpotAt(int x, int y) const {
    std::lock_guard lock{mutex};
    if (!catalog) return std::nullopt;
    for (const auto& f : catalog->fly_spots)
        if (f.x == x && f.y == y) return f;
    return std::nullopt;
}

std::vector<int> Assets::ZonesAtCell(int x, int y) const {
    std::lock_guard lock{mutex};
    std::vector<int> out;
    if (!catalog) return out;
    for (const auto& [zone, rows] : catalog->town_map)
        for (const auto& r : rows)
            if (r.x == x && r.y == y) { out.push_back(zone); break; }
    return out;
}

const Catalog::Personal* Assets::PersonalLocked(int species, int form) const {
    if (!catalog || species <= 0 || species >= static_cast<int>(catalog->personal.size())) return nullptr;
    const auto* p = &catalog->personal[species];
    // Alternate forms live after the species block: form_index + form - 1.
    if (form > 0 && form < p->form_max && p->form_index > 0) {
        const int idx = p->form_index + form - 1;
        if (idx > 0 && idx < static_cast<int>(catalog->personal.size())) p = &catalog->personal[idx];
    }
    return p;
}

std::array<int, 3> Assets::SpeciesAbilities(int species, int form) const {
    std::lock_guard lock{mutex};
    const auto* p = PersonalLocked(species, form);
    return p ? p->abilities : std::array<int, 3>{};
}

std::optional<std::array<int, 6>> Assets::BaseStats(int species, int form) const {
    std::lock_guard lock{mutex};
    const auto* p = PersonalLocked(species, form);
    if (!p) return std::nullopt;
    if (std::all_of(p->base.begin(), p->base.end(), [](int v) { return v == 0; })) return std::nullopt;
    return p->base;
}

std::optional<std::pair<int, int>> Assets::RoamerCell(int index) const {
    std::lock_guard lock{mutex};
    if (!catalog || index < 0 || index >= static_cast<int>(catalog->roamer_zones.size())) return std::nullopt;
    const auto it = catalog->marking_pos.find(catalog->roamer_zones[index]);
    if (it == catalog->marking_pos.end()) return std::nullopt;
    return it->second;
}

std::optional<ItemData> Assets::Item(int item) const {
    std::scoped_lock lock{mutex};
    if (!catalog)
        return std::nullopt;
    const auto it = catalog->item_data.find(item);
    if (it == catalog->item_data.end())
        return std::nullopt;
    return it->second;
}
std::optional<std::pair<int, int>> Assets::SpeciesTypes(int species, int form) const {
    std::scoped_lock lock{mutex};
    const auto* p = PersonalLocked(species, form);
    if (!p || p->type1 < 0)
        return std::nullopt;
    return std::make_pair(p->type1, p->type2);
}

std::map<int, int> Assets::SinnohNumbers() const {
    std::scoped_lock lock{mutex};
    return catalog ? catalog->sinnoh_numbers : std::map<int, int>{};
}

std::optional<lp_dex::Info> Assets::DexInfo(int species, int form) const {
    std::scoped_lock lock{mutex};
    const auto* p = PersonalLocked(species, form);
    if (!p)
        return std::nullopt;
    return p->dex;
}
std::vector<int> Assets::ValidForms(int species) const {
    std::scoped_lock lock{mutex};
    std::vector<int> out;
    if (!catalog || species <= 0 || species >= static_cast<int>(catalog->dex_table.size()))
        return out;
    const int forms = std::max(1, catalog->dex_table[static_cast<std::size_t>(species)].form_max);
    for (int f = 0; f < forms; ++f)
        if (f == 0 || lp_dex::ValidAt(catalog->dex_table, species, f))
            out.push_back(f);
    return out;
}
std::vector<lp_dex::Member> Assets::EvolutionChain(int species, int form) const {
    std::scoped_lock lock{mutex};
    return catalog ? lp_dex::Chain(catalog->dex_table, species, form) : std::vector<lp_dex::Member>{};
}
std::vector<std::pair<int, Encounter>> Assets::SpeciesEncounters(int species) const {
    std::scoped_lock lock{mutex};
    std::vector<std::pair<int, Encounter>> out;
    if (!catalog || species <= 0)
        return out;
    for (const auto& [zone, list] : catalog->encounters)
        for (const auto& e : list)
            if (e.species == species)
                out.emplace_back(zone, e);
    return out;
}
std::optional<std::pair<int, int>> Assets::ZoneCell(int zone) const {
    std::scoped_lock lock{mutex};
    if (!catalog)
        return std::nullopt;
    const auto it = catalog->town_map.find(zone);
    if (it == catalog->town_map.end() || it->second.empty())
        return std::nullopt;
    return std::pair{it->second.front().x, it->second.front().y};
}

std::shared_ptr<const std::vector<std::uint8_t>> Assets::FaceData(std::string_view face, std::string_view font_bundle) {
    {
        std::scoped_lock lock{font_mutex};
        for (const auto& [name, data] : face_cache)
            if (data && name == face)
                return data;
    }
    // The program romfs Data/resources.assets carries every game face as a Font object (the v0
    // layout; the Luminescent overlay's own copy holds only LiberationSans, so base: first). A
    // 1.3.0 romfs keeps them in the Dpr/font/<x>_font bundles instead.
    std::optional<std::vector<std::uint8_t>> otf;
    for (const char* path : {"base:/Data/resources.assets", "/Data/resources.assets"}) {
        otf = lp_unity::ExtractFontData(read, path, face);
        if (otf)
            break;
    }
    if (!otf && !font_bundle.empty())
        otf = lp_unity::ExtractBundleFontData(unity, "Dpr/font/" + std::string{font_bundle}, face);
    if (!otf)
        return nullptr;
    auto data = std::make_shared<const std::vector<std::uint8_t>>(std::move(*otf));
    std::scoped_lock lock{font_mutex};
    // the language's face and the Latin one: a third face replaces the older entry
    face_cache[1] = std::move(face_cache[0]);
    face_cache[0] = {std::string{face}, data};
    return data;
}

std::shared_ptr<const Assets::BuiltFont> Assets::BuildFont(const lp_lang::Info& lang, std::uint32_t px,
                                                           std::vector<std::uint32_t> cps) {
    std::sort(cps.begin(), cps.end());
    cps.erase(std::unique(cps.begin(), cps.end()), cps.end());
    const auto latin = lp_lang::Lookup(lp_lang::English);
    auto otf = FaceData(lang.face, lang.font_bundle);
    std::string face{lang.face};
    // The Latin face draws what a Korean / Chinese / Japanese face lacks (LP's English item names
    // with accents), and stands in for a face that cannot be read.
    std::shared_ptr<const std::vector<std::uint8_t>> fallback;
    if (lang.face != latin.face) {
        if (otf)
            fallback = FaceData(latin.face, latin.font_bundle);
        else {
            otf = FaceData(latin.face, latin.font_bundle);
            face = latin.face;
        }
    }
    if (!otf)
        return nullptr;
    // A large set (Korean, Chinese) is paged; keep it within three 2048x1024 pages (24 MiB, under
    // the runtime's 32 MiB page budget) by lowering the raster size, never below 28 px.
    std::optional<lp_unity::FontAtlas> atlas;
    for (std::uint32_t size = px;; size -= 4) {
        atlas = lp_unity::BuildFontAtlas(*otf, size, cps, FontPageH,
                                         fallback ? std::span<const std::uint8_t>{*fallback} : std::span<const std::uint8_t>{});
        if (!atlas || (atlas->atlas.height + FontPageH - 1) / FontPageH <= 3 || size < 32)
            break;
    }
    if (!atlas)
        return nullptr;
    auto f = std::make_shared<BuiltFont>();
    f->face = std::move(face);
    f->px = px;
    f->codepoints = std::move(cps);
    f->atlas = std::move(*atlas);
    return f;
}

std::shared_ptr<const lp_unity::Image> Assets::NurseryTitle() {
    lp_lang::Info language;
    std::string text;
    {
        std::scoped_lock lock{mutex};
        if (!catalog) return nullptr;
        language = catalog->lang;
        text = catalog->labels.Get("dp_poketch", "DP_poketch_72");
    }
    if (text.empty()) text = "Pokémon Nursery";
    const std::string key = std::string{language.face} + "/" + text;
    {
        std::scoped_lock lock{poketch_text_mutex};
        if (const auto it = poketch_text.find(key); it != poketch_text.end()) return it->second;
    }
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < text.size();) cps.push_back(lp_lang::NextCodepoint(text, i));
    auto font = BuildFont(language, 24, cps);
    if (!font) return nullptr;
    const auto& atlas = font->atlas;
    int width = 0;
    for (auto cp : cps) {
        if (cp < atlas.first_codepoint || cp - atlas.first_codepoint >= atlas.glyphs.size()) continue;
        width += atlas.glyphs[cp - atlas.first_codepoint].advance;
    }
    // Native UIText is centred on the sign. Compose at LCD resolution before the
    // colour/grid pass, so boxed and fullscreen versions share the exact same pixels.
    lp_unity::Image image{static_cast<uint32_t>(std::max(1, width + 8)), 48,
        std::vector<uint8_t>(static_cast<size_t>(std::max(1, width + 8)) * 48 * 4, 0)};
    int pen = 4;
    for (auto cp : cps) {
        if (cp < atlas.first_codepoint || cp - atlas.first_codepoint >= atlas.glyphs.size()) continue;
        const auto& glyph = atlas.glyphs[cp - atlas.first_codepoint];
        for (int y = 0; y < glyph.h; ++y) for (int x = 0; x < glyph.w; ++x) {
            const int dx = pen + glyph.bearing_x + x, dy = 34 - glyph.bearing_y + y;
            if (dx < 0 || dx >= static_cast<int>(image.width) || dy < 0 || dy >= 48) continue;
            auto* out = image.rgba.data() + (static_cast<size_t>(dy) * image.width + dx) * 4;
            out[3] = std::max(out[3], atlas.atlas.rgba[(static_cast<size_t>(glyph.y+y) * atlas.atlas.width + glyph.x+x)*4+3]);
        }
        pen += glyph.advance;
    }
    auto result = std::make_shared<const lp_unity::Image>(std::move(image));
    std::scoped_lock lock{poketch_text_mutex};
    if (poketch_text.size() >= 4) poketch_text.erase(poketch_text.begin());
    poketch_text[key] = result;
    return result;
}

std::optional<Font> Assets::FontMetrics(std::string_view spec_text) {
    const auto spec = ParseFontSpec(spec_text);
    if (!spec)
        return std::nullopt;
    std::shared_ptr<const BuiltFont> f;
    {
        std::scoped_lock lock{font_mutex};
        font_px = spec->second;
        if (prepared && prepared->px == spec->second)
            served = prepared; // a newer build (another language, more glyphs): the runtime switches now
        f = served;
    }
    if (!f) {
        // The first decode, before any language is known: the Latin face with the default set.
        auto latin = lp_lang::Lookup(lp_lang::English);
        f = BuildFont(latin, spec->second, lp_unity::DefaultCodepoints());
        if (!f)
            return std::nullopt;
        std::scoped_lock lock{font_mutex};
        if (!served)
            served = f;
        f = served;
    }
    return Font{f->atlas.line_height, f->atlas.first_codepoint, f->atlas.glyphs};
}

void Assets::NoteText(std::string_view text) const {
    for (const auto cp : font_known.Missing(text)) {
        font_known.Add(cp);
        font_extra.push_back(cp);
        font_more = true;
    }
}

void Assets::PumpFont(std::uint64_t tick) {
    if (font_running || stopping)
        return;
    std::shared_ptr<const Catalog> c;
    {
        std::scoped_lock lock{mutex};
        c = catalog;
    }
    std::uint32_t px;
    std::shared_ptr<const BuiltFont> have;
    {
        std::scoped_lock lock{font_mutex};
        px = font_px;
        have = prepared ? prepared : served;
    }
    if (!c || px == 0)
        return; // no catalog yet, or the runtime has not asked for the font
    if (c != font_catalog) {
        // a new catalog (another language): the set is its text plus everything published so far
        font_catalog = c;
        font_known = {};
        for (const auto cp : lp_unity::DefaultCodepoints())
            font_known.Add(cp);
        for (const auto cp : c->codepoints)
            font_known.Add(cp);
        for (const auto cp : font_extra)
            font_known.Add(cp);
        font_more = false;
        font_due = true;
    }
    if (font_more && tick >= font_next_tick) {
        font_more = false;
        font_due = true;
    }
    if (!font_due)
        return;
    font_due = false;
    auto cps = font_known.Sorted();
    const auto lang = c->lang;
    const auto face = lang.face;
    if (have && have->px == px && have->face == face && have->codepoints == cps)
        return; // the font in use already is this one
    font_next_tick = tick + 3 * 60; // a new nickname rebuilds at most every 3 s
    font_running = true;
    if (font_worker.joinable())
        font_worker.join();
    font_worker = std::thread([this, lang, px, cps = std::move(cps)]() mutable {
        if (auto f = BuildFont(lang, px, std::move(cps)); f && !stopping) {
            std::scoped_lock lock{font_mutex};
            prepared = std::move(f);
            ++font_epoch; // the runtime decodes again (__font_epoch) and FontMetrics serves it
        }
        font_running = false;
    });
}

std::optional<Img> Assets::Image(std::string_view key) {
    constexpr std::string_view Prefix = "module:lp:";
    if (!key.starts_with(Prefix))
        return std::nullopt;
    key.remove_prefix(Prefix.size());
    // "...@<sfx>": a composed picture drawn with that language's text art (the Pokétch)
    std::string_view sfx = "en";
    if (const auto at = key.rfind('@'); at != std::string_view::npos && ValidSfx(key.substr(at + 1))) {
        sfx = key.substr(at + 1);
        key = key.substr(0, at);
    }
    if (key.starts_with("square/")) {
        auto image = Image(std::string{Prefix} + std::string{key.substr(7)});
        if (!image || !image->w || !image->h || image->w > 1024 || image->h > 1024)
            return std::nullopt;
        return SquareIcon(std::move(*image)); // native pixels centered on transparent padding, without stretching
    }
    if (key.starts_with("classic/")) {
        auto image = Image(std::string{Prefix} + std::string{key.substr(8)});
        if (!image) return std::nullopt;
        ClassicRestyle(key.substr(8), *image);
        return image;
    }
    if (key.starts_with("mask/")) {
        // White with the source alpha: the widget's tint colour makes it a flat silhouette.
        auto image = Image(std::string{Prefix} + std::string{key.substr(5)});
        if (!image) return std::nullopt;
        for (size_t i = 0; i + 3 < image->rgba.size(); i += 4)
            image->rgba[i] = image->rgba[i + 1] = image->rgba[i + 2] = 255;
        return image;
    }
    if (key.starts_with("tex/")) {
        // Whole title-screen textures (they have no sprite objects): Luminescent Platinum's own
        // logo (the native title's without the mod) and the "Press the A Button" line, in the
        // language of the key ("tex/logo/fr"; no suffix = en). A slot whose texture cannot be read
        // falls back to English.
        auto name = key.substr(4);
        std::string_view lang = "en";
        if (const auto slash = name.find('/'); slash != std::string_view::npos) {
            lang = name.substr(slash + 1);
            name = name.substr(0, slash);
            if (!ValidSfx(lang)) return std::nullopt;
        }
        if (name != "logo" && name != "startlogo" && name != "pressa") return std::nullopt;
        for (const std::string_view l : {lang, std::string_view{"en"}}) {
            const std::string family{lp_vanilla::LogoFamily(pearl_title, IsLumi())};
            const std::string tex = name != "pressa" ? "logo_" + family + "_" + std::string{l} : "text_" + family + "_" + std::string{l} + "_op_pushbutton";
            const std::string bundle = "Dpr/movie/" + family + (name != "pressa" ? "/logo/" : "/text/") + tex;
            if (auto img = unity.LoadTexture(bundle, tex))
                return name == "startlogo" ? StartLogo(ToImg(std::move(*img))) : ToImg(std::move(*img));
            if (l == "en") break;
        }
        return std::nullopt;
    }
    if (key == "dex/seen") {
        auto native = unity.LoadSprite("UIs/textures/zukan", "dex_deco_list_01_01");
        if (!native) return std::nullopt;
        auto image = ToImg(std::move(*native));
        for (size_t i=0;i<image.rgba.size();i+=4) {
            const auto gray=static_cast<uint8_t>((image.rgba[i]*54+image.rgba[i+1]*183+image.rgba[i+2]*19)/256);
            image.rgba[i]=image.rgba[i+1]=image.rgba[i+2]=gray;
        }
        return image;
    }
    if (key.starts_with("ui/")) {
        key.remove_prefix(3);
        const auto slash = key.find('/');
        if (slash == std::string_view::npos)
            return std::nullopt;
        const auto bundle = BundlePath(key.substr(0, slash));
        if (!bundle)
            return std::nullopt;
        if (auto img = unity.LoadSprite(*bundle, key.substr(slash + 1)))
            return ToImg(std::move(*img));
        return std::nullopt;
    }
    if (key.starts_with("bag/")) {
        const auto v = SplitInts(key.substr(4));
        if (v.size() != 2 || (v[1] != 0 && v[1] != 1)) return std::nullopt;
        // Most outfits use three digits. The alternate default outfits use
        // 01/02, while several accepted outfit IDs have no bag art at all.
        // Resolve the exact native style first, then the player's default bag.
        for (const auto& name : lp_vanilla::BagSprites(v[0], v[1] != 0))
            if (auto image = unity.LoadSprite(lp_unity::bundles::Common, name))
                return ToImg(std::move(*image));
        return std::nullopt;
    }
    if (key.starts_with("maphead/")) {
        const auto v = SplitInts(key.substr(8));
        if (v.size() != 2 && v.size() != 3) return std::nullopt;
        const bool male = v.size() == 3 ? v[2] != 0 : v[0] < 100;
        const auto [fashion, colour] = lp_vanilla::MapHead(v[0], v[1], IsLumi(), male);
        char name[48];
        std::snprintf(name, sizeof name, "map_ico_player_01_%03d_%02d", fashion, colour);
        auto image = unity.LoadSprite("UIs/textures_mass/texturemass", name);
        if (!image) image = unity.LoadSprite("UIs/textures_mass/texturemass",
            male ? "map_ico_player_01_000_00" : "map_ico_player_01_100_00");
        return image ? std::optional{ToImg(std::move(*image))} : std::nullopt;
    }
    if (key.starts_with("map/")) {
        const auto v = SplitInts(key.substr(4));
        if (v.size() != 7) return std::nullopt;
        auto image = unity.LoadSprite("UIs/textures/map", "map_img_map_01_base");
        if (!image || !lp_raster::Fits(image->width, image->height, image->rgba.size())) return std::nullopt;
        std::vector<Catalog::MapRow> rows;
        { std::lock_guard lock{mutex};
          if (catalog) if (auto it = catalog->town_map.find(v[0]); it != catalog->town_map.end()) rows = it->second; }
        // Native prefab: GridRoot (-302,336), Image_Base (-12,0), 1500x732.
        // The sliced base has a 6px central strip stretched by 234px. Undo it for raw sprite coordinates.
        constexpr int origin_x = 226, origin_y = 30, cell = 24;
        auto blend = [&](int x, int y, const std::array<std::uint8_t, 4>& c) {
            if (x < 0 || y < 0 || x >= static_cast<int>(image->width) || y >= static_cast<int>(image->height)) return;
            auto* p = image->rgba.data() + (static_cast<size_t>(y) * image->width + x) * 4;
            for (int k = 0; k < 3; ++k) p[k] = static_cast<std::uint8_t>((p[k] * (255-c[3]) + c[k]*c[3]) / 255);
        };
        // Native hidden regions first, then roads and towns. Unknown towns stay grayscale.
        // Several overlays share a sprite: decode each one once per render.
        std::map<std::string_view, std::optional<lp_unity::Image>> sprites;
        for (size_t i=0;i<lp_map::overlays.size();++i) {
            if (!(v[3] & (1<<i))) continue;
            const auto& r=lp_map::overlays[i];
            auto [cached, fresh] = sprites.try_emplace(r.sprite);
            if (fresh) cached->second = unity.LoadSprite("UIs/textures/map", r.sprite);
            const auto& sprite = cached->second;
            if (!sprite || !lp_raster::Fits(sprite->width, sprite->height, sprite->rgba.size()) || r.w <= 0 || r.h <= 0)
                continue; // a 0x0 (or damaged) sprite has no pixel to sample
            const int left=528+r.x-r.w/2,top=366-r.y-r.h/2;
            const bool gray=!(v[4] & (1<<i));
            for (int y=0;y<r.h;++y) for (int x=0;x<r.w;++x) {
                const auto* px=sprite->rgba.data()+((static_cast<size_t>(y)*sprite->height/r.h)*sprite->width+
                                                    static_cast<size_t>(x)*sprite->width/r.w)*4;
                std::array<std::uint8_t,4> c{px[0],px[1],px[2],px[3]};
                if (gray) { const auto g=static_cast<std::uint8_t>((c[0]*77+c[1]*150+c[2]*29)>>8);c[0]=c[1]=c[2]=g; }
                blend(left+x,top+y,c);
            }
        }
        for (const auto& row : rows)
            for (int y = 0; y < cell; ++y) for (int x = 0; x < cell; ++x)
                blend(origin_x + row.x*cell+x, origin_y + row.y*cell+y, {255,210,32,96});
        if (v[1] >= 0 && v[2] >= 0) {
            const int cx = origin_x + v[1]*cell + cell/2, cy = origin_y + v[2]*cell + cell/2;
            // The town map's own player marker: the trainer's head for their outfit and skin tone.
            char name[40];
            std::snprintf(name, sizeof name, "map_ico_player_01_%03d_%02d", v[5], v[6]);
            auto head = unity.LoadSprite("UIs/textures_mass/texturemass", name);
            if (!head) head = unity.LoadSprite("UIs/textures_mass/texturemass", "map_ico_player_01_000_00");
            if (head && lp_raster::Fits(head->width, head->height, head->rgba.size())) {
                // 2x: the map picture is shown at about 0.76x, so the head ends up about 1.5x its native size
                const int hw = static_cast<int>(head->width) * 2, hh = static_cast<int>(head->height) * 2;
                for (int y = 0; y < hh; ++y) for (int x = 0; x < hw; ++x) {
                    const auto* p = head->rgba.data() + ((static_cast<size_t>(y) / 2) * head->width + x / 2) * 4;
                    blend(cx - hw/2 + x + 3, cy - hh/2 + y + 4, {0, 0, 0, static_cast<std::uint8_t>(p[3] / 3)});
                }
                for (int y = 0; y < hh; ++y) for (int x = 0; x < hw; ++x) {
                    const auto* p = head->rgba.data() + ((static_cast<size_t>(y) / 2) * head->width + x / 2) * 4;
                    blend(cx - hw/2 + x, cy - hh/2 + y, {p[0], p[1], p[2], p[3]});
                }
            }
        }
        return ToImg(std::move(*image));
    }
    if (key.starts_with("wz/")) {
        // move button body by type; +100 = the classic Platinum theme texture
        const auto v = SplitInts(key.substr(3));
        if (v.size() != 1 || v[0] < 0 || v[0] % 100 >= 18) return std::nullopt;
        const auto body = WazaBodyKey(v[0] % 100);
        return Image(v[0] >= 100 ? std::string{Prefix} + "classic/" + body.substr(Prefix.size()) : body);
    }
    if (key.starts_with("pktring/") || key.starts_with("pktdot/")) {
        const bool dot = key.starts_with("pktdot/");
        const auto v = SplitInts(key.substr(dot ? 7 : 8));
        if (v.empty() || v.size() > 2) return std::nullopt;
        if (auto img = lp_poketch::RenderRing(v[0], dot, v.size() == 2 ? v[1] : 1000))
            return ToImg(std::move(*img));
        return std::nullopt;
    }
    if (key.starts_with("pktarrow/")) {
        const auto v = SplitInts(key.substr(9));
        if (v.empty() || v.size() > 2) return std::nullopt;
        if (auto img = lp_poketch::RenderArrow(unity, v[0], v.size() == 2 ? v[1] : 1000, &poketch_cache))
            return ToImg(std::move(*img));
        return std::nullopt;
    }
    if (key.starts_with("poketchx/")) {  // the full-screen display: <scale1000>/<app>/<colour>/...
        auto v = SplitInts(key.substr(9));
        if (v.size() < 3) return std::nullopt;
        const int scale = v.front();
        v.erase(v.begin());
        auto title = !v.empty() && v[0] == lp_poketch::EggMonitor ? NurseryTitle() : nullptr;
        if (auto img = lp_poketch::Render(unity, v, scale, sfx, &poketch_cache, title.get()))
            return ToImg(std::move(*img));
        return std::nullopt;
    }
    if (key.starts_with("poketch/")) {
        const auto v = SplitInts(key.substr(8));
        auto title = !v.empty() && v[0] == lp_poketch::EggMonitor ? NurseryTitle() : nullptr;
        if (auto img = lp_poketch::Render(unity, v, 1000, sfx, &poketch_cache, title.get()))
            return ToImg(std::move(*img));
        return std::nullopt;
    }
    if (key.starts_with("icon/")) {
        const auto v = SplitInts(key.substr(5));
        if (v.size() != 4)
            return std::nullopt;
        if (auto img = unity.LoadPokemonIcon(v[0], v[1], v[2], false)) {
            auto out = SquareIcon(ToImg(std::move(*img)));
            // Native party art has no shiny recolours. Use its native sparkle alpha, tinted
            // red, in the sprite's top-left corner instead of changing Pokémon colours.
            // (lp_raster::StampSparkle skips an icon under 8 px or an empty / damaged sparkle.)
            if (v[3] != 0)
                if (auto sparkle = unity.LoadSprite(lp_unity::bundles::SharedUi, "box_icon_shiny_01"))
                    lp_raster::StampSparkle(out.w, out.h, out.rgba, sparkle->width, sparkle->height, sparkle->rgba);
            return out;
        }
        return std::nullopt;
    }
    if (key.starts_with("item/")) {
        const auto v = SplitInts(key.substr(5));
        if (v.size() != 1)
            return std::nullopt;
        const auto data = Item(v[0]);
        // The host caches every decoded key: wait for the catalog's iconid rather than caching a
        // guess, unless the catalog build has given up.
        if (!data && !CatalogReady() && !CatalogFailed())
            return std::nullopt;
        const int icon = data && data->icon_id >= 0 ? data->icon_id : v[0];
        if (auto img = unity.LoadItemIcon(icon))
            return ToImg(std::move(*img));
        return std::nullopt;
    }
    if (key.starts_with("font/")) {
        // The atlas of the font the runtime decoded last: whole ("font/<spec>") or one page of
        // FontPageH rows ("font/<spec>/<page>", the manifest's {p}).
        std::shared_ptr<const BuiltFont> f;
        {
            std::scoped_lock lock{font_mutex};
            f = served;
        }
        if (!f)
            return std::nullopt;
        const auto& a = f->atlas.atlas;
        auto rest = key.substr(5);
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos)
            return Img{a.width, a.height, a.rgba};
        const auto page = SplitInts(rest.substr(slash + 1));
        if (page.size() != 1 || page[0] < 0)
            return std::nullopt;
        const std::uint64_t top = static_cast<std::uint64_t>(page[0]) * FontPageH;
        if (top >= a.height)
            return std::nullopt;
        const auto rows = static_cast<std::uint32_t>(std::min<std::uint64_t>(FontPageH, a.height - top));
        const auto* begin = a.rgba.data() + top * a.width * 4;
        return Img{a.width, rows, std::vector<std::uint8_t>(begin, begin + static_cast<size_t>(rows) * a.width * 4)};
    }
    return std::nullopt;
}

} // namespace lp_assets
