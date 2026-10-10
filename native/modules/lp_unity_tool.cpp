// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Development CLI for lp_unity (Linux only, never packaged). Reads a romfs dump; several
// --romfs directories form an overlay (first match wins: pass the Luminescent romfs first).
//
//   lp-unity-tool [--romfs DIR]... list <bundle>               sprites + textures
//   lp-unity-tool [--romfs DIR]... objects <bundle> [class]    m_Name + path id (114 = MonoBehaviour)
//   lp-unity-tool [--romfs DIR]... sprite <bundle> <name> <out.png>
//   lp-unity-tool [--romfs DIR]... texture <bundle> <name> <out.png>
//   lp-unity-tool [--romfs DIR]... pokemon <species> <form> <gender> <shiny> <out.png>
//   lp-unity-tool [--romfs DIR]... item <id> <out.png>
//   lp-unity-tool [--romfs DIR]... poketch <out.png> <app> <colour> [args...]   (lp_poketch LCD)
//   lp-unity-tool [--romfs DIR]... poketchx <out.png> <scale1000> <app> <colour> [args...]
//   lp-unity-tool [--romfs DIR]... json <bundle> <object name> [out.json]
//   lp-unity-tool [--romfs DIR]... msg <bundle> <object name> [index]
//   lp-unity-tool [--romfs DIR]... font <assets path> <font name> <cap px> <out.png> [out.otf]
//   lp-unity-tool [--romfs DIR]... lang <msg_lang_id> <out dir> [kanji]   the companion's language
//       pipeline (lp_assets): catalog, variant, labels, the language's font pages and art
//       (pass a language mod's romfs first; base: and romfs: paths read the same overlay)
//   lp-unity-tool [--romfs DIR]... check <spec file> <out dir> <reference dir>
//   lp-unity-tool [--romfs DIR]... dexcheck     the Pokédex readers on the real tables (spot checks)
//   lp-unity-tool [--romfs DIR]... mapfit <msg_lang_id> <out.tsv> [kanji]
//       audit every native map objective with the actual localized font and banner layout
//   lp-unity-tool compare <a.png> <b.png>
// Bundle paths are relative to /Data/StreamingAssets/AssetAssistant/ (e.g. UIs/textures/common).
// check: each spec line is "<bundle> <sprite> <reference png relative to reference dir>".

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lp_assets.h"
#include "lp_dex_text.h"
#include "lp_strings_data.h"
#include "lp_lang.h"
#include "lp_poketch.h"
#include "lp_text.h"
#include "lp_unity.h"
#include "video_core/textures/astc.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "stb_image_write.h"

namespace fs = std::filesystem;
using lp_unity::Image;

namespace {

std::vector<fs::path> g_roots;

std::optional<fs::path> Resolve(std::string_view path) {
    for (const std::string_view source : {"base:", "romfs:"})
        if (path.starts_with(source))
            path.remove_prefix(source.size());
    while (!path.empty() && path.front() == '/')
        path.remove_prefix(1);
    for (const auto& r : g_roots) {
        fs::path p = r / std::string{path};
        std::error_code ec;
        if (fs::is_regular_file(p, ec))
            return p;
    }
    return std::nullopt;
}

lp_unity::RangeReader MakeReader() {
    lp_unity::RangeReader r;
    r.size = [](std::string_view path) -> std::optional<std::uint64_t> {
        const auto p = Resolve(path);
        if (!p)
            return std::nullopt;
        std::error_code ec;
        const auto n = fs::file_size(*p, ec);
        if (ec)
            return std::nullopt;
        return n;
    };
    r.read = [](std::string_view path, std::uint64_t off, std::uint8_t* out, std::size_t n) {
        const auto p = Resolve(path);
        if (!p)
            return false;
        std::ifstream f(*p, std::ios::binary);
        f.seekg(static_cast<std::streamoff>(off));
        f.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
        return static_cast<std::size_t>(f.gcount()) == n;
    };
    return r;
}

lp_unity::AstcDecoder CpuAstc() {
    return [](const std::uint8_t* blocks, std::size_t size, std::uint32_t w, std::uint32_t h,
              std::uint32_t bw, std::uint32_t bh, std::vector<std::uint8_t>& rgba) {
        const std::size_t need = static_cast<std::size_t>((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
        if (size < need)
            return false;
        rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
        Tegra::Texture::ASTC::Decompress(std::span<const std::uint8_t>{blocks, need}, w, h, 1, bw,
                                         bh, std::span<std::uint8_t>{rgba});
        return true;
    };
}

bool WritePng(const fs::path& path, const Image& img) {
    if (path.has_parent_path())
        fs::create_directories(path.parent_path());
    return stbi_write_png(path.string().c_str(), static_cast<int>(img.width),
                          static_cast<int>(img.height), 4, img.rgba.data(),
                          static_cast<int>(img.width * 4)) != 0;
}

std::optional<Image> ReadPng(const fs::path& path) {
    int w, h, n;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    if (!data)
        return std::nullopt;
    Image img;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.rgba.assign(data, data + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(data);
    return img;
}

struct Diff {
    bool size_match{};
    int max_diff{};          ///< any channel, any pixel
    int visible_max{};       ///< pixels where either alpha > 0
    std::size_t pixels_diff{};
    std::size_t pixels_over2{};
};

Diff Compare(const Image& a, const Image& b) {
    Diff d;
    d.size_match = a.width == b.width && a.height == b.height;
    if (!d.size_match)
        return d;
    for (std::size_t p = 0; p < a.rgba.size(); p += 4) {
        int m = 0;
        for (int c = 0; c < 4; ++c)
            m = std::max(m, std::abs(int{a.rgba[p + c]} - int{b.rgba[p + c]}));
        d.max_diff = std::max(d.max_diff, m);
        // Colour under alpha 0 is invisible (UnityPy zeroes it outside tight-sprite meshes).
        if (a.rgba[p + 3] || b.rgba[p + 3])
            d.visible_max = std::max(d.visible_max, m);
        if (m > 0)
            ++d.pixels_diff;
        if (m > 2)
            ++d.pixels_over2;
    }
    return d;
}

void JsonString(std::ostream& o, std::string_view s) {
    o << '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            o << "\\\"";
            break;
        case '\\':
            o << "\\\\";
            break;
        case '\n':
            o << "\\n";
            break;
        case '\r':
            o << "\\r";
            break;
        case '\t':
            o << "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                o << buf;
            } else {
                o << ch;
            }
        }
    }
    o << '"';
}

void Json(std::ostream& o, const lp_unity::Value& v) {
    using K = lp_unity::Value::Kind;
    char buf[64];
    switch (v.kind) {
    case K::None:
        o << "null";
        break;
    case K::Int:
        o << v.i;
        break;
    case K::UInt:
        o << v.u;
        break;
    case K::Bool:
        o << (v.i ? "true" : "false");
        break;
    case K::Float:
        std::snprintf(buf, sizeof(buf), "%.17g", v.f);
        o << (std::isfinite(v.f) ? buf : "null");
        break;
    case K::String:
        JsonString(o, v.s);
        break;
    case K::Bytes:
        o << '[';
        for (std::size_t k = 0; k < v.s.size(); ++k)
            o << (k ? "," : "") << int{static_cast<unsigned char>(v.s[k])};
        o << ']';
        break;
    case K::Array:
        o << '[';
        for (std::size_t k = 0; k < v.items.size(); ++k) {
            if (k)
                o << ',';
            Json(o, v.items[k]);
        }
        o << ']';
        break;
    case K::Object:
        o << '{';
        for (std::size_t k = 0; k < v.fields.size(); ++k) {
            if (k)
                o << ',';
            JsonString(o, v.fields[k].first);
            o << ':';
            Json(o, v.fields[k].second);
        }
        o << '}';
        break;
    }
}

// ---- the fit report (dev only) -----------------------------------------------------------------
// Every string where the manifest draws it (lp_strings_data.h Fits, CatalogFits, Sums), in the
// catalog's language, measured with that language's font exactly as the runtime lays labels out
// (fit_text: the largest scale down to min_scale whose one-line width fits; then the lines at the
// wrap width, max_lines kept). A string overflows when even its smallest scale needs more lines than
// the label shows, or (a label without wrapping) is wider than its room. Templates are filled with
// the widest of a few real names. Writes <out>/fit.txt; returns the number of overflows.
struct FitResult {
    int scale = 0, width = 0, lines = 0;
    bool over = false;
};
// Every text the Pokédex page composes for one kind of label (gen_manifest _fit_kind "dex_*"), for
// every species and form the game has (valid_flag).
std::vector<std::string> DexTexts(const lp_assets::Assets& a, const lp_strings::Table& t, std::string_view kind) {
    const lp_dex_text::Ctx c{t, a};
    std::vector<std::string> out;
    if (kind == "dex_gender") {
        for (const int p : {125, 250, 500, 750, 875, 1000})
            out.push_back("♂ " + lp_dex_text::Percent(c, p));
        return out;
    }
    for (int s = 1; s < 1100; ++s) {
        const auto base = a.DexInfo(s, 0);
        if (!base || !base->valid || a.SpeciesName(s).empty())
            continue;
        const auto forms = a.ValidForms(s);
        if (kind == "dex_category")
            out.push_back(lp_dex_text::Category(c, s));
        if (kind == "dex_area" || kind == "dex_area_way")
            for (const auto& r : lp_dex_text::Areas(c, s))
                out.push_back(kind == "dex_area" ? r.name : r.way);
        for (const int f : forms) {
            const auto d = a.DexInfo(s, f);
            if (!d)
                continue;
            if (kind == "dex_height") out.push_back(lp_dex_text::Height(c, s, f));
            if (kind == "dex_weight") out.push_back(lp_dex_text::Weight(c, s, f));
            if (kind == "dex_form") out.push_back(lp_dex_text::FormSubtitle(c, s, f, forms.size() > 1));
            if (kind == "dex_ev") out.push_back(lp_dex_text::EvYieldText(c, *d));
            if (kind == "dex_growth") out.push_back(lp_dex_text::Growth(c, d->growth));
            if (kind == "dex_egg") out.push_back(lp_dex_text::EggGroups(c, *d));
            if (kind == "dex_hatch") out.push_back(lp_dex_text::HatchSteps(c, *d));
            if (kind == "dex_evo" || kind == "dex_evo_name")
                for (const auto& m : a.EvolutionChain(s, f))
                    out.push_back(kind == "dex_evo" ? lp_dex_text::WaysText(c, m.via)
                                                    : lp_dex_text::DisplayName(c, m.species, m.form,
                                                                               a.ValidForms(m.species).size() > 1));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

FitResult FitOne(const lp_text::Measure& m, const std::string& text, int scale, int min_scale, int width, int lines) {
    FitResult r;
    r.scale = scale;
    if (width > 0 && min_scale < scale && text.find('\n') == std::string::npos)
        while (r.scale > min_scale && m.Width(text, r.scale) > width)
            --r.scale;
    r.width = m.Width(text, r.scale);
    r.lines = width > 0 ? m.Lines(text, r.scale, width) : 1;
    r.over = width > 0 && (lines > 0 ? r.lines > lines : false);
    if (lines == 1 && width > 0)
        r.over = r.width > width;
    return r;
}

int FitReport(const lp_assets::Assets& a, const lp_assets::Font& font, const fs::path& out_dir) {
    const auto l = a.Language();
    lp_strings::Table t;
    t.Build(lp_strings::Rows, l.variant,
            [&a](std::string_view table, std::string_view label) { return a.Label(table, label); });
    const lp_text::Measure m{font};
    const auto widest = [&m](std::initializer_list<std::string> names) {
        std::string best;
        for (const auto& n : names)
            if (m.Width(n, 5) > m.Width(best, 5))
                best = n;
        return best;
    };
    std::string type;
    for (int k = 0; k < 18; ++k)
        type = widest({type, a.TypeName(k)});
    const bool zh = l.variant == lp_lang::Variant::ZhHans || l.variant == lp_lang::Variant::ZhHant;
    const std::string item = widest({a.ItemName(17), a.ItemName(26), a.ItemName(28), a.ItemName(76), a.ItemName(77),
                                     a.ItemName(78), a.ItemName(431), a.ItemName(443)});
    const std::string who = widest({a.SpeciesName(6), a.SpeciesName(395), a.SpeciesName(445), a.SpeciesName(466),
                                    a.SpeciesName(479), a.SpeciesName(483)});
    const std::string move = widest({a.WazaName(57), a.WazaName(85), a.WazaName(89), a.WazaName(331),
                                     a.WazaName(424), a.WazaName(435)});
    const std::string foe = a.SpeciesName(395) + (zh ? "、" : " · ") + a.SpeciesName(445);
    // rows filled with one item only: the Poké Radar, the repels
    const std::string radar = a.ItemName(431);
    const std::string repel = widest({a.ItemName(76), a.ItemName(77), a.ItemName(79)});
    const auto fill = [&](const std::string& text, std::string_view key = {}) {
        const std::string& it = key == "route_radar_required" ? radar : key == "toast_repel_used" ? repel : item;
        return lp_strings::Fill(text, {{"item", it}, {"who", who}, {"move", move}, {"type", type}, {"foe", foe},
                                       {"ability", a.AbilityName(22)}, {"effect", a.Label("ss_btl_state", "BTR_STATE_08_01")},
                                       {"n", "100"}, {"min", "52"}, {"max", "58"}});
    };
    std::FILE* out = std::fopen((out_dir / "fit.txt").c_str(), "w");
    if (!out)
        return -1;
    std::fprintf(out, "fit report: lang %d variant %s (font line %u)\n\n", l.id,
                 std::string{lp_lang::Code(l.variant)}.c_str(), font.line_height);
    int overs = 0, shrinks = 0;
    std::string shrunk;
    for (const auto& f : lp_strings::Fits) {
        if (f.key >= lp_strings::Rows.size() || f.width <= 0)
            continue;
        const auto text = fill(t.Get(f.key), lp_strings::Rows[f.key].key);
        const auto r = FitOne(m, text, f.scale, f.min_scale, f.width, f.lines);
        char line[1024];
        std::snprintf(line, sizeof line, "%-12s %-24s s%d..%d room %4d lines %d: \"%s\" -> %d px at %d, %d line(s)\n",
                      std::string{f.where}.c_str(), std::string{lp_strings::Rows[f.key].key}.c_str(), f.scale,
                      f.min_scale, f.width, f.lines, text.c_str(), r.width, r.scale, r.lines);
        if (r.over) {
            ++overs;
            std::fprintf(out, "OVERFLOW %s", line);
        } else if (r.scale < f.scale) {
            ++shrinks;
            shrunk += "  shrinks " + std::string{line};
        }
    }
    // rows of labels laid out one after another (the Map legend)
    for (const auto& s : lp_strings::Sums) {
        if (s.limit <= 0)
            continue;
        int sum = 0;
        std::string names;
        for (const auto k : s.parts)
            if (k < lp_strings::Rows.size()) {
                sum += m.Width(t.Get(k), s.scale);
                names += " " + t.Get(k);
            }
        std::fprintf(out, "%s %s at %d: %d of %d px (%s)\n", sum <= s.limit ? "row fits" : "ROW TOO LONG",
                     std::string{s.name}.c_str(), s.scale, sum, s.limit, names.c_str());
        if (sum > s.limit) {
            // the page falls back to the same row at 4: it has to fit there
            int sum4 = 0;
            for (const auto k : s.parts)
                if (k < lp_strings::Rows.size())
                    sum4 += m.Width(t.Get(k), 4);
            std::fprintf(out, "%s %s at 4: %d of %d px\n", sum4 <= s.limit ? "row fits" : "OVERFLOW", std::string{s.name}.c_str(),
                         sum4, s.limit);
            overs += sum4 > s.limit;
        }
    }
    // the game's names where they are drawn
    for (const auto& c : lp_strings::CatalogFits) {
        if (c.kind.empty() || c.width <= 0)
            continue;
        std::vector<std::string> names;
        if (c.kind == "move")
            for (int k = 1; k < 1000; ++k) // not the Z-Moves and Max Moves: a Pokémon never knows them
                names.push_back((k >= 622 && k <= 658) || (k >= 695 && k <= 703) || k == 719 || (k >= 723 && k <= 728) ||
                                        (k >= 757 && k <= 774)
                                    ? std::string{}
                                    : a.WazaName(k));
        else if (c.kind == "item")
            for (int k = 1; k < 2000; ++k) names.push_back(a.ItemName(k));
        else if (c.kind == "ability")
            for (int k = 1; k < 400; ++k) names.push_back(a.AbilityName(k));
        else if (c.kind.starts_with("dex_"))
            names = DexTexts(a, t, c.kind);
        else
            for (int k = 1; k < 1100; ++k) names.push_back(a.SpeciesName(k));
        int bad = 0, small = 0, total = 0;
        std::string examples;
        for (const auto& n : names) {
            if (n.empty())
                continue;
            ++total;
            const auto r = FitOne(m, n, c.scale, c.min_scale, c.width, c.lines);
            if (r.over) {
                if (++bad <= 6)
                    examples += " \"" + n + "\"(" + std::to_string(r.width) + ")";
            } else if (r.scale < c.scale) {
                ++small;
            }
        }
        std::fprintf(out, "%s %-8s %-12s s%d..%d room %4d lines %d: %d names, %d smaller, %d overflow%s\n",
                     bad ? "NAMES   " : "names ok", std::string{c.kind}.c_str(), std::string{c.where}.c_str(), c.scale,
                     c.min_scale, c.width, c.lines, total, small, bad, examples.c_str());
        overs += bad;
    }
    std::fprintf(out, "\n%d overflow(s); %d string(s) drawn smaller than their scale (fit_text):\n%s", overs, shrinks,
                 shrunk.c_str());
    std::fclose(out);
    return overs;
}

int Usage() {
    std::fprintf(stderr, "usage: see the header of lp_unity_tool.cpp\n");
    return 2;
}

double Ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    bool pearl = false;
    std::string language = "en";
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--romfs" && i + 1 < argc)
            g_roots.emplace_back(argv[++i]);
        else if (std::string_view{argv[i]} == "--pearl")
            pearl = true;
        else if (std::string_view{argv[i]} == "--language" && i + 1 < argc)
            language = argv[++i];
        else
            args.emplace_back(argv[i]);
    }
    if (args.empty())
        return Usage();
    const std::string& cmd = args[0];

    if (cmd == "compare" && args.size() == 3) {
        const auto a = ReadPng(args[1]), b = ReadPng(args[2]);
        if (!a || !b)
            return 1;
        const Diff d = Compare(*a, *b);
        std::printf("size %ux%u vs %ux%u match=%d max_diff=%d visible_max=%d pixels_diff=%zu over2=%zu\n",
                    a->width, a->height, b->width, b->height, d.size_match, d.max_diff, d.visible_max,
                    d.pixels_diff, d.pixels_over2);
        return d.size_match && d.visible_max <= 2 ? 0 : 1;
    }

    lp_unity::Reader reader{MakeReader(), CpuAstc()};

    if (cmd == "mapfit" && (args.size() == 3 || args.size() == 4)) {
        const int id = std::atoi(args[1].c_str());
        const bool kanji = args.size() == 4 && args[3] == "kanji";
        const auto guides = reader.ReadObjectByName("UIs/masterdatas/uimasterdatas", "TownMapGuideTable", 114);
        const auto* rows = guides ? guides->Get("Guide") : nullptr;
        if (!rows) return 1;
        lp_assets::Assets assets{MakeReader(), pearl};
        assets.SetLanguage(id, kanji);
        assets.SetAstc(CpuAstc());
        std::uint64_t tick = 0;
        const auto started = std::chrono::steady_clock::now();
        while (!assets.CatalogReady()) {
            assets.StartCatalog(tick += 60);
            if (assets.CatalogFailed() || Ms(started) > 120000) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        for (const auto& row : rows->items) {
            const auto* table = row.Get("MSFile"); const auto* label = row.Get("MSLabel");
            if (table && label) assets.NoteText(assets.Label(table->AsString(), label->AsString()));
        }
        constexpr std::string_view spec = "FOT-UDKakugoC80Pro-DB:48";
        if (!assets.FontMetrics(spec)) return 1;
        const auto font_started = std::chrono::steady_clock::now();
        while (assets.FontEpoch() == 0 && Ms(font_started) < 30000) {
            assets.PumpFont(tick += 60);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto font = assets.FontMetrics(spec);
        if (!font) return 1;
        const lp_text::Measure measure{*font};
        // Bound save-derived names by twelve UTF-16 units, using the widest printable BMP
        // glyph actually present in this locale's font (more conservative than any real name).
        std::uint32_t widest_cp = 'W';
        float widest_advance = 0;
        for (std::size_t n = 0; n < font->glyphs.size(); ++n) {
            const auto cp = font->first_codepoint + n;
            if (cp >= 0x20 && cp <= 0xffff && !(cp >= 0xd800 && cp <= 0xdfff) &&
                cp != '<' && cp != '>' && font->glyphs[n].advance > widest_advance) {
                widest_cp = cp;
                widest_advance = font->glyphs[n].advance;
            }
        }
        std::string glyph;
        if (widest_cp < 0x80) glyph += static_cast<char>(widest_cp);
        else if (widest_cp < 0x800) {
            glyph += static_cast<char>(0xc0 | (widest_cp >> 6));
            glyph += static_cast<char>(0x80 | (widest_cp & 63));
        } else {
            glyph += static_cast<char>(0xe0 | (widest_cp >> 12));
            glyph += static_cast<char>(0x80 | ((widest_cp >> 6) & 63));
            glyph += static_cast<char>(0x80 | (widest_cp & 63));
        }
        std::string rival;
        for (int n = 0; n < 12; ++n) rival += glyph;
        const auto replace_names = [&](std::string text) {
            // The same twelve-wide-glyph bound also exceeds either localized supporter name.
            for (const auto marker : {std::string_view{"{rival}"}, std::string_view{"{supporter}"}})
                for (std::size_t at = 0; (at = text.find(marker, at)) != std::string::npos; at += rival.size())
                    text.replace(at, marker.size(), rival);
            return text;
        };
        const auto lang = assets.Language();
        std::ofstream out{args[2]};
        if (!out) return 1;
        out << "# language=" << id << " kanji=" << kanji << " variant=" << lp_lang::Code(lang.variant)
            << " font_line_height=" << font->line_height << " missing_glyphs=" << assets.FontMissing().size()
            << " name_bound_utf16=12 name_bound_codepoint=" << widest_cp << '\n';
        out << "id\tlabel\twidth5\tlines5\theight5\twidth4\tlines4\theight4\tscale\tlines\ttext_height\tbanner_height\toverflow\ttext\n";
        int checked = 0, overflows = 0;
        for (const auto& row : rows->items) {
            const auto* table = row.Get("MSFile"); const auto* label = row.Get("MSLabel"); const auto* key = row.Get("Id");
            if (!table || !label || !key) continue;
            auto text = replace_names(assets.Label(table->AsString(), label->AsString()));
            const int h5 = measure.Height(text, 5, lp_text::MapGoalWidth);
            const int h4 = measure.Height(text, 4, lp_text::MapGoalWidth);
            const auto layout = lp_text::FitMapGoal(h5,h4);
            const int lines = measure.Lines(text, layout.scale, lp_text::MapGoalWidth);
            const bool overflow = !text.empty() && lines > 3;
            out << key->AsInt() << '\t' << label->AsString() << '\t'
                << measure.Width(text,5) << '\t' << measure.Lines(text,5,lp_text::MapGoalWidth) << '\t' << h5 << '\t'
                << measure.Width(text,4) << '\t' << measure.Lines(text,4,lp_text::MapGoalWidth) << '\t' << h4 << '\t'
                << layout.scale << '\t' << lines << '\t' << layout.text_height << '\t' << layout.height << '\t' << overflow << '\t';
            for (char ch : text) {
                if (ch == '\n') out << "\\n";
                else if (ch == '\t') out << "\\t";
                else if (ch == '\r') out << "\\r";
                else out << ch;
            }
            out << '\n'; ++checked; overflows += overflow;
        }
        std::printf("mapfit lang%d kanji%d variant%s: %d objectives, %d overflows, %zu missing font glyphs\n",
            id, kanji, std::string{lp_lang::Code(lang.variant)}.c_str(), checked, overflows, assets.FontMissing().size());
        return overflows ? 1 : 0;
    }

    if (cmd == "list" && args.size() == 2) {
        for (const auto& n : reader.ListTextures(args[1]))
            std::printf("texture %s\n", n.c_str());
        for (const auto& n : reader.ListSprites(args[1]))
            std::printf("sprite %s\n", n.c_str());
        return 0;
    }
    if (cmd == "objects" && (args.size() == 2 || args.size() == 3)) {
        const int cls = args.size() == 3 ? std::atoi(args[2].c_str()) : -1;
        for (const auto& [n, id] : reader.ListObjects(args[1], cls))
            std::printf("%lld %s\n", static_cast<long long>(id), n.c_str());
        return 0;
    }
    if (cmd == "findfield" && args.size() == 3) {
        for (const auto& [name, id] : reader.ListObjects(args[1], 114))
            if (const auto obj = reader.ReadObject(args[1], id); obj && obj->Get(args[2])) {
                std::printf("%lld %s\n", static_cast<long long>(id), name.c_str());
                Json(std::cout, *obj);
                std::printf("\n");
            }
        return 0;
    }
    if ((cmd == "sprite" || cmd == "texture") && args.size() == 4) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto img = cmd == "sprite" ? reader.LoadSprite(args[1], args[2])
                                         : reader.LoadTexture(args[1], args[2]);
        if (!img) {
            std::fprintf(stderr, "decode failed\n");
            return 1;
        }
        std::printf("%ux%u in %.1f ms\n", img->width, img->height, Ms(t0));
        return WritePng(args[3], *img) ? 0 : 1;
    }
    if ((cmd == "poketch" && args.size() >= 4) || (cmd == "poketchx" && args.size() >= 5)) {
        const bool enlarged = cmd == "poketchx";
        const int scale = enlarged ? std::atoi(args[2].c_str()) : 1000;
        std::vector<int> v;
        for (std::size_t k = enlarged ? 3 : 2; k < args.size(); ++k)
            v.push_back(std::atoi(args[k].c_str()));
        lp_poketch::SetSurface(lp_poketch::DotArtSurface, lp_poketch::DefaultDotArt());
        const auto img = lp_poketch::Render(reader, v, scale, language);
        if (!img)
            return 1;
        return WritePng(args[1], *img) ? 0 : 1;
    }
    if (cmd == "pokemon" && args.size() == 6) {
        const auto img = reader.LoadPokemonIcon(std::atoi(args[1].c_str()), std::atoi(args[2].c_str()),
                                                std::atoi(args[3].c_str()), std::atoi(args[4].c_str()) != 0);
        if (!img)
            return 1;
        std::printf("%ux%u\n", img->width, img->height);
        return WritePng(args[5], *img) ? 0 : 1;
    }
    if (cmd == "item" && args.size() == 3) {
        const auto img = reader.LoadItemIcon(std::atoi(args[1].c_str()));
        if (!img)
            return 1;
        std::printf("%ux%u\n", img->width, img->height);
        return WritePng(args[2], *img) ? 0 : 1;
    }
    if (cmd == "json" && (args.size() == 3 || args.size() == 4)) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto v = args[2].starts_with("id:") ? reader.ReadObject(args[1], std::stoll(args[2].substr(3)))
                                                 : reader.ReadObjectByName(args[1], args[2]);
        if (!v) {
            std::fprintf(stderr, "object not found or unreadable\n");
            return 1;
        }
        std::fprintf(stderr, "read in %.1f ms\n", Ms(t0));
        if (args.size() == 4) {
            std::ofstream f(args[3]);
            Json(f, *v);
        } else {
            Json(std::cout, *v);
            std::printf("\n");
        }
        return 0;
    }
    if (cmd == "msg" && (args.size() == 3 || args.size() == 4)) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto table = lp_unity::ReadMessageTable(reader, args[1], args[2]);
        std::fprintf(stderr, "%zu labels in %.1f ms\n", table.size(), Ms(t0));
        if (args.size() == 4) {
            const auto i = static_cast<std::size_t>(std::atoi(args[3].c_str()));
            std::printf("%s\n", i < table.size() ? table[i].c_str() : "<out of range>");
        } else {
            for (std::size_t i = 0; i < table.size(); ++i)
                std::printf("%zu\t%s\n", i, table[i].c_str());
        }
        return table.empty() ? 1 : 0;
    }
    if (cmd == "itemcheck") {
        const int lang = args.size() > 1 ? std::atoi(args[1].c_str()) : 2;
        lp_assets::Assets a{MakeReader(), pearl};
        a.SetLanguage(lang, false);
        std::uint64_t tick = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (!a.CatalogReady()) {
            a.StartCatalog(tick += 60);
            if (a.CatalogFailed() || Ms(t0) > 120000) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto l = lp_lang::Lookup(lang, false);
        int checked = 0, fails = 0;
        for (const auto kind : {"ss_itemname", "ss_itemname_plural", "ss_iteminfo"}) {
            const auto values = lp_unity::ReadMessageTable(reader, "Message/" + std::string{l.bundle},
                std::string{l.prefix} + "_" + kind, false, true);
            for (size_t id = 1; id < values.size(); ++id) {
                if (values[id].empty()) continue;
                const auto actual = std::string_view{kind} == "ss_itemname" ? a.ItemName(static_cast<int>(id)) :
                    std::string_view{kind} == "ss_itemname_plural" ? a.ItemPlural(static_cast<int>(id)) :
                    a.ItemDescription(static_cast<int>(id));
                const bool ok = std::string_view{kind} == "ss_iteminfo" ? !actual.empty() : actual == values[id];
                ++checked;
                if (!ok) { std::printf("FAIL %s item %zu\n", kind, id); ++fails; }
            }
        }
        std::printf("itemcheck lang %d: %d entries checked, %d failures\n", lang, checked, fails);
        for (int id = 1808; id <= 1836; ++id)
            if (!a.ItemName(id).empty())
                std::printf("%d\t%s\t%s\n", id, a.ItemName(id).c_str(), a.ItemDescription(id).c_str());
        return fails ? 1 : 0;
    }
    if (cmd == "mapcheck" || (cmd == "asset" && args.size() == 3)) {
        lp_assets::Assets a{MakeReader(), pearl};
        a.SetAstc(CpuAstc());
        a.SetLanguage(2, false);
        std::uint64_t tick = 0;
        const auto start = std::chrono::steady_clock::now();
        while (!a.CatalogReady()) {
            a.StartCatalog(tick += 60);
            if (a.CatalogFailed() || Ms(start) > 120000) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (cmd == "asset") {
            const auto pic = a.Image(args[1]);
            if (!pic) return 1;
            return WritePng(args[2], lp_unity::Image{pic->w, pic->h, pic->rgba}) ? 0 : 1;
        }
        if (a.MapGuide(-1)) return 1;
        for (int id : {100, 1400, 2600, 999999}) {
            const auto guide = a.MapGuide(id);
            if (!guide || (id != 999999 && guide->id != id) || (id == 999999 && guide->id != 100)) return 1;
            const auto text = a.Label(guide->table, guide->label);
            if (text.empty()) return 1;
            std::printf("guide %d -> %d (%d,%d) %s\n", id, guide->id, guide->x, guide->y, text.c_str());
        }
        const auto head = a.Image("module:lp:maphead/999/99"), fallback = a.Image("module:lp:maphead/0/0");
        if (!head || !fallback || head->rgba != fallback->rgba) return 1;
        std::printf("maphead missing-outfit fallback matches native default\n");
        return 0;
    }
    if (cmd == "dexcheck") {
        // The Pokédex readers on the real tables (pass the LP romfs first): evolution chains with
        // branches, forms, gender ratios, Egg Groups. Exits non-zero on the first wrong fact.
        lp_assets::Assets a{MakeReader(), pearl};
        std::uint64_t tick = 0;
        const auto t0 = std::chrono::steady_clock::now();
        a.SetLanguage(2, false);
        while (!a.CatalogReady()) {
            a.StartCatalog(tick += 60);
            if (a.CatalogFailed() || Ms(t0) > 120000)
                return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        lp_strings::Table t;
        t.Build(lp_strings::Rows, lp_lang::Variant::En,
                [&a](std::string_view table, std::string_view label) { return a.Label(table, label); });
        const lp_dex_text::Ctx c{t, a};
        int fails = 0;
        const auto expect = [&](bool ok, const std::string& what) {
            std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
            fails += !ok;
        };
        const auto chain = [&](int s, int f) {
            std::string out;
            for (const auto& m : a.EvolutionChain(s, f))
                out += std::string(static_cast<std::size_t>(m.depth) * 2, ' ') + std::to_string(m.species) + "/" +
                       std::to_string(m.form) + " [" + lp_dex_text::WaysText(c, m.via) + "]\n";
            return out;
        };
        const auto has = [&](int s, int f, int want_s, int want_f, int depth, const std::string& way) {
            for (const auto& m : a.EvolutionChain(s, f))
                if (m.species == want_s && m.form == want_f && m.depth == depth)
                    return way.empty() || lp_dex_text::WaysText(c, m.via) == way;
            return false;
        };
        std::printf("Eevee:\n%s", chain(133, 0).c_str());
        expect(a.EvolutionChain(133, 0).size() == 9, "Eevee: itself and 8 evolutions");
        expect(has(133, 0, 470, 0, 1, "Leaf Stone / Level up · Eterna Forest"), "Leafeon: Leaf Stone or Eterna Forest");
        expect(has(133, 0, 700, 0, 1, ""), "Sylveon in the chain (LP)");
        expect(has(196, 0, 133, 0, 0, ""), "Espeon's chain starts at Eevee");
        std::printf("Wurmple:\n%s", chain(265, 0).c_str());
        expect(has(265, 0, 266, 0, 1, "Lv. 7 · Random") && has(265, 0, 268, 0, 1, "Lv. 7 · Random"),
               "Wurmple: Silcoon / Cascoon at random");
        expect(has(265, 0, 267, 0, 2, "Lv. 10") && has(265, 0, 269, 0, 2, "Lv. 10"), "Beautifly / Dustox at depth 2");
        const auto w = a.EvolutionChain(265, 0);
        expect(w.size() == 5 && w[1].species == 266 && w[2].species == 267 && w[3].species == 268,
               "Wurmple: depth-first order (Silcoon, Beautifly, Cascoon, Dustox)");
        std::printf("Nincada:\n%s", chain(290, 0).c_str());
        expect(has(290, 0, 291, 0, 1, "Lv. 20"), "Ninjask at 20");
        expect(has(290, 0, 292, 0, 1, "Lv. 20 · Free party slot and Poké Ball in the Bag"), "Shedinja (implicit)");
        expect(has(292, 0, 290, 0, 0, ""), "Shedinja's chain starts at Nincada");
        std::printf("Burmy (Sandy):\n%s", chain(412, 1).c_str());
        expect(has(412, 1, 413, 1, 1, "Lv. 20 ♀") && has(412, 1, 414, 0, 1, "Lv. 20 ♂"),
               "Sandy Burmy: Sandy Wormadam (female) / Mothim (male)");
        expect(has(413, 2, 412, 2, 0, ""), "Trash Wormadam's chain starts at Trash Burmy");
        expect(lp_dex_text::DisplayName(c, 413, 1, true) == "Wormadam · Sandy Cloak", "Wormadam form name");
        std::printf("Tyrogue:\n%s", chain(236, 0).c_str());
        expect(has(236, 0, 106, 0, 1, "Lv. 20 · Attack > Defense") && has(236, 0, 237, 0, 1, "Lv. 20 · Attack = Defense"),
               "Tyrogue: three branches by Attack / Defense");
        std::printf("Slowpoke:\n%s", chain(79, 0).c_str());
        expect(has(79, 0, 80, 0, 1, "Lv. 33") && has(79, 0, 199, 0, 1, "King’s Rock"), "Slowpoke (LP: King's Rock item)");
        std::printf("Pichu:\n%s", chain(172, 0).c_str());
        expect(has(172, 0, 26, 1, 2, "Sun Stone"), "Alolan Raichu from Pikachu by Sun Stone (LP)");
        const auto rotom = a.ValidForms(479);
        expect(rotom.size() == 6, "Rotom: 6 forms");
        const auto heat = a.SpeciesTypes(479, 1);
        expect(heat && heat->first == 12 && heat->second == 9, "Heat Rotom: Electric / Fire");
        expect(lp_dex_text::DisplayName(c, 479, 1, true) == "Heat Rotom", "Heat Rotom name");
        expect(a.ValidForms(487).size() == 2 && a.ValidForms(493).size() == 18 && a.ValidForms(1).size() == 1,
               "Giratina 2 forms, Arceus 18, Bulbasaur 1");
        expect(a.EvolutionChain(479, 3).size() == 1, "Frost Rotom does not evolve");
        const auto bulba = a.DexInfo(1, 0);
        expect(bulba && lp_dex::GenderOf(bulba->sex).male_permille == 875, "Bulbasaur 87.5% male");
        expect(bulba && lp_dex_text::EggGroups(c, *bulba) == "Monster · Grass", "Bulbasaur Egg Groups");
        expect(bulba && lp_dex_text::EvYieldText(c, *bulba) == "Sp. Atk +1", "Bulbasaur EV yield");
        expect(bulba && lp_dex_text::Growth(c, bulba->growth) == "Medium Slow", "Bulbasaur growth");
        expect(bulba && lp_dex_text::HatchSteps(c, *bulba) == "256 steps", "Bulbasaur hatch steps (LP: every species 1 egg cycle): " + (bulba ? lp_dex_text::HatchSteps(c, *bulba) : std::string{}));
        expect(bulba && bulba->catch_rate == 45 && bulba->base_exp == 64 && bulba->friendship == 50,
               "Bulbasaur catch rate 45, base Exp. 64, friendship 50");
        expect(lp_dex_text::Category(c, 1) == "Seed Pokémon", "Bulbasaur category");
        expect(lp_dex_text::Height(c, 487, 1) != lp_dex_text::Height(c, 487, 0), "Giratina Origin has its own height");
        const auto magnemite = a.DexInfo(81, 0);
        expect(magnemite && lp_dex::GenderOf(magnemite->sex).kind == lp_dex::Gender::Genderless, "Magnemite genderless");
        const auto happiny = a.DexInfo(440, 0);
        expect(happiny && lp_dex::GenderOf(happiny->sex).kind == lp_dex::Gender::FemaleOnly, "Happiny female only");
        const auto ditto = a.DexInfo(132, 0);
        expect(ditto && lp_dex_text::EggGroups(c, *ditto) == "Ditto", "Ditto's Egg Group is its name");
        const auto areas = lp_dex_text::Areas(c, 396); // Starly
        std::printf("Starly areas: %zu, first %s | %s\n", areas.size(), areas.empty() ? "" : areas[0].name.c_str(),
                    areas.empty() ? "" : areas[0].way.c_str());
        expect(!areas.empty(), "Starly is met in the wild");
        std::printf("%d failure(s)\n", fails);
        return fails ? 1 : 0;
    }
    if (cmd == "lang" && (args.size() == 3 || args.size() == 4)) {
        // The companion's language pipeline offline (lp_assets): catalog for msg_lang_id, variant
        // detection, label lookups, the font for the language (paged atlas) and its art.
        const int id = std::atoi(args[1].c_str());
        const fs::path out = args[2];
        const bool kanji = args.size() == 4 && args[3] == "kanji";
        fs::create_directories(out);
        lp_assets::Assets a{MakeReader(), pearl};
        a.SetAstc(CpuAstc());
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t tick = 0;
        // English first, as the module starts, then the save's language
        for (const int want : {2, id}) {
            a.SetLanguage(want, kanji);
            while (a.Language().id != want || a.Language().generation == 0) {
                a.StartCatalog(tick += 60);
                if (a.CatalogFailed() || Ms(t0) > 120000) {
                    std::fprintf(stderr, "catalog failed\n");
                    return 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            std::printf("catalog %d ready at %.0f ms\n", want, Ms(t0));
        }
        const auto l = a.Language();
        std::printf("lang %d kanji %d variant %s modded %d sfx %s logo_sfx %s\n", l.id, l.kanji ? 1 : 0,
                    std::string{lp_lang::Code(l.variant)}.c_str(), l.modded ? 1 : 0, l.sfx.c_str(), l.logo_sfx.c_str());
        std::printf("species 1 %s | 387 %s\n", a.SpeciesName(1).c_str(), a.SpeciesName(387).c_str());
        std::printf("move 33 %s | 85 %s\n", a.WazaName(33).c_str(), a.WazaName(85).c_str());
        std::printf("item 1 %s | 50 %s | 1624 %s\n", a.ItemName(1).c_str(), a.ItemName(50).c_str(), a.ItemName(1624).c_str());
        std::printf("ability 22 %s | type 1 %s\n", a.AbilityName(22).c_str(), a.TypeName(1).c_str());
        std::string areas;
        for (int z = 0, n = 0; z < 600 && n < 4; ++z)
            if (const auto name = a.AreaName(z); !name.empty()) {
                areas += " " + std::to_string(z) + ":" + name;
                ++n;
            }
        std::printf("areas%s | dex 387 %s\n", areas.c_str(), a.DexDescription(387).c_str());
        std::printf("move 85 desc %s\n", a.WazaDescription(85).c_str());
        for (const auto& [t, lbl] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"ss_btl_app", "msg_ui_btl_00"}, {"ss_bag_pocket", "SS_bag_pocket_001"}, {"dp_poketch", "DP_poketch_21"},
                 {"ss_status", "SS_status_28"}, {"ss_pokedex", "SS_pokedex_130"}, {"ss_btl_app", "no_such_label"}})
            std::printf("label %s/%s = [%s]\n", t, lbl, a.Label(t, lbl).c_str());

        // The font: the runtime's first decode (English defaults), then the language's build, which
        // also has the code points of every lp_strings row (as the module asks for them).
        {
            lp_strings::Table t;
            t.Build(lp_strings::Rows, l.variant,
                    [&a](std::string_view table, std::string_view label) { return a.Label(table, label); });
            for (std::size_t i = 0; i < t.Size(); ++i)
                a.NoteText(t.Get(i));
        }
        constexpr std::string_view Spec = "FOT-UDKakugoC80Pro-DB:48";
        const auto first = a.FontMetrics(Spec);
        if (!first) {
            std::fprintf(stderr, "no font\n");
            return 1;
        }
        std::printf("first font: line %u glyphs %zu first U+%04X\n", first->line_height, first->glyphs.size(), first->first_codepoint);
        const auto t1 = std::chrono::steady_clock::now();
        while (a.FontEpoch() == 0 && Ms(t1) < 30000) {
            a.PumpFont(tick += 60);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto font = a.FontMetrics(Spec);
        if (!font) {
            std::fprintf(stderr, "no font after the language's build\n");
            return 1;
        }
        {
            lp_lang::CodepointSet defaults;
            for (const auto cp : lp_unity::DefaultCodepoints())
                defaults.Add(cp);
            std::string extra;
            int n = 0;
            for (const auto cp : a.Codepoints())
                if (!defaults.Has(cp) && n++ < 40) {
                    char b[16];
                    std::snprintf(b, sizeof b, " U+%04X", cp);
                    extra += b;
                }
            std::printf("catalog code points %zu, %d beyond the defaults:%s\n", a.Codepoints().size(), n, extra.c_str());
            std::string miss;
            for (const auto cp : a.FontMissing()) {
                char b[16];
                std::snprintf(b, sizeof b, " U+%04X", cp);
                miss += b;
            }
            std::printf("face lacks %zu:%s\n", a.FontMissing().size(), miss.substr(0, 400).c_str());
        }
        std::printf("font epoch %lld after %.0f ms: line %u glyphs %zu first U+%04X\n",
                    static_cast<long long>(a.FontEpoch()), Ms(t1), font->line_height, font->glyphs.size(), font->first_codepoint);
        // the pages, stacked back into one picture
        std::vector<Image> pages;
        for (int p = 0;; ++p) {
            const auto img = a.Image("module:lp:font/" + std::string{Spec} + "/" + std::to_string(p));
            if (!img)
                break;
            pages.push_back(Image{img->w, img->h, img->rgba});
            WritePng(out / ("font_page" + std::to_string(p) + ".png"), pages.back());
        }
        std::printf("pages %zu (%ux%u first)\n", pages.size(), pages.empty() ? 0 : pages[0].width, pages.empty() ? 0 : pages[0].height);
        if (pages.empty()) {
            std::fprintf(stderr, "no font pages\n");
            return 1;
        }
        // vertical metrics of a few glyphs (px above the baseline: bearing_y, height h)
        for (const char32_t cp : {U'H', U'1', U'g', U'\uC9C0', U'\uD55C', U'\u56FD', U'\u3002'}) {
            if (cp < font->first_codepoint || cp - font->first_codepoint >= font->glyphs.size())
                continue;
            const auto& g = font->glyphs[cp - font->first_codepoint];
            std::printf("glyph U+%04X: top %d bottom %d (h %u, advance %u)\n", static_cast<unsigned>(cp), g.bearing_y,
                        g.bearing_y - static_cast<int>(g.h), g.h, g.advance);
        }
        int crossing = 0, outside = 0;
        for (const auto& g : font->glyphs) {
            if (g.h == 0)
                continue;
            const auto page = g.y / lp_assets::FontPageH;
            crossing += page != (g.y + g.h - 1u) / lp_assets::FontPageH;
            outside += page >= pages.size() || g.x + g.w > pages[page].width ||
                       g.y % lp_assets::FontPageH + g.h > pages[page].height;
        }
        std::printf("glyphs crossing a page edge %d, outside their page %d\n", crossing, outside);
        // sample lines drawn with the metrics and pages exactly as the runtime places glyphs
        const std::vector<std::string> lines = {a.SpeciesName(387) + " " + a.WazaName(85) + " " + a.ItemName(50),
                                                a.AbilityName(22) + " " + a.TypeName(1) + " " + a.AreaName(431),
                                                a.Label("ss_btl_app", "msg_ui_btl_00") + " " + a.Label("ss_bag_pocket", "SS_bag_pocket_001")};
        const int lh = static_cast<int>(font->line_height);
        Image page;
        page.width = 40 * lh;
        page.height = static_cast<std::uint32_t>(lh * 3 * static_cast<int>(lines.size()) + lh);
        page.rgba.assign(static_cast<std::size_t>(page.width) * page.height * 4, 0xFF);
        int missing = 0;
        for (std::size_t li = 0; li < lines.size(); ++li) {
            int pen = lh / 2;
            const int base = lh * 2 + static_cast<int>(li) * lh * 3;
            const auto& t = lines[li];
            for (std::size_t i = 0; i < t.size();) {
                const char32_t cp = lp_lang::NextCodepoint(t, i);
                if (cp < font->first_codepoint || cp - font->first_codepoint >= font->glyphs.size()) {
                    ++missing;
                    pen += lh / 2;
                    continue;
                }
                const auto& g = font->glyphs[cp - font->first_codepoint];
                const auto& src = pages[g.y / lp_assets::FontPageH];
                const int sy = g.y % lp_assets::FontPageH;
                if (g.w == 0 && cp != ' ')
                    ++missing;
                for (int y = 0; y < g.h; ++y)
                    for (int x = 0; x < g.w; ++x) {
                        const int px = pen + g.bearing_x + x, py = base - g.bearing_y + y;
                        if (px < 0 || py < 0 || px >= static_cast<int>(page.width) || py >= static_cast<int>(page.height))
                            continue;
                        const std::uint8_t al = src.rgba[((static_cast<std::size_t>(sy) + y) * src.width + g.x + x) * 4 + 3];
                        std::uint8_t* q = page.rgba.data() + (static_cast<std::size_t>(py) * page.width + px) * 4;
                        q[0] = q[1] = q[2] = std::min<std::uint8_t>(q[0], static_cast<std::uint8_t>(255 - al));
                    }
                pen += g.advance;
            }
            std::printf("line %zu: %s (width at scale 6: %d)\n", li, t.c_str(), lp_text::Measure{*font}.Width(t, 6));
        }
        std::printf("sample glyphs missing: %d\n", missing);
        WritePng(out / "sample.png", page);
        const int overs = FitReport(a, *font, out);
        std::printf("fit report: %d overflow(s) (%s)\n", overs, (out / "fit.txt").c_str());
        for (const auto& [name, key] : std::initializer_list<std::pair<const char*, std::string>>{
                 {"typetag1.png", lp_assets::TypeTagKey(1, l.sfx)},
                 {"logo.png", "module:lp:tex/startlogo/" + l.logo_sfx},
                 {"logo-raw.png", "module:lp:tex/logo/" + l.logo_sfx},
                 {"pressa.png", "module:lp:tex/pressa/" + l.logo_sfx},
                 {"hidden.png", "module:lp:poketch/19/0/2/2/2/2/2/2/2/2" + std::string{l.sfx == "en" ? "" : "@" + l.sfx}}}) {
            if (const auto img = a.Image(key))
                WritePng(out / name, Image{img->w, img->h, img->rgba});
            else
                std::printf("no image %s\n", key.c_str());
        }
        a.Stop();
        return 0;
    }
    if (cmd == "font" && (args.size() == 5 || args.size() == 6)) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto otf = lp_unity::ExtractFontData(MakeReader(), args[1], args[2]);
        if (!otf) {
            std::fprintf(stderr, "font not found; fonts here:\n");
            for (const auto& n : lp_unity::ListFonts(MakeReader(), args[1]))
                std::fprintf(stderr, "  %s\n", n.c_str());
            return 1;
        }
        std::printf("font %zu bytes in %.1f ms\n", otf->size(), Ms(t0));
        if (args.size() == 6) {
            std::ofstream f(args[5], std::ios::binary);
            f.write(reinterpret_cast<const char*>(otf->data()), static_cast<std::streamsize>(otf->size()));
        }
        const auto cps = lp_unity::DefaultCodepoints();
        std::vector<std::uint32_t> want = cps;
        want.push_back(0x20BD); // Poke Dollar: expected missing
        const auto t1 = std::chrono::steady_clock::now();
        const auto atlas = lp_unity::BuildFontAtlas(*otf, static_cast<std::uint32_t>(std::atoi(args[3].c_str())), want);
        if (!atlas)
            return 1;
        std::printf("atlas %ux%u line_height %u first U+%04X glyphs %zu ascent %d descent %d in %.1f ms\n",
                    atlas->atlas.width, atlas->atlas.height, atlas->line_height, atlas->first_codepoint,
                    atlas->glyphs.size(), atlas->ascent_px, atlas->descent_px, Ms(t1));
        for (const auto cp : atlas->missing)
            std::printf("missing U+%04X\n", cp);
        // Sample line under the atlas, laid out with the glyph metrics (black on white).
        const char32_t sample[] = U"Pokémon Lv.50 × HP 123/456 ♂♀ … \"Earthquake\" 1,234";
        const std::uint32_t lh = atlas->line_height;
        const std::uint32_t pad = lh;
        Image page;
        page.width = std::max<std::uint32_t>(atlas->atlas.width, 40 * lh);
        page.height = atlas->atlas.height + 3 * lh + pad;
        page.rgba.assign(static_cast<std::size_t>(page.width) * page.height * 4, 0xFF);
        for (std::uint32_t y = 0; y < atlas->atlas.height; ++y)
            for (std::uint32_t x = 0; x < atlas->atlas.width; ++x) {
                const std::uint8_t a = atlas->atlas.rgba[(static_cast<std::size_t>(y) * atlas->atlas.width + x) * 4 + 3];
                std::uint8_t* q = page.rgba.data() + (static_cast<std::size_t>(y) * page.width + x) * 4;
                q[0] = q[1] = q[2] = static_cast<std::uint8_t>(255 - a);
            }
        int pen = static_cast<int>(lh / 2);
        const int base = static_cast<int>(atlas->atlas.height + pad + 2 * lh);
        for (const char32_t ch : sample) {
            if (!ch)
                break;
            const std::uint32_t cp = ch;
            if (cp < atlas->first_codepoint || cp - atlas->first_codepoint >= atlas->glyphs.size())
                continue;
            const auto& g = atlas->glyphs[cp - atlas->first_codepoint];
            for (int y = 0; y < g.h; ++y)
                for (int x = 0; x < g.w; ++x) {
                    const int px = pen + g.bearing_x + x, py = base - g.bearing_y + y;
                    if (px < 0 || py < 0 || px >= static_cast<int>(page.width) || py >= static_cast<int>(page.height))
                        continue;
                    const std::uint8_t a = atlas->atlas.rgba[((static_cast<std::size_t>(g.y) + y) * atlas->atlas.width + g.x + x) * 4 + 3];
                    std::uint8_t* q = page.rgba.data() + (static_cast<std::size_t>(py) * page.width + px) * 4;
                    q[0] = q[1] = q[2] = std::min<std::uint8_t>(q[0], static_cast<std::uint8_t>(255 - a));
                }
            pen += g.advance;
        }
        return WritePng(args[4], page) ? 0 : 1;
    }
    if (cmd == "check" && args.size() == 4) {
        std::ifstream spec(args[1]);
        std::string line;
        int fails = 0, total = 0;
        while (std::getline(spec, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            std::string bundle, name, ref, kind;
            if (line.find('\t') != std::string::npos) { // tab-separated: names may hold spaces
                std::vector<std::string> cols;
                std::size_t at = 0;
                while (true) {
                    const auto tab = line.find('\t', at);
                    cols.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
                    if (tab == std::string::npos)
                        break;
                    at = tab + 1;
                }
                cols.resize(4);
                bundle = cols[0], name = cols[1], ref = cols[2], kind = cols[3];
            } else {
                std::istringstream ls(line);
                ls >> bundle >> name >> ref >> kind;
            }
            ++total;
            const auto t0 = std::chrono::steady_clock::now();
            std::optional<Image> img = kind == "texture" ? reader.LoadTexture(bundle, name)
                                                         : reader.LoadSprite(bundle, name);
            const double ms = Ms(t0);
            const fs::path out = fs::path{args[2]} / (fs::path{bundle}.filename().string() + "__" +
                                                      (kind == "texture" ? "tex_" : "") + name + ".png");
            if (!img) {
                std::printf("FAIL  %-40s %-34s decode failed\n", bundle.c_str(), name.c_str());
                ++fails;
                continue;
            }
            WritePng(out, *img);
            const auto r = ReadPng(fs::path{args[3]} / ref);
            if (!r) {
                std::printf("NOREF %-40s %-34s %ux%u (%s missing)\n", bundle.c_str(), name.c_str(),
                            img->width, img->height, ref.c_str());
                ++fails;
                continue;
            }
            const Diff d = Compare(*img, *r);
            const bool ok = d.size_match && d.visible_max <= 2;
            fails += ok ? 0 : 1;
            std::printf("%s %-46s %-30s %ux%u ref %ux%u max %d visible_max %d diff_px %zu/%u over2 %zu  %.1f ms\n",
                        !ok ? "FAIL " : d.max_diff == 0 ? "EXACT" : "OK<=2", bundle.c_str(), name.c_str(),
                        img->width, img->height, r->width, r->height, d.max_diff, d.visible_max,
                        d.pixels_diff, img->width * img->height, d.pixels_over2, ms);
        }
        std::printf("%d/%d passed\n", total - fails, total);
        return fails ? 1 : 0;
    }
    return Usage();
}
