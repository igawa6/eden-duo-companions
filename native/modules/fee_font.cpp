// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fee_font.h"

#include <array>
#include <cstring>

#include <algorithm>
#include <cmath>

namespace fee_font {
namespace {

float F(const lp_unity::Value* v) {
    return v ? static_cast<float>(v->AsFloat(0.0)) : 0.0f;
}
std::int32_t I(const lp_unity::Value* v) {
    return v ? static_cast<std::int32_t>(v->AsInt(0)) : 0;
}

std::int16_t Round16(float v) {
    return static_cast<std::int16_t>(std::lround(v));
}
std::uint16_t RoundU16(float v) {
    return static_cast<std::uint16_t>(std::clamp<long>(std::lround(v), 0, 65535));
}

} // namespace

std::string_view Bundle(Face face) {
    return face == Face::Bold ? "fe_assets_fonts/systemboldjpsdf.bundle"
                              : "fe_assets_fonts/systemjpsdf.bundle";
}
std::string_view AssetName(Face face) {
    return face == Face::Bold ? "SystemBoldJP SDF" : "SystemJP SDF";
}

const TmpGlyph* TmpFont::Find(std::uint32_t codepoint) const {
    const auto it = chars.find(codepoint);
    return it == chars.end() || it->second >= glyphs.size() ? nullptr : &glyphs[it->second];
}

std::uint32_t SubstituteFor(std::uint32_t codepoint) {
    switch (codepoint) {
    case 0x2014: // em dash
    case 0x2013: // en dash
        return '-';
    case 0x2019: // right single quotation mark
    case 0x2018:
        return '\'';
    default:
        return 0;
    }
}

std::optional<TmpFont> LoadTmpFont(lp_unity::Reader& reader, std::string_view bundle,
                                   std::string_view name) {
    try {
        const auto v = reader.ReadObjectByName(bundle, name, 114);
        if (!v || !v->Get("m_GlyphTable") || !v->Get("m_CharacterTable"))
            return std::nullopt;
        TmpFont f;
        if (const lp_unity::Value* fi = v->Get("m_FaceInfo")) {
            f.point_size = F(fi->Get("m_PointSize"));
            f.cap_line = F(fi->Get("m_CapLine"));
            f.ascent_line = F(fi->Get("m_AscentLine"));
            f.descent_line = F(fi->Get("m_DescentLine"));
        }
        f.atlas_w = I(v->Get("m_AtlasWidth"));
        f.atlas_h = I(v->Get("m_AtlasHeight"));
        f.padding = I(v->Get("m_AtlasPadding"));
        if (f.point_size <= 0 || f.cap_line <= 0 || f.atlas_w <= 0 || f.atlas_h <= 0)
            return std::nullopt;

        std::unordered_map<std::uint32_t, std::uint32_t> by_index;
        for (const lp_unity::Value& g : v->Get("m_GlyphTable")->items) {
            TmpGlyph t;
            t.index = static_cast<std::uint32_t>(I(g.Get("m_Index")));
            if (const lp_unity::Value* m = g.Get("m_Metrics")) {
                t.width = F(m->Get("m_Width"));
                t.height = F(m->Get("m_Height"));
                t.bearing_x = F(m->Get("m_HorizontalBearingX"));
                t.bearing_y = F(m->Get("m_HorizontalBearingY"));
                t.advance = F(m->Get("m_HorizontalAdvance"));
            }
            if (const lp_unity::Value* r = g.Get("m_GlyphRect")) {
                t.rect_x = I(r->Get("m_X"));
                t.rect_y = I(r->Get("m_Y"));
                t.rect_w = I(r->Get("m_Width"));
                t.rect_h = I(r->Get("m_Height"));
            }
            by_index.emplace(t.index, static_cast<std::uint32_t>(f.glyphs.size()));
            f.glyphs.push_back(t);
        }
        for (const lp_unity::Value& c : v->Get("m_CharacterTable")->items) {
            const lp_unity::Value* u = c.Get("m_Unicode");
            const lp_unity::Value* gi = c.Get("m_GlyphIndex");
            if (!u || !gi)
                continue;
            const auto it = by_index.find(static_cast<std::uint32_t>(gi->AsInt()));
            if (it != by_index.end())
                f.chars.emplace(static_cast<std::uint32_t>(u->AsInt()), it->second);
        }
        if (f.chars.empty() || f.glyphs.empty())
            return std::nullopt;

        const lp_unity::Value* at = v->Get("m_AtlasTextures");
        if (!at || at->items.empty())
            return std::nullopt;
        const lp_unity::Value* ptr = &at->items[0];
        const lp_unity::Value* file_id = ptr->Get("m_FileID");
        const lp_unity::Value* path_id = ptr->Get("m_PathID");
        if (!path_id || (file_id && file_id->AsInt() != 0) || path_id->AsInt() == 0)
            return std::nullopt;
        const auto tv = reader.ReadObject(bundle, path_id->AsInt());
        const lp_unity::Value* tn = tv ? tv->Get("m_Name") : nullptr;
        if (!tn)
            return std::nullopt;
        auto img = reader.LoadTexture(bundle, tn->AsString());
        if (!img || img->rgba.empty())
            return std::nullopt;
        f.atlas_image = std::move(*img);
        return f;
    } catch (...) {
        return std::nullopt;
    }
}

lp_unity::Image HostFontAtlas(const TmpFont& font) {
    lp_unity::Image out = font.atlas_image;
    // Re-centre the baked SDF around its 0.5 isoline (the design renderer's ramp, fe_lib.py
    // SDFFont.mask: a = clip((alpha - 0.5) * k + 0.5, 0, 1), k = 2 * padding at scale 1 -- this
    // atlas is served at its native bake resolution, so s == 1 and the same k applies here).
    const float k = std::max(1.0f, 2.0f * static_cast<float>(font.padding));
    for (std::size_t p = 0; p + 3 < out.rgba.size(); p += 4) {
        out.rgba[p] = out.rgba[p + 1] = out.rgba[p + 2] = 0xFF;
        const float a = static_cast<float>(out.rgba[p + 3]) / 255.0f;
        const float c = std::clamp((a - 0.5f) * k + 0.5f, 0.0f, 1.0f);
        out.rgba[p + 3] = static_cast<std::uint8_t>(std::lround(c * 255.0f));
    }
    return out;
}

void PackAtlas(TmpFont& font, std::uint32_t max_codepoint) {
    std::vector<std::uint32_t> used; // indices into font.glyphs
    std::vector<char> mark(font.glyphs.size(), 0);
    const auto want = [&](std::uint32_t cp) {
        if (const auto it = font.chars.find(cp); it != font.chars.end() && !mark[it->second]) {
            mark[it->second] = 1;
            used.push_back(it->second);
        }
    };
    for (const auto& [cp, idx] : font.chars) {
        (void)idx;
        if (cp <= max_codepoint)
            want(cp);
    }
    want('-');
    want('\'');
    // Shelf pack, tallest first, 2 px gutters.
    std::sort(used.begin(), used.end(), [&](std::uint32_t a, std::uint32_t b) {
        return font.glyphs[a].rect_h > font.glyphs[b].rect_h;
    });
    constexpr std::int32_t PackW = 1024, Gut = 2;
    std::int32_t x = 0, y = 0, shelf = 0;
    std::vector<std::array<std::int32_t, 2>> pos(font.glyphs.size());
    for (const std::uint32_t i : used) {
        const TmpGlyph& g = font.glyphs[i];
        const std::int32_t w = g.rect_w + 2 * Gut, h = g.rect_h + 2 * Gut;
        if (x + w > PackW) {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        pos[i] = {x + Gut, y + Gut};
        x += w;
        shelf = std::max(shelf, h);
    }
    const std::int32_t PackH = std::max(1, y + shelf);
    lp_unity::Image out;
    out.width = PackW;
    out.height = static_cast<std::uint32_t>(PackH);
    out.rgba.assign(std::size_t(PackW) * PackH * 4, 0);
    const lp_unity::Image& src = font.atlas_image;
    const std::int32_t ah = font.atlas_h;
    for (const std::uint32_t i : used) {
        TmpGlyph& g = font.glyphs[i];
        // Source rect in top-left image coordinates (TMP rects are bottom-origin).
        const std::int32_t sx = g.rect_x, sy = ah - g.rect_y - g.rect_h;
        for (std::int32_t r = 0; r < g.rect_h; ++r) {
            const std::int32_t yy = sy + r;
            if (yy < 0 || yy >= std::int32_t(src.height) || sx < 0 || sx + g.rect_w > std::int32_t(src.width))
                continue;
            std::memcpy(&out.rgba[(std::size_t(pos[i][1] + r) * PackW + pos[i][0]) * 4],
                        &src.rgba[(std::size_t(yy) * src.width + sx) * 4], std::size_t(g.rect_w) * 4);
        }
        g.rect_x = pos[i][0];
        g.rect_y = PackH - pos[i][1] - g.rect_h; // back to bottom-origin in the packed atlas
    }
    for (std::size_t i = 0; i < font.glyphs.size(); ++i)
        if (!mark[i])
            font.glyphs[i].rect_w = font.glyphs[i].rect_h = 0; // not packed: not drawable
    font.atlas_image = std::move(out);
    font.atlas_w = PackW;
    font.atlas_h = PackH;
}

bool BuildHostGlyphs(const TmpFont& font, std::uint32_t& first_codepoint,
                     std::vector<EdenDsmodFontGlyph>& glyphs, std::uint32_t& line_height,
                     std::uint32_t max_codepoint) {
    glyphs.clear();
    std::uint32_t lo = 0, hi = 0;
    bool any = false;
    for (const auto& [cp, idx] : font.chars) {
        (void)idx;
        if (cp > max_codepoint)
            continue;
        lo = any ? std::min(lo, cp) : cp;
        hi = any ? std::max(hi, cp) : cp;
        any = true;
    }
    if (!any)
        return false;
    first_codepoint = lo;
    line_height = std::max<std::uint32_t>(1, RoundU16(font.cap_line));
    glyphs.assign(hi - lo + 1, EdenDsmodFontGlyph{});
    const std::int32_t ah = font.atlas_h;

    const auto fill = [&](std::uint32_t cp, const TmpGlyph& g) {
        EdenDsmodFontGlyph& h = glyphs[cp - lo];
        h.x = RoundU16(static_cast<float>(g.rect_x));
        h.y = RoundU16(static_cast<float>(ah - g.rect_y - g.rect_h));
        h.w = RoundU16(static_cast<float>(g.rect_w));
        h.h = RoundU16(static_cast<float>(g.rect_h));
        h.bearing_x = Round16(g.bearing_x);
        h.bearing_y = Round16(g.bearing_y);
        h.advance = RoundU16(std::max(0.0f, g.advance));
    };

    for (std::uint32_t cp = lo; cp <= hi; ++cp) {
        if (const TmpGlyph* g = font.Find(cp); g && g->rect_w > 0 && g->rect_h > 0) {
            fill(cp, *g);
            continue;
        }
        if (const TmpGlyph* g = font.Find(cp); g && g->advance > 0 && (cp == ' ' || cp == 0xA0 || cp == 0x3000)) {
            fill(cp, *g); // blank glyphs (space) keep their advance with an empty rect
            glyphs[cp - lo].w = glyphs[cp - lo].h = 0;
            continue;
        }
        if (const std::uint32_t sub = SubstituteFor(cp); sub != 0) {
            if (const TmpGlyph* g = font.Find(sub); g && g->rect_w > 0 && g->rect_h > 0)
                fill(cp, *g);
        }
    }
    return true;
}

bool ParseRequest(std::span<const std::uint8_t> bytes, Face& face) {
    const std::string_view s{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    constexpr std::string_view Magic = "FEEFONT ";
    if (!s.starts_with(Magic))
        return false;
    std::string_view rest = s.substr(Magic.size());
    rest = rest.substr(0, rest.find_first_of("\r\n \t"));
    if (rest == "regular")
        face = Face::Regular;
    else if (rest == "bold")
        face = Face::Bold;
    else
        return false;
    return true;
}

FontCache::FontCache(lp_unity::RangeReader read, std::string asset_root)
    : reader_{std::move(read), {}, std::move(asset_root), 2} {}

void FontCache::SetAstcDecoder(lp_unity::AstcDecoder astc) {
    if (!astc) {
        reader_.SetAstcDecoder({});
        return;
    }
    // The host's decode_astc takes at most 2048x2048 per call; the font atlas is 4096x4096
    // (block-padded 4098). ASTC blocks are independent, so decode it in block-aligned tiles.
    reader_.SetAstcDecoder([astc = std::move(astc)](const std::uint8_t* blocks, std::size_t size,
                                                    std::uint32_t w, std::uint32_t h,
                                                    std::uint32_t bw, std::uint32_t bh,
                                                    std::vector<std::uint8_t>& rgba) {
        if (w <= 2048 && h <= 2048)
            return astc(blocks, size, w, h, bw, bh, rgba);
        if (!bw || !bh)
            return false;
        const std::uint32_t nbx = (w + bw - 1) / bw, nby = (h + bh - 1) / bh;
        if (size < std::size_t(nbx) * nby * 16)
            return false;
        const std::uint32_t tbx = 2048 / bw, tby = 2048 / bh;
        rgba.assign(std::size_t(w) * h * 4, 0);
        std::vector<std::uint8_t> in, out;
        for (std::uint32_t by0 = 0; by0 < nby; by0 += tby)
            for (std::uint32_t bx0 = 0; bx0 < nbx; bx0 += tbx) {
                const std::uint32_t cx = std::min(tbx, nbx - bx0), cy = std::min(tby, nby - by0);
                in.resize(std::size_t(cx) * cy * 16);
                for (std::uint32_t r = 0; r < cy; ++r)
                    std::memcpy(&in[std::size_t(r) * cx * 16],
                                blocks + (std::size_t(by0 + r) * nbx + bx0) * 16, std::size_t(cx) * 16);
                const std::uint32_t tw = cx * bw, th = cy * bh;
                if (!astc(in.data(), in.size(), tw, th, bw, bh, out) || out.size() < std::size_t(tw) * th * 4)
                    return false;
                const std::uint32_t px0 = bx0 * bw, py0 = by0 * bh;
                for (std::uint32_t y = 0; y < th && py0 + y < h; ++y) {
                    const std::uint32_t n = std::min(tw, w - px0);
                    std::memcpy(&rgba[(std::size_t(py0 + y) * w + px0) * 4], &out[std::size_t(y) * tw * 4],
                                std::size_t(n) * 4);
                }
            }
        return true;
    });
}

FontCache::Entry& FontCache::Decode(Face face) {
    Entry& e = cache_[static_cast<int>(face)];
    if (e.atlas && e.font)
        return e; // already decoded (success only -- a failure is retried next call)
    try {
        auto font = LoadTmpFont(reader_, Bundle(face), AssetName(face));
        if (!font)
            return e;
        PackAtlas(*font);
        auto atlas = HostFontAtlas(*font);
        if (atlas.rgba.empty())
            return e;
        font->atlas_image = {}; // the packed copy lives in `atlas`
        e.atlas = std::make_shared<lp_unity::Image>(std::move(atlas));
        e.font = std::make_shared<TmpFont>(std::move(*font));
        reader_.ClearCache(); // drop the parsed 4096x4096 source bundle
    } catch (...) {
    }
    return e;
}

bool FontCache::DecodeFont(const std::uint8_t* bytes, std::size_t size, void* receiver,
                           EdenDsmodFontSink sink) {
    if (!bytes || !sink || size == 0 || size > 256)
        return false;
    Face face{};
    if (!ParseRequest(std::span<const std::uint8_t>{bytes, size}, face))
        return false;
    std::scoped_lock lock{mutex_};
    const Entry& e = Decode(face);
    if (!e.font)
        return false;
    std::uint32_t first = 0, line = 0;
    std::vector<EdenDsmodFontGlyph> glyphs;
    if (!BuildHostGlyphs(*e.font, first, glyphs, line) || glyphs.empty())
        return false;
    sink(receiver, line, first, glyphs.data(), static_cast<std::uint32_t>(glyphs.size()));
    return true;
}

std::shared_ptr<const lp_unity::Image> FontCache::Atlas(Face face) {
    std::scoped_lock lock{mutex_};
    return Decode(face).atlas;
}

} // namespace fee_font
